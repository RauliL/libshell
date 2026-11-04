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
#include <string>
#include <vector>

#include "shell.h"
#include "shell_context.h"
#define DEFINE_OPTIONS
#include "options.h"
#undef DEFINE_OPTIONS
#include "nodes.h"	/* for other header files */
#include "eval.h"
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "show.h"

			/* value of $0 */
	/* current positional parameters */
			/* argument list for builtin commands */
		/* set by nextopt (like getopt) */
			/* used by nextopt */

			/* argument to -c option */

static const char *const optnames[NOPTS] = {
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




static shell::Result<int> options(ShellContext &sh, int,
				  std::optional<std::string> *minusc);
static shell::Result<void> minus_o(ShellContext &sh, char *, int);
static shell::Result<void> setoption(ShellContext &sh, int, int);
static shell::Result<int> getopts(ShellContext &sh, char *, char *, char **);


/*
 * Process the shell command line arguments.
 */

shell::Result<ProcArgsResult>
procargs(ShellContext &sh, char **xargv)
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
	result.login |= TRY_SH(options(sh, 1, &result.minusc));
	xargv = sh.bopt.argptr;
	if (*xargv == NULL) {
		if (result.minusc)
			return sh_error(sh, "-c requires an argument");
		sflag = 1;
	}
	if (iflag == 2 && sflag == 1) {
		input_init(sh);
		if (sh.input.stdin_istty && isatty(2))
			iflag = 1;
	}
	if (mflag == 2)
		mflag = iflag;
	for (i = 0; i < NOPTS; i++)
		if (sh.opts.list[i] == 2)
			sh.opts.list[i] = 0;
#if DEBUG == 2
	debug = 1;
#endif
	/* POSIX 1003.2: first arg after -c cmd is $0, remainder $1... */
	if (result.minusc) {
		*result.minusc = *xargv++;
		if (*xargv)
			goto setarg0;
	} else if (!sflag) {
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
optschanged(ShellContext &sh)
{
#ifdef DEBUG
	opentrace(sh);
#endif
	setsignal(sh, SIGINT);
	setsignal(sh, SIGQUIT);
	setsignal(sh, SIGTERM);
}

/*
 * Process shell options.  The global variable sh.bopt.argptr contains a pointer
 * to the argument list; we advance it past the options.
 */

static shell::Result<int>
options(ShellContext &sh, int cmdline, std::optional<std::string> *minusc)
{
	char *p;
	int val;
	int c;
	int login = 0;

	if (cmdline && minusc)
		minusc->reset();
	while ((p = *sh.bopt.argptr) != NULL) {
		sh.bopt.argptr++;
		if ((c = *p++) == '-') {
			val = 1;
                        if (p[0] == '\0' || (p[0] == '-' && p[1] == '\0')) {
                                if (!cmdline) {
                                        /* "-" means turn off -x and -v */
                                        if (p[0] == '\0')
                                                xflag = vflag = 0;
                                        /* "--" means reset params */
                                        else if (*sh.bopt.argptr == NULL)
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

static shell::Result<void>
minus_o(ShellContext &sh, char *name, int val)
{
	int i;

	if (name == NULL) {
		if (val) {
			out1str("Current option settings\n");
			for (i = 0; i < NOPTS; i++)
				out1fmt(sh, "%-16s%s\n", optnames[i],
					sh.opts.list[i] ? "on" : "off");
		} else {
			for (i = 0; i < NOPTS; i++)
                out1str(std::format(
                    "set %s %s\n",
					sh.opts.list[i] ? "-o" : "+o",
					optnames[i]
                ));

		}
	} else {
		for (i = 0; i < NOPTS; i++)
			if (equal(name, optnames[i])) {
				sh.opts.list[i] = val;
				return {};
			}
		return sh_error(sh, "Illegal option -o {}", name);
	}
	return {};
}


static shell::Result<void>
setoption(ShellContext &sh, int flag, int val)
{
	int i;

	for (i = 0; i < NOPTS; i++)
		if (optletters[i] == flag) {
			sh.opts.list[i] = val;
			return {};
		}
	return sh_error(sh, "Illegal option -{}", flag);
}



/*
 * Set the shell parameters.
 */

void
setparam(ShellContext &sh, char **argv)
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
freeparam(ShellContext &sh, struct shparam *param)
{
	param->p.clear();
}



/*
 * The shift builtin command.
 */

shell::Result<int>
shiftcmd(ShellContext &sh, int argc, char **argv)
{
	int n = 1;

	if (argc > 1)
		n = TRY_SH(number(sh, argv[1]));
	if (static_cast<std::size_t>(n) > sh.shellparam.p.size())
		return sh_error(sh, "can't shift that many");
	INTOFF;
	sh.shellparam.p.erase(sh.shellparam.p.begin(),
			      sh.shellparam.p.begin() + n);
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
	INTON;
	return 0;
}



/*
 * The set command builtin.
 */

shell::Result<int>
setcmd(ShellContext &sh, int argc, char **argv)
{
	if (argc == 1)
		return showvars(sh, nullstr, {}, VarFlag::unset);
	INTOFF;
	TRY_SHV(options(sh, 0, nullptr));
	optschanged(sh);
	if (*sh.bopt.argptr != NULL) {
		setparam(sh, sh.bopt.argptr);
	}
	INTON;
	return 0;
}


void
getoptsreset(ShellContext &sh, std::string_view value)
{
	(void)value;
	sh.shellparam.optind = 1;
	sh.shellparam.optoff = -1;
}

/*
 * The getopts builtin.  Shellparam.optnext points to the next argument
 * to be processed.  Shellparam.optptr points to the next character to
 * be processed in the current argument.  If sh.shellparam.optnext is NULL,
 * then it's the first time getopts has been called.
 */

shell::Result<int>
getoptscmd(ShellContext &sh, int argc, char **argv)
{
	char **optbase;

	TRY_SHV(nextopt(sh, nullstr));
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
		if ((unsigned)sh.shellparam.optind > sh.shellparam.p.size() + 1) {
			sh.shellparam.optind = 1;
			sh.shellparam.optoff = -1;
		}
	}
	else {
		optbase = &argv[3];
		if ((unsigned)sh.shellparam.optind > argc - 2) {
			sh.shellparam.optind = 1;
			sh.shellparam.optoff = -1;
		}
	}

	return getopts(sh, argv[1], argv[2], optbase);
}

static shell::Result<int>
getopts(ShellContext &sh, char *optstr, char *optvar, char **optfirst)
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

	if (ind <= 1 || off < 0 || std::strlen(optnext[-1]) < off)
		p = NULL;
	else
		p = optnext[-1] + off;
	if (p == NULL || *p == '\0') {
		/* Current word is done, advance */
		p = *optnext;
		if (p == NULL || *p != '-' || *++p == '\0') {
atend:
			p = NULL;
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
		if (*p == '\0' && (p = *optnext) == NULL) {
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
		p = NULL;
	} else
		TRY_SHV(setvar(sh, "OPTARG", nullstr));

out:
	ind = optnext - optfirst + 1;
	TRY_SHV(setvarint(sh, "OPTIND", ind, VarFlag::nofunc));
	s[0] = c;
	s[1] = '\0';
	TRY_SHV(setvar(sh, optvar, s));

	sh.shellparam.optoff = p ? p - *(optnext - 1) : -1;
	sh.shellparam.optind = ind;

	return done;
}

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

shell::Result<int>
nextopt(ShellContext &sh, std::string_view optstring)
{
	char *p;
	char c;

	if ((p = sh.bopt.optptr) == NULL || *p == '\0') {
		p = *sh.bopt.argptr;
		if (p == NULL || *p != '-' || *++p == '\0')
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
		if (*p == '\0' && (p = *sh.bopt.argptr++) == NULL)
			return sh_error(sh, "No arg for -{} option", c);
		sh.bopt.optionarg = p;
		p = NULL;
	}
	sh.bopt.optptr = p;
	return c;
}
