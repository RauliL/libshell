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
#include <string.h>

#include "builtins.h"
#include "shell.h"
#include "shell_context.h"
#include "main.h"
#include "nodes.h"	/* for other headers */
#include "eval.h"
#include "init.h"
#include "jobs.h"
#include "show.h"
#include "options.h"
#include "syntax.h"
#include "output.h"
#include "parser.h"
#include "memalloc.h"
#include "error.h"
#include "trap.h"
#include "mystring.h"

/*
 * Sigmode records the current value of the signal handlers for the various
 * modes.  A value of zero means that the current sh.handler is not known.
 * S_HARD_IGN indicates that the signal was ignored on entry to the shell,
 */

#define S_DFL 1			/* default signal handling (SIG_DFL) */
#define S_CATCH 2		/* signal is caught */
#define S_IGN 3			/* signal is ignored (SIG_IGN) */
#define S_HARD_IGN 4		/* signal is ignored permenantly */
#define S_RESET 5		/* temporary - to reset a hard ignored sig */


/* sh.trap sh.handler commands */

/* traps have not been fully cleared */

/* number of non-null traps */

/* current value of signal */

/* indicates specified signal received */

/* last pending signal */

/* received SIGCHLD */


extern const char *const signal_names[];

static void signal_entry(int signo)
{
	if (ShellContext::signaled)
		onsig(*ShellContext::signaled, signo);
}

static int decode_signum(ShellContext &sh, const char *);
MKINIT void clear_traps(ShellContext &sh, union node *);

#ifdef mkinit
INCLUDE "memalloc.h"
INCLUDE "trap.h"

INIT {
	sh.sigmode[SIGCHLD - 1] = S_DFL;
	setsignal(sh, SIGCHLD);
}

FORKRESET {
	clear_traps(sh, n);
}
#endif

/*
 * The sh.trap builtin.
 */

int
trapcmd(ShellContext &sh, int argc, char **argv)
{
	char *action;
	char **ap;
	int signo;

	nextopt(sh, nullstr);
	ap = sh.argptr;
	if (!*ap) {
		for (signo = 0 ; signo < NSIG ; signo++) {
			if (sh.trap[signo] != NULL) {
				out1fmt(sh, "trap -- %s %s\n",
					single_quote(sh, sh.trap[signo]),
					signal_names[signo]
				);
			}
		}
		return 0;
	}
	if (sh.ptrap)
		clear_traps(sh, NULL);
	if (!ap[1] || decode_signum(sh, *ap) >= 0)
		action = NULL;
	else
		action = *ap++;
	while (*ap) {
		if ((signo = decode_signal(sh, *ap, 0)) < 0) {
			outfmt(sh, sh.out2, "trap: %s: bad trap\n", *ap);
			return 1;
		}
		INTOFF;
		if (action) {
			if (action[0] == '-' && action[1] == '\0')
				action = NULL;
			else {
				if (*action)
					sh.trapcnt++;
				action = savestr(sh, action);
			}
		}
		if (sh.trap[signo]) {
			if (*sh.trap[signo])
				sh.trapcnt--;
			ckfree(sh.trap[signo]);
		}
		sh.trap[signo] = action;
		if (signo != 0)
			setsignal(sh, signo);
		INTON;
		ap++;
	}
	return 0;
}



/*
 * Clear traps on a fork.
 */

void
clear_traps(ShellContext &sh, union node *n)
{
	int simplecmd;
	char **tp;

	simplecmd = issimplecmd(sh, n, TRAPCMD->name);

	INTOFF;
	for (tp = sh.trap ; tp < &sh.trap[NSIG] ; tp++) {
		if (*tp && **tp) {	/* sh.trap not NULL or SIG_IGN */
			char *otp = *tp;

			*tp = NULL;
			if (tp != &sh.trap[0])
				setsignal(sh, tp - sh.trap);

			if (simplecmd)
				*tp = otp;
			else
				ckfree(*tp);
		}
	}
	sh.trapcnt = 0;
	sh.ptrap = simplecmd;
	INTON;
}



/*
 * Set the signal sh.handler for the specified signal.  The routine figures
 * out what it should be set to.
 */

void
setsignal(ShellContext &sh, int signo)
{
	int action;
	int lvforked;
	char *t, tsig;
	struct sigaction act;

	lvforked = sh.vforked;

	if ((t = sh.trap[signo]) == NULL)
		action = S_DFL;
	else if (*t != '\0')
		action = S_CATCH;
	else
		action = S_IGN;
	if (rootshell && action == S_DFL && !lvforked) {
		switch (signo) {
		case SIGINT:
			if (iflag || sh.minusc || sflag == 0)
				action = S_CATCH;
			break;
		case SIGQUIT:
#ifdef DEBUG
			if (debug)
				break;
#endif
			/* FALLTHROUGH */
		case SIGTERM:
			if (iflag)
				action = S_IGN;
			break;
#if JOBS
		case SIGTSTP:
		case SIGTTOU:
			if (mflag)
				action = S_IGN;
			break;
#endif
		}
	}

	if (signo == SIGCHLD)
		action = S_CATCH;

	t = &sh.sigmode[signo - 1];
	tsig = *t;
	if (tsig == 0) {
		/*
		 * current setting unknown
		 */
		if (sigaction(signo, 0, &act) == -1) {
			/*
			 * Pretend it worked; maybe we should give a warning
			 * here, but other shells don't. We don't alter
			 * sh.sigmode, so that we retry every time.
			 */
			return;
		}
		if (act.sa_handler == SIG_IGN) {
			if (mflag && (signo == SIGTSTP ||
			     signo == SIGTTIN || signo == SIGTTOU)) {
				tsig = S_IGN;	/* don't hard ignore these */
			} else
				tsig = S_HARD_IGN;
		} else {
			tsig = S_RESET;	/* force to be set */
		}
	}
	if (tsig == S_HARD_IGN || tsig == action)
		return;
	switch (action) {
	case S_CATCH:
		act.sa_handler = signal_entry;
		break;
	case S_IGN:
		act.sa_handler = SIG_IGN;
		break;
	default:
		act.sa_handler = SIG_DFL;
	}
	if (!lvforked)
		*t = action;
	act.sa_flags = 0;
	sigfillset(&act.sa_mask);
	sigaction(signo, &act, 0);
}

/*
 * Ignore a signal.
 */

void
ignoresig(ShellContext &sh, int signo)
{
	if (sh.sigmode[signo - 1] == S_IGN || sh.sigmode[signo - 1] == S_HARD_IGN)
		return;
	signal(signo, SIG_IGN);
	if (!sh.vforked)
		sh.sigmode[signo - 1] = S_IGN;
}



/*
 * Signal sh.handler.
 */

void
onsig(ShellContext &sh, int signo)
{
	if (sh.vforked && getpid() != sh.vforked)
		return;

	if (signo == SIGCHLD) {
		sh.gotsigchld = 1;
		if (!sh.trap[SIGCHLD])
			return;
	}

	sh.gotsig[signo - 1] = 1;
	sh.pending_sig = signo;

	if (signo == SIGINT && !sh.trap[SIGINT]) {
		if (!sh.suppressint)
			onint(sh);
		sh.intpending = 1;
	}
}



/*
 * Called to execute a sh.trap.  Perhaps we should avoid entering new sh.trap
 * handlers while we are executing a sh.trap sh.handler.
 */

void
dotrap(ShellContext &sh)
{
	char *p;
	char *q;
	int i;
	int status, last_status;

	if (!sh.pending_sig)
		return;

	status = sh.savestatus;
	last_status = status;
	if (likely(status < 0)) {
		status = sh.exitstatus;
		sh.savestatus = status;
	}
	sh.pending_sig = 0;
	barrier();

	for (i = 0, q = sh.gotsig; i < NSIG - 1; i++, q++) {
		if (!*q)
			continue;

		if (sh.evalskip) {
			sh.pending_sig = i + 1;
			break;
		}

		*q = 0;

		p = sh.trap[i + 1];
		if (!p)
			continue;
		evalstring(sh, p, {});
		if (sh.evalskip != SKIPFUNC)
			sh.exitstatus = status;
	}

	sh.savestatus = last_status;
}



/*
 * Controls whether the shell is interactive or not.
 */


void
setinteractive(ShellContext &sh, int on)
{

	if (++on == sh.is_interactive)
		return;
	sh.is_interactive = on;
	setsignal(sh, SIGINT);
	setsignal(sh, SIGQUIT);
	setsignal(sh, SIGTERM);
}



/*
 * Called to exit the shell.
 */

void
exitshell(ShellContext &sh)
{
	char *p;

	sh.savestatus = sh.exitstatus;
	TRACE(("pid %d, exitshell(%d)\n", getpid(), sh.savestatus));
	try {
		if ((p = sh.trap[0])) {
			sh.trap[0] = NULL;
			if (!sh.ptrap) {
				sh.evalskip = 0;
				evalstring(sh, p, {});
				sh.evalskip = SKIPFUNCDEF;
			}
		}
	} catch (const dash::ShellError &) {
		/* EXIT trap failed; still tear down. */
	}
	exitreset(sh);
	postexitreset(sh);
	/*
	 * Disable job control so that whoever had the foreground before we
	 * started can get it back.
	 */
	try {
		setjobctl(sh, 0);
	} catch (const dash::ShellError &) {
	}
	flushall(sh);
	_exit(sh.exitstatus);
	/* NOTREACHED */
}

static int
decode_signum(ShellContext &sh, const char *string)
{
	int signo = -1;

	if (is_number(sh, string)) {
		signo = atoi(string);
		if (signo >= NSIG)
			signo = -1;
	}

	return signo;
}

int
decode_signal(ShellContext &sh, const char *string, int minsig)
{
	int signo;

	signo = decode_signum(sh, string);
	if (signo >= 0)
		return signo;

	for (signo = minsig; signo < NSIG; signo++) {
		if (!strcasecmp(string, signal_names[signo])) {
			return signo;
		}
	}

	return -1;
}

void
sigblockall(ShellContext &sh, sigset_t *oldmask)
{
	sigset_t mask;

	sigfillset(&mask);
	sigprocmask(SIG_SETMASK, &mask, oldmask);
}
