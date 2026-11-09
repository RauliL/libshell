#include <libshell.hpp>

#include "../src/init.h"
#include "../src/main.h"
#include "../src/parse_error.h"
#include "../src/shell.h"
#include "../src/shell_context.h"
#include "../src/trap.h"

using namespace shell;

static Result<int> run_interactive(Context &);

int
main(int argc, char** argv)
{
	Context sh;
	int state = 0;

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif

resume:
	Error err{};
	bool failed = false;
	std::optional<std::string> minusc;

	if (state == 0) {
		const auto pr = procargs(sh, argv);

		if (!pr) {
			err = pr.error();
			failed = true;
		} else {
			minusc = std::move(pr->minusc);
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
		if (minusc)
			take(sh.eval(*minusc, sh.opts.stdin_opt() ? EvalFlags{} : EvalFlag::Exit));
	}
	if (!failed) {
		if (sh.opts.interactive())
			take(run_interactive(sh));
		else if (sh.opts.stdin_opt() || !minusc)
			take(cmdloop(sh));
	}

	if (failed) {
		Error ecode = err;

handle_err:
		exitreset(sh);
		sh.exception = ecode;
		if (
			ecode == Error::End ||
			ecode == Error::Exit ||
			state == 0 ||
			sh.opts.interactive() == 0 ||
			sh.shlvl
		)
			goto exit;

		reset(sh);

		if (ecode == Error::Interrupt) {
			out2c('\n');
#ifdef FLUSHERR
			sh.out2->flush(sh);
#endif
		}
		barrier();
		sh.suppressint = 0;
		if (sh.intpending) {
			const auto u = onint(sh);

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
static Result<int>
run_interactive(Context &sh)
{
	int status = 0;
	int numeof = 0;

	for (;;) {
		int skip;

		try {
			ParseCmdResult cmd = parsecmd(sh, 1);
			if (cmd.eof) {
				if (numeof >= 50)
					break;
				if (!sh.opts.ignoreeof()) {
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
					if (r.error() == Error::Generic) {
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
		} catch (const ParseError &e) {
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
