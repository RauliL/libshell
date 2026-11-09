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
 *	@(#)input.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <climits>
#include <memory>
#include <string_view>
#include <vector>

#include <sys/types.h>
#include <termios.h>

#include "alias.h"
#include "error.h"

#define PUNGETC_MAX (MB_LEN_MAX > 16 ? MB_LEN_MAX : 16)
#define IBUFSIZ (BUFSIZ + PUNGETC_MAX + 1)

/* PEOF (the end of file marker) is defined in syntax.h */
#define PEOA ((PEOF) - 1)

enum {
	INPUT_PUSH_FILE = 1,
	INPUT_NOFILE_OK = 2,
};

/*
 * One pushed string overlay on a parsefile (alias text or parser
 * re-injection).  Saved cursor state is restored when the string is
 * exhausted.  A non-null ap keeps the alias alive for the push and
 * sets alias::in_use.
 */
struct StringPush {
	char *prev_string = nullptr;
	int prev_nleft = 0;
	std::shared_ptr<alias> ap;	/* set if this push is an alias expansion */
	char *string = nullptr;	/* start of the pushed text */
	int unget = 0;
};

/*
 * One level of the input file stack (script, sourced file, or string).
 */
struct parsefile {
	parsefile *prev = nullptr;	/* preceding file on stack */
	int linno = 0;			/* current line */
	int fd = 0;			/* file descriptor (or -1 if string) */
	int nleft = 0;			/* number of chars left in this line */
	int eof = 0;			/* do not read again once we hit EOF */
	char *nextc = nullptr;		/* next char in buffer */
	char *buf = nullptr;		/* input buffer (owned_buf or shared basebuf) */
	std::unique_ptr<char[]> owned_buf; /* heap buffer for pushed files */
	std::vector<StringPush> string_stack;

	/* Aliases whose in_use flag is cleared on the next pgetc. */
	std::vector<std::shared_ptr<alias>> pending_alias_release;

	/* Number of outstanding calls to pungetc. */
	int unget = 0;
};

struct stdin_state {
	off_t seekable = 0;
	int pip[2] = {};
	int pending = 0;
	tcflag_t bufferable = 0;
};

/*
 * Nested input sources for one shell: base stdin, sourced files, eval
 * strings, and alias string pushes. Owned by ShellContext; parse and
 * input helpers take InputStack & rather than reaching through context
 * fields.
 */
struct InputStack {
	parsefile base{};
	char basebuf[IBUFSIZ]{};
	parsefile *top = &base;
	parsefile *current = &base;
	stdin_state stdin{};
	int stdin_istty = -1;

	InputStack()
	{
		base.nextc = base.buf = basebuf;
		base.linno = 1;
	}

	InputStack(const InputStack &) = delete;
	InputStack &operator=(const InputStack &) = delete;

	parsefile &cur() { return *current; }
	const parsefile &cur() const { return *current; }
	int &linno() { return current->linno; }
};

/*
 * The input line number. Requires ShellContext &sh in scope.
 */
#define plinno (sh.input.linno())

struct ShellContext;
void input_init(ShellContext &sh, InputStack &in);
/** Drain the current line after an error/interrupt in an interactive shell. */
void input_reset_interactive(ShellContext &sh);
/** Drop nested input files and stdin pipe state when entering a subshell. */
void input_forkreset(ShellContext &sh);
int pgetc(ShellContext &sh, InputStack &in);
int pgetc_eoa(ShellContext &sh, InputStack &in);
void pungetc(ShellContext &sh, InputStack &in);
void pungetn(InputStack &in, int);
void pushstring(ShellContext &sh, InputStack &in, char *,
		std::shared_ptr<alias> = nullptr);
shell::Result<int> setinputfile(ShellContext &sh, InputStack &in,
				    std::string_view, int);
void setinputstring(ShellContext &sh, InputStack &in, char *);
void pushstdin(ShellContext &sh, InputStack &in);
void popfile(ShellContext &sh, InputStack &in);
void unwindfiles(ShellContext &sh, InputStack &in, parsefile *stop);
void popallfiles(ShellContext &sh, InputStack &in);
void flush_input(ShellContext &sh, InputStack &in);
void reset_input(ShellContext &sh, InputStack &in);

/* Overloads that use the shell's own input stack. */
void input_init(ShellContext &sh);
int pgetc(ShellContext &sh);
int pgetc_eoa(ShellContext &sh);
void pungetc(ShellContext &sh);
void pungetn(ShellContext &sh, int);
void pushstring(ShellContext &sh, char *, std::shared_ptr<alias> = nullptr);
shell::Result<int> setinputfile(ShellContext &sh, std::string_view, int);
void setinputstring(ShellContext &sh, char *);
void pushstdin(ShellContext &sh);
void popfile(ShellContext &sh);
void unwindfiles(ShellContext &sh, parsefile *);
void flush_input(ShellContext &sh);
void reset_input(ShellContext &sh);
