#ifndef INPUT_H
#define INPUT_H
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

#include <limits.h>
#include <sys/types.h>
#include <termios.h>

#include "error.h"

#define PUNGETC_MAX (MB_LEN_MAX > 16 ? MB_LEN_MAX : 16)
#define IBUFSIZ (BUFSIZ + PUNGETC_MAX + 1)

/* PEOF (the end of file marker) is defined in syntax.h */
#define PEOA ((PEOF) - 1)

enum {
	INPUT_PUSH_FILE = 1,
	INPUT_NOFILE_OK = 2,
};

struct alias;

struct strpush {
	struct strpush *prev;	/* preceding string on stack */
	char *prevstring;
	int prevnleft;
	struct alias *ap;	/* if push was associated with an alias */
	char *string;		/* remember the string since it may change */

	/* Delay freeing so we can stop nested aliases. */
	struct strpush *spfree;

	/* Number of outstanding calls to pungetc. */
	int unget;
};

/*
 * One level of the input file stack (script, sourced file, or string).
 */
struct parsefile {
	struct parsefile *prev;	/* preceding file on stack */
	int linno;		/* current line */
	int fd;			/* file descriptor (or -1 if string) */
	int nleft;		/* number of chars left in this line */
	int eof;		/* do not read again once we hit EOF */
	char *nextc;		/* next char in buffer */
	char *buf;		/* input buffer */
	struct strpush *strpush; /* for pushing strings at this level */
	struct strpush basestrpush; /* so pushing one is fast */

	/* Delay freeing so we can stop nested aliases. */
	struct strpush *spfree;

	/* Number of outstanding calls to pungetc. */
	int unget;
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
int pgetc(ShellContext &sh, InputStack &in);
int pgetc_eoa(ShellContext &sh, InputStack &in);
void pungetc(ShellContext &sh, InputStack &in);
void pungetn(InputStack &in, int);
void pushstring(ShellContext &sh, InputStack &in, char *, void *);
dash::ShellResult<int> setinputfile(ShellContext &sh, InputStack &in,
				    const char *, int);
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
void pushstring(ShellContext &sh, char *, void *);
dash::ShellResult<int> setinputfile(ShellContext &sh, const char *, int);
void setinputstring(ShellContext &sh, char *);
void pushstdin(ShellContext &sh);
void popfile(ShellContext &sh);
void unwindfiles(ShellContext &sh, parsefile *);
void popallfiles(ShellContext &sh);
void flush_input(ShellContext &sh);
void reset_input(ShellContext &sh);

#endif
