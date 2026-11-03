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

/*
 * Errors — shell unwind via ShellResult (tl::expected), not exceptions.
 */

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include <cstdio>

#include "jobs.h"
#include "shell.h"
#include "shell_context.h"
#include "main.h"
#include "output.h"
#include "error.h"
#include "show.h"
#include "eval.h"
#include "parser.h"
#include "system.h"


/*
 * Raise a shell unwind.  Stores the code in sh.exception and returns
 * tl::unexpected for the caller to propagate as ShellResult.
 */

tl::unexpected<shell::Error>
exraise(ShellContext &sh, shell::Error e)
{
	if (sh.vforked)
		_exit(sh.exitstatus);

	INTOFF;

	sh.exception = e;

	return tl::unexpected(e);
}


/*
 * Called when a deferred SIGINT is delivered (INTON).  Restore the
 * default disposition and re-raise so the process exits.
 */

tl::unexpected<shell::Error>
onint(ShellContext &sh)
{

	sh.intpending = 0;
	sigclearmask();
	signal(SIGINT, SIG_DFL);
	::raise(SIGINT);
	sh.exitstatus = SIGINT + 128;

	return exraise(sh, shell::Error::Interrupt);
}

static void
exwarning_msg(ShellContext &sh, std::string_view msg)
{
	struct output *errs = sh.out2;
	const char *name = sh.arg0.empty() ? "sh" : sh.arg0.c_str();

	if (sh.commandname.empty())
        errs->write(sh, std::format("{}: {}: ", name, sh.errlinno));
	else
        errs->write(sh, std::format("{}: {}: {}: ", name, sh.errlinno, sh.commandname));
	errs->write(sh, msg);
#if FLUSHERR
	outc(sh, '\n', errs);
#else
	outcslow(sh, '\n', errs);
#endif
}

tl::unexpected<shell::Error>
sh_error_msg(ShellContext &sh, std::string msg)
{
	sh.exitstatus = 2;

#ifdef DEBUG
	TRACE(("sh_error_msg(\"%s\") pid=%d\n", msg.c_str(), getpid()));
#endif
	exwarning_msg(sh, msg);
	flushall(sh);

	return exraise(sh, shell::Error::Generic);
}

tl::unexpected<shell::Error>
exerror_msg(ShellContext &sh, shell::Error cond, std::string msg)
{
#ifdef DEBUG
	TRACE(("exerror_msg(%d, \"%s\") pid=%d\n", static_cast<int>(cond),
	       msg.c_str(), getpid()));
#endif
	exwarning_msg(sh, msg);
	flushall(sh);

	return exraise(sh, cond);
}

void
sh_warnx_msg(ShellContext &sh, std::string msg)
{
	exwarning_msg(sh, msg);
}


const char *
errmsg(int e, int action)
{
	if (e != ENOENT && e != ENOTDIR)
		return std::strerror(e);

	if (action & E_OPEN)
		return "No such file";
	else if (action & E_CREAT)
		return "Directory nonexistent";
	else
		return "not found";
}


#ifdef REALLY_SMALL
shell::Result<void>
__inton(ShellContext &sh)
{
	if (--sh.suppressint == 0 && sh.intpending)
		return onint(sh);
	return {};
}
#endif
