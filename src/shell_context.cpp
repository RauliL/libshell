#include "shell_context.h"

#include <utility>
#include <unistd.h>

#include "cd.h"
#include "init.h"

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
Context::set_stdout(const shell::Output::sink_type &sink)
{
	output.sink = sink;
}

void
Context::set_stderr(const shell::Output::sink_type &sink)
{
	errout.sink = sink;
}

std::string_view
Context::get_cwd() const
{
	return cwd.curdir;
}

shell::Result<void>
Context::set_cwd(std::string_view path)
{
	return setpwd(*this, path, 0);
}
