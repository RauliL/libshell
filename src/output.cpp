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
 * Shell sh.output routines.  We use our own sh.output routines because:
 *	When a builtin command is interrupted we have to discard
 *		any pending sh.output.
 *	When a builtin command appears in back quotes, we want to
 *		save the sh.output of the command in a region obtained
 *		via malloc, rather than doing a fork and reading the
 *		sh.output of the command via a pipe.
 *	Our sh.output routines may be smaller than the stdio routines.
 */

#include <sys/types.h>		/* quad_t */
#include <sys/param.h>		/* BSD4_4 */
#include <sys/ioctl.h>

#include <stdio.h>	/* defines BUFSIZ */
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef USE_GLIBC_STDIO
#include <fcntl.h>
#endif
#include <limits.h>

#include "shell.h"
#include "shell_context.h"
#include "syntax.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "main.h"
#include "system.h"


#define OUTBUFSIZ BUFSIZ
#define MEM_OUT -3		/* sh.output to dynamically allocated memory */


static int xvsnprintf(ShellContext &sh, char *, size_t, const char *, va_list);


#ifdef mkinit

INCLUDE "output.h"
INCLUDE "memalloc.h"

INIT {
#ifdef USE_GLIBC_STDIO
	initstreams(sh);
#endif
}

RESET {
#ifdef notyet
	sh.out1 = &sh.output;
	sh.out2 = &sh.errout;
#ifdef USE_GLIBC_STDIO
	if (memout.stream != NULL)
		__closememout(sh);
#endif
	if (memout.buf != NULL) {
		ckfree(memout.buf);
		memout.buf = NULL;
	}
#endif
}

#endif


void
outmem(ShellContext &sh, const char *p, size_t len, struct output *dest)
{
#ifdef USE_GLIBC_STDIO
	INTOFF;
	fwrite(p, 1, len, dest->stream);
	INTON_DEFER;
#else
	size_t bufsize;
	size_t offset;
	size_t nleft;

	nleft = dest->end - dest->nextc;
	if (likely(nleft >= len)) {
buffered:
		dest->nextc = static_cast<char *>(mempcpy(dest->nextc, p, len));
		return;
	}

	bufsize = dest->bufsize;
	if (!bufsize) {
		;
	} else if (dest->buf == NULL) {
#ifdef notyet
		if (dest->fd == MEM_OUT && len > bufsize) {
			bufsize = len;
		}
#endif
		offset = 0;
#ifdef notyet
		goto alloc;
	} else if (dest->fd == MEM_OUT) {
		offset = bufsize;
		if (bufsize >= len) {
			bufsize <<= 1;
		} else {
			bufsize += len;
		}
		if (bufsize < offset)
			goto err;
alloc:
#endif
		INTOFF;
		dest->buf = ckrealloc(sh, dest->buf, bufsize);
		dest->bufsize = bufsize;
		dest->end = dest->buf + bufsize;
		dest->nextc = dest->buf + offset;
		INTON_DEFER;
	} else {
		flushout(sh, dest);
	}

	nleft = dest->end - dest->nextc;
	if (nleft > len)
		goto buffered;

	if ((xwrite(sh, dest->fd, p, len))) {
#ifdef notyet
err:
#endif
		dest->flags |= OUTPUT_ERR;
	}
#endif
}


void
outstr(ShellContext &sh, const char *p, struct output *file)
{
#ifdef USE_GLIBC_STDIO
	INTOFF;
	fputs(p, file->stream);
	INTON_DEFER;
#else
	size_t len;

	len = strlen(p);
	outmem(sh, p, len, file);
#endif
}


#ifndef USE_GLIBC_STDIO


void
outcslow(ShellContext &sh, int c, struct output *dest)
{
	char buf = c;
	outmem(sh, &buf, 1, dest);
}
#endif


void
flushall(ShellContext &sh)
{
	flushout(sh, &sh.output);
#ifdef FLUSHERR
	flushout(sh, &sh.errout);
#endif
}


void
flushout(ShellContext &sh, struct output *dest)
{
#ifdef USE_GLIBC_STDIO
	INTOFF;
	fflush(dest->stream);
	INTON_DEFER;
#else
	size_t len;

	len = dest->nextc - dest->buf;
	if (!len || dest->fd < 0)
		return;
	dest->nextc = dest->buf;
	if ((xwrite(sh, dest->fd, dest->buf, len)))
		dest->flags |= OUTPUT_ERR;
#endif
}


void
outfmt(ShellContext &sh, struct output *file, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(sh, file, fmt, ap);
	va_end(ap);
}


void
out1fmt(ShellContext &sh, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(sh, sh.out1, fmt, ap);
	va_end(ap);
}


int
fmtstr(ShellContext &sh, char *outbuf, size_t length, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = xvsnprintf(sh, outbuf, length, fmt, ap);
	va_end(ap);
	return ret > (int)length ? length : ret;
}


static dash::ShellResult<int>
xvasprintf(ShellContext &sh, char **sp, size_t size, const char *f, va_list ap)
{
	char *s;
	int len;
	va_list ap2;

	va_copy(ap2, ap);
	len = xvsnprintf(sh, *sp, size, f, ap2);
	va_end(ap2);
	if (len < 0)
		return sh_error(sh, "xvsnprintf failed");
	if (len < size)
		return len;

	s = stalloc(sh, (len >= stackblocksize() ? len : stackblocksize()) + 1);
	*sp = s;
	len = xvsnprintf(sh, s, len + 1, f, ap);
	return len;
}


dash::ShellResult<int>
xasprintf(ShellContext &sh, char **sp, const char *f, ...)
{
	va_list ap;
	int ret;

	va_start(ap, f);
	ret = TRY_SH(xvasprintf(sh, sp, 0, f, ap));
	va_end(ap);
	return ret;
}


#ifndef USE_GLIBC_STDIO
void
doformat(ShellContext &sh, struct output *dest, const char *f, va_list ap)
{
	char *s;
	int len;
	int olen;
	std::size_t su = sh.stackstr.used;
	std::size_t hc = sh.stackheaps.size();

	s = dest->nextc;
	olen = dest->end - dest->nextc;
	{
		auto r = xvasprintf(sh, &s, olen, f, ap);
		if (!r) {
			dest->flags |= OUTPUT_ERR;
			goto out;
		}
		len = *r;
	}
	if (likely(olen > len)) {
		dest->nextc += len;
		goto out;
	}
	outmem(sh, s, len, dest);
out:
	while (sh.stackheaps.size() > hc) {
		ckfree(sh.stackheaps.back());
		sh.stackheaps.pop_back();
	}
	sh.stackstr.rewind(su);
}
#endif



/*
 * Version of write which resumes after a signal is caught.
 */

int
xwrite(ShellContext &sh, int fd, const void *p, size_t n)
{
	const char *buf = static_cast<const char *>(p);

	while (n) {
		ssize_t i;
		size_t m;

		m = n;
		if (m > SSIZE_MAX)
			m = SSIZE_MAX;
		do {
			i = write(fd, buf, m);
		} while (i < 0 && errno == EINTR);
		if (i < 0)
			return -1;
		buf += i;
		n -= i;
	}
	return 0;
}


#ifdef notyet
#ifdef USE_GLIBC_STDIO
void
initstreams(ShellContext &sh)
{
	sh.output.stream = stdout;
	sh.errout.stream = stderr;
}


void
openmemout(ShellContext &sh)
{
	INTOFF;
	memout.stream = open_memstream(&memout.buf, &memout.bufsize);
	INTON_DEFER;
}


int
__closememout(ShellContext &sh)
{
	int error;
	error = fclose(memout.stream);
	memout.stream = NULL;
	return error;
}
#endif
#endif


static int
xvsnprintf(ShellContext &sh, char *outbuf, size_t length, const char *fmt, va_list ap)
{
	int ret;

#ifdef __sun
	/*
	 * vsnprintf() on older versions of Solaris returns -1 when
	 * passed a length of 0.  To avoid this, use a dummy
	 * 1-character buffer instead.
	 */
	char dummy[1];

	if (length == 0) {
		outbuf = dummy;
		length = sizeof(dummy);
	}
#endif

	INTOFF;
	ret = vsnprintf(outbuf, length, fmt, ap);
	INTON_DEFER;
	return ret;
}
