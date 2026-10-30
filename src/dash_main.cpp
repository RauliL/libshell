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
		} catch (const dash::ShellError &e) {
			sh.exception = e.code();
			clear_stack_temps(sh);
			if (e.code() == EXERROR) {
				skip = sh.evalskip;
				if (skip) {
					sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
					break;
				}
				continue;
			}
			throw;
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
 * Executable entry.  ShellError replaces setjmp for interactive recovery:
 * abort the current stage and continue (profiles → ENV → main loop), or
 * exit when non-interactive / EXEXIT / EXEND.
 */
static int
run(ShellContext &sh, int argc, char **argv)
{
	char *shinit;
	int state = 0;
	int login = 0;

#ifdef __GLIBC__
	dash_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif

#ifdef DEBUG
	opentrace(sh);
	trputs(sh, "Shell args:  ");  trargs(sh, argv);
#endif
	sh.mypid = sh.rootpid = getpid();
	init(sh);
	login = procargs(sh, argv);

resume:
	try {
		if (state <= 1 && login) {
			if (state < 1) {
				state = 1;
				read_profile(sh, "/etc/profile");
			}
			state = 2;
			read_profile(sh, "$HOME/.profile");
		}
		if (state < 3) {
			state = 3;
			if (
#ifndef linux
				getuid() == geteuid() && getgid() == getegid() &&
#endif
				iflag
			) {
				if ((shinit = lookupvar(sh, "ENV")) != NULL &&
				    *shinit != '\0') {
					read_profile(sh, shinit);
				}
			}
			clear_stack_temps(sh);
		}
		if (state < 4) {
			state = 4;
			if (sh.minusc)
				execute(sh, sh.minusc->data(),
					sflag ? EvalFlags{} : EvalFlag::exit);
		}
		if (iflag)
			interactive_cmdloop(sh);
		else if (sflag || !sh.minusc)
			cmdloop(sh);
	} catch (const dash::ShellError &e) {
		int ecode = e.code();

		exitreset(sh);
		sh.exception = ecode;
		if (ecode == EXEND || ecode == EXEXIT || state == 0 ||
		    iflag == 0 || sh.shlvl)
			goto exit;

		reset(sh);

		if (ecode == EXINT
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
		goto resume;
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
