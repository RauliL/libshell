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

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif
#include <sys/types.h>
#include <sys/param.h>
#ifdef BSD
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif
#include <sys/ioctl.h>

#include "shell.h"
#include "shell_context.h"
#if JOBS
#include <termios.h>
#undef CEOF			/* syntax.h redefines this */
#endif
#include "builtins.h"
#include "exec.h"
#include "eval.h"
#include "init.h"
#include "redir.h"
#include "show.h"
#include "main.h"
#include "parser.h"
#include "nodes.h"
#include "jobs.h"
#include "options.h"
#include "trap.h"
#include "syntax.h"
#include "input.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "system.h"

/* mode flags for set_curjob */
#define CUR_DELETE 2
#define CUR_RUNNING 1
#define CUR_STOPPED 0

/* mode flags for dowait */
#define DOWAIT_NONBLOCK 0
#define DOWAIT_BLOCK 1
#define DOWAIT_WAITCMD 2
#define DOWAIT_WAITCMD_ALL 4

/* array of jobs */

/* size of array */

/* pid of last background process */


#if JOBS
/* pgrp of shell on invocation */

/* control terminal */

#endif

/* current job */


/* Set if we are in the sh.vforked child */


static void set_curjob(ShellContext &sh, struct job *, unsigned);
static int jobno(ShellContext &sh, const struct job *);
static int sprint_status(ShellContext &sh, char *, int, int);
static void freejob(ShellContext &sh, struct job *);
static dash::ShellResult<struct job *> getjob(ShellContext &sh, const char *, int);
static struct job * growjobtab(ShellContext &sh);
static void forkchild(ShellContext &sh, struct job *, union node *, int);
static dash::ShellResult<void> forkparent(ShellContext &sh, struct job *, union node *, int, pid_t);
static int dowait(ShellContext &sh, int, struct job *);
#ifdef SYSV
STATIC int onsigchild(void);
#endif
static int waitproc(ShellContext &sh, int, int *);
static char *commandtext(ShellContext &sh, union node *);
static void cmdtxt(ShellContext &sh, union node *);
static void cmdlist(ShellContext &sh, union node *, int);
static void cmdputs(ShellContext &sh, const char *);
static void showpipe(ShellContext &sh, struct job *, struct output *);
static int getstatus(ShellContext &sh, struct job *);

#if JOBS
static dash::ShellResult<int> restartjob(ShellContext &sh, struct job *, int);
static dash::ShellResult<void> xtcsetpgrp(ShellContext &sh, int, pid_t);
#endif

static void
set_curjob(ShellContext &sh, struct job *jp, unsigned mode)
{
	struct job *jp1;
	struct job **jpp, **curp;

	/* first remove from list */
	jpp = curp = &sh.curjob;
	do {
		jp1 = *jpp;
		if (jp1 == jp)
			break;
		jpp = &jp1->prev_job;
	} while (1);
	*jpp = jp1->prev_job;

	/* Then re-insert in correct position */
	jpp = curp;
	switch (mode) {
	default:
#ifdef DEBUG
		abort();
#endif
	case CUR_DELETE:
		/* job being deleted */
		break;
	case CUR_RUNNING:
		/* newly created job or backgrounded job,
		   put after all stopped jobs. */
		do {
			jp1 = *jpp;
			if (!JOBS || !jp1 || jp1->state != JOBSTOPPED)
				break;
			jpp = &jp1->prev_job;
		} while (1);
		/* FALLTHROUGH */
#if JOBS
	case CUR_STOPPED:
#endif
		/* newly stopped job - becomes sh.curjob */
		jp->prev_job = *jpp;
		*jpp = jp;
		break;
	}
}

#if JOBS
/*
 * Turn job control on and off.
 *
 * Note:  This code assumes that the third arg to ioctl is a character
 * pointer, which is true on Berkeley systems but not System V.  Since
 * System V doesn't have job control yet, this isn't a problem now.
 *
 * Called with interrupts off.
 */



static dash::ShellResult<void>
xxtcsetpgrp(ShellContext &sh, pid_t pgrp)
{
	int fd = sh.ttyfd;

	if (fd < 0)
		return {};

	TRY_SHV(xtcsetpgrp(sh, fd, pgrp));
	return {};
}

dash::ShellResult<void>
setjobctl(ShellContext &sh, int on)
{
	int pgrp = -1;
	int fd;

	if (on == sh.jobctl || rootshell == 0)
		return {};
	if (on) {
		int ofd;
		ofd = fd = TRY_SH(sh_open(sh, _PATH_TTY, O_RDWR, 1));
		if (fd < 0) {
			fd += 3;
			while (!isatty(fd))
				if (--fd < 0)
					goto out;
		}
		fd = TRY_SH(savefd(sh, fd, ofd));
		do { /* while we are in the background */
			if ((pgrp = tcgetpgrp(fd)) < 0) {
close:
				close(fd);
				fd = -1;
out:
				if (!iflag)
					break;
				sh_warnx(sh, "can't access tty; job control turned off");
				mflag = on = 0;
				return {};
			}
			if (pgrp == getpgrp())
				break;
			if (!iflag)
				goto close;
			killpg(0, SIGTTIN);
		} while (1);
		sh.initialpgrp = pgrp;
		pgrp = sh.rootpid;
	} else {
		/* turning job control off */
		fd = sh.ttyfd;
		pgrp = sh.initialpgrp;
	}

	setsignal(sh, SIGTSTP);
	setsignal(sh, SIGTTOU);
	setsignal(sh, SIGTTIN);
	if (fd >= 0) {
		setpgid(0, pgrp);
		TRY_SHV(xtcsetpgrp(sh, fd, pgrp));

		if (!on) {
			close(fd);
			fd = -1;
		}
	}

	sh.ttyfd = fd;
	sh.jobctl = on;
	return {};
}
#endif


dash::ShellResult<int>
killcmd(ShellContext &sh, int argc, char **argv)
{
	extern const char *const signal_names[];
	int signo = -1;
	int list = 0;
	int i;
	pid_t pid;
	struct job *jp;

	if (argc <= 1) {
usage:
		return sh_error(sh, "Usage: kill [-s sigspec | -signum | -sigspec] [pid | job]... or\n"
"kill -l [exitstatus]"
		);
	}

	if (**++argv == '-') {
		signo = decode_signal(sh, *argv + 1, 1);
		if (signo < 0) {
			int c;

			while ((c = TRY_SH(nextopt(sh, "ls:"))) != '\0')
				switch (c) {
				default:
#ifdef DEBUG
					abort();
#endif
				case 'l':
					list = 1;
					break;
				case 's':
					signo = decode_signal(sh, sh.optionarg, 1);
					if (signo < 0) {
						return sh_error(sh, "invalid signal number or name: %s",
							sh.optionarg
						);
					}
		                        break;
				}
			argv = sh.argptr;
		} else
			argv++;
	}

	if (!list && signo < 0)
		signo = SIGTERM;

	if ((signo < 0 || !*argv) ^ list) {
		goto usage;
	}

	if (list) {
		struct output *out;

		out = sh.out1;
		if (!*argv) {
			outstr(sh, "0\n", out);
			for (i = 1; i < NSIG; i++) {
				outfmt(sh, out, snlfmt, signal_names[i]);
			}
			return 0;
		}
		signo = TRY_SH(number(sh, *argv));
		if (signo > 128)
			signo -= 128;
		if (0 < signo && signo < NSIG)
			outfmt(sh, out, snlfmt, signal_names[signo]);
		else
			return sh_error(sh, "invalid signal number or exit status: %s",
				 *argv);
		return 0;
	}

	i = 0;
	do {
		if (**argv == '%') {
			jp = TRY_SH(getjob(sh, *argv, 0));
			pid = -jp->ps[0].pid;
		} else
			pid = **argv == '-' ?
				-TRY_SH(number(sh, *argv + 1)) : TRY_SH(number(sh, *argv));
		if (kill(pid, signo) != 0) {
			sh_warnx(sh, snlfmt, strerror(errno));
			i = 1;
		}
	} while (*++argv);

	return i;
}

static int
jobno(ShellContext &sh, const struct job *jp)
{
	return jp - sh.jobtab + 1;
}

#if JOBS
dash::ShellResult<int>
fgcmd(ShellContext &sh, int argc, char **argv)
{
	struct job *jp;
	struct output *out;
	int mode;
	int retval;

	mode = (**argv == 'f') ? FORK_FG : FORK_BG;
	TRY_SHV(nextopt(sh, nullstr));
	argv = sh.argptr;
	out = sh.out1;
	do {
		jp = TRY_SH(getjob(sh, *argv, 1));
		if (mode == FORK_BG) {
			set_curjob(sh, jp, CUR_RUNNING);
			outfmt(sh, out, "[%d] ", jobno(sh, jp));
		}
		outstr(sh, jp->ps->cmd, out);
		showpipe(sh, jp, out);
		retval = TRY_SH(restartjob(sh, jp, mode));
	} while (*argv && *++argv);
	return retval;
}

dash::ShellResult<int>
bgcmd(ShellContext &sh, int argc, char **argv)
#ifdef HAVE_ALIAS_ATTRIBUTE
	__attribute__((__alias__("fgcmd")));
#else
{
	return fgcmd(sh, argc, argv);
}
#endif


static dash::ShellResult<int>
restartjob(ShellContext &sh, struct job *jp, int mode)
{
	struct procstat *ps;
	int i;
	int status;
	pid_t pgid;

	INTOFF;
	if (jp->state == JOBDONE)
		goto out;
	jp->state = JOBRUNNING;
	pgid = jp->ps->pid;
	if (mode == FORK_FG)
		TRY_SHV(xxtcsetpgrp(sh, pgid));
	killpg(pgid, SIGCONT);
	ps = jp->ps;
	i = jp->nprocs;
	do {
		if (WIFSTOPPED(ps->status)) {
			ps->status = -1;
		}
	} while (ps++, --i);
out:
	status = (mode == FORK_FG) ? TRY_SH(waitforjob(sh, jp)) : 0;
	INTON;
	return status;
}
#endif

static int
sprint_status(ShellContext &sh, char *os, int status, int sigonly)
{
	char *s = os;
	int st;

	st = WEXITSTATUS(status);
	if (!WIFEXITED(status)) {
#if JOBS
		st = WSTOPSIG(status);
		if (!WIFSTOPPED(status))
#endif
			st = WTERMSIG(status);
		if (sigonly) {
			if (st == SIGINT || st == SIGPIPE)
				goto out;
#if JOBS
			if (WIFSTOPPED(status))
				goto out;
#endif
		}
		s = stpncpy(s, strsignal(st), 32);
#ifdef WCOREDUMP
		if (WCOREDUMP(status)) {
			s = stpcpy(s, " (core dumped)");
		}
#endif
	} else if (!sigonly) {
		if (st)
			s += fmtstr(sh, s, 16, "Done(%d)", st);
		else
			s = stpcpy(s, "Done");
	}

out:
	return s - os;
}

static void
showjob(ShellContext &sh, struct output *out, struct job *jp, int mode)
{
	struct procstat *ps;
	struct procstat *psend;
	int col;
	int indent;
	char s[80];

	ps = jp->ps;

	if (mode & SHOW_PGID) {
		/* just sh.output process (group) id of pipeline */
		outfmt(sh, out, "%d\n", ps->pid);
		return;
	}

	col = fmtstr(sh, s, 16, "[%d]   ", jobno(sh, jp));
	indent = col;

	if (jp == sh.curjob)
		s[col - 2] = '+';
	else if (sh.curjob && jp == sh.curjob->prev_job)
		s[col - 2] = '-';

	if (mode & SHOW_PID)
		col += fmtstr(sh, s + col, 16, "%d ", ps->pid);

	psend = ps + jp->nprocs;

	if (jp->state == JOBRUNNING) {
		scopy("Running", s + col);
		col += strlen("Running");
	} else {
		int status = psend[-1].status;
#if JOBS
		if (jp->state == JOBSTOPPED)
			status = jp->stopstatus;
#endif
		col += sprint_status(sh, s + col, status, 0);
	}

	goto start;

	do {
		/* for each process */
		col = fmtstr(sh, s, 48, " |\n%*c%d ", indent, ' ', ps->pid) - 3;

start:
		outfmt(sh, out, "%s%*c%s",
			s, 33 - col >= 0 ? 33 - col : 0, ' ', ps->cmd
		);
		if (!(mode & SHOW_PID)) {
			showpipe(sh, jp, out);
			break;
		}
		if (++ps == psend) {
			outcslow(sh, '\n', out);
			break;
		}
	} while (1);

	jp->changed = 0;

	if (jp->state == JOBDONE) {
		TRACE(("showjob: freeing job %d\n", jobno(sh, jp)));
		freejob(sh, jp);
	}
}


dash::ShellResult<int>
jobscmd(ShellContext &sh, int argc, char **argv)
{
	int mode, m;
	struct output *out;

	mode = 0;
	while ((m = TRY_SH(nextopt(sh, "lp"))))
		if (m == 'l')
			mode = SHOW_PID;
		else
			mode = SHOW_PGID;

	out = sh.out1;
	argv = sh.argptr;
	if (*argv)
		do
			showjob(sh, out, TRY_SH(getjob(sh, *argv,0)), mode);
		while (*++argv);
	else
		showjobs(sh, out, mode);

	return 0;
}


/*
 * Print a list of jobs.  If "change" is nonzero, only print jobs whose
 * statuses have sh.changed since the last call to showjobs.
 */

void
showjobs(ShellContext &sh, struct output *out, int mode)
{
	struct job *jp;

	TRACE(("showjobs(%x) called\n", mode));

	/* If not even one job sh.changed, there is nothing to do */
	dowait(sh, DOWAIT_NONBLOCK, NULL);

	for (jp = sh.curjob; jp; jp = jp->prev_job) {
		if (!(mode & SHOW_CHANGED) || jp->changed)
			showjob(sh, out, jp, mode);
	}
}

/*
 * Mark a job structure as unused.
 */

static void
freejob(ShellContext &sh, struct job *jp)
{
	struct procstat *ps;
	int i;

	INTOFF;
	for (i = jp->nprocs, ps = jp->ps ; --i >= 0 ; ps++) {
		if (ps->cmd != nullstr)
			ckfree(ps->cmd);
	}
	if (jp->ps != &jp->ps0)
		ckfree(jp->ps);
	jp->used = 0;
	set_curjob(sh, jp, CUR_DELETE);
	INTON_DEFER;
}



dash::ShellResult<int>
waitcmd(ShellContext &sh, int argc, char **argv)
{
	struct job *job;
	int retval;
	struct job *jp;

	TRY_SHV(nextopt(sh, nullstr));
	retval = 0;

	argv = sh.argptr;
	if (!*argv) {
		/* wait for all jobs */
		for (;;) {
			jp = sh.curjob;
			while (1) {
				if (!jp) {
					/* no running procs */
					goto out;
				}
				if (jp->state == JOBRUNNING)
					break;
				jp->waited = 1;
				jp = jp->prev_job;
			}
			if (!dowait(sh, DOWAIT_WAITCMD_ALL, 0))
				goto sigout;
		}
	}

	retval = 127;
	do {
		if (**argv != '%') {
			pid_t pid = TRY_SH(number(sh, *argv));
			job = sh.curjob;
			goto start;
			do {
				if (job->ps[job->nprocs - 1].pid == pid)
					break;
				job = job->prev_job;
start:
				if (!job)
					goto repeat;
			} while (1);
		} else
			job = TRY_SH(getjob(sh, *argv, 0));
		/* loop until process terminated or stopped */
		if (!dowait(sh, DOWAIT_WAITCMD, job))
			goto sigout;
		job->waited = 1;
		retval = getstatus(sh, job);
repeat:
		;
	} while (*++argv);

out:
	return retval;

sigout:
	retval = 128 + sh.pending_sig;
	goto out;
}



/*
 * Convert a job name to a job structure.
 */

static dash::ShellResult<struct job *>
getjob(ShellContext &sh, const char *name, int getctl)
{
	struct job *jp;
	struct job *found;
	const char *err_msg = "No such job: %s";
	unsigned num;
	int c;
	const char *p;
	char *(*match)(const char *, const char *);

	jp = sh.curjob;
	p = name;
	if (!p)
		goto currentjob;

	if (*p != '%')
		goto err;

	c = *++p;
	if (!c)
		goto currentjob;

	if (!p[1]) {
		if (c == '+' || c == '%') {
currentjob:
			err_msg = "No current job";
			goto check;
		} else if (c == '-') {
			if (jp)
				jp = jp->prev_job;
			err_msg = "No previous job";
check:
			if (!jp)
				goto err;
			goto gotit;
		}
	}

	if (is_number(sh, p)) {
		num = atoi(p);
		if (num > 0 && num <= sh.njobs) {
			jp = sh.jobtab + num - 1;
			if (jp->used)
				goto gotit;
			goto err;
		}
	}

	match = prefix;
	if (*p == '?') {
		match = [](const char *haystack, const char *needle) -> char * {
			return const_cast<char *>(std::strstr(haystack, needle));
		};
		p++;
	}

	found = 0;
	while (jp) {
		if (match(jp->ps[0].cmd, p)) {
			if (found)
				goto err;
			found = jp;
			err_msg = "%s: ambiguous";
		}
		jp = jp->prev_job;
	}

	if (!found)
		goto err;
	jp = found;

gotit:
#if JOBS
	err_msg = "job %s not created under job control";
	if (getctl && jp->jobctl == 0)
		goto err;
#endif
	return jp;
err:
	return sh_error(sh, err_msg, name);
}



/*
 * Return a new job structure.
 * Called with interrupts off.
 */

struct job *
makejob(ShellContext &sh, int nprocs)
{
	struct procstat *ps;
	struct job *jp;
	int i;

	for (i = sh.njobs, jp = sh.jobtab ; ; jp++) {
		if (--i < 0) {
			jp = growjobtab(sh);
			break;
		}
		if (jp->used == 0)
			break;
		if (jp->state != JOBDONE || !jp->waited)
			continue;
		if (sh.jobctl)
			continue;
		freejob(sh, jp);
		break;
	}
	memset(jp, 0, sizeof(*jp));
	ps = &jp->ps0;
	if (nprocs > 1)
		ps = ckmalloc(sh, nprocs * sizeof(*ps));
#if JOBS
	if (sh.jobctl)
		jp->jobctl = 1;
#endif
	jp->prev_job = sh.curjob;
	sh.curjob = jp;
	jp->used = 1;
	jp->ps = ps;
	TRACE(("makejob(%d) returns %%%d\n", nprocs,
	    jobno(sh, jp)));
	return jp;
}

static struct job *
growjobtab(ShellContext &sh)
{
	size_t len;
	ptrdiff_t offset;
	struct job *jp, *jq;

	len = sh.njobs * sizeof(*jp);
	jq = sh.jobtab;
	jp = ckrealloc(sh, jq, len + 4 * sizeof(*jp));

	offset = (char *)jp - (char *)jq;
	if (offset) {
		/* Relocate pointers */
		size_t l = len;

		jq = (struct job *)((char *)jq + l);
		while (l) {
			l -= sizeof(*jp);
			jq--;
#define joff(p) ((struct job *)((char *)(p) + l))
#define jmove(p) ((p) = reinterpret_cast<std::remove_reference_t<decltype(p)>>( \
	static_cast<void *>(reinterpret_cast<char *>(p) + (offset))))
			if (likely(joff(jp)->ps == &jq->ps0))
				jmove(joff(jp)->ps);
			if (joff(jp)->prev_job)
				jmove(joff(jp)->prev_job);
		}
		if (sh.curjob)
			jmove(sh.curjob);
#undef joff
#undef jmove
	}

	sh.njobs += 4;
	sh.jobtab = jp;
	jp = (struct job *)((char *)jp + len);
	jq = jp + 3;
	do {
		jq->used = 0;
	} while (--jq >= jp);
	return jp;
}


/*
 * Fork off a subshell.  If we are doing job control, give the subshell its
 * own process group.  Jp is a job structure that the job is to be added to.
 * N is the command that will be evaluated by the child.  Both jp and n may
 * be NULL.  The mode parameter can be one of the following:
 *	FORK_FG - Fork off a foreground process.
 *	FORK_BG - Fork off a background process.
 *	FORK_NOJOB - Like FORK_FG, but don't give the process its own
 *		     process group even if job control is on.
 *
 * When job control is turned off, background processes have their standard
 * input redirected to /dev/null (except for the second and later processes
 * in a pipeline).
 *
 * Called with interrupts off.
 */

static void
forkchild(ShellContext &sh, struct job *jp, union node *n, int mode)
{
	int lvforked;
	int oldlvl;

	TRACE(("Child shell %d\n", getpid()));

	oldlvl = sh.shlvl;
	lvforked = sh.vforked;

	if (!lvforked) {
		sh.mypid = 0;
		sh.shlvl++;

		forkreset(sh, mode == FORK_NOJOB ? n : NULL);

#if JOBS
		/* do job control only in root shell */
		sh.jobctl = 0;
#endif
	}

#if JOBS
	if (mode != FORK_NOJOB && jp->jobctl && !oldlvl) {
		pid_t pgrp;

		if (jp->nprocs == 0)
			sh.mypid = pgrp = getpid();
		else
			pgrp = jp->ps[0].pid;
		/* This can fail because we are doing it in the parent also */
		(void)setpgid(0, pgrp);
		if (mode == FORK_FG)
			(void)xxtcsetpgrp(sh, pgrp);
		setsignal(sh, SIGTSTP);
		setsignal(sh, SIGTTOU);
	} else
#endif
	if (mode == FORK_BG) {
		ignoresig(sh, SIGINT);
		ignoresig(sh, SIGQUIT);
		if (jp->nprocs == 0) {
			close(0);
			(void)sh_open(sh, _PATH_DEVNULL, O_RDONLY, 0);
			/* Should call reset_input here, but it's harmless
			 * for now.
			 */
		}
	}

	if (lvforked)
		return;

	if (!jp)
		return;

	freejob(sh, jp);

	if (issimplecmd(sh, n, JOBSCMD->name))
		return;

	for (jp = sh.curjob; jp; jp = jp->prev_job)
		freejob(sh, jp);
}

static dash::ShellResult<void>
forkparent(ShellContext &sh, struct job *jp, union node *n, int mode, pid_t pid)
{
	if (pid < 0) {
		TRACE(("Fork failed, errno=%d", errno));
		if (jp)
			freejob(sh, jp);
		return sh_error(sh, "Cannot fork");
	}

	TRACE(("In parent shell:  child = %d\n", pid));
	if (!jp)
		return {};
#if JOBS
	if (mode != FORK_NOJOB && jp->jobctl) {
		int pgrp;

		if (jp->nprocs == 0)
			pgrp = pid;
		else
			pgrp = jp->ps[0].pid;
		/* This can fail because we are doing it in the child also */
		(void)setpgid(pid, pgrp);
	}
#endif
	if (mode == FORK_BG) {
		sh.backgndpid = pid;		/* set $! */
		set_curjob(sh, jp, CUR_RUNNING);
	}
	if (jp) {
		struct procstat *ps = &jp->ps[jp->nprocs++];
		ps->pid = pid;
		ps->status = -1;
		ps->cmd = nullstr;
		if (sh.jobctl && n)
			ps->cmd = commandtext(sh, n);
	}
	return {};
}

dash::ShellResult<int>
forkshell(ShellContext &sh, struct job *jp, union node *n, int mode)
{
	int pid;

	TRACE(("forkshell(%%%d, %p, %d) called\n", jobno(sh, jp), n, mode));

	flush_input(sh);

	pid = fork();
	if (pid == 0)
		forkchild(sh, jp, n, mode);
	else
		TRY_SHV(forkparent(sh, jp, n, mode, pid));

	return pid;
}

dash::ShellResult<struct job *>
vforkexec(ShellContext &sh, union node *n, char **argv, const char *path, int idx)
{
	struct job *jp;
	int pid;

	jp = makejob(sh, 1);

	if (!sh.mypid)
		sh.mypid = getpid();
	sh.vforked = sh.mypid;

	pid = vfork();

	if (!pid) {
		forkchild(sh, jp, n, FORK_FG);
		shellexec(sh, argv, path, idx);
		/* NOTREACHED */
	}

	sh.vforked = 0;
	TRY_SHV(forkparent(sh, jp, n, FORK_FG, pid));

	return jp;
}

/*
 * Wait for job to finish.
 *
 * Under job control we have the problem that while a child process is
 * running interrupts generated by the user are sent to the child but not
 * to the shell.  This means that an infinite loop started by an inter-
 * active user may be hard to kill.  With job control turned off, an
 * interactive user may place an interactive program inside a loop.  If
 * the interactive program catches interrupts, the user doesn't want
 * these interrupts to also abort the loop.  The approach we take here
 * is to have the shell ignore interrupt signals while waiting for a
 * forground process to terminate, and then send itself an interrupt
 * signal if the child process was terminated by an interrupt signal.
 * Unfortunately, some programs want to do a bit of cleanup and then
 * exit on interrupt; unless these processes terminate themselves by
 * sending a signal to themselves (instead of calling exit) they will
 * confuse this approach.
 *
 * Called with interrupts off.
 */

dash::ShellResult<int>
waitforjob(ShellContext &sh, struct job *jp)
{
	int st;

	TRACE(("waitforjob(%%%d) called\n", jp ? jobno(sh, jp) : 0));
	dowait(sh, jp ? DOWAIT_BLOCK : DOWAIT_NONBLOCK, jp);
	if (!jp)
		return sh.exitstatus;

	st = getstatus(sh, jp);
#if JOBS
	if (jp->jobctl) {
		TRY_SHV(xxtcsetpgrp(sh, sh.rootpid));
		/*
		 * This is truly gross.
		 * If we're doing job control, then we did a TIOCSPGRP which
		 * caused us (the shell) to no longer be in the controlling
		 * session -- so we wouldn't have seen any ^C/SIGINT.  So, we
		 * intuit from the subprocess exit status whether a SIGINT
		 * occurred, and if so interrupt ourselves.  Yuck.  - mycroft
		 */
		if (jp->sigint)
			raise(SIGINT);
	}
#endif
	if (! JOBS || jp->state == JOBDONE)
		freejob(sh, jp);
	return st;
}



/*
 * Wait for a process to terminate.
 */

static int
waitone(ShellContext &sh, int block, struct job *job)
{
	int pid;
	int status;
	struct job *jp;
	struct job *thisjob = NULL;
	int state;

	INTOFF;
	TRACE(("dowait(%d) called\n", block));
	pid = waitproc(sh, block, &status);
	TRACE(("wait returns pid %d, status=%d\n", pid, status));
	if (pid <= 0)
		goto out;

	for (jp = sh.curjob; jp; jp = jp->prev_job) {
		struct procstat *sp;
		struct procstat *spend;
		if (jp->state == JOBDONE)
			continue;
		state = JOBDONE;
		spend = jp->ps + jp->nprocs;
		sp = jp->ps;
		do {
			if (sp->pid == pid) {
				TRACE(("Job %d: changing status of proc %d from 0x%x to 0x%x\n", jobno(sh, jp), pid, sp->status, status));
				sp->status = status;
				thisjob = jp;
			}
			if (sp->status == -1)
				state = JOBRUNNING;
#if JOBS
			if (state == JOBRUNNING)
				continue;
			if (WIFSTOPPED(sp->status)) {
				jp->stopstatus = sp->status;
				state = JOBSTOPPED;
			}
#endif
		} while (++sp < spend);
		if (thisjob)
			goto gotjob;
	}
	goto out;

gotjob:
	if (state != JOBRUNNING) {
		thisjob->changed = 1;

		if (thisjob->state != state) {
			TRACE(("Job %d: changing state from %d to %d\n", jobno(sh, thisjob), thisjob->state, state));
			thisjob->state = state;
#if JOBS
			if (state == JOBSTOPPED) {
				set_curjob(sh, thisjob, CUR_STOPPED);
			}
#endif
		}
	}

out:
	INTON_DEFER;

	if (thisjob && thisjob == job) {
		char s[48 + 1];
		int len;

		len = sprint_status(sh, s, status, 1);
		if (len) {
			s[len] = '\n';
			s[len + 1] = 0;
			outstr(sh, s, sh.out2);
		}
	}
	return pid;
}

static int
dowait(ShellContext &sh, int block, struct job *jp)
{
	int gotchld = *(volatile int *)&sh.gotsigchld;
	int rpid;
	int pid;

	if (jp && jp->state != JOBRUNNING)
		block = DOWAIT_NONBLOCK;

	if (block == DOWAIT_NONBLOCK && !gotchld)
		return 1;

	rpid = 1;

	do {
		pid = waitone(sh, block, jp);
		rpid &= !!pid;

		block &= ~DOWAIT_WAITCMD_ALL;
		if (!pid || (jp && jp->state != JOBRUNNING))
			block = DOWAIT_NONBLOCK;
	} while (pid >= 0);

	return rpid;
}

/*
 * Do a wait system call.  If block is zero, we return -1 rather than
 * blocking.  If block is DOWAIT_WAITCMD, we return 0 when a signal
 * other than SIGCHLD interrupted the wait.
 *
 * We use sigsuspend in conjunction with a non-blocking wait3 in
 * order to ensure that waitcmd exits promptly upon the reception
 * of a signal.
 *
 * For code paths other than waitcmd we either use a blocking wait3
 * or a non-blocking wait3.  For the latter case the caller of dowait
 * must ensure that it is called over and over again until all dead
 * children have been reaped.  Otherwise zombies may linger.
 */


static int
waitproc(ShellContext &sh, int block, int *status)
{
	sigset_t oldmask;
	int flags = block == DOWAIT_BLOCK ? 0 : WNOHANG;
	int err;

#if JOBS
	if (sh.jobctl)
		flags |= WUNTRACED;
#endif

	do {
		sh.gotsigchld = 0;
		do
#ifdef HAVE_WAIT3
			err = wait3(status, flags, NULL);
#else
			err = waitpid((pid_t)-1, status, flags, NULL);
#endif
		while (err < 0 && errno == EINTR);

		if (err || (err = -!block))
			break;

		sigblockall(sh, &oldmask);

		while (!sh.gotsigchld && !sh.pending_sig)
			sigsuspend(&oldmask);

		sigclearmask();
	} while (sh.gotsigchld);

	return err;
}

/*
 * return 1 if there are stopped jobs, otherwise 0
 */

int
stoppedjobs(ShellContext &sh)
{
	struct job *jp;
	int retval;

	retval = 0;
	if (!JOBS)
		goto out;
	if (sh.job_warning)
		goto out;
	jp = sh.curjob;
	if (jp && jp->state == JOBSTOPPED) {
		out2str("You have stopped jobs.\n");
		sh.job_warning = 2;
		retval++;
	}

out:
	return retval;
}

/*
 * Return a string identifying a command (to be printed by the
 * jobs command).
 */



static char *
commandtext(ShellContext &sh, union node *n)
{
	STARTSTACKSTR(sh.cmdnextc);
	cmdtxt(sh, n);
	TRACE(("commandtext: name %p, end %p\n", stackblock(), sh.cmdnextc));
	/* Dup into a durable C string; jobtab is still POD/realloc-backed. */
	return savestr(sh, stackblock());
}

static void
cmdtxt(ShellContext &sh, union node *n)
{
	union node *np;
	const char *p;
	char s[2];

	if (!n)
		return;
	switch (n->type) {
	default:
#if DEBUG
		abort();
#endif
	case NPIPE:
		for (std::size_t i = 0; i < n->npipe.cmdlist.size(); i++) {
			if (i)
				cmdputs(sh, " | ");
			cmdtxt(sh, n->npipe.cmdlist[i]);
		}
		break;
	case NSEMI:
		p = "; ";
		goto binop;
	case NAND:
		p = " && ";
		goto binop;
	case NOR:
		p = " || ";
binop:
		cmdtxt(sh, n->nbinary.ch1);
		cmdputs(sh, p);
		n = n->nbinary.ch2;
		goto donode;
	case NREDIR:
	case NBACKGND:
		n = n->nredir.n;
		goto donode;
	case NNOT:
		cmdputs(sh, "!");
		n = n->nnot.com;
donode:
		cmdtxt(sh, n);
		break;
	case NIF:
		cmdputs(sh, "if ");
		cmdtxt(sh, n->nif.test);
		cmdputs(sh, "; then ");
		if (n->nif.elsepart) {
			cmdtxt(sh, n->nif.ifpart);
			cmdputs(sh, "; else ");
			n = n->nif.elsepart;
		} else {
			n = n->nif.ifpart;
		}
		p = "; fi";
		goto dotail;
	case NSUBSHELL:
		cmdputs(sh, "(");
		n = n->nredir.n;
		p = ")";
		goto dotail;
	case NWHILE:
		p = "while ";
		goto until;
	case NUNTIL:
		p = "until ";
until:
		cmdputs(sh, p);
		cmdtxt(sh, n->nbinary.ch1);
		n = n->nbinary.ch2;
		p = "; done";
dodo:
		cmdputs(sh, "; do ");
dotail:
		cmdtxt(sh, n);
		goto dotail2;
	case NFOR:
		cmdputs(sh, "for ");
		cmdputs(sh, n->nfor.var.c_str());
		cmdputs(sh, " in ");
		cmdlist(sh, n->nfor.args, 1);
		n = n->nfor.body;
		p = "; done";
		goto dodo;
	case NDEFUN:
		cmdputs(sh, n->ndefun.text.c_str());
		p = "() { ... }";
		goto dotail2;
	case NCMD:
		cmdlist(sh, n->ncmd.args, 1);
		cmdlist(sh, n->ncmd.redirect, 0);
		break;
	case NARG:
		p = n->narg.text.c_str();
dotail2:
		cmdputs(sh, p);
		break;
	case NHERE:
	case NXHERE:
		p = "<<...";
		goto dotail2;
	case NCASE:
		cmdputs(sh, "case ");
		cmdputs(sh, n->ncase.expr->narg.text.c_str());
		cmdputs(sh, " in ");
		for (np = n->ncase.cases; np; np = np->nclist.next) {
			cmdtxt(sh, np->nclist.pattern);
			cmdputs(sh, ") ");
			cmdtxt(sh, np->nclist.body);
			cmdputs(sh, ";; ");
		}
		p = "esac";
		goto dotail2;
	case NTO:
		p = ">";
		goto redir;
	case NCLOBBER:
		p = ">|";
		goto redir;
	case NAPPEND:
		p = ">>";
		goto redir;
	case NTOFD:
		p = ">&";
		goto redir;
	case NFROM:
		p = "<";
		goto redir;
	case NFROMFD:
		p = "<&";
		goto redir;
	case NFROMTO:
		p = "<>";
redir:
		s[0] = n->nfile.fd + '0';
		s[1] = '\0';
		cmdputs(sh, s);
		cmdputs(sh, p);
		if (n->type == NTOFD || n->type == NFROMFD) {
			s[0] = n->ndup.dupfd + '0';
			p = s;
			goto dotail2;
		} else {
			n = n->nfile.fname;
			goto donode;
		}
	}
}

static void
cmdlist(ShellContext &sh, union node *np, int sep)
{
	for (; np; np = np->narg.next) {
		if (!sep)
			cmdputs(sh, spcstr);
		cmdtxt(sh, np);
		if (sep && np->narg.next)
			cmdputs(sh, spcstr);
	}
}


static void
cmdputs(ShellContext &sh, const char *s)
{
	const char *p, *str;
	char cc[2] = " ";
	char *nextc;
	signed char c;
	int subtype = 0;
	int quoted = 0;
	static const char vstype[VSTYPE + 1][4] = {
		"", "}", "-", "+", "?", "=",
		"%", "%%", "#", "##",
	};

	nextc = makestrspace(sh, (strlen(s) + 1) * 8, sh.cmdnextc);
	p = s;
	while ((c = *p++) != 0) {
		str = 0;
		switch (c) {
		case CTLESC:
			c = *p++;
			break;
		case CTLVAR:
			subtype = *p++;
			if ((subtype & VSTYPE) == VSLENGTH)
				str = "${#";
			else
				str = "${";
			goto dostr;
		case CTLENDVAR:
			str = "\"}";
			str += !(quoted & 1);
			quoted >>= 1;
			subtype = 0;
			goto dostr;
		case CTLBACKQ:
			str = "$(...)";
			goto dostr;
		case CTLARI:
			str = "$((";
			goto dostr;
		case CTLENDARI:
			str = "))";
			goto dostr;
		case CTLQUOTEMARK:
			quoted ^= 1;
			c = '"';
			break;
		case '=':
			if (subtype == 0)
				break;
			if ((subtype & VSTYPE) != VSNORMAL)
				quoted <<= 1;
			str = vstype[subtype & VSTYPE];
			if (subtype & VSNUL)
				c = ':';
			else
				goto checkstr;
			break;
		case '\'':
		case '\\':
		case '"':
		case '$':
			/* These can only happen inside quotes */
			cc[0] = c;
			str = cc;
			c = '\\';
			break;
		default:
			break;
		}
		USTPUTC(c, nextc);
checkstr:
		if (!str)
			continue;
dostr:
		while ((c = *str++)) {
			USTPUTC(c, nextc);
		}
	}
	if (quoted & 1) {
		USTPUTC('"', nextc);
	}
	*nextc = 0;
	sh.cmdnextc = nextc;
}


static void
showpipe(ShellContext &sh, struct job *jp, struct output *out)
{
	struct procstat *sp;
	struct procstat *spend;

	spend = jp->ps + jp->nprocs;
	for (sp = jp->ps + 1; sp < spend; sp++)
		outfmt(sh, out, " | %s", sp->cmd);
	outcslow(sh, '\n', out);
	flushall(sh);
}


#if JOBS
static dash::ShellResult<void>
xtcsetpgrp(ShellContext &sh, int fd, pid_t pgrp)
{
	int err;

	sigblockall(sh, NULL);
	err = tcsetpgrp(fd, pgrp);
	sigclearmask();

	if (err)
		return sh_error(sh, "Cannot set tty process group (%s)", strerror(errno));
	return {};
}
#endif


static int
getstatus(ShellContext &sh, struct job *job)
{
	int status;
	int retval;
	struct procstat *ps;

	ps = job->ps + job->nprocs - 1;
	status = ps->status;
	if (pipefail) {
		while (status == 0 && --ps >= job->ps)
			status = ps->status;
	}

	retval = WEXITSTATUS(status);
	if (!WIFEXITED(status)) {
#if JOBS
		retval = WSTOPSIG(status);
		if (!WIFSTOPPED(status))
#endif
		{
			/* XXX: limits number of signals */
			retval = WTERMSIG(status);
#if JOBS
			if (retval == SIGINT)
				job->sigint = 1;
#endif
		}
		retval += 128;
	}
	TRACE(("getstatus: job %d, nproc %d, status %x, retval %x\n",
		jobno(sh, job), job->nprocs, status, retval));
	return retval;
}
