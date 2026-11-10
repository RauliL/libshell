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

#include "main.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "eval.h"
#include "input.h"
#include "var.h"
#include "error.h"
#include "mystring.h"
#include "exec.h"
#include "parse_error.h"
#include <string_view>
#include <utility>

using namespace shell;

namespace shell {

namespace {
Result<std::string> find_dot_file(Context &sh, char *);
} // namespace

/*
 * Read and execute commands until EOF.  No prompting or other interactive
 * behaviour — used for scripts, -c companions, `.`, and sourced files.
 * Error::Generic is converted to an exit status at this boundary; Error::Exit/EXEND/EXINT
 * propagate as ShellResult errors to the driver.
 */
Result<int>
cmdloop(Context &sh)
{
	int status = 0;

	for (;;) {
		EvalSkipFlags skip;

		try {
			ParseCmdResult cmd = parsecmd(sh, 0);
			if (cmd.eof) {
				break;
			}
			Node *n = cmd.tree.get();
			auto r = evaltree(sh, n, {});
			if (!r) {
				sh.exception = r.error();
				if (r.error() == Error::Generic)
					return sh.exitstatus;
				return r;
			}
			status = *r;
		} catch (const ParseError &e) {
			report_parse_error(sh, e);
			return sh.exitstatus;
		}

		skip = sh.evalskip;
		if (skip) {
			sh.evalskip.reset(EvalSkip::Func).reset(EvalSkip::FuncDef);
			break;
		}
	}

	return status;
}

/*
 * Top-level interactive parse/eval loop: prompting, ignoreeof, and
 * recovering from parse errors at the prompt.
 */
Result<int>
Context::run_interactive()
{
	Context &sh = *this;
	int status = 0;
	int numeof = 0;

	for (;;) {
		EvalSkipFlags skip;

		try {
			ParseCmdResult cmd = parsecmd(sh, 1);
			if (cmd.eof) {
				if (numeof >= 50)
					break;
				if (!sh.opts.ignoreeof()) {
					out2c('\n');
					break;
				}
				out2str("\nUse \"exit\" to leave shell.\n");
				numeof++;
			} else {
				Node *n = cmd.tree.get();

				numeof = 0;

				auto r = evaltree(sh, n, {});

				if (!r) {
					sh.exception = r.error();
					if (r.error() == Error::Generic) {
						skip = sh.evalskip;
						if (skip) {
							sh.evalskip.reset(EvalSkip::Func).reset(EvalSkip::FuncDef);
							break;
						}
						continue;
					}
					return r;
				}
				if (n)
					status = *r;
			}
		} catch (const ParseError &e) {
			report_parse_error(sh, e);
			skip = sh.evalskip;
			if (skip) {
				sh.evalskip.reset(EvalSkip::Func).reset(EvalSkip::FuncDef);
				break;
			}
			continue;
		}

		skip = sh.evalskip;
		if (skip) {
			sh.evalskip.reset(EvalSkip::Func).reset(EvalSkip::FuncDef);
			break;
		}
	}

	return status;
}

/*
 * Read a file containing shell functions / commands.
 */
Result<void>
readcmdfile(Context &sh, std::string_view name)
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
namespace {
Result<std::string>
find_dot_file(Context &sh, char *basename)
{
	const char *path = pathval(sh);
	struct stat64 statb;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return std::string(basename);

	std::string fullname;
	while (padvance(sh, &path, basename, fullname) >= 0) {
		if ((!sh.pathopt || *sh.pathopt == 'f') &&
		    !stat64(fullname.c_str(), &statb) && S_ISREG(statb.st_mode)) {
			return fullname;
		}
	}

	/* not found in the PATH */
	return sh_error(sh, "{}: not found", basename);
}
} // namespace

Result<int>
dotcmd(Context &sh, int argc, char **argv)
{
	int status = 0;

	TRY_SHV(nextopt(sh, ""));
	argv = sh.bopt.argptr;

	if (*argv) {
		auto fullname_r = find_dot_file(sh, *argv);
		if (!fullname_r)
			return tl::unexpected(fullname_r.error());
		std::string fullname = *std::move(fullname_r);
		TRY_SHV(setinputfile(sh, fullname, INPUT_PUSH_FILE));
		sh.commandname = fullname;
		auto r = cmdloop(sh);
		popfile(sh);
		if (!r)
			return r;
		status = *r;
	}

	return status;
}

Result<int>
exitcmd(Context &sh, int argc, char **argv)
{
	if (argc > 1)
		TRY_SH_SET(sh.savestatus, number(sh, argv[1]));

	return exraise(sh, Error::Exit);
}

} // namespace shell
