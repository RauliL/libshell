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
 * Shell output routines.  We use our own output routines because:
 *	When a builtin command is interrupted we have to discard
 *		any pending output.
 *	When a builtin command appears in back quotes, we want to
 *		save the output of the command in a region obtained
 *		via malloc, rather than doing a fork and reading the
 *		output of the command via a pipe.
 *	Our output routines may be smaller than the stdio routines.
 */

#include <sys/types.h>		/* quad_t */

#include <stdio.h>	/* defines BUFSIZ */
#include <string>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

#include "shell.h"
#include "output.h"
#include "error.h"
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstring>

using namespace shell;

namespace shell {

namespace {
int xvsnprintf(Context &sh, char *, size_t, const char *, va_list);
} // namespace

void
Output::ensure_buf()
{
	if (buf.empty() && bufsize)
		buf.resize(bufsize);
}

int
Output::emit(Context &context, const char *p, std::size_t len)
{
	if (sink) {
		sink(std::string_view(p, len));
		return 0;
	}
	if (fd < 0)
		return -1;
	return xwrite(context, fd, p, len);
}

void
Output::flush(Context &context)
{
	const std::size_t len = pos;

	if (!len)
		return;
	if (!sink && fd < 0)
		return;

	pos = 0;
	if (emit(context, buf.data(), len))
		flags |= OUTPUT_ERR;
}

void
Output::write(Context &sh, const char *p, std::size_t len)
{
	std::size_t nleft = space();

	if (likely(nleft >= len)) {
buffered:
		std::memcpy(buf.data() + pos, p, len);
		pos += len;
		return;
	}

	if (!bufsize)
		;
	else if (buf.empty()) {
		ensure_buf();
	} else {
		flush(sh);
	}

	nleft = space();
	if (nleft >= len)
		goto buffered;

	if (emit(sh, p, len))
		flags |= OUTPUT_ERR;
}

void
outcslow(Context &sh, int c, Output *dest)
{
	char buf = c;

	dest->write(sh, &buf, 1);
}

void
flushall(Context &sh)
{
	sh.output.flush(sh);
#ifdef FLUSHERR
	sh.errout.flush(sh);
#endif
}

void
outfmt(Context &sh, Output *file, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(sh, file, fmt, ap);
	va_end(ap);
}

void
out1fmt(Context &sh, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(sh, sh.out1, fmt, ap);
	va_end(ap);
}

namespace {
Result<int>
xvasprintf(Context &sh, char **sp, size_t size, std::string &overflow,
	   const char *f, va_list ap)
{
	int len;
	va_list ap2;

	va_copy(ap2, ap);
	len = xvsnprintf(sh, *sp, size, f, ap2);
	va_end(ap2);
	if (len < 0)
		return sh_error(sh, "xvsnprintf failed");
	if (len < static_cast<int>(size))
		return len;

	overflow.resize(static_cast<size_t>(len) + 1);
	*sp = overflow.data();
	len = xvsnprintf(sh, *sp, static_cast<size_t>(len) + 1, f, ap);
	return len;
}
} // namespace

Result<int>
xasprintf(Context &sh, std::string &out, const char *f, ...)
{
	va_list ap;
	char *p = nullptr;
	int ret;

	va_start(ap, f);
	TRY_SH_SET(ret, xvasprintf(sh, &p, 0, out, f, ap));
	va_end(ap);
	return ret;
}

void
doformat(Context &sh, Output *dest, const char *f, va_list ap)
{
	char *s;
	int len;
	std::size_t olen;
	std::string overflow;

	s = dest->buf.empty() ? nullptr : dest->buf.data() + dest->pos;
	olen = dest->space();
	{
		auto r = xvasprintf(sh, &s, olen, overflow, f, ap);
		if (!r) {
			dest->flags |= OUTPUT_ERR;
			return;
		}
		len = *r;
	}
	if (likely(olen > static_cast<std::size_t>(len))) {
		dest->pos += static_cast<std::size_t>(len);
		return;
	}
	dest->write(sh, s, static_cast<std::size_t>(len));
}

/*
 * Version of write which resumes after a signal is caught.
 */

int
xwrite(Context &sh, int fd, const void *p, size_t n)
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

namespace {
int
xvsnprintf(Context &sh, char *outbuf, size_t length, const char *fmt, va_list ap)
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

	INTOFF(sh);
	ret = vsnprintf(outbuf, length, fmt, ap);
	INTON_DEFER(sh);

	return ret;
}
} // namespace

} // namespace shell
