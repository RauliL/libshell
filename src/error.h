#ifndef ERROR_H
#define ERROR_H
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
 *
 *	@(#)error.h	8.2 (Berkeley) 5/4/95
 */

#include <signal.h>

#include "result.h"

/*
 * Types of operations (passed to the errmsg routine).
 */

#define E_OPEN 01	/* opening a file */
#define E_CREAT 02	/* creating a file */
#define E_EXEC 04	/* executing a program */

/* Shell unwind codes (dash::ShellErr::code). */
#define EXINT 0		/* SIGINT received */
#define EXERROR 1	/* a generic error */
#define EXEND 3		/* exit the shell */
#define EXEXIT 4	/* exit the shell via exitcmd */

/*
 * Interrupt deferral macros require ShellContext &sh in scope.
 * INTON/FORCEINTON/RESTOREINT return from the enclosing ShellResult
 * function when a deferred SIGINT must unwind (via onint → exraise).
 */
struct ShellContext;
tl::unexpected<dash::ShellErr> onint(ShellContext &sh);

#define barrier() ({ __asm__ __volatile__ ("": : :"memory"); })
#define INTOFF \
	({ \
		sh.suppressint++; \
		barrier(); \
		0; \
	})
#ifdef REALLY_SMALL
dash::ShellResult<void> __inton(ShellContext &sh);
#define INTON \
	do { \
		if (auto _r = __inton(sh); !_r) \
			return _r.error(); \
	} while (0)
#else
#define INTON \
	do { \
		barrier(); \
		if (--sh.suppressint == 0 && sh.intpending) \
			return onint(sh); \
	} while (0)
#endif
#define FORCEINTON \
	do { \
		barrier(); \
		sh.suppressint = 0; \
		if (sh.intpending) \
			return onint(sh); \
	} while (0)
#define SAVEINT(v) ((v) = sh.suppressint)
#define RESTOREINT(v) \
	do { \
		barrier(); \
		if (((sh.suppressint = (v)) == 0) && sh.intpending) \
			return onint(sh); \
	} while (0)
#define CLEAR_PENDING_INT sh.intpending = 0
#define int_pending() sh.intpending

/* For void helpers that cannot return ShellResult: drop suppressint and
 * leave intpending set so a later ShellResult INTON/FORCEINTON unwinds. */
#define INTON_DEFER 	do { 		barrier(); 		--sh.suppressint; 	} while (0)

/* Avoid `.error()` in macros: bltin.h `#define error(...)` would rewrite it. */
template <typename T>
inline tl::unexpected<dash::ShellErr>
shell_fail_of(const dash::ShellResult<T> &r)
{
	return tl::unexpected(r.error());
}

/* Propagate a nested ShellResult; for void results use TRY_SHV.
 * Returns tl::unexpected so the enclosing ShellResult<T> may differ in T. */
#define TRY_SH(expr) \
	({ \
		auto _try_sh_r = (expr); \
		if (!_try_sh_r) \
			return shell_fail_of(_try_sh_r); \
		*_try_sh_r; \
	})
#define TRY_SHV(expr) \
	do { \
		if (auto _try_sh_r = (expr); !_try_sh_r) \
			return shell_fail_of(_try_sh_r); \
	} while (0)

tl::unexpected<dash::ShellErr> exraise(ShellContext &sh, int);
tl::unexpected<dash::ShellErr> sh_error(ShellContext &sh, const char *, ...);
tl::unexpected<dash::ShellErr> exerror(ShellContext &sh, int, const char *, ...);

const char *errmsg(int, int);

void sh_warnx(ShellContext &sh, const char *, ...);
#endif
