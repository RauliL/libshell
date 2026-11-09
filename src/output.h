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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace shell { struct Context; }
using Context = shell::Context;
void outcslow(Context &sh, int, shell::Output *);
void flushall(Context &sh);
void outfmt(Context &sh, shell::Output *, const char *, ...)
    __attribute__((__format__(__printf__,3,4)));
void out1fmt(Context &sh, const char *, ...)
    __attribute__((__format__(__printf__,2,3)));
shell::Result<int> xasprintf(Context &, std::string &, const char *, ...);
void doformat(Context &sh, shell::Output *, const char *, va_list);
int xwrite(Context &sh, int, const void *, std::size_t);

inline void outc(Context &sh, int ch, shell::Output *file)
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
