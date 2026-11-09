#include "shell_context.h"

#include <unistd.h>

#include "init.h"
#include "main.h"

ShellContext::ShellContext()
{
	signaled = this;
	mypid = rootpid = getpid();

#ifdef __GLIBC__
	/*
	 * Several translation units redirect errno through dash_errno
	 * (see main.h). Point it at this thread's errno before any
	 * library code runs.
	 */
	dash_errno = __errno_location();
#endif

	init(*this);
}
