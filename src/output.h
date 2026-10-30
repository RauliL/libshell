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
 *	@(#)output.h	8.2 (Berkeley) 5/4/95
 */

#ifndef OUTPUT_INCL

#include <stdarg.h>
#ifdef USE_GLIBC_STDIO
#include <stdio.h>
#endif
#include <sys/types.h>

struct output {
#ifdef USE_GLIBC_STDIO
	FILE *stream;
#endif
	char *nextc;
	char *end;
	char *buf;
	size_t bufsize;
	int fd;
	int flags;
};

struct ShellContext;

void outmem(ShellContext &sh, const char *, size_t, struct output *);
void outstr(ShellContext &sh, const char *, struct output *);
#ifndef USE_GLIBC_STDIO
void outcslow(ShellContext &sh, int, struct output *);
#endif
void flushall(ShellContext &sh);
void flushout(ShellContext &sh, struct output *);
void outfmt(ShellContext &sh, struct output *, const char *, ...)
    __attribute__((__format__(__printf__,3,4)));
void out1fmt(ShellContext &sh, const char *, ...)
    __attribute__((__format__(__printf__,2,3)));
int fmtstr(ShellContext &sh, char *, size_t, const char *, ...)
    __attribute__((__format__(__printf__,4,5)));
dash::ShellResult<int> xasprintf(ShellContext &sh, char **, const char *, ...);
#ifndef USE_GLIBC_STDIO
void doformat(ShellContext &sh, struct output *, const char *, va_list);
#endif
int xwrite(ShellContext &sh, int, const void *, size_t);
#ifdef notyet
#ifdef USE_GLIBC_STDIO
void initstreams(ShellContext &sh);
void openmemout(ShellContext &sh);
int __closememout(ShellContext &sh);
#endif
#endif

#define OUTPUT_ERR 01		/* error occurred on output */

#ifdef USE_GLIBC_STDIO
#define doformat(sh, d, f, a)	vfprintf((d)->stream, (f), (a))
#endif
#define out1c(c)	outc(sh, (c), sh.out1)
#define out2c(c)	outcslow(sh, (c), sh.out2)
#define out1mem(s, l)	outmem(sh, (s), (l), sh.out1)
#define out1str(s)	outstr(sh, (s), sh.out1)
#define out2str(s)	outstr(sh, (s), sh.out2)
#define outerr(f)	(f)->flags

#define OUTPUT_INCL
#endif
