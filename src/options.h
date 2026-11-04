#ifndef OPTIONS_H
#define OPTIONS_H
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
 *	@(#)options.h	8.2 (Berkeley) 5/4/95
 */

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "error.h"

struct shparam {
	std::vector<std::string> p;	/* positional parameters (without $0) */
	int optind = 1;		/* next parameter to be processed by getopts */
	int optoff = -1;	/* used by getopts */
};

#define NOPTS	15

/** Shell `set` / startup option bits (`$-`, `set -e`, …). */
struct ShellOptions {
	char list[NOPTS] = {};
	/** True when the host started the shell with `-c`. */
	bool c_mode = false;
};

/**
 * Scratch for builtin `nextopt` / argv walking. Not durable shell state.
 */
struct NextOptState {
	char **argptr = nullptr;
	char *optionarg = nullptr;
	char *optptr = nullptr;
};

/**
 * Host-facing result of parsing process argv (`-c`, login shell, …).
 * The `-c` script string lives here, not on ShellContext.
 */
struct ProcArgsResult {
	int login = 0;
	std::optional<std::string> minusc;
};

#define eflag (sh.opts.list[0])
#define fflag (sh.opts.list[1])
#define Iflag (sh.opts.list[2])
#define iflag (sh.opts.list[3])
#define mflag (sh.opts.list[4])
#define nflag (sh.opts.list[5])
#define sflag (sh.opts.list[6])
#define xflag (sh.opts.list[7])
#define vflag (sh.opts.list[8])
#define	Cflag (sh.opts.list[9])
#define	aflag (sh.opts.list[10])
#define	bflag (sh.opts.list[11])
#define	uflag (sh.opts.list[12])
#define	pipefail (sh.opts.list[13])
#define	debug (sh.opts.list[14])

extern const char optletters[NOPTS];

struct ShellContext;
shell::Result<ProcArgsResult> procargs(ShellContext &sh, char **);
void optschanged(ShellContext &sh);
void setparam(ShellContext &sh, char **);
void freeparam(ShellContext &sh, struct shparam *);
shell::Result<int> shiftcmd(ShellContext &sh, int, char **);
shell::Result<int> setcmd(ShellContext &sh, int, char **);
shell::Result<int> getoptscmd(ShellContext &sh, int, char **);
shell::Result<int> nextopt(ShellContext &sh, std::string_view);
void getoptsreset(ShellContext &sh, std::string_view);
#endif
