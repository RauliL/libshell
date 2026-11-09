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
ShellContext::set_stdout(std::function<void(std::string_view)> sink)
{
	output.sink = std::move(sink);
}

void
ShellContext::set_stderr(std::function<void(std::string_view)> sink)
{
	errout.sink = std::move(sink);
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
