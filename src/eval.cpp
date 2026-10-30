/*-
 * Copyright (c) 1993
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

#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

/*
 * Evaluate a command.
 */

#include "init.h"
#include "main.h"
#include "shell.h"
#include "shell_context.h"
#include "nodes.h"
#include "syntax.h"
#include "expand.h"
#include "parser.h"
#include "jobs.h"
#include "eval.h"
#include "builtins.h"
#include "options.h"
#include "exec.h"
#include "redir.h"
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "show.h"
#include "mystring.h"
#include "parse_error.h"
#include "nodes_free.h"


			/* set if we are skipping commands */
		/* number of levels to skip */
		/* current loop nesting level */
		/* starting line number of current function, or 0 if not in a function */



			/* exit status of last command */
		/* exit status of backquoted command */
		/* exit status of last command outside traps */

/* Prevent PS4 nesting. */




void evaltreenr(union node *, EvalFlags) __attribute__ ((__noreturn__));
static int evalloop(ShellContext &sh, union node *, EvalFlags);
static int evalfor(ShellContext &sh, union node *, EvalFlags);
static int evalcase(ShellContext &sh, union node *, EvalFlags);
static int evalsubshell(ShellContext &sh, union node *, EvalFlags);
static void expredir(ShellContext &sh, union node *);
static int evalpipe(ShellContext &sh, union node *, EvalFlags);
#ifdef notyet
static int evalcommand(ShellContext &sh, union node *, int, struct backcmd *);
#else
static int evalcommand(ShellContext &sh, union node *, EvalFlags);
#endif
static int evalbltin(ShellContext &sh, const struct builtincmd *, int, char **, EvalFlags);
static int evalfun(ShellContext &sh, struct funcnode *, int, char **, EvalFlags);
static void prehash(ShellContext &sh, union node *);
static int eprintlist(ShellContext &sh, struct output *, struct strlist *, int);
static int bltincmd(ShellContext &sh, int, char **);


STATIC const struct builtincmd bltin = {
	.name = nullstr,
	.builtin = bltincmd,
	.flags = BUILTIN_REGULAR,
};


/*
 * Called to reset things after an sh.exception.
 */

#ifdef mkinit
INCLUDE "eval.h"

EXITRESET {
	if (sh.savestatus >= 0) {
		if (sh.exception == EXEXIT || sh.evalskip == SKIPFUNCDEF)
			sh.exitstatus = sh.savestatus;
		sh.savestatus = -1;
	}
	sh.evalskip = 0;
	sh.loopnest = 0;
	sh.inps4 = 0;

	if (sh.tpip[0] >= 0) {
		close(sh.tpip[0]);
		close(sh.tpip[1]);
	}
}
#endif



/*
 * The eval commmand.
 */

int
evalcmd(ShellContext &sh, int argc, char **argv, EvalFlags flags)
{
        char *p;
        char *concat;
        char **ap;

        if (argc > 1) {
                p = argv[1];
                if (argc > 2) {
                        STARTSTACKSTR(concat);
                        ap = argv + 2;
                        for (;;) {
                        	concat = stputs(sh, p, concat);
                                if ((p = *ap++) == NULL)
                                        break;
                                STPUTC(' ', concat);
                        }
                        STPUTC('\0', concat);
                        p = grabstackstr(concat);
                }
                return evalstring(sh, p, flags & EvalFlag::tested);
        }
        return 0;
}


/*
 * Execute a command or commands contained in a string.
 */

int
evalstring(ShellContext &sh, char *s, EvalFlags flags)
{
	union node *n;
	int status;

	s = savestr(sh, s);
	setinputstring(sh, s);

	status = 0;
	try {
		for (; (n = parsecmd(sh, 0)) != NEOF; clear_stack_temps(sh)) {
			int i;

			EvalFlags child = flags;
			if (!parser_eof(sh))
				child.reset(EvalFlag::exit);
			i = evaltree(sh, n, child);
			if (n)
				status = i;
			if (n && n != NEOF)
				freenode(n);

			if (sh.evalskip)
				break;
		}
		clear_stack_temps(sh);
		popfile(sh);
		ckfree(s);
		return status;
	} catch (const dash::ParseError &e) {
		report_parse_error(sh, e);
		clear_stack_temps(sh);
		popfile(sh);
		ckfree(s);
		return sh.exitstatus;
	} catch (const dash::ShellError &e) {
		sh.exception = e.code();
		clear_stack_temps(sh);
		popfile(sh);
		ckfree(s);
		if (e.code() == EXERROR)
			return sh.exitstatus;
		throw;
	}
}



/*
 * Evaluate a parse tree.  The value is left in the global variable
 * sh.exitstatus.
 */

int
evaltree(ShellContext &sh, union node *n, EvalFlags flags)
{
	bool checkexit = false;
	int (*evalfn)(ShellContext &, union node *, EvalFlags);
	unsigned isor;
	int status = 0;

	if (nflag)
		goto out;

	if (n == NULL) {
		TRACE(("evaltree(NULL) called\n"));
		goto out;
	}

	dotrap(sh);

	TRACE(("pid %d, evaltree(%p: %d, %lu) called\n",
	    getpid(), n, n->type, flags.to_ulong()));
	switch (n->type) {
	default:
#ifdef DEBUG
		out1fmt("Node type = %d\n", n->type);
#ifndef USE_GLIBC_STDIO
		flushout(sh, sh.out1);
#endif
		break;
#endif
	case NNOT:
		status = evaltree(sh, n->nnot.com, EvalFlag::tested);
		if (!sh.evalskip)
			status = !status;
		break;
	case NREDIR:
		sh.errlinno = sh.lineno = n->nredir.linno;
		if (sh.funcline)
			sh.lineno -= sh.funcline - 1;
		expredir(sh, n->nredir.redirect);
		pushredir(sh, n->nredir.redirect);
		status = redirectsafe(sh, n->nredir.redirect, REDIR_PUSH);
		if (status)
			checkexit = true;
		else
			status = evaltree(sh, n->nredir.n, flags & EvalFlag::tested);
		if (n->nredir.redirect)
			popredir(sh, 0);
		break;
	case NCMD:
		evalfn = evalcommand;
checkexit:
		checkexit = true;
		goto calleval;
	case NFOR:
		evalfn = evalfor;
		goto calleval;
	case NWHILE:
	case NUNTIL:
		evalfn = evalloop;
		goto calleval;
	case NSUBSHELL:
	case NBACKGND:
		evalfn = evalsubshell;
		goto checkexit;
	case NPIPE:
		evalfn = evalpipe;
		goto checkexit;
	case NCASE:
		evalfn = evalcase;
		goto calleval;
	case NAND:
	case NOR:
	case NSEMI: {
#if NAND + 1 != NOR
#error NAND + 1 != NOR
#endif
#if NOR + 1 != NSEMI
#error NOR + 1 != NSEMI
#endif
		isor = n->type - NAND;
		EvalFlags left;
		if (n->type != NSEMI || flags.test(EvalFlag::tested))
			left.set(EvalFlag::tested);
		status = evaltree(sh, n->nbinary.ch1, left);
		if ((!status) == isor || sh.evalskip)
			break;
		n = n->nbinary.ch2;
		goto evaln;
	}
evaln:
		evalfn = evaltree;
calleval:
		status = evalfn(sh, n, flags);
		break;
	case NIF:
		status = evaltree(sh, n->nif.test, EvalFlag::tested);
		if (sh.evalskip)
			break;
		if (!status) {
			n = n->nif.ifpart;
			goto evaln;
		} else if (n->nif.elsepart) {
			n = n->nif.elsepart;
			goto evaln;
		}
		status = 0;
		break;
	case NDEFUN:
		defun(sh, n);
		break;
	}

	sh.exitstatus = status;

out:
	dotrap(sh);

	if (eflag && checkexit && !flags.test(EvalFlag::tested) && status)
		goto exexit;

	if (flags.test(EvalFlag::exit)) {
exexit:
		exraise(sh, EXEND);
	}

	return sh.exitstatus;
}


void
evaltreenr(ShellContext &sh, union node *n, EvalFlags flags)
#ifdef HAVE_ATTRIBUTE_ALIAS
	__attribute__ ((alias("evaltree")));
#else
{
	evaltree(sh, n, flags);
	abort();
}
#endif


static int
skiploop(ShellContext &sh)
{
	int skip = sh.evalskip;

	switch (skip) {
	case 0:
		break;

	case SKIPBREAK:
	case SKIPCONT:
		if (likely(--sh.skipcount <= 0)) {
			sh.evalskip = 0;
			break;
		}

		skip = SKIPBREAK;
		break;
	}

	return skip;
}


static int
evalloop(ShellContext &sh, union node *n, EvalFlags flags)
{
	int skip;
	int status;

	sh.loopnest++;
	status = 0;
	flags &= EvalFlag::tested;
	do {
		int i;

		i = evaltree(sh, n->nbinary.ch1, EvalFlag::tested);
		skip = skiploop(sh);
		if (skip == SKIPFUNC)
			status = i;
		if (skip)
			continue;
		if (n->type != NWHILE)
			i = !i;
		if (i != 0)
			break;
		status = evaltree(sh, n->nbinary.ch2, flags);
		skip = skiploop(sh);
	} while (!(skip & ~SKIPCONT));
	sh.loopnest--;

	return status;
}



static int
evalfor(ShellContext &sh, union node *n, EvalFlags flags)
{
	struct arglist arglist;
	union node *argp;
	struct strlist *sp;
	int status;

	sh.errlinno = sh.lineno = n->nfor.linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	arglist.lastp = &arglist.list;
	for (argp = n->nfor.args ; argp ; argp = argp->narg.next) {
		expandarg(sh, argp, &arglist, EXP_FULL | EXP_TILDE);
	}
	*arglist.lastp = NULL;

	status = 0;
	sh.loopnest++;
	flags &= EvalFlag::tested;
	for (sp = arglist.list ; sp ; sp = sp->next) {
		setvar(sh, n->nfor.var, sp->text, 0);
		status = evaltree(sh, n->nfor.body, flags);
		if (skiploop(sh) & ~SKIPCONT)
			break;
	}
	sh.loopnest--;

	return status;
}



static int
evalcase(ShellContext &sh, union node *n, EvalFlags flags)
{
	union node *cp;
	union node *patp;
	struct arglist arglist;
	int status = 0;

	sh.errlinno = sh.lineno = n->ncase.linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	arglist.lastp = &arglist.list;
	expandarg(sh, n->ncase.expr, &arglist, FNMATCH_IS_ENABLED ? EXP_TILDE :
					   EXP_TILDE | EXP_MBCHAR);
	for (cp = n->ncase.cases ; cp && sh.evalskip == 0 ; cp = cp->nclist.next) {
		for (patp = cp->nclist.pattern ; patp ; patp = patp->narg.next) {
			if (casematch(sh, patp, arglist.list->text)) {
				/* Ensure body is non-empty as otherwise
				 * EvalFlag::exit may prevent us from setting the
				 * exit status.
				 */
				if (sh.evalskip == 0 && cp->nclist.body) {
					status = evaltree(sh, cp->nclist.body,
							  flags);
				}
				goto out;
			}
		}
	}
out:
	return status;
}



/*
 * Kick off a subshell to evaluate a tree.
 */

static int
evalsubshell(ShellContext &sh, union node *n, EvalFlags flags)
{
	struct job *jp;
	int backgnd = (n->type == NBACKGND);
	int status;

	sh.errlinno = sh.lineno = n->nredir.linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	expredir(sh, n->nredir.redirect);
	INTOFF;
	if (!backgnd && flags.test(EvalFlag::exit) && !have_traps(sh)) {
		forkreset(sh, NULL);
		goto nofork;
	}
	jp = makejob(sh, 1);
	if (forkshell(sh, jp, n->nredir.n, backgnd) == 0) {
		flags.set(EvalFlag::exit);
		if (backgnd)
			flags.reset(EvalFlag::tested);
nofork:
		INTON;
		redirect(sh, n->nredir.redirect, 0);
		evaltreenr(sh, n->nredir.n, flags);
		/* never returns */
	}
	status = 0;
	if (! backgnd)
		status = waitforjob(sh, jp);
	INTON;
	return status;
}



/*
 * Compute the names of the files in a redirection list.
 */

static void
expredir(ShellContext &sh, union node *n)
{
	union node *redir;

	for (redir = n ; redir ; redir = redir->nfile.next) {
		struct arglist fn;
		fn.lastp = &fn.list;
		switch (redir->type) {
		case NFROMTO:
		case NFROM:
		case NTO:
		case NCLOBBER:
		case NAPPEND:
			expandarg(sh, redir->nfile.fname, &fn, EXP_TILDE | EXP_REDIR);
			redir->nfile.expfname = fn.list->text;
			break;
		case NFROMFD:
		case NTOFD:
			if (redir->ndup.vname) {
				expandarg(sh, redir->ndup.vname, &fn, EXP_TILDE | EXP_REDIR);
				fixredir(sh, redir, fn.list->text, 1);
			}
			break;
		}
	}
}



/*
 * Evaluate a pipeline.  All the processes in the pipeline are children
 * of the process creating the pipeline.  (This differs from some versions
 * of the shell, which make the last process in a pipeline the parent
 * of all the rest.)
 */

static int
evalpipe(ShellContext &sh, union node *n, EvalFlags flags)
{
	struct job *jp;
	struct nodelist *lp;
	int pipelen;
	int prevfd;
	int pip[2];
	int status = 0;

	TRACE(("evalpipe(0x%lx) called\n", (long)n));
	pipelen = 0;
	for (lp = n->npipe.cmdlist ; lp ; lp = lp->next)
		pipelen++;
	flags.set(EvalFlag::exit);
	INTOFF;
	jp = makejob(sh, pipelen);
	prevfd = -1;
	for (lp = n->npipe.cmdlist ; lp ; lp = lp->next) {
		prehash(sh, lp->n);
		pip[1] = -1;
		if (lp->next) {
			if (pipe(pip) < 0) {
				close(prevfd);
				sh_error(sh, "Pipe call failed");
			}
		}
		if (forkshell(sh, jp, lp->n, n->npipe.backgnd) == 0) {
			INTON;
			if (pip[1] >= 0) {
				close(pip[0]);
			}
			if (prevfd > 0) {
				reset_input(sh);
				dup2(prevfd, 0);
				close(prevfd);
			}
			if (pip[1] > 1) {
				dup2(pip[1], 1);
				close(pip[1]);
			}
			evaltreenr(sh, lp->n, flags);
			/* never returns */
		}
		if (prevfd >= 0)
			close(prevfd);
		prevfd = pip[0];
		close(pip[1]);
	}
	if (n->npipe.backgnd == 0) {
		status = waitforjob(sh, jp);
		TRACE(("evalpipe:  job done exit status %d\n", status));
	}
	INTON;

	return status;
}



/*
 * Execute a command inside back quotes.  If it's a builtin command, we
 * want to save its sh.output in a block obtained from malloc.  Otherwise
 * we fork off a subprocess and get the sh.output of the command via a pipe.
 * Should be called with interrupts off.
 */

void
evalbackcmd(ShellContext &sh, union node *n, struct backcmd *result)
{
	struct job *jp;
	int pip[2];
	int pid;

	result->fd = -1;
	result->buf = NULL;
	result->nleft = 0;
	result->jp = NULL;
	if (n == NULL) {
		goto out;
	}

	sh_pipe(sh, pip, 0);
	sh.tpip[0] = pip[0];
	sh.tpip[1] = pip[1];
	jp = makejob(sh, 1);
	pid = forkshell(sh, jp, n, FORK_NOJOB);
	sh.tpip[0] = -1;
	if (pid == 0) {
		FORCEINTON;
		close(pip[0]);
		if (pip[1] != 1) {
			dup2(pip[1], 1);
			close(pip[1]);
		}
		ifsfree(sh);
		evaltreenr(sh, n, EvalFlag::exit);
		/* NOTREACHED */
	}
	close(pip[1]);
	result->fd = pip[0];
	result->jp = jp;

out:
	TRACE(("evalbackcmd done: fd=%d buf=0x%x nleft=%d jp=0x%x\n",
		result->fd, result->buf, result->nleft, result->jp));
}

static struct strlist *
fill_arglist(ShellContext &sh, struct arglist *arglist,
				    union node **argpp)
{
	struct strlist **lastp = arglist->lastp;
	union node *argp;

	while ((argp = *argpp)) {
		expandarg(sh, argp, arglist, EXP_FULL | EXP_TILDE);
		*argpp = argp->narg.next;
		if (*lastp)
			break;
	}

	return *lastp;
}

static int
parse_command_args(ShellContext &sh, struct arglist *arglist, union node **argpp,
			      const char **path)
{
	struct strlist *sp = arglist->list;
	char *cp, c;

	for (;;) {
		sp = unlikely(sp->next) ? sp->next :
					  fill_arglist(sh, arglist, argpp);
		if (!sp)
			return 0;
		cp = sp->text;
		if (*cp++ != '-')
			break;
		if (!(c = *cp++))
			break;
		if (c == '-' && !*cp) {
			if (likely(!sp->next) && !fill_arglist(sh, arglist, argpp))
				return 0;
			sp = sp->next;
			break;
		}
		do {
			switch (c) {
			case 'p':
				*path = defpath;
				break;
			default:
				/* run 'typecmd' for other options */
				return 0;
			}
		} while ((c = *cp++));
	}

	arglist->list = sp;
	return DO_NOFUNC;
}

/*
 * Execute a simple command.
 */

 int
#ifdef notyet
evalcommand(ShellContext &sh, union node *cmd, int flags, struct backcmd *backcmd)
#else
evalcommand(ShellContext &sh, union node *cmd, EvalFlags flags)
#endif
{
	struct localvar_list *localvar_stop;
	struct parsefile *file_stop;
	struct redirtab *redir_stop;
	union node *argp;
	struct arglist arglist;
	struct arglist varlist;
	char **argv;
	int argc;
	struct strlist *osp;
	struct strlist *sp;
#ifdef notyet
	int pip[2];
#endif
	struct cmdentry cmdentry;
	struct job *jp;
	char *lastarg;
	const char *path;
	int spclbltin;
	int cmd_flag;
	int execcmd;
	int status;
	char **nargv;
	int vflags;
	int vlocal;

	sh.errlinno = sh.lineno = cmd->ncmd.linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	/* First expand the arguments. */
	TRACE(("evalcommand(0x%lx, %d) called\n", (long)cmd, flags));
	file_stop = sh.parsefile;
	sh.back_exitstatus = 0;

	cmdentry.cmdtype = CMDBUILTIN;
	cmdentry.u.cmd = &bltin;
	varlist.lastp = &varlist.list;
	*varlist.lastp = NULL;
	arglist.lastp = &arglist.list;
	*arglist.lastp = NULL;

	cmd_flag = 0;
	execcmd = 0;
	spclbltin = -1;
	vflags = 0;
	vlocal = 0;
	path = NULL;

	argc = 0;
	argp = cmd->ncmd.args;
	if ((osp = fill_arglist(sh, &arglist, &argp))) {
		int pseudovarflag = 0;

		for (;;) {
			find_command(sh, arglist.list->text, &cmdentry,
				     cmd_flag | DO_REGBLTIN, pathval());

			vlocal++;

			/* implement bltin and command here */
			if (cmdentry.cmdtype != CMDBUILTIN)
				break;

			pseudovarflag = cmdentry.u.cmd->flags & BUILTIN_ASSIGN;
			if (likely(spclbltin < 0)) {
				spclbltin =
					cmdentry.u.cmd->flags &
					BUILTIN_SPECIAL
				;
				vlocal = spclbltin ^ BUILTIN_SPECIAL;
			}
			execcmd = cmdentry.u.cmd == EXECCMD;
			if (likely(cmdentry.u.cmd != COMMANDCMD))
				break;

			cmd_flag = parse_command_args(sh, &arglist, &argp, &path);
			if (!cmd_flag)
				break;
		}

		for (; argp; argp = argp->narg.next)
			expandarg(sh, argp, &arglist,
				  pseudovarflag &&
				  isassignment(sh, argp->narg.text) ?
				  EXP_VARTILDE : EXP_FULL | EXP_TILDE);

		for (sp = arglist.list; sp; sp = sp->next)
			argc++;

		if (execcmd && argc > 1)
			vflags = VEXPORT;
	}

	localvar_stop = pushlocalvars(sh, vlocal);

	/* Reserve one extra spot at the front for shellexec. */
	nargv = stalloc(sh, sizeof (char *) * (argc + 2));
	argv = ++nargv;
	for (sp = arglist.list ; sp ; sp = sp->next) {
		TRACE(("evalcommand arg: %s\n", sp->text));
		*nargv++ = sp->text;
	}
	*nargv = NULL;

	lastarg = NULL;
	if (iflag && sh.funcline == 0 && argc > 0)
		lastarg = nargv[-1];

	sh.preverrout.fd = 2;
	expredir(sh, cmd->ncmd.redirect);
	redir_stop = pushredir(sh, cmd->ncmd.redirect);
	status = redirectsafe(sh, cmd->ncmd.redirect, REDIR_PUSH|REDIR_SAVEFD2);

	if (unlikely(status)) {
bail:
		sh.exitstatus = status;

		/* We have a redirection error. */
		if (spclbltin > 0)
			exraise(sh, EXERROR);

		goto out;
	}

	for (argp = cmd->ncmd.assign; argp; argp = argp->narg.next) {
		struct strlist **spp;

		spp = varlist.lastp;
		expandarg(sh, argp, &varlist, EXP_VARTILDE);

		if (vlocal)
			mklocal(sh, (*spp)->text, VEXPORT);
		else
			setvareq(sh, (*spp)->text, vflags);
	}

	/* Print the command if xflag is set. */
	if (xflag && !sh.inps4) {
		struct output *out;
		int sep;

		out = &sh.preverrout;
		sh.inps4 = 1;
		outstr(sh, expandstr(sh, ps4val()), out);
		sh.inps4 = 0;
		sep = 0;
		sep = eprintlist(sh, out, varlist.list, sep);
		eprintlist(sh, out, osp, sep);
		outcslow(sh, '\n', out);
#ifdef FLUSHERR
		flushout(sh, out);
#endif
	}

	/* Now locate the command. */
	if (cmdentry.cmdtype != CMDBUILTIN ||
	    !(cmdentry.u.cmd->flags & BUILTIN_REGULAR)) {
		path = unlikely(path) ? path : pathval();
		find_command(sh, argv[0], &cmdentry, cmd_flag | DO_ERR, path);
	}

	jp = NULL;

	/* Execute the command. */
	switch (cmdentry.cmdtype) {
	case CMDUNKNOWN:
		status = 127;
#ifdef FLUSHERR
		flushout(sh, &sh.errout);
#endif
		goto bail;

	default:
		flush_input(sh);

		/* Fork off a child process if necessary. */
		if (!flags.test(EvalFlag::exit) || have_traps(sh)) {
			INTOFF;
			jp = vforkexec(sh, cmd, argv, path, cmdentry.u.index);
			break;
		}
		shellexec(sh, argv, path, cmdentry.u.index);
		/* NOTREACHED */

	case CMDBUILTIN:
		if (evalbltin(sh, cmdentry.u.cmd, argc, argv, flags) &&
		    !(sh.exception == EXERROR && spclbltin <= 0))
			exraise(sh, sh.exception);
		break;

	case CMDFUNCTION:
		evalfun(sh, cmdentry.u.func, argc, argv, flags);
		break;
	}

	status = waitforjob(sh, jp);
	FORCEINTON;

out:
	if (cmd->ncmd.redirect)
		popredir(sh, execcmd);
	unwindredir(sh, redir_stop);
	unwindfiles(sh, file_stop);
	unwindlocalvars(sh, localvar_stop);
	if (lastarg)
		setvar(sh, "_", lastarg, 0);

	return status;
}

int
evalbltin(ShellContext &sh, const struct builtincmd *cmd, int argc, char **argv, EvalFlags flags)
{
	char *savecmdname;
	int status;
	int i = 0;

	savecmdname = sh.commandname;
	try {
		sh.commandname = argv[0];
		sh.argptr = argv + 1;
		sh.optptr = NULL;			/* initialize nextopt */
		if (cmd == EVALCMD)
			status = evalcmd(sh, argc, argv, flags);
		else
			status = cmd->builtin(sh, argc, argv);
		flushall(sh);
		if (outerr(sh.out1))
			sh_warnx(sh, "%s: I/O error", sh.commandname);
		status |= outerr(sh.out1);
		sh.exitstatus = status;
	} catch (const dash::ShellError &ex) {
		sh.exception = ex.code();
		i = 1;
	}
	freestdout(sh);
	sh.commandname = savecmdname;

	return i;
}

int
evalfun(ShellContext &sh, struct funcnode *func, int argc, char **argv, EvalFlags flags)
{
	struct shparam saveparam = sh.shellparam;
	int savefuncline = sh.funcline;
	int saveloopnest = sh.loopnest;
	bool failed = false;

	try {
		INTOFF;
		sh.shellparam.malloc = 0;
		func->count++;
		sh.funcline = func->n.ndefun.linno;
		sh.loopnest = 0;
		INTON;
		sh.shellparam.nparam = argc - 1;
		sh.shellparam.p = argv + 1;
		sh.shellparam.optind = 1;
		sh.shellparam.optoff = -1;
		evaltree(sh, func->n.ndefun.body, flags & EvalFlag::tested);
	} catch (const dash::ShellError &ex) {
		sh.exception = ex.code();
		failed = true;
	}
	INTOFF;
	sh.loopnest = saveloopnest;
	sh.funcline = savefuncline;
	freefunc(sh, func);
	freeparam(sh, &sh.shellparam);
	sh.shellparam = saveparam;
	INTON;
	sh.evalskip &= ~(SKIPFUNC | SKIPFUNCDEF);
	if (failed)
		exraise(sh, sh.exception);
	return 0;
}


/*
 * Search for a command.  This is called before we fork so that the
 * location of the command will be available in the parent as well as
 * the child.  The check for "goodname" is an overly conservative
 * check that the name will not be subject to expansion.
 */

void
prehash(ShellContext &sh, union node *n)
{
	struct cmdentry entry;

	if (n->type == NCMD && n->ncmd.args)
		if (goodname(sh, n->ncmd.args->narg.text))
			find_command(sh, n->ncmd.args->narg.text, &entry, 0,
				     pathval());
}



/*
 * Builtin commands.  Builtin commands whose functions are closely
 * tied to evaluation are implemented here.
 */

/*
 * No command given.
 */

int
bltincmd(ShellContext &sh, int argc, char **argv)
{
	/*
	 * Preserve sh.exitstatus of a previous possible redirection
	 * as POSIX mandates
	 */
	return sh.back_exitstatus;
}


/*
 * Handle break and continue commands.  Break, continue, and return are
 * all handled by setting the sh.evalskip flag.  The evaluation routines
 * above all check this flag, and if it is set they start skipping
 * commands rather than executing them.  The variable sh.skipcount is
 * the number of loops to break/continue, or the number of function
 * levels to return.  (The latter is always 1.)  It should probably
 * be an error to break out of more loops than exist, but it isn't
 * in the standard shell so we don't make it one here.
 */

int
breakcmd(ShellContext &sh, int argc, char **argv)
{
	int n = argc > 1 ? number(sh, argv[1]) : 1;

	if (n <= 0)
		badnum(sh, argv[1]);
	if (n > sh.loopnest)
		n = sh.loopnest;
	if (n > 0) {
		sh.evalskip = (**argv == 'c')? SKIPCONT : SKIPBREAK;
		sh.skipcount = n;
	}
	return 0;
}


/*
 * The return command.
 */

int
returncmd(ShellContext &sh, int argc, char **argv)
{
	int skip;
	int status;

	/*
	 * If called outside a function, do what ksh does;
	 * skip the rest of the file.
	 */
	if (argv[1]) {
		skip = SKIPFUNC;
		status = number(sh, argv[1]);
	} else {
		skip = SKIPFUNCDEF;
		status = sh.exitstatus;
	}
	sh.evalskip = skip;

	return status;
}


int
falsecmd(ShellContext &sh, int argc, char **argv)
{
	return 1;
}


int
truecmd(ShellContext &sh, int argc, char **argv)
{
	return 0;
}


int
execcmd(ShellContext &sh, int argc, char **argv)
{
	if (argc > 1) {
		iflag = 0;		/* exit on error */
		mflag = 0;
		optschanged(sh);
		flush_input(sh);
		shellexec(sh, argv + 1, pathval(), 0);
	}
	return 0;
}


int
eprintlist(ShellContext &sh, struct output *out, struct strlist *sp, int sep)
{
	while (sp) {
		const char *p;

		p = " %s";
		p += (1 - sep);
		sep |= 1;
		outfmt(sh, out, p, sp->text);
		sp = sp->next;
	}

	return sep;
}
