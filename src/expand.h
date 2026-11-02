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

#include <inttypes.h>
#include <string>
#include <string_view>
#include <vector>

using ArgList = std::vector<std::string>;

struct ifsregion {
	struct ifsregion *next;
	int begoff;
	int endoff;
	int nulonly;
};

struct mbpair {
	unsigned ml;
	unsigned ql;
};

/*
 * expandarg() flags
 */
#define EXP_FULL	0x1	/* perform word splitting & file globbing */
#define EXP_TILDE	0x2	/* do normal tilde expansion */
#define	EXP_VARTILDE	0x4	/* expand tildes in an assignment */
#define	EXP_REDIR	0x8	/* file glob for a redirection (1 match only) */
#define EXP_CASE	0x10	/* keeps quotes around for CASE pattern */
#define EXP_MBCHAR	0x20	/* mark multi-byte characters */
#define EXP_VARTILDE2	0x40	/* expand tildes after colons only */
#define EXP_WORD	0x80	/* expand word in parameter expansion */
#define EXP_QUOTED	0x100	/* expand word in double quotes */
#define EXP_KEEPNUL	0x200	/* do not skip NUL characters */
#define EXP_DISCARD	0x400	/* discard result of expansion */


struct Node;

struct ShellContext;
#include "error.h"
dash::ShellResult<void> expandarg(ShellContext &sh, Node *, ArgList *, int);
/* Expand a single word to an owned string (here-docs, prompts, etc.). */
dash::ShellResult<std::string> expandarg_to_string(ShellContext &sh, Node *, int);
struct ExpandBuf;
#define rmescapes(p) _rmescapes(sh, (p), 0, nullptr)
char *_rmescapes(ShellContext &sh, char *, int, ExpandBuf *eb = nullptr);
dash::ShellResult<int> casematch(ShellContext &sh, Node *, char *);
void recordregion(ShellContext &sh, int, int, int);
dash::ShellResult<void> removerecordregions(ShellContext &sh, int);
void ifsbreakup(ShellContext &sh, char *, int, ArgList *);
void ifsfree(ShellContext &sh);
dash::ShellResult<void> restore_handler_expandarg(ShellContext &sh, int err);
void changeifs(ShellContext &sh, std::string_view);

#endif
