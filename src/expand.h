#ifndef EXPAND_H
#define EXPAND_H
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
 *	@(#)expand.h	8.2 (Berkeley) 5/4/95
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <cwchar>

#include <libshell.hpp>

#include "nodes.h"

using ArgList = std::vector<std::string>;
using ifsregion = shell::IfsRegion;
using ExpandState = shell::ExpandState;

struct mbpair {
	unsigned ml;
	unsigned ql;
};

/** Flags for expandarg() / word expansion. Bitmask values match historic dash. */
enum class ExpandFlag : unsigned {
	full = 0x1,	/* word splitting & file globbing */
	tilde = 0x2,	/* normal tilde expansion */
	vartilde = 0x4,	/* tildes in an assignment */
	redir = 0x8,	/* glob for a redirection (1 match only) */
	case_pat = 0x10,/* keep quotes for CASE pattern */
	mbchar = 0x20,	/* mark multi-byte characters */
	vartilde2 = 0x40,/* tildes after colons only */
	word = 0x80,	/* expand word in parameter expansion */
	quoted = 0x100,	/* expand word in double quotes */
	keepnul = 0x200,/* do not skip NUL characters */
	discard = 0x400,/* discard result of expansion */
};

class ExpandFlags {
	unsigned bits_ = 0;

	explicit constexpr ExpandFlags(unsigned bits) : bits_(bits) {}

public:
	constexpr ExpandFlags() = default;
	constexpr ExpandFlags(ExpandFlag flag)
		: bits_(static_cast<unsigned>(flag))
	{
	}

	constexpr bool test(ExpandFlag flag) const
	{
		return bits_ & static_cast<unsigned>(flag);
	}

	constexpr ExpandFlags &operator|=(ExpandFlags other)
	{
		bits_ |= other.bits_;
		return *this;
	}

	constexpr ExpandFlags &operator&=(ExpandFlags other)
	{
		bits_ &= other.bits_;
		return *this;
	}

	constexpr ExpandFlags &operator^=(ExpandFlags other)
	{
		bits_ ^= other.bits_;
		return *this;
	}

	friend constexpr ExpandFlags operator|(ExpandFlags a, ExpandFlags b)
	{
		return ExpandFlags{a.bits_ | b.bits_};
	}

	friend constexpr ExpandFlags operator&(ExpandFlags a, ExpandFlags b)
	{
		return ExpandFlags{a.bits_ & b.bits_};
	}

	friend constexpr ExpandFlags operator^(ExpandFlags a, ExpandFlags b)
	{
		return ExpandFlags{a.bits_ ^ b.bits_};
	}

	friend constexpr ExpandFlags operator~(ExpandFlags a)
	{
		return ExpandFlags{~a.bits_};
	}

	friend constexpr bool operator==(ExpandFlags a, ExpandFlags b)
	{
		return a.bits_ == b.bits_;
	}

	friend constexpr bool operator==(ExpandFlags a, ExpandFlag b)
	{
		return a == ExpandFlags{b};
	}

	friend constexpr bool operator==(ExpandFlag a, ExpandFlags b)
	{
		return ExpandFlags{a} == b;
	}

	friend constexpr bool operator!=(ExpandFlags a, ExpandFlags b)
	{
		return !(a == b);
	}

	explicit constexpr operator bool() const { return bits_ != 0; }

	constexpr unsigned value() const { return bits_; }
};

inline constexpr ExpandFlags operator|(ExpandFlag a, ExpandFlag b)
{
	return ExpandFlags{a} | ExpandFlags{b};
}

inline constexpr ExpandFlags operator|(ExpandFlag a, ExpandFlags b)
{
	return ExpandFlags{a} | b;
}

inline constexpr ExpandFlags operator|(ExpandFlags a, ExpandFlag b)
{
	return a | ExpandFlags{b};
}

inline constexpr ExpandFlags operator&(ExpandFlag a, ExpandFlags b)
{
	return ExpandFlags{a} & b;
}

inline constexpr ExpandFlags operator&(ExpandFlags a, ExpandFlag b)
{
	return a & ExpandFlags{b};
}

inline constexpr ExpandFlags operator~(ExpandFlag a)
{
	return ~ExpandFlags{a};
}

static_assert(std::is_trivially_copyable_v<ExpandFlags>);
static_assert(std::is_trivially_destructible_v<ExpandFlags>);

/* Historic names used throughout expand/eval. */
inline constexpr ExpandFlag EXP_FULL = ExpandFlag::full;
inline constexpr ExpandFlag EXP_TILDE = ExpandFlag::tilde;
inline constexpr ExpandFlag EXP_VARTILDE = ExpandFlag::vartilde;
inline constexpr ExpandFlag EXP_REDIR = ExpandFlag::redir;
inline constexpr ExpandFlag EXP_CASE = ExpandFlag::case_pat;
inline constexpr ExpandFlag EXP_MBCHAR = ExpandFlag::mbchar;
inline constexpr ExpandFlag EXP_VARTILDE2 = ExpandFlag::vartilde2;
inline constexpr ExpandFlag EXP_WORD = ExpandFlag::word;
inline constexpr ExpandFlag EXP_QUOTED = ExpandFlag::quoted;
inline constexpr ExpandFlag EXP_KEEPNUL = ExpandFlag::keepnul;
inline constexpr ExpandFlag EXP_DISCARD = ExpandFlag::discard;

using Context = shell::Context;
#include "error.h"
shell::Result<void> expandarg(Context &sh, Node *, ArgList *, ExpandFlags);
/* Expand a single word to an owned string (here-docs, prompts, etc.). */
shell::Result<std::string> expandarg_to_string(Context &sh, Node *,
					      ExpandFlags);
struct ExpandBuf;
#define rmescapes(p) _rmescapes(sh, (p), 0, nullptr)
char *_rmescapes(Context &sh, char *, int, ExpandBuf *eb = nullptr);
shell::Result<int> casematch(Context &sh, Node *, char *);
void recordregion(Context &sh, int, int, int);
shell::Result<void> removerecordregions(Context &sh, int);
void ifsbreakup(Context &sh, char *, int, ArgList *);
void ifsfree(Context &sh);
shell::Result<void> restore_handler_expandarg(Context &sh, int err);
void changeifs(Context &sh, std::string_view);

#endif
