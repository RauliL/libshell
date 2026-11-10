#include <libshell.hpp>

#include "cd.h"
#include "init.h"
#include "main.h"
#include "options.h"
#include "trap.h"

using namespace shell;

namespace shell {

Context::Context(SignalOwnership signals)
{
	mypid = rootpid = getpid();

	if (signals == SignalOwnership::Claim)
		claim_signals();

	init(*this);
}

void
Context::claim_signals()
{
	signaled = this;
}

void
Context::release_signals()
{
	if (signaled == this)
		signaled = nullptr;
}

void
Context::set_stdout(const Output::sink_type &sink)
{
	output.sink = sink;
}

void
Context::set_stderr(const Output::sink_type &sink)
{
	errout.sink = sink;
}

Result<void>
Context::set_cwd(const std::filesystem::path &path)
{
	return setpwd(*this, path, false);
}

Result<ProcArgsResult>
Context::parse_args(char **argv)
{
	return procargs(*this, argv);
}

Result<int>
Context::cmdloop()
{
	return shell::cmdloop(*this);
}

void
Context::exit_reset()
{
	shell::exitreset(*this);
}

void
Context::reset()
{
	shell::reset(*this);
}

void
Context::exit_shell()
{
	shell::exitshell(*this);
}

Result<void>
Context::on_interrupt()
{
	return onint(*this);
}

} // namespace shell
