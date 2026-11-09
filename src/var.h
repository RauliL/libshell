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
 *	@(#)var.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <bitset>
#include <cstddef>
#include <inttypes.h>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "error.h"

struct ShellContext;

/*
 * Shell variables.
 */

/* Value helpers use ShellContext &sh from the enclosing scope. */

extern const char defpathvar[];
inline constexpr char defifs[] = " \t\n";
#define defpath (defpathvar + 36)

/*
 * The following macros access builtin variable values.  They skip over
 * the name= prefix.  Builtins are always present in sh.variables after initvar.
 */

#define ifsset(sh)	(!(sh).variables.at("IFS").flags.test(shell::Variable::Flag::Unset))
#define pathval(sh)	((sh).variables.at("PATH").text.c_str() + 5)
#define ps1val(sh)	((sh).variables.at("PS1").text.c_str() + 4)
#define ps2val(sh)	((sh).variables.at("PS2").text.c_str() + 4)
#define ps4val(sh)	((sh).variables.at("PS4").text.c_str() + 4)

struct ShellContext;
void initvar(ShellContext &sh);
/** Seed builtins, import environ, and set IFS/OPTIND/PPID/PWD. */
void init_shell_vars(ShellContext &sh);
shell::Result<shell::Variable *> setvar(ShellContext &sh, const std::string &name,
				       const std::optional<std::string> &val,
				       shell::Variable::Flags flags = {});
shell::Result<shell::Variable *> setvareq(ShellContext &sh, std::string s, shell::Variable::Flags flags = {});
/** Name=value strings for variables matching on/off flags (no sentinel). */
std::vector<std::string_view> listvars(ShellContext &sh, const shell::Variable::Flags &on,
				       const shell::Variable::Flags &off);
/** Environ block for execve: pointers into the variable map + trailing nullptr. */
std::vector<char *> environment(ShellContext &sh);
shell::Result<int> showvars(ShellContext &sh, std::string_view prefix,
				const shell::Variable::Flags &on, const shell::Variable::Flags &off);
shell::Result<int> exportcmd(ShellContext &sh, int, char **);
shell::Result<int> localcmd(ShellContext &sh, int, char **);
shell::Result<void> mklocal(ShellContext &sh, std::string_view name,
				const shell::Variable::Flags &flags = {});
shell::Result<std::size_t> pushlocalvars(ShellContext &sh, int push);
shell::Result<void> unwindlocalvars(ShellContext &sh, std::size_t stop);
shell::Result<int> unsetcmd(ShellContext &sh, int, char **);
shell::Result<void> unsetvar(ShellContext &sh, const std::string &);
