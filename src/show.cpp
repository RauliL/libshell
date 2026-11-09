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

#include <stdio.h>
#include <stdarg.h>

#include "shell.h"
#include "shell_context.h"
#include "parser.h"
#include "nodes.h"
#include "mystring.h"
#include "show.h"
#include "options.h"


#ifdef DEBUG
static void trstring(ShellContext &sh, char *);


/*
 * Debugging stuff.
 */





static void
trputc(ShellContext &sh, int c)
{
	if (debug != 1)
		return;
	putc(c, sh.tracefile);
}

static void
trace(ShellContext &sh, const char *fmt, ...)
{
	va_list va;

	if (debug != 1)
		return;
	va_start(va, fmt);
	std::vfprintf(sh.tracefile, fmt, va);
	va_end(va);
}

static void
tracev(ShellContext &sh, const char *fmt, va_list va)
{
	if (debug != 1)
		return;
	std::vfprintf(sh.tracefile, fmt, va);
}


void
trputs(ShellContext &sh, const char *s)
{
	if (debug != 1)
		return;
	fputs(s, sh.tracefile);
}


static void
trstring(ShellContext &sh, char *s)
{
	char *p;
	char c;

	if (debug != 1)
		return;
	putc('"', sh.tracefile);
	for (p = s ; *p ; p++) {
		switch ((signed char)*p) {
		case '\n':  c = 'n';  goto backslash;
		case '\t':  c = 't';  goto backslash;
		case '\r':  c = 'r';  goto backslash;
		case '\0':  goto backslash;
		case '\\':  c = '\\';  goto backslash;
		case CTLESC:  c = 'e';  goto backslash;
		case CTLVAR:  c = 'v';  goto backslash;
		case CTLBACKQ:  c = 'q';  goto backslash;
backslash:	  putc('\\', sh.tracefile);
			putc(c, sh.tracefile);
			break;
		default:
			if (*p >= ' ' && *p <= '~')
				putc(*p, sh.tracefile);
			else {
				putc('\\', sh.tracefile);
				putc(*p >> 6 & 03, sh.tracefile);
				putc(*p >> 3 & 07, sh.tracefile);
				putc(*p & 07, sh.tracefile);
			}
			break;
		}
	}
	putc('"', sh.tracefile);
}


void
trargs(ShellContext &sh, char **ap)
{
	if (debug != 1)
		return;
	while (*ap) {
		trstring(sh, *ap++);
		if (*ap)
			putc(' ', sh.tracefile);
		else
			putc('\n', sh.tracefile);
	}
}


void
opentrace(ShellContext &sh)
{
	const char *s = "./trace";
#ifdef O_APPEND
	int flags;
#endif

	if (debug != 1) {
		if (sh.tracefile)
			fflush(sh.tracefile);
		/* leave open because libedit might be using it */
		return;
	}
	if (sh.tracefile) {
#ifndef __KLIBC__
		if (!freopen(s, "a", sh.tracefile)) {
#else
		if (!(!fclose(sh.tracefile) && (sh.tracefile = fopen(s, "a")))) {
#endif /* __KLIBC__ */
			fprintf(stderr, "Can't re-open %s\n", s);
			debug = 0;
			return;
		}
	} else {
		if ((sh.tracefile = fopen(s, "a")) == nullptr) {
			fprintf(stderr, "Can't open %s\n", s);
			debug = 0;
			return;
		}
	}
#ifdef O_APPEND
	if ((flags = fcntl(fileno(sh.tracefile), F_GETFL, 0)) >= 0)
		fcntl(fileno(sh.tracefile), F_SETFL, flags | O_APPEND);
#endif
#ifndef __KLIBC__
	setlinebuf(sh.tracefile);
#endif /* __KLIBC__ */
	fputs("\nTracing started.\n", sh.tracefile);
}
#endif /* DEBUG */
