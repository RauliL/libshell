#ifndef EVAL_H
#define EVAL_H
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
 *	@(#)eval.h	8.2 (Berkeley) 5/4/95
 */

#include <string>

struct Job;

struct backcmd {		/* result of evalbackcmd */
	int fd = -1;		/* file descriptor to read from */
	std::string buf;	/* buffer */
	Job *jp = nullptr;	/* job structure for command */
};

#include <bitset>
#include <cstddef>
#include <type_traits>

/*
 * How to evaluate a command tree. Passed to execute(), evalstring(), and
 * evaltree().
 *
 * exit:   this tree is the last action of the shell. After it runs, exit
 *         instead of returning. Also lets a subshell or external command
 *         replace the current process.
 * tested: the caller inspects the status (if, while, &&, ||, !, a function
 *         body). A failing command does not trigger set -e.
 */
enum class EvalFlag : std::size_t {
	exit,
	tested,
};

class EvalFlags {
	static constexpr std::size_t nbits =
		static_cast<std::size_t>(EvalFlag::tested) + 1;
	std::bitset<nbits> bits{};

	static constexpr std::size_t index(EvalFlag flag)
	{
		return static_cast<std::size_t>(flag);
	}

public:
	EvalFlags() = default;
	EvalFlags(EvalFlag flag) { bits.set(index(flag)); }

	bool test(EvalFlag flag) const { return bits.test(index(flag)); }

	EvalFlags &set(EvalFlag flag)
	{
		bits.set(index(flag));
		return *this;
	}

	EvalFlags &reset(EvalFlag flag)
	{
		bits.reset(index(flag));
		return *this;
	}

	/* Keep only this flag, and only when it was already set. */
	EvalFlags &operator&=(EvalFlag flag)
	{
		bool on = test(flag);
		bits.reset();
		if (on)
			bits.set(index(flag));
		return *this;
	}

	EvalFlags operator&(EvalFlag flag) const
	{
		EvalFlags out = *this;
		out &= flag;
		return out;
	}

	EvalFlags &operator|=(EvalFlag flag) { return set(flag); }

	EvalFlags operator|(EvalFlag flag) const
	{
		EvalFlags out = *this;
		out.set(flag);
		return out;
	}

	unsigned long to_ulong() const { return bits.to_ulong(); }
};

inline EvalFlags operator|(EvalFlag a, EvalFlag b)
{
	EvalFlags flags(a);
	flags.set(b);
	return flags;
}

static_assert(std::is_trivially_copyable_v<EvalFlags>);
static_assert(std::is_trivially_destructible_v<EvalFlags>);

#include "error.h"

struct ShellContext;
struct Node;	/* BLETCH for ansi C */
dash::ShellResult<int> evaltree(ShellContext &sh, Node *, EvalFlags);
dash::ShellResult<void> evalbackcmd(ShellContext &sh, Node *, struct backcmd *);

extern int evalskip;

/* reasons for skipping commands (see comment on breakcmd routine) */
#define SKIPBREAK	(1 << 0)
#define SKIPCONT	(1 << 1)
#define SKIPFUNC	(1 << 2)
#define SKIPFUNCDEF	(1 << 3)
#endif
