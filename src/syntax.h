/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California. All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>. All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 */

#pragma once

#include <array>
#include <cctype>
#include <cstddef>

#ifdef CEOF
#undef CEOF
#endif

/* Syntax classes for lexer / expander character tables. */
enum : char {
	CWORD = 0,	/* character is nothing special */
	CNL = 1,	/* newline character */
	CBACK = 2,	/* a backslash character */
	CSQUOTE = 3,	/* single quote */
	CDQUOTE = 4,	/* double quote */
	CENDQUOTE = 5,	/* a terminating quote */
	CBQUOTE = 6,	/* backwards single quote */
	CVAR = 7,	/* a dollar sign */
	CENDVAR = 8,	/* a '}' character */
	CLP = 9,	/* a left paren in arithmetic */
	CRP = 10,	/* a right paren in arithmetic */
	CEOF = 11,	/* end of file */
	CCTL = 12,	/* like CWORD, except it must be escaped */
	CSPCL = 13,	/* these terminate a word */
};

inline constexpr int SYNBASE = 129;
inline constexpr int PEOF = -129;
inline constexpr std::size_t syn_table_size = 257;

extern const std::array<char, syn_table_size> basesyntax;
extern const std::array<char, syn_table_size> dqsyntax;
extern const std::array<char, syn_table_size> sqsyntax;
extern const std::array<char, syn_table_size> arisyntax;
/** Bitflags used as a never-escape syntax table in expand (not SYNBASE-biased). */
extern const std::array<char, syn_table_size> is_type;

#define BASESYNTAX (basesyntax.data() + SYNBASE)
#define DQSYNTAX (dqsyntax.data() + SYNBASE)
#define SQSYNTAX (sqsyntax.data() + SYNBASE)
#define ARISYNTAX (arisyntax.data() + SYNBASE)

constexpr bool is_digit(int c)
{
	return static_cast<unsigned>(c - '0') <= 9;
}

inline bool is_alpha(int c)
{
	return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

inline bool is_name(int c)
{
	return c == '_' || is_alpha(c);
}

inline bool is_in_name(int c)
{
	return c == '_' ||
	       std::isalnum(static_cast<unsigned char>(c)) != 0;
}

constexpr bool is_special(int c)
{
	switch (c) {
	case '#':
	case '?':
	case '$':
	case '!':
	case '-':
	case '*':
	case '@':
		return true;
	default:
		return is_digit(c);
	}
}

constexpr int digit_val(int c)
{
	return c - '0';
}
