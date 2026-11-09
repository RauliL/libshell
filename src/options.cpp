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
 */

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define DEFINE_OPTIONS
#include "options.h"
#undef DEFINE_OPTIONS
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "error.h"
#include "mystring.h"
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <utility>

using namespace shell;

namespace shell {

			/* value of $0 */
	/* current positional parameters */
			/* argument list for builtin commands */
		/* set by nextopt (like getopt) */
			/* used by nextopt */

			/* argument to -c option */

namespace {
const char *const optnames[NOPTS] = {
	"errexit",
	"noglob",
	"ignoreeof",
	"interactive",
	"monitor",
	"noexec",
	"stdin",
	"xtrace",
	"verbose",
	"noclobber",
	"allexport",
	"notify",
	"nounset",
	"pipefail",
	"debug",
};
} // namespace

const char optletters[NOPTS] = {
	'e',
	'f',
	'I',
	'i',
	'm',
	'n',
	's',
	'x',
	'v',
	'C',
	'a',
	'b',
	'u',
	0,
	0,
};

namespace {
Result<int> options(Context &sh, int,
				  std::optional<std::string> *minusc);
Result<void> minus_o(Context &sh, char *, int);
Result<void> setoption(Context &sh, int, int);
Result<int> getopts(Context &sh, char *, char *, char **);
} // namespace

/*
 * Process the shell command line arguments.
 */

Result<ProcArgsResult>
procargs(Context &sh, char **xargv)
{
	int i;
	ProcArgsResult result;

	result.login = xargv[0] && xargv[0][0] == '-';
	if (xargv[0])
		sh.arg0 = xargv[0];
	if (xargv[0])
		xargv++;
	for (i = 0; i < NOPTS; i++)
		sh.opts.list[i] = 2;
	sh.bopt.argptr = xargv;
	{
		int tmp;
		TRY_SH_SET(tmp, options(sh, 1, &result.minusc));
		result.login |= tmp;
	}
	xargv = sh.bopt.argptr;
	if (!*xargv) {
		if (result.minusc)
			return sh_error(sh, "-c requires an argument");
		sh.opts.stdin_opt() = 1;
	}
	if (sh.opts.interactive() == 2 && sh.opts.stdin_opt() == 1) {
		input_init(sh);
		if (sh.input.stdin_istty && isatty(2))
			sh.opts.interactive() = 1;
	}
	if (sh.opts.monitor() == 2)
		sh.opts.monitor() = sh.opts.interactive();
	for (i = 0; i < NOPTS; i++)
		if (sh.opts.list[i] == 2)
			sh.opts.list[i] = 0;
	/* POSIX 1003.2: first arg after -c cmd is $0, remainder $1... */
	if (result.minusc) {
		*result.minusc = *xargv++;
		if (*xargv)
			goto setarg0;
	} else if (!sh.opts.stdin_opt()) {
		TRY_SHV(setinputfile(sh, *xargv, 0));
setarg0:
		sh.arg0 = *xargv++;
	}

	sh.opts.c_mode = result.minusc.has_value();

	sh.shellparam.p.clear();
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
	while (*xargv)
		sh.shellparam.p.emplace_back(*xargv++);
	optschanged(sh);

	return result;
}

void
optschanged(Context &sh)
{
	setsignal(sh, SIGINT);
	setsignal(sh, SIGQUIT);
	setsignal(sh, SIGTERM);
}

/*
 * Process shell options.  The global variable sh.bopt.argptr contains a pointer
 * to the argument list; we advance it past the options.
 */

namespace {
Result<int>
options(Context &sh, int cmdline, std::optional<std::string> *minusc)
{
	char *p;
	int val;
	int c;
	int login = 0;

	if (cmdline && minusc)
		minusc->reset();
	while ((p = *sh.bopt.argptr)) {
		sh.bopt.argptr++;
		if ((c = *p++) == '-') {
			val = 1;
                        if (p[0] == '\0' || (p[0] == '-' && p[1] == '\0')) {
                                if (!cmdline) {
                                        /* "-" means turn off -x and -v */
                                        if (p[0] == '\0')
                                                sh.opts.xtrace() = sh.opts.verbose() = 0;
                                        /* "--" means reset params */
                                        else if (!*sh.bopt.argptr)
						setparam(sh, sh.bopt.argptr);
                                }
				break;	  /* "-" or  "--" terminates options */
			}
		} else if (c == '+') {
			val = 0;
		} else {
			sh.bopt.argptr--;
			break;
		}
		while ((c = *p++) != '\0') {
			if (c == 'c' && cmdline && minusc) {
				*minusc = std::string(); /* filled after shell args */
			} else if (c == 'l' && cmdline) {
				login = 1;
			} else if (c == 'o') {
				TRY_SHV(minus_o(sh, *sh.bopt.argptr, val));
				if (*sh.bopt.argptr)
					sh.bopt.argptr++;
			} else {
				TRY_SHV(setoption(sh, c, val));
			}
		}
	}

	return login;
}

Result<void>
minus_o(Context &sh, char *name, int val)
{
	if (!name) {
		if (val) {
			out1str("Current option settings\n");
			for (int i = 0; i < NOPTS; i++)
				out1fmt(sh, "%-16s%s\n", optnames[i],
					sh.opts.list[i] ? "on" : "off");
		} else {
			for (int i = 0; i < NOPTS; i++)
                out1str(std::format(
                    "set %s %s\n",
					sh.opts.list[i] ? "-o" : "+o",
					optnames[i]
                ));

		}
	} else {
		for (int i = 0; i < NOPTS; i++)
			if (std::string_view(name) == optnames[i]) {
				sh.opts.list[i] = val;
				return {};
			}
		return sh_error(sh, "Illegal option -o {}", name);
	}

	return {};
}

Result<void>
setoption(Context &sh, int flag, int val)
{
	int i;

	for (i = 0; i < NOPTS; i++)
		if (optletters[i] == flag) {
			sh.opts.list[i] = val;
			return {};
		}
	return sh_error(sh, "Illegal option -{}", flag);
}
} // namespace

void
Context::set_arg0(std::string name)
{
	arg0 = std::move(name);
}

void
Context::set_args(std::span<const std::string_view> args)
{
	shellparam.p.clear();
	shellparam.p.reserve(args.size());
	for (auto arg : args)
		shellparam.p.emplace_back(arg);
	shellparam.optind = 1;
	shellparam.optoff = -1;
}

Result<void>
Context::set_option(std::string_view name, bool enabled)
{
	for (int i = 0; i < NOPTS; i++) {
		if (name == optnames[i]) {
			opts.list[i] = enabled ? 1 : 0;
			optschanged(*this);
			return {};
		}
	}
	return sh_error(*this, "Illegal option -o {}", name);
}

Result<void>
Context::set_option(char letter, bool enabled)
{
	TRY_SHV(setoption(*this, letter, enabled ? 1 : 0));
	optschanged(*this);
	return {};
}

std::optional<bool>
Context::get_option(std::string_view name) const
{
	for (int i = 0; i < NOPTS; i++) {
		if (name == optnames[i])
			return opts.list[i] != 0;
	}
	return std::nullopt;
}

/*
 * Set the shell parameters.
 */

void
setparam(Context &sh, char **argv)
{
	std::vector<std::string> newparam;

	while (*argv)
		newparam.emplace_back(*argv++);
	freeparam(sh, &sh.shellparam);
	sh.shellparam.p = std::move(newparam);
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
}

/*
 * Free the list of positional parameters.
 */

void
freeparam(Context &sh, ShellParam *param)
{
	param->p.clear();
}

/*
 * The shift builtin command.
 */

Result<int>
shiftcmd(Context &sh, int argc, char **argv)
{
	int n = 1;

	if (argc > 1)
		TRY_SH_SET(n, number(sh, argv[1]));
	if (static_cast<std::size_t>(n) > sh.shellparam.p.size())
		return sh_error(sh, "can't shift that many");
	INTOFF(sh);
	sh.shellparam.p.erase(sh.shellparam.p.begin(),
			      sh.shellparam.p.begin() + n);
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
	INTON(sh);

	return EXIT_SUCCESS;
}

/*
 * The set command builtin.
 */

Result<int>
setcmd(Context &sh, int argc, char **argv)
{
	if (argc == 1)
		return showvars(sh, "", {}, Variable::Flag::Unset);
	INTOFF(sh);
	TRY_SHV(options(sh, 0, nullptr));
	optschanged(sh);
	if (*sh.bopt.argptr) {
		setparam(sh, sh.bopt.argptr);
	}
	INTON(sh);

	return EXIT_SUCCESS;
}

void
getoptsreset(Context &sh, std::string_view value)
{
	(void)value;
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
}

/*
 * The getopts builtin.  Shellparam.optnext points to the next argument
 * to be processed.  Shellparam.optptr points to the next character to
 * be processed in the current argument.  If sh.shellparam.optnext is nullptr,
 * then it's the first time getopts has been called.
 */

Result<int>
getoptscmd(Context &sh, int argc, char **argv)
{
	char **optbase;

	TRY_SHV(nextopt(sh, ""));
	argc -= sh.bopt.argptr - argv - 1;
	argv = sh.bopt.argptr - 1;
	std::vector<char *> shell_ptrs;
	if (argc < 3)
		return sh_error(sh, "Usage: getopts optstring var [arg...]");
	else if (argc == 3) {
		shell_ptrs.reserve(sh.shellparam.p.size() + 1);
		for (auto &s : sh.shellparam.p)
			shell_ptrs.push_back(s.data());
		shell_ptrs.push_back(nullptr);
		optbase = shell_ptrs.data();
		if (static_cast<unsigned>(sh.shellparam.optind) > sh.shellparam.p.size() + 1) {
			sh.shellparam.optind = 1;
			sh.shellparam.optoff = -1;
		}
	}
	else {
		optbase = &argv[3];
		if (static_cast<unsigned>(sh.shellparam.optind) >
		    static_cast<unsigned>(argc - 2)) {
			sh.shellparam.optind = 1;
			sh.shellparam.optoff = -1;
		}
	}

	return getopts(sh, argv[1], argv[2], optbase);
}

namespace {
Result<int>
getopts(Context &sh, char *optstr, char *optvar, char **optfirst)
{
	char *p, *q;
	char c = '?';
	int done = 0;
	char s[2];
	char **optnext;
	int ind = sh.shellparam.optind;
	int off = sh.shellparam.optoff;

	sh.shellparam.optind = -1;
	optnext = optfirst + ind - 1;

	if (ind <= 1 || off < 0 ||
	    std::strlen(optnext[-1]) < static_cast<size_t>(off))
		p = nullptr;
	else
		p = optnext[-1] + off;
	if (!p || *p == '\0') {
		/* Current word is done, advance */
		p = *optnext;
		if (!p || *p != '-' || *++p == '\0') {
atend:
			p = nullptr;
			done = 1;
			goto out;
		}
		optnext++;
		if (p[0] == '-' && p[1] == '\0')	/* check for "--" */
			goto atend;
	}

	c = *p++;
	for (q = optstr[0] == ':' ? optstr + 1 : optstr; *q != c; ) {
		if (*q == '\0') {
			if (optstr[0] == ':') {
				s[0] = c;
				s[1] = '\0';
				TRY_SHV(setvar(sh, "OPTARG", s));
			} else {
                sh.errout.write(sh, std::format("Illegal option -{}\n", static_cast<char>(c)));
				unsetvar(sh, "OPTARG");
			}
			c = '?';
			goto out;
		}
		if (*++q == ':')
			q++;
	}

	if (*++q == ':') {
		if (*p == '\0' && !(p = *optnext)) {
			if (optstr[0] == ':') {
				s[0] = c;
				s[1] = '\0';
				TRY_SHV(setvar(sh, "OPTARG", s));
				c = ':';
			} else {
                sh.errout.write(sh, std::format("No arg for -{} option\n", static_cast<char>(c)));
				unsetvar(sh, "OPTARG");
				c = '?';
			}
			goto out;
		}

		if (p == *optnext)
			optnext++;
		TRY_SHV(setvar(sh, "OPTARG", p));
		p = nullptr;
	} else
		TRY_SHV(setvar(sh, "OPTARG", ""));

out:
	ind = optnext - optfirst + 1;
	TRY_SHV(sh.set_int_var("OPTIND", ind, Variable::Flag::NoFunc));
	s[0] = c;
	s[1] = '\0';
	TRY_SHV(setvar(sh, optvar, s));

	sh.shellparam.optoff = p ? p - *(optnext - 1) : -1;
	sh.shellparam.optind = ind;

	return done;
}
} // namespace

/*
 * XXX - should get rid of.  have all builtins use getopt(3).  the
 * library getopt must have the BSD extension static variable "optreset"
 * otherwise it can't be used within the shell safely.
 *
 * Standard option processing (a la getopt) for builtin routines.  The
 * only argument that is passed to nextopt is the option string; the
 * other arguments are unnecessary.  It return the character, or '\0' on
 * end of input.
 */

Result<int>
nextopt(Context &sh, std::string_view optstring)
{
	char *p;
	char c;

	if (!(p = sh.bopt.optptr) || *p == '\0') {
		p = *sh.bopt.argptr;
		if (!p || *p != '-' || *++p == '\0')
			return '\0';
		sh.bopt.argptr++;
		if (p[0] == '-' && p[1] == '\0')	/* check for "--" */
			return '\0';
	}
	c = *p++;
	std::size_t i = 0;
	for (;;) {
		if (i >= optstring.size())
			return sh_error(sh, "Illegal option -{}", c);
		if (optstring[i] == c)
			break;
		if (i + 1 < optstring.size() && optstring[i + 1] == ':')
			i += 2;
		else
			++i;
	}
	if (i + 1 < optstring.size() && optstring[i + 1] == ':') {
		if (*p == '\0' && !(p = *sh.bopt.argptr++))
			return sh_error(sh, "No arg for -{} option", c);
		sh.bopt.optionarg = p;
		p = nullptr;
	}
	sh.bopt.optptr = p;
	return c;
}

} // namespace shell
