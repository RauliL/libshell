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
#include <string>
#include <utility>

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

#define S_DFL 1			/* default signal handling (SIG_DFL) */
#define S_CATCH 2		/* signal is caught */
#define S_IGN 3			/* signal is ignored (SIG_IGN) */
#define S_HARD_IGN 4		/* signal is ignored permenantly */
#define S_RESET 5		/* temporary - to reset a hard ignored sig */

extern const char *const signal_names[];

static void signal_entry(int signo)
{
	if (ShellContext::signaled)
		onsig(*ShellContext::signaled, signo);
}

static int decode_signum(ShellContext &sh, std::string_view);
MKINIT dash::ShellResult<void> clear_traps(ShellContext &sh, Node *);

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

dash::ShellResult<int>
trapcmd(ShellContext &sh, int argc, char **argv)
{
	const char *action;
	char **ap;
	int signo;

	TRY_SHV(nextopt(sh, nullstr));
	ap = sh.argptr;
	if (!*ap) {
		for (signo = 0 ; signo < NSIG ; signo++) {
			if (sh.trap[signo]) {
				out1fmt(sh, "trap -- %s %s\n",
					single_quote(sh, sh.trap[signo]->c_str()),
					signal_names[signo]
				);
			}
		}
		return 0;
	}
	if (sh.ptrap)
		TRY_SHV(clear_traps(sh, NULL));
	if (!ap[1] || decode_signum(sh, *ap) >= 0)
		action = nullptr;
	else
		action = *ap++;
	while (*ap) {
		if ((signo = decode_signal(sh, *ap, 0)) < 0) {
			outfmt(sh, sh.out2, "trap: %s: bad trap\n", *ap);
			return 1;
		}
		INTOFF;
		if (sh.trap[signo] && !sh.trap[signo]->empty())
			sh.trapcnt--;
		if (action) {
			if (action[0] == '-' && action[1] == '\0')
				sh.trap[signo].reset();
			else {
				if (*action)
					sh.trapcnt++;
				sh.trap[signo] = std::string(action);
			}
		} else {
			sh.trap[signo].reset();
		}
		if (signo != 0)
			setsignal(sh, signo);
		INTON;
		ap++;
	}
	return 0;
}

dash::ShellResult<void>
clear_traps(ShellContext &sh, Node *n)
{
	int simplecmd;

	simplecmd = issimplecmd(n, TRAPCMD->name);

	INTOFF;
	for (int i = 0; i < NSIG; i++) {
		if (sh.trap[i] && !sh.trap[i]->empty()) {
			auto otp = std::move(sh.trap[i]);
			sh.trap[i].reset();
			if (i != 0)
				setsignal(sh, i);
			if (simplecmd)
				sh.trap[i] = std::move(otp);
		}
	}
	sh.trapcnt = 0;
	sh.ptrap = simplecmd;
	INTON;
	return {};
}

void
setsignal(ShellContext &sh, int signo)
{
	int action;
	int lvforked;
	char *t, tsig;
	struct sigaction act;

	lvforked = sh.vforked;

	if (!sh.trap[signo])
		action = S_DFL;
	else if (!sh.trap[signo]->empty())
		action = S_CATCH;
	else
		action = S_IGN;
	if (rootshell && action == S_DFL && !lvforked) {
		switch (signo) {
		case SIGINT:
			/* Catch so INTOFF/INTON can defer; onint re-raises. */
			if (sh.minusc || sflag == 0)
				action = S_CATCH;
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
		if (sigaction(signo, 0, &act) == -1) {
			return;
		}
		if (act.sa_handler == SIG_IGN) {
			if (mflag && (signo == SIGTSTP ||
			     signo == SIGTTIN || signo == SIGTTOU)) {
				tsig = S_IGN;
			} else
				tsig = S_HARD_IGN;
		} else {
			tsig = S_RESET;
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

void
ignoresig(ShellContext &sh, int signo)
{
	if (sh.sigmode[signo - 1] == S_IGN || sh.sigmode[signo - 1] == S_HARD_IGN)
		return;
	signal(signo, SIG_IGN);
	if (!sh.vforked)
		sh.sigmode[signo - 1] = S_IGN;
}

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

dash::ShellResult<void>
dotrap(ShellContext &sh)
{
	char *q;
	int i;
	int status, last_status;

	if (!sh.pending_sig)
		return {};

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

		if (!sh.trap[i + 1])
			continue;
		auto r = sh.eval(*sh.trap[i + 1]);
		if (!r) {
			sh.savestatus = last_status;
			return tl::unexpected(r.error());
		}
		if (sh.evalskip != SKIPFUNC)
			sh.exitstatus = status;
	}

	sh.savestatus = last_status;
	return {};
}

void
exitshell(ShellContext &sh)
{
	sh.savestatus = sh.exitstatus;
	TRACE(("pid %d, exitshell(%d)\n", getpid(), sh.savestatus));
	if (sh.trap[0]) {
		std::string exittrap = std::move(*sh.trap[0]);
		sh.trap[0].reset();
		if (!sh.ptrap) {
			sh.evalskip = 0;
			/* EXIT trap failure must not skip teardown. */
			sh.eval(exittrap);
			sh.evalskip = SKIPFUNCDEF;
		}
	}
	exitreset(sh);
	postexitreset(sh);
	setjobctl(sh, 0);
	flushall(sh);
	_exit(sh.exitstatus);
	/* NOTREACHED */
}

static int
decode_signum(ShellContext &sh, std::string_view string)
{
	int signo = -1;

	if (is_number(string)) {
		std::string tmp(string);

		signo = std::atoi(tmp.c_str());
		if (signo >= NSIG)
			signo = -1;
	}

	return signo;
}

int
decode_signal(ShellContext &sh, std::string_view string, int minsig)
{
	int signo;

	signo = decode_signum(sh, string);
	if (signo >= 0)
		return signo;

	std::string tmp(string);
	for (signo = minsig; signo < NSIG; signo++) {
		if (!strcasecmp(tmp.c_str(), signal_names[signo])) {
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
