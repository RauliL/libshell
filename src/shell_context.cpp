#include "shell_context.h"

#include <utility>
#include <unistd.h>

#include "cd.h"
#include "init.h"

ShellContext::ShellContext(SignalOwnership signals)
{
	mypid = rootpid = getpid();

	if (signals == SignalOwnership::Claim)
		claim_signals();

	init(*this);
}

void
ShellContext::claim_signals()
{
	signaled = this;
}

void
ShellContext::release_signals()
{
	if (signaled == this)
		signaled = nullptr;
}

void
ShellContext::set_stdout(const shell::Output::sink_type &sink)
{
	output.sink = sink;
}

void
ShellContext::set_stderr(const shell::Output::sink_type &sink)
{
	errout.sink = sink;
}

std::string_view
ShellContext::get_cwd() const
{
	return cwd.curdir;
}

shell::Result<void>
ShellContext::set_cwd(std::string_view path)
{
	return setpwd(*this, path, 0);
}
