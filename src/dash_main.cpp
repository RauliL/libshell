/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * Interactive / process driver for the dash executable.  Non-interactive
 * command looping, `.`, and `exit` live in main.cpp (the library).
 */

#include <stdio.h>
#include <unistd.h>

#include "shell.h"

#include "main.h"
#include "shell_context.h"
#include "mail.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
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
#include "parse_error.h"
#include "nodes_free.h"

#define PROFILE 0

#if PROFILE
short profile_buf[16384];
extern int etext();
#endif

static void read_profile(ShellContext &sh, const char *name);
static int interactive_cmdloop(ShellContext &sh);

/*
 * Top-level interactive parse/eval loop: prompting, mail checks, ignoreeof,
 * and recovering from parse errors at the prompt.
 */
static int
interactive_cmdloop(ShellContext &sh)
{
	union node *n;
	int status = 0;
	int numeof = 0;

	TRACE(("interactive_cmdloop() called\n"));
	for (;;) {
		int skip;

		if (sh.jobctl)
			showjobs(sh, sh.out2, SHOW_CHANGED);
		chkmail(sh);
		try {
			n = parsecmd(sh, 1);
			if (n == NEOF) {
				clear_stack_temps(sh);
				if (numeof >= 50)
					break;
				if (!stoppedjobs(sh)) {
					if (!Iflag) {
						out2c('\n');
#ifdef FLUSHERR
						flushout(sh, sh.out2);
#endif
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

static void
read_profile(ShellContext &sh, const char *name)
{
	name = expandstr(sh, name);
	if (setinputfile(sh, name, INPUT_PUSH_FILE | INPUT_NOFILE_OK) < 0)
		return;

	cmdloop(sh);
	popfile(sh);
}

/*
 * Executable entry: initialize, parse args, run login/ENV profiles when
 * appropriate, then either -c, a non-interactive cmdloop, or the interactive
 * loop.  setjmp recovers to the main loop only for an interactive root shell.
 */
static int
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
		FORCEINTON;
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
state4:
		if (iflag)
			interactive_cmdloop(sh);
		else
			cmdloop(sh);
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

int
main(int argc, char **argv)
{
	ShellContext shell;
	return run(shell, argc, argv);
}
