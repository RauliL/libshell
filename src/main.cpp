/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <string>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

#ifdef __GLIBC__
int *dash_errno;
#endif
#include "shell_context.h"
#include "main.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "exec.h"
#include "parse_error.h"
#include "nodes_free.h"
#include "show.h"

static dash::ShellResult<std::string> find_dot_file(ShellContext &sh, char *);

/*
 * Top-level interactive parse/eval loop: prompting, ignoreeof, and
 * recovering from parse errors at the prompt.
 */
dash::ShellResult<int>
run_interactive(ShellContext &sh)
{
	union node *n;
	int status = 0;
	int numeof = 0;

	TRACE(("run_interactive() called\n"));
	for (;;) {
		int skip;

		if (sh.jobctl)
			showjobs(sh, sh.out2, SHOW_CHANGED);
		try {
			n = parsecmd(sh, 1);
			if (n == NEOF) {
				clear_stack_temps(sh);
				if (numeof >= 50)
					break;
				if (!stoppedjobs(sh)) {
					if (!Iflag) {
						out2c('\n');
#ifdef FLUSHERR
						flushout(sh, sh.out2);
#endif
						break;
					}
					out2str("\nUse \"exit\" to leave shell.\n");
				}
				numeof++;
			} else {
				sh.job_warning = (sh.job_warning == 2) ? 1 : 0;
				numeof = 0;
				auto r = evaltree(sh, n, {});
				if (!r) {
					sh.exception = r.error().code;
					if (n && n != NEOF)
						freenode(n);
					clear_stack_temps(sh);
					if (r.error().code == EXERROR) {
						skip = sh.evalskip;
						if (skip) {
							sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
							break;
						}
						continue;
					}
					return r;
				}
				if (n)
					status = *r;
				if (n && n != NEOF)
					freenode(n);
				clear_stack_temps(sh);
			}
		} catch (const dash::ParseError &e) {
			report_parse_error(sh, e);
			clear_stack_temps(sh);
			skip = sh.evalskip;
			if (skip) {
				sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
				break;
			}
			continue;
		}

		skip = sh.evalskip;
		if (skip) {
			sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
			break;
		}
	}

	return status;
}

/*
 * Read and execute commands until EOF.  No prompting or other interactive
 * behaviour — used for scripts, -c companions, `.`, and sourced files.
 * EXERROR is converted to an exit status at this boundary; EXEXIT/EXEND/EXINT
 * propagate as ShellResult errors to the driver.
 */
dash::ShellResult<int>
cmdloop(ShellContext &sh)
{
	union node *n;
	int status = 0;

	TRACE(("cmdloop() called\n"));
	for (;;) {
		int skip;

		if (sh.jobctl)
			showjobs(sh, sh.out2, SHOW_CHANGED);
		try {
			n = parsecmd(sh, 0);
			if (n == NEOF) {
				clear_stack_temps(sh);
				break;
			}
			sh.job_warning = (sh.job_warning == 2) ? 1 : 0;
			auto r = evaltree(sh, n, {});
			if (!r) {
				sh.exception = r.error().code;
				if (n && n != NEOF)
					freenode(n);
				clear_stack_temps(sh);
				if (r.error().code == EXERROR)
					return sh.exitstatus;
				return r;
			}
			status = *r;
			if (n && n != NEOF)
				freenode(n);
			clear_stack_temps(sh);
		} catch (const dash::ParseError &e) {
			report_parse_error(sh, e);
			clear_stack_temps(sh);
			return sh.exitstatus;
		}

		skip = sh.evalskip;
		if (skip) {
			sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
			break;
		}
	}

	return status;
}

/*
 * Read a file containing shell functions / commands.
 */
dash::ShellResult<void>
readcmdfile(ShellContext &sh, std::string_view name)
{
	TRY_SHV(setinputfile(sh, name, INPUT_PUSH_FILE));
	auto r = cmdloop(sh);
	popfile(sh);
	if (!r)
		return tl::unexpected(r.error());
	return {};
}

/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */
static dash::ShellResult<std::string>
find_dot_file(ShellContext &sh, char *basename)
{
	const char *path = pathval();
	struct stat64 statb;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return std::string(basename);

	while (padvance(sh, &path, basename) >= 0) {
		const char *fullname = stackblock();
		if ((!sh.pathopt || *sh.pathopt == 'f') &&
		    !stat64(fullname, &statb) && S_ISREG(statb.st_mode)) {
			return std::string(fullname);
		}
	}

	/* not found in the PATH */
	return sh_error(sh, "%s: not found", basename);
}

dash::ShellResult<int>
dotcmd(ShellContext &sh, int argc, char **argv)
{
	int status = 0;

	TRY_SHV(nextopt(sh, nullstr));
	argv = sh.argptr;

	if (*argv) {
		auto fullname_r = find_dot_file(sh, *argv);
		if (!fullname_r)
			return tl::unexpected(fullname_r.error());
		std::string fullname = *std::move(fullname_r);
		TRY_SHV(setinputfile(sh, fullname.c_str(), INPUT_PUSH_FILE));
		sh.commandname = fullname;
		auto r = cmdloop(sh);
		popfile(sh);
		if (!r)
			return r;
		status = *r;
	}

	return status;
}

dash::ShellResult<int>
exitcmd(ShellContext &sh, int argc, char **argv)
{
	if (stoppedjobs(sh))
		return 0;

	if (argc > 1)
		sh.savestatus = TRY_SH(number(sh, argv[1]));

	return exraise(sh, EXEXIT);
}
