#include <libshell.hpp>

using namespace shell;

int
main(int argc, char **argv)
{
	Context sh;
	int state = 0;

resume:
	Error err{};
	bool failed = false;
	std::optional<std::string> minusc;

	if (state == 0) {
		const auto pr = sh.parse_args(argv);

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
			take(sh.run_interactive());
		else if (sh.opts.stdin_opt() || !minusc)
			take(sh.cmdloop());
	}

	if (failed) {
		Error ecode = err;

handle_err:
		sh.exit_reset();
		sh.exception = ecode;
		if (
			ecode == Error::End ||
			ecode == Error::Exit ||
			state == 0 ||
			sh.opts.interactive() == 0 ||
			sh.shlvl
		)
			goto exit;

		sh.reset();

		if (ecode == Error::Interrupt) {
			sh.out2->write(sh, "\n");
		}
		sh.suppressint = 0;
		if (sh.intpending) {
			ecode = sh.on_interrupt().error();
			goto handle_err;
		}
		goto resume;
	}
exit:
	sh.exit_shell();
}
