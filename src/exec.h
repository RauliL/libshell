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
 *	@(#)exec.h	8.3 (Berkeley) 6/8/95
 */
#pragma once

#include <string_view>

#include "nodes.h"

/* values of cmdtype */
#define CMDUNKNOWN	-1	/* no entry in table for command */
#define CMDNORMAL	0	/* command is an executable program */
#define CMDFUNCTION	1	/* command is a shell function */
#define CMDBUILTIN	2	/* command is a shell builtin */


struct cmdparam {
	int index = 0;
	const struct builtincmd *cmd = nullptr;
    std::shared_ptr<Node> func;
};

struct cmdentry {
	int cmdtype = CMDUNKNOWN;
	cmdparam u;
};

/* One remembered command: a function, a builtin, or a hashed path. */
struct tblentry {
	cmdparam param{};
	short cmdtype = CMDUNKNOWN;
	bool rehash = false;
};


/* action to find_command() */
#define DO_ERR		0x01	/* prints errors */
#define DO_ABS		0x02	/* checks absolute paths */
#define DO_NOFUNC	0x04	/* don't return shell functions, for command */
#define DO_ALTPATH	0x08	/* using alternate path */
#define DO_REGBLTIN	0x10	/* regular built-ins and functions only */

struct ShellContext;
struct Node;

extern const char *pathopt;	/* set by padvance */

struct stat64;

void shellexec(ShellContext &, char **, const char *, int);
int padvance_magic(ShellContext &sh, const char **path, const char *name,
		   int magic, std::string &out);
shell::Result<void> find_command(ShellContext &, char *, struct cmdentry *, int, const char *);
void hashcd(ShellContext &);
void changepath(ShellContext &, std::string_view);
void defun(ShellContext &, Node *);
void unsetfunc(ShellContext &, std::string_view);
shell::Result<int> hashcmd(ShellContext &sh, int, char **);
shell::Result<int> typecmd(ShellContext &sh, int, char **);
shell::Result<int> commandcmd(ShellContext &sh, int, char **);

int test_file_access(ShellContext &sh, const char *path, int mode);
int test_access(const struct stat64 *sp, int stmode);

inline int padvance(ShellContext &sh, const char **path, const char *name,
		    std::string &out)
{
	return padvance_magic(sh, path, name, 1, out);
}
