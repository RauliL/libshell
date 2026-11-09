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
 *	@(#)parser.h	8.3 (Berkeley) 5/4/95
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include <libshell.hpp>

#include "nodes.h"
#include "token.h"

namespace shell { class ParseError; }

using heredoc = shell::HereDoc;
using ParseSession = shell::ParseSession;

/* control characters in argument strings */
#define CTL_FIRST -127		/* first 'special' character */
#define CTLESC -127		/* escape next character */
#define CTLVAR -126		/* variable defn */
#define CTLENDVAR -125
#define CTLBACKQ -124
#define CTLMBCHAR -123
#define	CTLARI -122		/* arithmetic expression */
#define	CTLENDARI -121
#define	CTLQUOTEMARK -120
#define	CTL_LAST -120		/* last 'special' character */

/* variable substitution byte (follows CTLVAR) */
#define VSTYPE	0x0f		/* type of variable substitution */
#define VSNUL	0x10		/* colon--treat the empty string as unset */
#define VSBIT	0x20		/* Ensure subtype is not zero */

/* values of VSTYPE field */
#define VSNORMAL	0x1		/* normal variable:  $var or ${var} */
#define VSMINUS		0x2		/* ${var-text} */
#define VSPLUS		0x3		/* ${var+text} */
#define VSQUESTION	0x4		/* ${var?message} */
#define VSASSIGN	0x5		/* ${var=text} */
#define VSTRIMRIGHT	0x6		/* ${var%pattern} */
#define VSTRIMRIGHTMAX 	0x7		/* ${var%%pattern} */
#define VSTRIMLEFT	0x8		/* ${var#pattern} */
#define VSTRIMLEFTMAX	0x9		/* ${var##pattern} */
#define VSLENGTH	0xa		/* ${#var} */
/* VSLENGTH must come last. */

/* values of checkkwd variable */
#define CHKALIAS	0x1
#define CHKKWD		0x2
#define CHKNL		0x4
#define CHKEOFMARK	0x8


struct ShellContext;

/**
 * Result of `parsecmd()`: EOF vs blank line vs parse tree.
 */
struct ParseCmdResult {
	bool eof = false;
	Node::ptr tree;
};

bool isassignment(std::string_view p);
bool issimplecmd(const Node *n, std::string_view name);
ParseCmdResult parsecmd(ShellContext &sh, int);
void fixredir(ShellContext &sh, Node *, const char *, int);
std::string getprompt(ShellContext &, void *);
std::optional<Token> findkwd(std::string_view);
/* Length of a leading legal variable name; 0 if none. */
std::size_t endofname(std::string_view name);
std::string expandstr(ShellContext &sh, const char *);
unsigned getmbc(ShellContext &sh, int c, char *out, int mode);
void report_parse_error(ShellContext &sh, const shell::ParseError &e);
tl::expected<void, std::string> try_parse_string(ShellContext &sh, std::string_view s);
