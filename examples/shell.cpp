#include <iostream>

#include <libshell.hpp>

#include "../src/init.h"
#include "../src/main.h"
#include "../src/parse_error.h"
#include "../src/shell.h"
#include "../src/shell_context.h"
#include "../src/trap.h"

static shell::Result<int> run_interactive(ShellContext &);

int
main(int argc, char** argv)
{
	ShellContext sh;
	int state = 0;

#ifdef __GLIBC__
	dash_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif

	sh.mypid = sh.rootpid = getpid();
	init(sh);

resume:
	shell::Error err{};
	bool failed = false;

	if (state == 0) {
		const auto pr = procargs(sh, argv);

		if (!pr) {
			err = pr.error();
			failed = true;
		}
	}

	auto take = [&](auto &&r) {
		if (!failed && !r) {
			err = r.error();
			failed = true;
		}
	};

	if (!failed && state < 4) {
		state = 4;
		if (sh.minusc)
			take(sh.eval(*sh.minusc, sflag ? EvalFlags{} : EvalFlag::exit));
	}
	if (!failed) {
		if (iflag)
			take(run_interactive(sh));
		else if (sflag || !sh.minusc)
			take(cmdloop(sh));
	}

	if (failed) {
		shell::Error ecode = err;

handle_err:
		exitreset(sh);
		sh.exception = ecode;
		if (
			ecode == shell::Error::End ||
			ecode == shell::Error::Exit ||
			state == 0 ||
			iflag == 0 ||
			sh.shlvl
		)
			goto exit;

		reset(sh);

		if (ecode == shell::Error::Interrupt) {
			out2c('\n');
#ifdef FLUSHERR
			sh.out2->flush(sh);
#endif
		}
		barrier();
		sh.suppressint = 0;
		if (sh.intpending) {
			auto u = onint(sh);
			ecode = u.value(); /* tl::unexpected<ShellErr>::value */
			goto handle_err;
		}
		goto resume;
	}
exit:
#if PROFILE
	monitor(0);
#endif
#if GPROF
	{
		extern void _mcleanup();
		_mcleanup();
	}
#endif
	exitshell(sh);

	return EXIT_SUCCESS;
}

/*
 * Top-level interactive parse/eval loop: prompting, ignoreeof, and
 * recovering from parse errors at the prompt.
 */
static shell::Result<int>
run_interactive(ShellContext &sh)
{
	int status = 0;
	int numeof = 0;

	TRACE(("run_interactive() called\n"));
	for (;;) {
		int skip;

		try {
			ParseCmdResult cmd = parsecmd(sh, 1);
			if (cmd.eof) {
				if (numeof >= 50)
					break;
				if (!Iflag) {
					out2c('\n');
#ifdef FLUSHERR
					sh.out2->flush(sh);
#endif
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
					if (r.error() == shell::Error::Generic) {
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
			}
		} catch (const shell::ParseError &e) {
			report_parse_error(sh, e);
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
