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
 * String functions.
 *
 *	number(s)		Convert a string of digits to an integer.
 *	is_number(s)		Return true if s is a string of digits.
 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "shell.h"
#include "syntax.h"
#include "error.h"
#include "mystring.h"
#include "parser.h"
#include <string>
#include <string_view>

using namespace shell;

namespace shell {

const char dolatstr[] = { CTLQUOTEMARK, CTLVAR, VSNORMAL | VSBIT, '@', '=',
			  CTLQUOTEMARK, '\0' };
const char cqchars[] = {
	'\\',
	CTLESC, CTLMBCHAR, CTLQUOTEMARK, 0
};

/*
 * prefix -- see if pfx is a prefix of string.
 */

char *
prefix(const char *string, const char *pfx)
{
	std::string_view text(string);
	std::string_view head(pfx);
	if (!text.starts_with(head))
		return nullptr;
	return const_cast<char *>(string + head.size());
}

tl::unexpected<Error>
badnum(Context &sh, std::string_view s)
{
	std::string tmp(s);
	return sh_error(sh, "Illegal number: {}", tmp);
}

/*
 * Convert a string into an integer of type intmax_t.  Alow trailing spaces.
 */
Result<std::intmax_t>
atomax(Context &sh, std::string_view s, int base)
{
	std::string tmp(s);
	char *p;
	std::intmax_t r;

	errno = 0;
	r = std::strtoimax(tmp.c_str(), &p, base);

	/*
	 * Disallow completely blank strings in non-arithmetic (base != 0)
	 * contexts.
	 */
	if (p == tmp.c_str() && base)
		return badnum(sh, s);

	while (std::isspace(static_cast<unsigned char>(*p)))
	      p++;

	if (*p)
		return badnum(sh, s);

	return r;
}

Result<std::intmax_t>
atomax10(Context &sh, std::string_view s)
{
	return atomax(sh, s, 10);
}

/*
 * Convert a string of digits to an integer, printing an error message on
 * failure.
 */

Result<int>
number(Context &sh, std::string_view s)
{
	std::intmax_t n;

	TRY_SH_SET(n, atomax10(sh, s));

	if (n < 0 || n > INT_MAX)
		return badnum(sh, s);

	return static_cast<int>(n);
}

/*
 * Check for a valid number.  This should be elsewhere.
 */

bool
is_number(std::string_view text)
{
	if (text.empty())
		return false;
	return std::all_of(text.begin(), text.end(), [](char c) {
		return is_digit(static_cast<unsigned char>(c));
	});
}

/*
 * Produce a possibly single quoted string suitable as input to the shell.
 */

std::string
single_quote(std::string_view s)
{
	std::string out;

	do {
		size_t len = s.find('\'');
		if (len == std::string_view::npos)
			len = s.size();

		out.push_back('\'');
		out.append(s.substr(0, len));
		out.push_back('\'');
		s.remove_prefix(len);

		len = 0;
		while (len < s.size() && s[len] == '\'')
			++len;
		if (!len)
			break;

		out.push_back('"');
		out.append(s.substr(0, len));
		out.push_back('"');
		s.remove_prefix(len);
	} while (!s.empty());

	return out;
}

/*
 * Wrapper around strcmp for qsort/bsearch/...
 */
int
pstrcmp(const void *a, const void *b)
{
	return std::strcmp(*static_cast<const char *const *>(a),
			   *static_cast<const char *const *>(b));
}

std::string
ascii_upper(std::string_view s)
{
	std::string out(s);

	for (auto &c : out)
	{
		c = std::toupper(c);
	}

	return out;
}

} // namespace shell
