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

/*
 * Types of operations (passed to the errmsg routine).
 */

#define E_OPEN 01	/* opening a file */
#define E_CREAT 02	/* creating a file */
#define E_EXEC 04	/* executing a program */

/* Shell unwind codes (dash::ShellError::code()). */
#define EXINT 0		/* SIGINT received */
#define EXERROR 1	/* a generic error */
#define EXEND 3		/* exit the shell */
#define EXEXIT 4	/* exit the shell via exitcmd */

namespace dash {

/* Thrown by exraise/sh_error instead of longjmp; caught at eval/cmdloop boundaries. */
class ShellError {
public:
	explicit ShellError(int code) : code_(code) {}
	int code() const { return code_; }

private:
	int code_;
};

} /* namespace dash */


/*
 * These macros allow the user to suspend the handling of interrupt signals
 * over a period of time.  This is similar to SIGHOLD to or sigblock, but
 * much more efficient and portable.  (But hacking the kernel is so much
 * more fun than worrying about efficiency and portability. :-))
 */

/*
 * Interrupt deferral macros require ShellContext &sh in scope (same as the
 * former ash globals suppressint / sh.intpending, now fields of ShellContext).
 */
struct ShellContext;
void onint(ShellContext &sh) __attribute__((__noreturn__));

/*
 * INTOFF/INTON use suppressint/sh.intpending, which shell_access.h maps to
 * suppressint / sh.intpending when ShellContext &sh is in scope.
 */
#define barrier() ({ __asm__ __volatile__ ("": : :"memory"); })
#define INTOFF \
	({ \
		sh.suppressint++; \
		barrier(); \
		0; \
	})
#ifdef REALLY_SMALL
void __inton(ShellContext &sh);
#define INTON __inton(sh)
#else
#define INTON \
	({ \
		barrier(); \
		if (--sh.suppressint == 0 && sh.intpending) onint(sh); \
		0; \
	})
#endif
#define FORCEINTON \
	({ \
		barrier(); \
		sh.suppressint = 0; \
		if (sh.intpending) onint(sh); \
		0; \
	})
#define SAVEINT(v) ((v) = sh.suppressint)
#define RESTOREINT(v) \
	({ \
		barrier(); \
		if ((sh.suppressint = (v)) == 0 && sh.intpending) onint(sh); \
		0; \
	})
#define CLEAR_PENDING_INT sh.intpending = 0
#define int_pending() sh.intpending

void exraise(ShellContext &sh, int) __attribute__((__noreturn__));
void sh_error(ShellContext &sh, const char *, ...) __attribute__((__noreturn__));
void exerror(ShellContext &sh, int, const char *, ...) __attribute__((__noreturn__));
const char *errmsg(ShellContext &sh, int, int);

void sh_warnx(ShellContext &sh, const char *, ...);
#endif
