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
 *	@(#)bltin.h	8.1 (Berkeley) 5/31/93
 */

/*
 * This file is included by programs which are optionally built into the
 * shell.  If SHELL is defined, we try to map the standard UNIX library
 * routines to ash routines using defines.
 */

#include "../shell.h"
#include "../mystring.h"
#include "../options.h"
#ifdef SHELL
#include "../memalloc.h"
#include "../output.h"
#include "../error.h"
#undef stdout
#undef stderr
#undef putc
#undef putchar
#undef fileno
#define stdout (sh.out1)
#define stderr (sh.out2)
#define printf(...) out1fmt(sh, __VA_ARGS__)
#define putc(c, file)	outc(sh, c, file)
#define putchar(c)	out1c(c)
#define FILE struct output
#define fprintf(f, ...) outfmt(sh, f, __VA_ARGS__)
#define fputs(s, f) ((f)->write(sh, (s)))
#define fflush(f) ((f)->flush(sh))
#define fileno(f) ((f)->fd)
#define ferror outerr
#define INITARGS(argv)
#define	error(...) sh_error(sh, __VA_ARGS__)
#define	warn(...) sh_warnx(sh, __VA_ARGS__)
#define	warnx(...) sh_warnx(sh, __VA_ARGS__)
#define exit(status) exitshell(sh)
#define setprogname(s)
#define getprogname() (sh.commandname.c_str())
#define setlocate(l,s) 0

#define getenv(p) bltinlookup(sh, (p))

#else
#undef NULL
#include <stdio.h>
#undef main
#define INITARGS(argv)	if ((commandname = argv[0]) == NULL) {fputs("Argc is zero\n", stderr); exit(2);} else
#endif

shell::Result<int> echocmd(ShellContext &sh, int, char **);
