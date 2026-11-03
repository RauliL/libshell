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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
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
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

#include <libshell.hpp>

struct ShellContext;

/*
 * Buffered writer for a file descriptor. Owns its buffer via std::vector;
 * allocation does not go through ShellContext / ckmalloc.
 *
 * bufsize == 0 means unbuffered (writes go straight to the fd).
 * Otherwise the buffer is allocated lazily on first write.
 */
struct output {
	using value_type = char;
	using container_type = std::vector<value_type>;
	using size_type = container_type::size_type;

	container_type buf;
	/** Bytes pending in buffer. */
	size_type pos = 0;
	/** Capacity once allocated; 0 = unbuffered. */
	size_type bufsize = 0;
	int fd = -1;
	int flags = 0;

	output() = default;

	output(size_type size, int filedes)
		: bufsize(size)
		, fd(filedes) {}

	inline size_type space() const
	{
		return buf.size() >= pos ? buf.size() - pos : 0;
	}

	/**
	 * Drop pending data without writing (e.g. after an interrupted builtin).
	 */
	void discard()
	{
		pos = 0;
		flags = 0;
	}

	void flush(ShellContext &);

	void write(ShellContext &, const char *, std::size_t);

	inline void write(ShellContext &context, std::string_view s)
	{
		write(context, s.data(), s.size());
	}

private:
	void ensure_buf();
};

void outcslow(ShellContext &sh, int, output *);
void flushall(ShellContext &sh);
void outfmt(ShellContext &sh, output *, const char *, ...)
    __attribute__((__format__(__printf__,3,4)));
void out1fmt(ShellContext &sh, const char *, ...)
    __attribute__((__format__(__printf__,2,3)));
shell::Result<int> xasprintf(ShellContext &, std::string &, const char *, ...);
void doformat(ShellContext &sh, output *, const char *, va_list);
int xwrite(ShellContext &sh, int, const void *, std::size_t);

inline void outc(ShellContext &sh, int ch, output *file)
{
	if (file->space() == 0)
		outcslow(sh, ch, file);
	else
		file->buf[file->pos++] = static_cast<char>(ch);
}

#define OUTPUT_ERR 01		/* error occurred on output */

#define out1c(c)	outc(sh, (c), sh.out1)
#define out2c(c)	outcslow(sh, (c), sh.out2)
#define out1str(s)	sh.out1->write(sh, (s))
#define out2str(s)	sh.out2->write(sh, (s))
#define outerr(f)	(f)->flags
