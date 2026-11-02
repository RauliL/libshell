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
 *	@(#)mystring.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#ifdef HAVE_FNMATCH
#define FNMATCH_IS_ENABLED 1
#ifdef HAVE_GLOB
#define GLOB_IS_ENABLED 1
#else
#define GLOB_IS_ENABLED 0
#endif
#else
#define FNMATCH_IS_ENABLED 0
#define GLOB_IS_ENABLED 0
#endif

extern const char snlfmt[];
extern const char spcstr[];
extern const char dolatstr[];
#define DOLATSTRLEN 6
extern const char cqchars[];
#define qchars (cqchars + 1)
extern const char illnum[];
extern const char homestr[];
extern const char dotdir[];

#include "error.h"

struct ShellContext;
char *prefix(const char *, const char *);
tl::unexpected<shell::Error> badnum(ShellContext &sh, std::string_view s);
shell::Result<intmax_t> atomax(ShellContext &sh, std::string_view, int);
shell::Result<intmax_t> atomax10(ShellContext &sh, std::string_view);
shell::Result<int> number(ShellContext &sh, std::string_view);
bool is_number(std::string_view);
std::string single_quote(const char *);
int pstrcmp(const void *, const void *);
std::string ascii_upper(std::string_view);

inline bool equal(const char *s1, const char *s2)
{
	return std::string_view(s1) == s2;
}

inline void scopy(const char *from, char *to)
{
	std::strcpy(to, from);
}
