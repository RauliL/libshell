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
 *	@(#)nodes.c.pat	8.2 (Berkeley) 5/4/95
 */

#include <stdlib.h>
/*
 * Routine for dealing with parsed shell commands.
 */

#include "shell.h"
#include "shell_context.h"
#include "nodes.h"
#include "nodes_free.h"
#include "memalloc.h"
#include "mystring.h"
#include "system.h"

static union node *copynode(ShellContext &sh, union node *);
static std::vector<union node *>
copynodelist(ShellContext &sh, const std::vector<union node *> &);

/*
 * Make a deep copy of a parse tree for a function definition.
 */
struct funcnode *
copyfunc(ShellContext &sh, union node *n)
{
	struct funcnode *f = new funcnode;
	f->count = 0;
	f->n = copynode(sh, n);
	return f;
}

static union node *copynode(ShellContext &sh, union node *n)
{
	union node *copied;

	%COPY
	return copied;
}

static std::vector<union node *>
copynodelist(ShellContext &sh, const std::vector<union node *> &src)
{
	std::vector<union node *> out;
	out.reserve(src.size());
	for (union node *child : src)
		out.push_back(copynode(sh, child));
	return out;
}

/*
 * Free a function parse tree when the last reference drops.
 */
void
freefunc(ShellContext &sh, struct funcnode *f)
{
	(void)sh;
	if (f && --f->count < 0) {
		freenode(f->n);
		delete f;
	}
}
