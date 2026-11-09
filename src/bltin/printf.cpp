#include "shell.h"
#include "shell_context.h"
/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
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

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

/** Argument/result scratch for one `printf` builtin invocation. */
struct PrintfState {
	char **gargv = nullptr;
	int rval = 0;
};

static int conv_escape_str(ShellContext &sh, const char *, std::string &);
static int getchr(PrintfState &);
static double getdouble(ShellContext &sh, PrintfState &);
static std::uintmax_t getuintmax(ShellContext &sh, PrintfState &, int);
static const char *getstr(PrintfState &);
static std::string mklong(const char *, const char *);
static void check_conversion(ShellContext &sh, PrintfState &, const char *,
			     const char *);

#define isodigit(c)	((c) >= '0' && (c) <= '7')
#define octtobin(c)	((c) - '0')

#include "bltin.h"
#include "parser.h"
#include "syntax.h"
#include "system.h"

#define PF(f, func) { \
	switch (reinterpret_cast<char *>(param) - reinterpret_cast<char *>(array)) { \
	default: \
		printf(f, array[0], array[1], func); \
		break; \
	case sizeof(*param): \
		printf(f, array[0], func); \
		break; \
	case 0: \
		printf(f, func); \
		break; \
	} \
}

#define ASPF(out, f, func) ({ \
	shell::Result<int> ret; \
	switch (reinterpret_cast<char *>(param) - reinterpret_cast<char *>(array)) { \
	default: \
		ret = xasprintf(sh, out, f, array[0], array[1], func); \
		break; \
	case sizeof(*param): \
		ret = xasprintf(sh, out, f, array[0], func); \
		break; \
	case 0: \
		ret = xasprintf(sh, out, f, func); \
		break; \
	} \
	if (!ret) \
		return shell_fail_of(ret); \
	*ret; \
})


static shell::Result<int>
print_escape_str(ShellContext &sh, const char *f, int *param, int *array, const char *s)
{
	int done;
	int total;
	std::string escaped;
	std::string formatted;

	done = conv_escape_str(sh, s, escaped);
	/* conv_escape_str leaves a trailing NUL as a real byte for width math */
	if (escaped.empty())
		escaped.push_back('\0');
	total = static_cast<int>(escaped.size()) - 1;

	escaped.back() = (!!((f[1] - 's') | done) - 1) & f[2];
	total += !!escaped.back();
	if (f[1] == 's') {
		sh.out1->write(sh, escaped);
		return done;
	}

	std::string width(static_cast<size_t>(total), 'X');
	total = ASPF(formatted, f, width.c_str());
	char *p = formatted.data();

	int len = strchrnul(p, 'X') - p;
	std::memcpy(p + len, escaped.data(), strspn(p + len, "X"));

	sh.out1->write(sh, p, total);
	return done;
}


shell::Result<int>
printfcmd(ShellContext &sh, int argc, char *argv[])
{
	char *fmt;
	char *format;
	int ch;
	PrintfState st;

	TRY_SHV(nextopt(sh, ""));

	argv = sh.bopt.argptr;
	format = *argv;

	if (!format)
		return error("usage: printf format [arg ...]");

	st.gargv = ++argv;

#define SKIP1	"#-+ 0"
#define SKIP2	"*0123456789"
	do {
		/*
		 * Basic algorithm is to scan the format string for conversion
		 * specifications -- once one is found, find out if the field
		 * width or precision is a '*'; if it is, gather up value.
		 * Note, format strings are reused as necessary to use up the
		 * provided arguments, arguments of zero/null string are
		 * provided to use up the format string.
		 */

		/* find next format specification */
		for (fmt = format; (ch = *fmt++) ;) {
			char *start;
			char nextch;
			int array[2];
			int *param;

			if (ch == '\\') {
				unsigned ret;
				char cp[4];

				ret = conv_escape(sh, fmt, cp, false);
				fmt += ret >> 4;
				sh.out1->write(sh, cp, ret & 15);
				continue;
			}
			if (ch != '%' || (*fmt == '%' && (++fmt || 1))) {
				putchar(ch);
				continue;
			}

			/* Ok - we've found a format specification,
			   Save its address for a later printf(). */
			start = fmt - 1;
			param = array;

			/* skip to field width */
			fmt += strspn(fmt, SKIP1);
			if (*fmt == '*') {
				++fmt;
				*param++ = getuintmax(sh, st, 1);
			} else {
				/* skip to possible '.',
				 * get following precision
				 */
				fmt += strspn(fmt, SKIP2);
			}

			if (*fmt == '.') {
				++fmt;
				if (*fmt == '*') {
					++fmt;
					*param++ = getuintmax(sh, st, 1);
				} else
					fmt += strspn(fmt, SKIP2);
			}

			ch = *fmt;
			if (!ch)
				return error("missing format character");
			/* null terminate format string to we can use it
			   as an argument to printf. */
			nextch = fmt[1];
			fmt[1] = 0;
			switch (ch) {

			case 'b':
				*fmt = 's';
				/* escape if a \c was encountered */
				if (TRY_SH(print_escape_str(sh, start, param, array, getstr(st))))
					goto out;
				*fmt = 'b';
				break;
			case 'c': {
				int p = getchr(st);
				PF(start, p);
				break;
			}
			case 's': {
				const char *p = getstr(st);
				PF(start, p);
				break;
			}
			case 'd':
			case 'i': {
                std::uintmax_t p = getuintmax(sh, st, 1);
				std::string longfmt = mklong(start, fmt);
				PF(longfmt.c_str(), p);
				break;
			}
			case 'o':
			case 'u':
			case 'x':
			case 'X': {
                std::uintmax_t p = getuintmax(sh, st, 0);
				std::string longfmt = mklong(start, fmt);
				PF(longfmt.c_str(), p);
				break;
			}
			case 'a':
			case 'A':
			case 'e':
			case 'E':
			case 'f':
			case 'F':
			case 'g':
			case 'G': {
				double p = getdouble(sh, st);
				PF(start, p);
				break;
			}
			default:
				return error("{}: invalid directive", start);
			}
			*++fmt = nextch;
		}
	} while (st.gargv != argv && *st.gargv);

out:
	return st.rval;
}


/*
 * Print SysV echo(1) style escape string
 *	Halts processing string if a \c escape is encountered.
 */
static int
conv_escape_str(ShellContext &sh, const char *str, std::string &out)
{
	int c;

	out.clear();

	do {
		unsigned ret;
		int ch;
		char tmp[4];

		c = *str++;
		if (c != '\\') {
putchar:
			out.push_back(static_cast<char>(c));
			continue;
		}

		ch = *str;
		if (ch == 'c') {
			/* \c as in SYSV echo - abort all processing.... */
			c = 0x100;
			goto putchar;
		}

		/*
		 * %b string octal constants are not like those in C.
		 * They start with a \0, and are followed by 0, 1, 2,
		 * or 3 octal digits.
		 */
		if (ch == '0' && isodigit(str[1]))
			str++;

		/* Finally test for sequences valid in the format string */
		ret = conv_escape(sh, str, tmp, false);
		str += ret >> 4;
		out.append(tmp, ret & 15);
	} while (c & 0xff);

	return c;
}

/*
 * Print "standard" escape characters
 */
unsigned
conv_escape(ShellContext &sh, const char *str0, char *out0, bool mbchar)
{
	char *out = out0;
	const char *str = str0;
	unsigned value;
	int och;
	int ch;

	ch = *str;
	value = ch;

	switch (ch) {
	default:
		if (mbchar && (ch == '"' || ch == '\''))
			break;

		if (ch == 'U') {
			ch = 8;
			goto hex;
		}

		value = '\\';

		if (unlikely(isodigit(ch))) {
			ch = 3;
			value = 0;
			do {
				value <<= 3;
				value += octtobin(*str++);
			} while (--ch && isodigit(*str));
		}

		str--;

check_value:
		if (SQSYNTAX[(signed char)value] != CCTL)
			break;
		/* fall through */

	case '\\':
		if (mbchar)
			(*(out)++ = (CTLESC));
		break;

	case 'x':
		ch = 2;

hex:
		och = ch;
		value = 0;
		do {
			int c = *++str;
			int d;

			if (c >= '0' && c <= '9')
				d = c - '0';
			else {
				int cl;

				cl = c & ~0x20;
				if (cl >= 'A' && cl <= 'F')
					d = cl - 'A' + 10;
				else {
					str--;
					break;
				}
			}

			value <<= 4;
			value += d;
		} while (--ch);

		if (och <= 2)
			goto check_value;

		if (value < 0x80)
			goto check_value;

		if (value < 0x110000) {
			int mboff = (mbchar - 1) * 2;
			unsigned uni = value;
			int len;

			value = 0x80 << 8 | (value & 0xfc0) << 2 |
				0x80 | (value & 0x3f);

			if (uni < 0x800) {
				value |= 0x40 << 8;
				len = 2;
			} else {
				value |= 0x80 << 16 | (uni & 0x3f000) << 4;
				if (uni < 0x10000) {
					value |= 0x60 << 16;
					len = 3;
				} else {
					value |= 0xf0 << 24 |
						 (uni & ~0x3ffff) << 6;
					len = 4;
				}
			}

			value = htonl(value << (4 - len) * 8);

			(*(out)++ = (CTLMBCHAR));
			(*(out)++ = (len));
			((out) += (mboff));
			memcpy(out, &value, 4);
			((out) += (len));
			(*(out)++ = (len));
			(*(out)++ = (CTLMBCHAR));
			((out) += (mboff));
		}

		goto out_noput;

	case 'u':
		ch = 4;
		goto hex;

	case 'a':				/* alert */
	case 'b':				/* backspace */
	case 'f':				/* form-feed */
		value -= 'a';
		value += '\a';
		break;

	case 'e':	value = '\033';	break;	/* <ESC> */
	case 'n':	value = '\n';	break;	/* newline */
	case 'r':	value = '\r';	break;	/* carriage-return */
	case 't':	value = '\t';	break;	/* tab */
	case 'v':	value = '\v';	break;	/* vertical-tab */
	}

	(*(out)++ = (value));

out_noput:
	str++;
	return (out - out0) | (str - str0) << 4;
}

static std::string
mklong(const char *str, const char *ch)
{
	/*
	 * Replace a string like "%92.3u" with "%92.3"PRIuMAX.
	 *
	 * Although C99 does not guarantee it, we assume PRIiMAX,
	 * PRIoMAX, PRIuMAX, PRIxMAX, and PRIXMAX are all the same
	 * as PRIdMAX with the final 'd' replaced by the corresponding
	 * character.
	 */

	size_t len = ch - str + sizeof(PRIdMAX);
	std::string copy(len, '\0');
	memcpy(copy.data(), str, len - sizeof(PRIdMAX));
	memcpy(copy.data() + len - sizeof(PRIdMAX), PRIdMAX, sizeof(PRIdMAX));
	copy[len - 2] = *ch;
	return copy;
}

static int
getchr(PrintfState &st)
{
	int val = 0;

	if (*st.gargv)
		val = **st.gargv++;
	return val;
}

static const char *
getstr(PrintfState &st)
{
	const char *val = "";

	if (*st.gargv)
		val = *st.gargv++;

	return val;
}

static std::uintmax_t
getuintmax(ShellContext &sh, PrintfState &st, int sign)
{
	uintmax_t val = 0;
	char *cp, *ep;

	cp = *st.gargv;
	if (!cp)
		goto out;
	st.gargv++;

	val = (unsigned char) cp[1];
	if (*cp == '\"' || *cp == '\'')
		goto out;

	errno = 0;
	val = sign ? std::strtoimax(cp, &ep, 0) : std::strtoumax(cp, &ep, 0);
	check_conversion(sh, st, cp, ep);
out:
	return val;
}

static double
getdouble(ShellContext &sh, PrintfState &st)
{
	double val;
	char *cp, *ep;

	cp = *st.gargv;
	if (!cp)
		return 0;
	st.gargv++;

	if (*cp == '\"' || *cp == '\'')
		return (unsigned char) cp[1];

	errno = 0;
	val = std::strtod(cp, &ep);
	check_conversion(sh, st, cp, ep);
	return val;
}

static void
check_conversion(ShellContext &sh, PrintfState &st, const char *s,
		 const char *ep)
{
	if (*ep) {
		if (ep == s)
			warnx("{}: expected numeric value", s);
		else
			warnx("{}: not completely converted", s);
		st.rval = 1;
	} else if (errno == ERANGE) {
		warnx("{}: {}", s, strerror(ERANGE));
		st.rval = 1;
	}
}

shell::Result<int>
echocmd(ShellContext &sh, int argc, char **argv)
{
	const char *lastfmt = "%s\n";
	int nonl;

	if (*++argv && std::string_view(*argv) == "-n") {
		argv++;
		lastfmt = "%s";
	}

	do {
		const char *fmt = "%s ";
		char *s = *argv;

		if (!s || !*++argv)
			fmt = lastfmt;

		nonl = TRY_SH(print_escape_str(sh, fmt, nullptr, nullptr, s ? s : ""));
	} while (!nonl && *argv);
	return 0;
}
