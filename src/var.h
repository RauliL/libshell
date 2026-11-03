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
 *	@(#)var.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <bitset>
#include <cstddef>
#include <inttypes.h>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "error.h"

struct ShellContext;

/*
 * Shell variables.
 */

enum class VarFlag : std::size_t {
	exported,	/* variable is exported */
	readonly,	/* variable cannot be modified */
	str_fixed,	/* variable struct is statically allocated */
	unset,		/* the variable is not set */
	nofunc,		/* don't call the callback function */
	full,		/* pass value suitable for putenv */
};

class VarFlags {
	static constexpr std::size_t nbits =
		static_cast<std::size_t>(VarFlag::full) + 1;
	std::bitset<nbits> bits{};

	static constexpr std::size_t index(VarFlag flag)
	{
		return static_cast<std::size_t>(flag);
	}

	explicit VarFlags(std::bitset<nbits> b) : bits(b) {}

public:
	VarFlags() = default;
	VarFlags(VarFlag flag) { bits.set(index(flag)); }

	bool test(VarFlag flag) const { return bits.test(index(flag)); }
	bool any() const { return bits.any(); }
	bool none() const { return bits.none(); }

	VarFlags &set(VarFlag flag)
	{
		bits.set(index(flag));
		return *this;
	}

	VarFlags &reset(VarFlag flag)
	{
		bits.reset(index(flag));
		return *this;
	}

	VarFlags &operator|=(VarFlag flag) { return set(flag); }

	VarFlags &operator|=(VarFlags other)
	{
		bits |= other.bits;
		return *this;
	}

	VarFlags &operator&=(VarFlags other)
	{
		bits &= other.bits;
		return *this;
	}

	VarFlags operator|(VarFlag flag) const
	{
		VarFlags out = *this;
		out.set(flag);
		return out;
	}

	VarFlags operator|(VarFlags other) const
	{
		VarFlags out = *this;
		out |= other;
		return out;
	}

	VarFlags operator&(VarFlag flag) const
	{
		return *this & VarFlags(flag);
	}

	VarFlags operator&(VarFlags other) const
	{
		VarFlags out = *this;
		out &= other;
		return out;
	}

	VarFlags operator~() const { return VarFlags(~bits); }

	friend bool operator==(VarFlags a, VarFlags b)
	{
		return a.bits == b.bits;
	}

	friend bool operator!=(VarFlags a, VarFlags b) { return !(a == b); }

	friend bool operator==(VarFlags a, VarFlag b)
	{
		return a == VarFlags(b);
	}

	friend bool operator!=(VarFlags a, VarFlag b) { return !(a == b); }
};

inline VarFlags operator|(VarFlag a, VarFlag b)
{
	VarFlags flags(a);
	flags.set(b);
	return flags;
}

inline VarFlags operator|(VarFlag a, VarFlags b)
{
	return b | a;
}

inline VarFlags operator~(VarFlag flag)
{
	return ~VarFlags(flag);
}

static_assert(std::is_trivially_copyable_v<VarFlags>);
static_assert(std::is_trivially_destructible_v<VarFlags>);


struct var {
	struct var *next;		/* next entry in hash list */
	VarFlags flags;
	std::string text;		/* name=value (owned; empty value when unset) */
	void (*func)(ShellContext &, std::string_view);
					/* function to be called when  */
					/* the variable gets set/unset */
};


struct localvar {
	struct var *vp = nullptr;	/* the variable that was made local */
	VarFlags flags{};		/* saved flags */
	std::string text;		/* saved text or $- optlist bytes */
};

/* One function/scope frame: ordered restore records (undo log), not a map. */
struct LocalVarFrame {
	std::vector<localvar> vars;
};


/* varinit / defifsvar live on ShellContext; macros require &sh. */

#define vifs (sh.varinit[0])
#define vpath (&vifs)[1]
#define vps1 (&vpath)[1]
#define vps2 (&vps1)[1]
#define vps4 (&vps2)[1]
#define voptind (&vps4)[1]
#ifdef WITH_LINENO
#define vlineno (&voptind)[1]
#endif

extern const char defpathvar[];
#define defifs (sh.defifsvar + 4)
#define defpath (defpathvar + 36)

/*
 * The following macros access the values of the above variables.
 * They have to skip over the name.  They return the null string
 * for unset variables.
 */

#define ifsval()	(vifs.text.c_str() + 4)
#define ifsset()	(!vifs.flags.test(VarFlag::unset))
#define pathval()	(vpath.text.c_str() + 5)
#define ps1val()	(vps1.text.c_str() + 4)
#define ps2val()	(vps2.text.c_str() + 4)
#define ps4val()	(vps4.text.c_str() + 4)
#define optindval()	(voptind.text.c_str() + 7)
#define linenoval()	(vlineno.text.c_str() + 7)

struct ShellContext;
void initvar(ShellContext &sh);
shell::Result<struct var *> setvar(ShellContext &sh, const std::string &name,
				       std::optional<std::string> val,
				       VarFlags flags = {});
shell::Result<intmax_t> setvarint(ShellContext &sh, const std::string &,
				      intmax_t, VarFlags flags = {});
shell::Result<struct var *> setvareq(ShellContext &sh, std::string s,
					 VarFlags flags = {});
char *lookupvar(ShellContext &sh, std::string_view name);
shell::Result<intmax_t> lookupvarint(ShellContext &sh, std::string_view name);
std::vector<char *> listvars(ShellContext &sh, VarFlags on, VarFlags off);
inline std::vector<char *> environment(ShellContext &sh)
{
	return listvars(sh, VarFlag::exported, VarFlag::unset);
}
shell::Result<int> showvars(ShellContext &sh, std::string_view prefix,
				VarFlags on, VarFlags off);
shell::Result<int> exportcmd(ShellContext &sh, int, char **);
shell::Result<int> localcmd(ShellContext &sh, int, char **);
shell::Result<void> mklocal(ShellContext &sh, std::string_view name,
				VarFlags flags = {});
shell::Result<std::size_t> pushlocalvars(ShellContext &sh, int push);
shell::Result<void> unwindlocalvars(ShellContext &sh, std::size_t stop);
shell::Result<int> unsetcmd(ShellContext &sh, int, char **);
shell::Result<void> unsetvar(ShellContext &sh, const std::string &);
int varcmp(std::string_view, std::string_view);

static inline unsigned int hashval(std::string_view p)
{
	if (p.empty())
		return 0;

	unsigned int h = static_cast<unsigned char>(p[0]) << 4;
	for (std::size_t i = 0;; ++i) {
		h += static_cast<unsigned char>(p[i]);
		if (i + 1 >= p.size() || p[i + 1] == '=')
			break;
	}
	return h;
}


static inline int varequal(std::string_view a, std::string_view b)
{
	return !varcmp(a, b);
}
