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

#include <libshell.hpp>

#include "alias.h"
#include "error.h"

#define PUNGETC_MAX shell::pungetc_max
#define IBUFSIZ shell::ibufsiz

/* PEOF (the end of file marker) is defined in syntax.h */
#define PEOA ((PEOF) - 1)

enum {
	INPUT_PUSH_FILE = 1,
	INPUT_NOFILE_OK = 2,
};

using StringPush = shell::StringPush;
using parsefile = shell::ParseFile;
using stdin_state = shell::StdinState;
using InputStack = shell::InputStack;

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
void pushstring(ShellContext &sh, InputStack &in, char *, const std::shared_ptr<shell::Alias> & = nullptr);
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
void pushstring(ShellContext &sh, char *, const std::shared_ptr<shell::Alias> & = nullptr);
shell::Result<int> setinputfile(ShellContext &sh, std::string_view, int);
void setinputstring(ShellContext &sh, char *);
void pushstdin(ShellContext &sh);
void popfile(ShellContext &sh);
void unwindfiles(ShellContext &sh, parsefile *);
void flush_input(ShellContext &sh);
void reset_input(ShellContext &sh);
