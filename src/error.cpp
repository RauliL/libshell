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

#include <cstdarg>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cstdio>

#include "jobs.h"
#include "shell.h"
#include "shell_context.h"
#include "main.h"
#include "options.h"
#include "output.h"
#include "error.h"
#include "show.h"
#include "eval.h"
#include "parser.h"
#include "system.h"


static tl::unexpected<dash::ShellErr>
exverror(ShellContext &sh, int, const char *, va_list);

/*
 * Raise a shell unwind.  Stores the code in sh.exception and returns
 * tl::unexpected for the caller to propagate as ShellResult.
 */

tl::unexpected<dash::ShellErr>
exraise(ShellContext &sh, int e)
{
	if (sh.vforked)
		_exit(sh.exitstatus);

	INTOFF;

	sh.exception = e;
	return dash::fail(e);
}


/*
 * Called when a deferred SIGINT is delivered (INTON).  Non-interactive
 * shells re-raise SIGINT; interactive shells unwind with EXINT.
 */

tl::unexpected<dash::ShellErr>
onint(ShellContext &sh)
{

	sh.intpending = 0;
	sigclearmask();
	if (!(rootshell && iflag)) {
		signal(SIGINT, SIG_DFL);
		::raise(SIGINT);
	}
	sh.exitstatus = SIGINT + 128;
	return exraise(sh, EXINT);
}

static void
exvwarning2(ShellContext &sh, const char *msg, va_list ap)
{
	struct output *errs;
	const char *name;
	const char *fmt;

	errs = sh.out2;
	name = sh.arg0.empty() ? "sh" : sh.arg0.c_str();
	if (sh.commandname.empty())
		fmt = "%s: %d: ";
	else
		fmt = "%s: %d: %s: ";
	outfmt(sh, errs, fmt, name, sh.errlinno, sh.commandname.c_str());
	doformat(sh, errs, msg, ap);
#if FLUSHERR
	outc(sh, '\n', errs);
#else
	outcslow(sh, '\n', errs);
#endif
}

#define exvwarning(a, b, c) exvwarning2(sh, b, c)

static tl::unexpected<dash::ShellErr>
exverror(ShellContext &sh, int cond, const char *msg, va_list ap)
{
#ifdef DEBUG
	if (msg) {
		va_list aq;
		TRACE(("exverror(%d, \"", cond));
		va_copy(aq, ap);
		TRACEV((msg, aq));
		va_end(aq);
		TRACE(("\") pid=%d\n", getpid()));
	} else
		TRACE(("exverror(%d, NULL) pid=%d\n", getpid()));
	if (msg)
#endif
		exvwarning(-1, msg, ap);

	flushall(sh);
	return exraise(sh, cond);
}


tl::unexpected<dash::ShellErr>
sh_error(ShellContext &sh, const char *msg, ...)
{
	va_list ap;

	sh.exitstatus = 2;

	va_start(ap, msg);
	auto r = exverror(sh, EXERROR, msg, ap);
	va_end(ap);
	return r;
}


tl::unexpected<dash::ShellErr>
exerror(ShellContext &sh, int cond, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	auto r = exverror(sh, cond, msg, ap);
	va_end(ap);
	return r;
}

void
sh_warnx(ShellContext &sh, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	exvwarning(-1, fmt, ap);
	va_end(ap);
}


const char *
errmsg(ShellContext &sh, int e, int action)
{
	(void)sh;
	if (e != ENOENT && e != ENOTDIR)
		return strerror(e);

	if (action & E_OPEN)
		return "No such file";
	else if (action & E_CREAT)
		return "Directory nonexistent";
	else
		return "not found";
}


#ifdef REALLY_SMALL
dash::ShellResult<void>
__inton(ShellContext &sh)
{
	if (--sh.suppressint == 0 && sh.intpending)
		return onint(sh);
	return {};
}
#endif
