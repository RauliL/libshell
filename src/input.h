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


namespace shell {

#define PUNGETC_MAX pungetc_max
#define IBUFSIZ ibufsiz

/* PEOF (the end of file marker) is defined in syntax.h */
#define PEOA ((PEOF) - 1)

enum {
	INPUT_PUSH_FILE = 1,
	INPUT_NOFILE_OK = 2,
};


/*
 * The input line number. Requires Context &sh in scope.
 */
#define plinno (sh.input.linno())

void input_init(Context &sh, InputStack &in);
/** Drain the current line after an error/interrupt in an interactive shell. */
void input_reset_interactive(Context &sh);
/** Drop nested input files and stdin pipe state when entering a subshell. */
void input_forkreset(Context &sh);
int pgetc(Context &sh, InputStack &in);
int pgetc_eoa(Context &sh, InputStack &in);
void pungetc(Context &sh, InputStack &in);
void pungetn(InputStack &in, int);
void pushstring(Context &sh, InputStack &in, char *, const std::shared_ptr<Alias> & = nullptr);
Result<int> setinputfile(Context &sh, InputStack &in,
				    std::string_view, int);
void setinputstring(Context &sh, InputStack &in, char *);
void pushstdin(Context &sh, InputStack &in);
void popfile(Context &sh, InputStack &in);
void unwindfiles(Context &sh, InputStack &in, ParseFile *stop);
void popallfiles(Context &sh, InputStack &in);
void flush_input(Context &sh, InputStack &in);
void reset_input(Context &sh, InputStack &in);

/* Overloads that use the shell's own input stack. */
void input_init(Context &sh);
int pgetc(Context &sh);
int pgetc_eoa(Context &sh);
void pungetc(Context &sh);
void pungetn(Context &sh, int);
void pushstring(Context &sh, char *, const std::shared_ptr<Alias> & = nullptr);
Result<int> setinputfile(Context &sh, std::string_view, int);
void setinputstring(Context &sh, char *);
void pushstdin(Context &sh);
void popfile(Context &sh);
void unwindfiles(Context &sh, ParseFile *);
void flush_input(Context &sh);
void reset_input(Context &sh);

} // namespace shell
