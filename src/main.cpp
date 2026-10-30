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

#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>


#include "shell.h"

#ifdef __GLIBC__
int *dash_errno;
#endif
#include "shell_context.h"
#include "main.h"
#include "mail.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "expand.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#include "var.h"
#include "show.h"
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#include "exec.h"
#include "cd.h"
#include "parse_error.h"
#include "nodes_free.h"

#define PROFILE 0




#ifdef __GLIBC__

#endif
#if PROFILE
short profile_buf[16384];
extern int etext();
#endif


static void read_profile(ShellContext &sh, const char *);
static char * find_dot_file(ShellContext &sh, char *);
int cmdloop(ShellContext &sh, int);
/*
 * Main routine.  We initialize things, parse the arguments, execute
 * profiles if we're a login shell, and then call cmdloop to execute
 * commands.  The setjmp call sets up the location to jump to when an
 * sh.exception occurs.  When an sh.exception occurs the variable "state"
 * is used to figure out how far we had gotten.
 */

int
run(ShellContext &sh, int argc, char **argv)
{
	char *shinit;
	volatile int state;
	int login;

#ifdef __GLIBC__
	dash_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif

	state = 0;
	if (unlikely(setjmp(sh.main_handler.loc))) {
		int e;
		int s;

		exitreset(sh);

		e = sh.exception;

		s = state;
		if (e == EXEND || e == EXEXIT || s == 0 || iflag == 0 || sh.shlvl)
			goto exit;

		reset(sh);

		if (e == EXINT
#if ATTY
		 && (! attyset() || equal(termval(), "emacs"))
#endif
		 ) {
			out2c('\n');
#ifdef FLUSHERR
			flushout(sh, sh.out2);
#endif
		}
		clear_stack_temps(sh);
		FORCEINTON;				/* enable interrupts */
		if (s == 1)
			goto state1;
		else if (s == 2)
			goto state2;
		else if (s == 3)
			goto state3;
		else
			goto state4;
	}
	sh.handler = &sh.main_handler;
#ifdef DEBUG
	opentrace(sh);
	trputs(sh, "Shell args:  ");  trargs(sh, argv);
#endif
	sh.mypid = sh.rootpid = getpid();
	init(sh);
	login = procargs(sh, argv);
	if (login) {
		state = 1;
		read_profile(sh, "/etc/profile");
state1:
		state = 2;
		read_profile(sh, "$HOME/.profile");
	}
state2:
	state = 3;
	if (
#ifndef linux
		getuid() == geteuid() && getgid() == getegid() &&
#endif
		iflag
	) {
		if ((shinit = lookupvar(sh, "ENV")) != NULL && *shinit != '\0') {
			read_profile(sh, shinit);
		}
	}
	clear_stack_temps(sh);
state3:
	state = 4;
	if (sh.minusc)
		execute(sh, sh.minusc, sflag ? EvalFlags{} : EvalFlag::exit);

	if (sflag || sh.minusc == NULL) {
state4:	/* XXX ??? - why isn't this before the "if" statement */
		cmdloop(sh, 1);
	}
exit:
#if PROFILE
	monitor(0);
#endif
#if GPROF
	{
		extern void _mcleanup(void);
		_mcleanup();
	}
#endif
	exitshell(sh);
	/* NOTREACHED */
}


/*
 * Read and execute commands.  "Top" is nonzero for the top level command
 * loop; it turns on prompting if the shell is interactive.
 */

int
cmdloop(ShellContext &sh, int top)
{
	union node *n;
	int inter;
	int status = 0;
	int numeof = 0;

	TRACE(("cmdloop(%d) called\n", top));
	for (;;) {
		int skip;

		if (sh.jobctl)
			showjobs(sh, sh.out2, SHOW_CHANGED);
		inter = 0;
		if (iflag && top) {
			inter++;
			chkmail(sh);
		}
		try {
			n = parsecmd(sh, inter);
			/* showtree(n); DEBUG */
			if (n == NEOF) {
				clear_stack_temps(sh);
				if (!top || numeof >= 50)
					break;
				if (!stoppedjobs(sh)) {
					if (!Iflag) {
						if (iflag) {
							out2c('\n');
#ifdef FLUSHERR
							flushout(sh, sh.out2);
#endif
						}
						break;
					}
					out2str("\nUse \"exit\" to leave shell.\n");
				}
				numeof++;
			} else {
				int i;

				sh.job_warning = (sh.job_warning == 2) ? 1 : 0;
				numeof = 0;
				i = evaltree(sh, n, {});
				if (n)
					status = i;
				if (n && n != NEOF)
					freenode(n);
				clear_stack_temps(sh);
			}
		} catch (const dash::ParseError &e) {
			report_parse_error(sh, e);
			clear_stack_temps(sh);
			if (!(iflag && top))
				return sh.exitstatus;
			skip = sh.evalskip;
			if (skip) {
				sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
				break;
			}
			continue;
		}

		skip = sh.evalskip;
		if (skip) {
			sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
			break;
		}
	}

	return status;
}



/*
 * Read /etc/profile or .profile.  Return on error.
 */

static void
read_profile(ShellContext &sh, const char *name)
{
	name = expandstr(sh, name);
	if (setinputfile(sh, name, INPUT_PUSH_FILE | INPUT_NOFILE_OK) < 0)
		return;

	cmdloop(sh, 0);
	popfile(sh);
}



/*
 * Read a file containing shell functions.
 */

void
readcmdfile(ShellContext &sh, char *name)
{
	setinputfile(sh, name, INPUT_PUSH_FILE);
	cmdloop(sh, 0);
	popfile(sh);
}



/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */


static char *
find_dot_file(ShellContext &sh, char *basename)
{
	char *fullname;
	const char *path = pathval();
	struct stat64 statb;
	int len;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return basename;

	while ((len = padvance(sh, &path, basename)) >= 0) {
		fullname = stackblock();
		if ((!sh.pathopt || *sh.pathopt == 'f') &&
		    !stat64(fullname, &statb) && S_ISREG(statb.st_mode)) {
			/* This will be freed by the caller. */
			return stalloc(sh, len);
		}
	}

	/* not found in the PATH */
	sh_error(sh, "%s: not found", basename);
	/* NOTREACHED */
}

int
dotcmd(ShellContext &sh, int argc, char **argv)
{
	int status = 0;

	nextopt(sh, nullstr);
	argv = sh.argptr;

	if (*argv) {
		char *fullname;

		fullname = find_dot_file(sh, *argv);
		setinputfile(sh, fullname, INPUT_PUSH_FILE);
		sh.commandname = fullname;
		status = cmdloop(sh, 0);
		popfile(sh);
	}

	return status;
}


int
exitcmd(ShellContext &sh, int argc, char **argv)
{
	if (stoppedjobs(sh))
		return 0;

	if (argc > 1)
		sh.savestatus = number(sh, argv[1]);

	exraise(sh, EXEXIT);
	/* NOTREACHED */
}

#ifdef mkinit
INCLUDE "error.h"

FORKRESET {
	sh.handler = &sh.main_handler;
}
#endif
