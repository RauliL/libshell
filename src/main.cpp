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

static char *find_dot_file(ShellContext &sh, char *);

/*
 * Read and execute commands until EOF.  No prompting or other interactive
 * behaviour — used for scripts, -c companions, `.`, and sourced files.
 */
int
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
			status = evaltree(sh, n, {});
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
void
readcmdfile(ShellContext &sh, char *name)
{
	setinputfile(sh, name, INPUT_PUSH_FILE);
	cmdloop(sh);
	popfile(sh);
}

/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */
static char *
find_dot_file(ShellContext &sh, char *basename)
{
	char *fullname;
	const char *path = pathval();
	struct stat64 statb;
	int len;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return basename;

	while ((len = padvance(sh, &path, basename)) >= 0) {
		fullname = stackblock();
		if ((!sh.pathopt || *sh.pathopt == 'f') &&
		    !stat64(fullname, &statb) && S_ISREG(statb.st_mode)) {
			/* This will be freed by the caller. */
			return stalloc(sh, len);
		}
	}

	/* not found in the PATH */
	sh_error(sh, "%s: not found", basename);
	/* NOTREACHED */
}

int
dotcmd(ShellContext &sh, int argc, char **argv)
{
	int status = 0;

	nextopt(sh, nullstr);
	argv = sh.argptr;

	if (*argv) {
		char *fullname;

		fullname = find_dot_file(sh, *argv);
		setinputfile(sh, fullname, INPUT_PUSH_FILE);
		sh.commandname = fullname;
		status = cmdloop(sh);
		popfile(sh);
	}

	return status;
}

int
exitcmd(ShellContext &sh, int argc, char **argv)
{
	if (stoppedjobs(sh))
		return 0;

	if (argc > 1)
		sh.savestatus = number(sh, argv[1]);

	exraise(sh, EXEXIT);
	/* NOTREACHED */
}

#ifdef mkinit
INCLUDE "error.h"

FORKRESET {
	sh.handler = &sh.main_handler;
}
#endif
