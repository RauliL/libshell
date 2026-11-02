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

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>

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

static void set_curjob(ShellContext &sh, Job *, unsigned);
static int sprint_status(char *, int, int);
static void freejob(ShellContext &sh, Job *);
static shell::Result<Job *> getjob(ShellContext &sh, const char *, int);
static void forkchild(ShellContext &sh, Job *, Node *, int);
static shell::Result<void> forkparent(ShellContext &sh, Job *, Node *, int, pid_t);
static int dowait(ShellContext &sh, int, Job *);
#ifdef SYSV
STATIC int onsigchild();
#endif
static int waitproc(ShellContext &sh, int, int *);
static std::string commandtext(ShellContext &sh, Node *);
static void cmdtxt(ShellContext &sh, Node *, std::string &);
static void cmdlist(ShellContext &sh, Node *, int, std::string &);
static void cmdputs(std::string &, std::string_view);
static void showjob(ShellContext &sh, struct output *, Job *, int);

#if JOBS
static shell::Result<int> restartjob(ShellContext &sh, Job *, int);
static shell::Result<void> xtcsetpgrp(ShellContext &sh, int, pid_t);
#endif

ProcStat &
Job::add_process(pid_t pid, std::string cmd)
{
	procs.push_back(ProcStat{pid, -1, std::move(cmd)});
	return procs.back();
}

void
Job::clear_stopped()
{
	for (auto &ps : procs) {
		if (WIFSTOPPED(ps.status))
			ps.status = -1;
	}
}

int
Job::calc_status(bool pipefail_on)
{
	if (procs.empty())
		return 0;

	auto it = procs.end() - 1;
	int status = it->status;
	if (pipefail_on) {
		while (status == 0 && it != procs.begin()) {
			--it;
			status = it->status;
		}
	}

	int retval = WEXITSTATUS(status);
	if (!WIFEXITED(status)) {
#if JOBS
		retval = WSTOPSIG(status);
		if (!WIFSTOPPED(status))
#endif
		{
			retval = WTERMSIG(status);
#if JOBS
			if (retval == SIGINT)
				sigint = true;
#endif
		}
		retval += 128;
	}
	TRACE(("getstatus: job %d, nproc %zu, status %x, retval %x\n",
		number(), procs.size(), status, retval));
	return retval;
}

void
Job::show_pipe(ShellContext &sh, struct output *out) const
{
	for (std::size_t i = 1; i < procs.size(); i++)
		outfmt(sh, out, " | %s", procs[i].cmd.c_str());
	outcslow(sh, '\n', out);
	flushall(sh);
}

static void
set_curjob(ShellContext &sh, Job *jp, unsigned mode)
{
	auto &order = sh.job_order;
	order.erase(std::remove(order.begin(), order.end(), jp), order.end());

	switch (mode) {
	default:
#ifdef DEBUG
		abort();
#endif
	case CUR_DELETE:
		break;
	case CUR_RUNNING: {
		/* newly created job or backgrounded job,
		   put after all stopped jobs. */
		auto it = order.begin();
		while (it != order.end()) {
#if JOBS
			if ((*it)->state != Job::State::Stopped)
#endif
				break;
			++it;
		}
		order.insert(it, jp);
		break;
	}
#if JOBS
	case CUR_STOPPED:
		/* newly stopped job - becomes current */
		order.insert(order.begin(), jp);
		break;
#endif
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



static shell::Result<void>
xxtcsetpgrp(ShellContext &sh, pid_t pgrp)
{
	int fd = sh.ttyfd;

	if (fd < 0)
		return {};

	TRY_SHV(xtcsetpgrp(sh, fd, pgrp));
	return {};
}

shell::Result<void>
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


shell::Result<int>
killcmd(ShellContext &sh, int argc, char **argv)
{
	int signo = -1;
	int list = 0;
	int i;
	pid_t pid;
	Job *jp;

	if (argc <= 1) {
usage:
		return sh_error(sh, "Usage: kill [-s sigspec | -signum | -sigspec] [pid | job]... or\n"
"kill -l [exitstatus]");
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
						return sh_error(sh, "invalid signal number or name: {}",
							sh.optionarg);
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
				outfmt(sh, out, snlfmt, sh.find_signal_name(i).value_or("").data());
			}
			return 0;
		}
		signo = TRY_SH(number(sh, *argv));
		if (signo > 128)
			signo -= 128;
		if (0 < signo && signo < NSIG)
			outfmt(sh, out, snlfmt, sh.find_signal_name(signo).value_or("").data());
		else
			return sh_error(sh, "invalid signal number or exit status: {}",
				 *argv);
		return 0;
	}

	i = 0;
	do {
		if (**argv == '%') {
			jp = TRY_SH(getjob(sh, *argv, 0));
			pid = -jp->pgid();
		} else
			pid = **argv == '-' ?
				-TRY_SH(number(sh, *argv + 1)) : TRY_SH(number(sh, *argv));
		if (kill(pid, signo) != 0) {
			sh_warnx(sh, "{}", strerror(errno));
			i = 1;
		}
	} while (*++argv);

	return i;
}

#if JOBS
shell::Result<int>
fgcmd(ShellContext &sh, int argc, char **argv)
{
	Job *jp;
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
			outfmt(sh, out, "[%d] ", jp->number());
		}
		outstr(sh, jp->procs.front().cmd.c_str(), out);
		jp->show_pipe(sh, out);
		retval = TRY_SH(restartjob(sh, jp, mode));
	} while (*argv && *++argv);
	return retval;
}

shell::Result<int>
bgcmd(ShellContext &sh, int argc, char **argv)
#ifdef HAVE_ALIAS_ATTRIBUTE
	__attribute__((__alias__("fgcmd")));
#else
{
	return fgcmd(sh, argc, argv);
}
#endif


static shell::Result<int>
restartjob(ShellContext &sh, Job *jp, int mode)
{
	int status;

	INTOFF;
	if (jp->state == Job::State::Done)
		goto out;
	jp->state = Job::State::Running;
	if (mode == FORK_FG)
		TRY_SHV(xxtcsetpgrp(sh, jp->pgid()));
	killpg(jp->pgid(), SIGCONT);
	jp->clear_stopped();
out:
	status = (mode == FORK_FG) ? TRY_SH(waitforjob(sh, jp)) : 0;
	INTON;
	return status;
}
#endif
static int
sprint_status(char *os, int status, int sigonly)
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
			s = stpcpy(s, std::format("Done({})", st).c_str());
		else
			s = stpcpy(s, "Done");
	}

out:
	return s - os;
}

static void
showjob(ShellContext &sh, struct output *out, Job *jp, int mode)
{
	int col;
	int indent;
	char s[80];
	std::size_t pi = 0;

	if (jp->procs.empty())
		return;

	if (mode & SHOW_PGID) {
		/* just output process (group) id of pipeline */
		outfmt(sh, out, "%d\n", jp->pgid());
		return;
	}

	{
		auto t = std::format("[{}]   ", jp->number());
		col = static_cast<int>(t.size());
		memcpy(s, t.c_str(), t.size() + 1);
	}
	indent = col;

	if (!sh.job_order.empty() && jp == sh.job_order.front())
		s[col - 2] = '+';
	else if (sh.job_order.size() > 1 && jp == sh.job_order[1])
		s[col - 2] = '-';

	if (mode & SHOW_PID) {
		auto t = std::format("{} ", jp->procs[0].pid);
		memcpy(s + col, t.c_str(), t.size() + 1);
		col += static_cast<int>(t.size());
	}

	if (jp->state == Job::State::Running) {
		scopy("Running", s + col);
		col += strlen("Running");
	} else {
		int status = jp->procs.back().status;
#if JOBS
		if (jp->state == Job::State::Stopped)
			status = jp->stopstatus;
#endif
		col += sprint_status(s + col, status, 0);
	}

	goto start;

	do {
		/* for each process */
		{
			auto t = std::format(" |\n{:{}c}{} ", ' ', indent,
				jp->procs[pi].pid);
			memcpy(s, t.c_str(), t.size() + 1);
			col = static_cast<int>(t.size()) - 3;
		}

start:
		outfmt(sh, out, "%s%*c%s",
			s, 33 - col >= 0 ? 33 - col : 0, ' ',
			jp->procs[pi].cmd.c_str()
		);
		if (!(mode & SHOW_PID)) {
			jp->show_pipe(sh, out);
			break;
		}
		if (++pi == jp->procs.size()) {
			outcslow(sh, '\n', out);
			break;
		}
	} while (1);

	jp->changed = false;

	if (jp->state == Job::State::Done) {
		TRACE(("showjob: freeing job %d\n", jp->number()));
		freejob(sh, jp);
	}
}

shell::Result<int>
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
	TRACE(("showjobs(%x) called\n", mode));

	dowait(sh, DOWAIT_NONBLOCK, nullptr);

	/* Copy pointers: showjob may free a job and mutate job_order. */
	std::vector<Job *> snapshot = sh.job_order;
	for (Job *jp : snapshot) {
		if (!jp)
			continue;
		if (jp->slot >= sh.jobs.size() || sh.jobs[jp->slot].get() != jp)
			continue;
		if (!(mode & SHOW_CHANGED) || jp->changed)
			showjob(sh, out, jp, mode);
	}
}

/*
 * Mark a job structure as unused.
 */

static void
freejob(ShellContext &sh, Job *jp)
{
	INTOFF;
	set_curjob(sh, jp, CUR_DELETE);
	if (jp->slot < sh.jobs.size())
		sh.jobs[jp->slot].reset();
	INTON_DEFER;
}



shell::Result<int>
waitcmd(ShellContext &sh, int argc, char **argv)
{
	Job *job;
	int retval;
	Job *jp;

	TRY_SHV(nextopt(sh, nullstr));
	retval = 0;

	argv = sh.argptr;
	if (!*argv) {
		/* wait for all jobs */
		for (;;) {
			jp = nullptr;
			for (Job *j : sh.job_order) {
				if (j->state == Job::State::Running) {
					jp = j;
					break;
				}
				j->waited = true;
			}
			if (!jp)
				goto out;
			if (!dowait(sh, DOWAIT_WAITCMD_ALL, nullptr))
				goto sigout;
		}
	}

	retval = 127;
	do {
		if (**argv != '%') {
			pid_t pid = TRY_SH(number(sh, *argv));
			job = nullptr;
			for (Job *j : sh.job_order) {
				if (!j->procs.empty() && j->procs.back().pid == pid) {
					job = j;
					break;
				}
			}
			if (!job)
				goto repeat;
		} else
			job = TRY_SH(getjob(sh, *argv, 0));
		/* loop until process terminated or stopped */
		if (!dowait(sh, DOWAIT_WAITCMD, job))
			goto sigout;
		job->waited = true;
		retval = job->calc_status(pipefail);
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

static shell::Result<Job *>
getjob(ShellContext &sh, const char *name, int getctl)
{
	enum class JobErr {
		NoSuch,
		NoCurrent,
		NoPrevious,
		Ambiguous,
		NotJobCtl,
	};

	Job *jp;
	Job *found;
	JobErr err_kind = JobErr::NoSuch;
	unsigned num;
	int c;
	const char *p;
	char *(*match)(const char *, const char *);

	jp = sh.job_order.empty() ? nullptr : sh.job_order.front();
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
			err_kind = JobErr::NoCurrent;
			goto check;
		} else if (c == '-') {
			jp = sh.job_order.size() > 1 ? sh.job_order[1] : nullptr;
			err_kind = JobErr::NoPrevious;
check:
			if (!jp)
				goto err;
			goto gotit;
		}
	}

	if (is_number(p)) {
		num = std::atoi(p);
		if (num > 0 && num <= sh.jobs.size()) {
			jp = sh.jobs[num - 1].get();
			if (jp)
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

	found = nullptr;
	for (Job *j : sh.job_order) {
		if (j->procs.empty())
			continue;
		if (match(j->procs[0].cmd.c_str(), p)) {
			if (found) {
				err_kind = JobErr::Ambiguous;
				goto err;
			}
			found = j;
		}
	}

	if (!found)
		goto err;
	jp = found;

gotit:
#if JOBS
	if (getctl && !jp->jobctl) {
		err_kind = JobErr::NotJobCtl;
		goto err;
	}
#endif
	return jp;
err:
	switch (err_kind) {
	case JobErr::NoCurrent:
		return sh_error(sh, "No current job");
	case JobErr::NoPrevious:
		return sh_error(sh, "No previous job");
	case JobErr::Ambiguous:
		return sh_error(sh, "{}: ambiguous", name);
	case JobErr::NotJobCtl:
		return sh_error(sh, "job {} not created under job control", name);
	case JobErr::NoSuch:
	default:
		return sh_error(sh, "No such job: {}", name ? name : "");
	}
}



/*
 * Return a new job structure.
 * Called with interrupts off.
 */

Job *
makejob(ShellContext &sh, int nprocs)
{
	std::size_t slot = 0;
	for (; slot < sh.jobs.size(); ++slot) {
		Job *jp = sh.jobs[slot].get();
		if (!jp)
			break;
		if (jp->state != Job::State::Done || !jp->waited)
			continue;
		if (sh.jobctl)
			continue;
		freejob(sh, jp);
		break;
	}
	if (slot == sh.jobs.size())
		sh.jobs.emplace_back();

	auto owned = std::make_unique<Job>();
	owned->slot = slot;
	owned->procs.reserve(static_cast<std::size_t>(nprocs > 0 ? nprocs : 0));
#if JOBS
	if (sh.jobctl)
		owned->jobctl = true;
#endif
	Job *jp = owned.get();
	sh.jobs[slot] = std::move(owned);
	sh.job_order.insert(sh.job_order.begin(), jp);
	TRACE(("makejob(%d) returns %%%d\n", nprocs, jp->number()));
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
forkchild(ShellContext &sh, Job *jp, Node *n, int mode)
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
	if (mode != FORK_NOJOB && jp && jp->jobctl && !oldlvl) {
		pid_t pgrp;

		if (jp->procs.empty())
			sh.mypid = pgrp = getpid();
		else
			pgrp = jp->pgid();
		/* This can fail because we are doing it in the parent also */
		setpgid(0, pgrp);
		if (mode == FORK_FG)
			xxtcsetpgrp(sh, pgrp);
		setsignal(sh, SIGTSTP);
		setsignal(sh, SIGTTOU);
	} else
#endif
	if (mode == FORK_BG) {
		ignoresig(sh, SIGINT);
		ignoresig(sh, SIGQUIT);
		if (jp && jp->procs.empty()) {
			close(0);
			sh_open(sh, _PATH_DEVNULL, O_RDONLY, 0);
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

	if (issimplecmd(n, builtin_jobs.name))
		return;

	while (!sh.job_order.empty())
		freejob(sh, sh.job_order.front());
}

static shell::Result<void>
forkparent(ShellContext &sh, Job *jp, Node *n, int mode, pid_t pid)
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

		if (jp->procs.empty())
			pgrp = pid;
		else
			pgrp = jp->pgid();
		/* This can fail because we are doing it in the child also */
		setpgid(pid, pgrp);
	}
#endif
	if (mode == FORK_BG) {
		sh.backgndpid = pid;		/* set $! */
		set_curjob(sh, jp, CUR_RUNNING);
	}
	{
		std::string cmd;
		if (sh.jobctl && n)
			cmd = commandtext(sh, n);
		jp->add_process(pid, std::move(cmd));
	}
	return {};
}

shell::Result<int>
forkshell(ShellContext &sh, Job *jp, Node *n, int mode)
{
	int pid;

	TRACE(("forkshell(%%%d, %p, %d) called\n", jp ? jp->number() : 0, n, mode));

	flush_input(sh);

	pid = fork();
	if (pid == 0)
		forkchild(sh, jp, n, mode);
	else
		TRY_SHV(forkparent(sh, jp, n, mode, pid));

	return pid;
}

shell::Result<Job *>
vforkexec(ShellContext &sh, Node *n, char **argv, const char *path, int idx)
{
	Job *jp;
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

shell::Result<int>
waitforjob(ShellContext &sh, Job *jp)
{
	int st;

	TRACE(("waitforjob(%%%d) called\n", jp ? jp->number() : 0));
	dowait(sh, jp ? DOWAIT_BLOCK : DOWAIT_NONBLOCK, jp);
	if (!jp)
		return sh.exitstatus;

	st = jp->calc_status(pipefail);
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
	if (! JOBS || jp->state == Job::State::Done)
		freejob(sh, jp);
	return st;
}



/*
 * Wait for a process to terminate.
 */

static int
waitone(ShellContext &sh, int block, Job *job)
{
	int pid;
	int status;
	Job *thisjob = nullptr;
	Job::State state = Job::State::Done;

	INTOFF;
	TRACE(("dowait(%d) called\n", block));
	pid = waitproc(sh, block, &status);
	TRACE(("wait returns pid %d, status=%d\n", pid, status));
	if (pid <= 0)
		goto out;

	for (Job *jp : sh.job_order) {
		if (jp->state == Job::State::Done)
			continue;
		state = Job::State::Done;
		for (auto &sp : jp->procs) {
			if (sp.pid == pid) {
				TRACE(("Job %d: changing status of proc %d from 0x%x to 0x%x\n",
					jp->number(), pid, sp.status, status));
				sp.status = status;
				thisjob = jp;
			}
			if (sp.status == -1)
				state = Job::State::Running;
#if JOBS
			if (state == Job::State::Running)
				continue;
			if (WIFSTOPPED(sp.status)) {
				jp->stopstatus = sp.status;
				state = Job::State::Stopped;
			}
#endif
		}
		if (thisjob)
			goto gotjob;
	}
	goto out;

gotjob:
	if (state != Job::State::Running) {
		thisjob->changed = true;

		if (thisjob->state != state) {
			TRACE(("Job %d: changing state from %d to %d\n",
				thisjob->number(),
				static_cast<int>(thisjob->state),
				static_cast<int>(state)));
			thisjob->state = state;
#if JOBS
			if (state == Job::State::Stopped) {
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

		len = sprint_status(s, status, 1);
		if (len) {
			s[len] = '\n';
			s[len + 1] = 0;
			outstr(sh, s, sh.out2);
		}
	}
	return pid;
}

static int
dowait(ShellContext &sh, int block, Job *jp)
{
	int gotchld = *(volatile int *)&sh.gotsigchld;
	int rpid;
	int pid;

	if (jp && jp->state != Job::State::Running)
		block = DOWAIT_NONBLOCK;

	if (block == DOWAIT_NONBLOCK && !gotchld)
		return 1;

	rpid = 1;

	do {
		pid = waitone(sh, block, jp);
		rpid &= !!pid;

		block &= ~DOWAIT_WAITCMD_ALL;
		if (!pid || (jp && jp->state != Job::State::Running))
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
	int retval = 0;

	if (!JOBS)
		goto out;
	if (sh.job_warning)
		goto out;
	if (!sh.job_order.empty() &&
	    sh.job_order.front()->state == Job::State::Stopped) {
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



static std::string
commandtext(ShellContext &sh, Node *n)
{
	std::string out;
	cmdtxt(sh, n, out);
	return out;
}

static void
cmdtxt(ShellContext &sh, Node *n, std::string &out)
{
	Node *np;
	const char *p;
	char s[2];

	if (!n)
		return;
	switch (n->type) {
	default:
#if DEBUG
		abort();
#endif
	case Node::Type::Pipe:
		for (std::size_t i = 0; i < as<Node::Pipe>(n)->cmdlist.size(); i++) {
			if (i)
				cmdputs(out, " | ");
			cmdtxt(sh, as<Node::Pipe>(n)->cmdlist[i].get(), out);
		}
		break;
	case Node::Type::Semi:
		p = "; ";
		goto binop;
	case Node::Type::And:
		p = " && ";
		goto binop;
	case Node::Type::Or:
		p = " || ";
binop:
		cmdtxt(sh, as<Node::Binary>(n)->ch1.get(), out);
		cmdputs(out, p);
		n = as<Node::Binary>(n)->ch2.get();
		goto donode;
	case Node::Type::Redir:
	case Node::Type::Backgnd:
		n = as<Node::Redir>(n)->n.get();
		goto donode;
	case Node::Type::Not:
		cmdputs(out, "!");
		n = as<Node::Not>(n)->com.get();
donode:
		cmdtxt(sh, n, out);
		break;
	case Node::Type::If:
		cmdputs(out, "if ");
		cmdtxt(sh, as<Node::If>(n)->test.get(), out);
		cmdputs(out, "; then ");
		if (as<Node::If>(n)->elsepart) {
			cmdtxt(sh, as<Node::If>(n)->ifpart.get(), out);
			cmdputs(out, "; else ");
			n = as<Node::If>(n)->elsepart.get();
		} else {
			n = as<Node::If>(n)->ifpart.get();
		}
		p = "; fi";
		goto dotail;
	case Node::Type::Subshell:
		cmdputs(out, "(");
		n = as<Node::Redir>(n)->n.get();
		p = ")";
		goto dotail;
	case Node::Type::While:
		p = "while ";
		goto until;
	case Node::Type::Until:
		p = "until ";
until:
		cmdputs(out, p);
		cmdtxt(sh, as<Node::Binary>(n)->ch1.get(), out);
		n = as<Node::Binary>(n)->ch2.get();
		p = "; done";
dodo:
		cmdputs(out, "; do ");
dotail:
		cmdtxt(sh, n, out);
		goto dotail2;
	case Node::Type::For:
		cmdputs(out, "for ");
		cmdputs(out, as<Node::For>(n)->var);
		cmdputs(out, " in ");
		cmdlist(sh, as<Node::For>(n)->args.get(), 1, out);
		n = as<Node::For>(n)->body.get();
		p = "; done";
		goto dodo;
	case Node::Type::Defun:
		cmdputs(out, as<Node::Defun>(n)->text);
		p = "() { ... }";
		goto dotail2;
	case Node::Type::Cmd:
		cmdlist(sh, as<Node::Command>(n)->args.get(), 1, out);
		cmdlist(sh, as<Node::Command>(n)->redirect.get(), 0, out);
		break;
	case Node::Type::Arg:
		cmdputs(out, as<Node::Arg>(n)->text);
		break;
	case Node::Type::Here:
	case Node::Type::XHere:
		p = "<<...";
dotail2:
		cmdputs(out, p);
		break;
	case Node::Type::Case:
		cmdputs(out, "case ");
		cmdputs(out, as<Node::Arg>(as<Node::Case>(n)->expr)->text);
		cmdputs(out, " in ");
		for (np = as<Node::Case>(n)->cases.get(); np; np = as<Node::CaseList>(np)->next.get()) {
			cmdtxt(sh, as<Node::CaseList>(np)->pattern.get(), out);
			cmdputs(out, ") ");
			cmdtxt(sh, as<Node::CaseList>(np)->body.get(), out);
			cmdputs(out, ";; ");
		}
		p = "esac";
		goto dotail2;
	case Node::Type::To:
		p = ">";
		goto redir;
	case Node::Type::Clobber:
		p = ">|";
		goto redir;
	case Node::Type::Append:
		p = ">>";
		goto redir;
	case Node::Type::ToFd:
		p = ">&";
		goto redir;
	case Node::Type::From:
		p = "<";
		goto redir;
	case Node::Type::FromFd:
		p = "<&";
		goto redir;
	case Node::Type::FromTo:
		p = "<>";
redir:
		s[0] = as<Node::RedirList>(n)->fd + '0';
		s[1] = '\0';
		cmdputs(out, s);
		cmdputs(out, p);
		if (n->type == Node::Type::ToFd || n->type == Node::Type::FromFd) {
			s[0] = as<Node::Dup>(n)->dupfd + '0';
			p = s;
			goto dotail2;
		} else {
			n = as<Node::File>(n)->fname.get();
			goto donode;
		}
	}
}

static void
cmdlist(ShellContext &sh, Node *np, int sep, std::string &out)
{
	for (; np; np = as<Node::Arg>(np)->next.get()) {
		if (!sep)
			cmdputs(out, spcstr);
		cmdtxt(sh, np, out);
		if (sep && as<Node::Arg>(np)->next)
			cmdputs(out, spcstr);
	}
}


static void
cmdputs(std::string &out, std::string_view s)
{
	const char *p, *end, *str;
	char cc[2] = " ";
	signed char c;
	int subtype = 0;
	int quoted = 0;
	static const char vstype[VSTYPE + 1][4] = {
		"", "}", "-", "+", "?", "=",
		"%", "%%", "#", "##",
	};

	p = s.data();
	end = p + s.size();
	while (p < end) {
		c = *p++;
		str = 0;
		switch (c) {
		case CTLESC:
			if (p < end)
				c = *p++;
			break;
		case CTLVAR:
			if (p < end)
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
		out.push_back(c);
checkstr:
		if (!str)
			continue;
dostr:
		while ((c = *str++)) {
			out.push_back(c);
		}
	}
	if (quoted & 1) {
		out.push_back('"');
	}
}


#if JOBS
static shell::Result<void>
xtcsetpgrp(ShellContext &sh, int fd, pid_t pgrp)
{
	int err;

	sigblockall(sh, NULL);
	err = tcsetpgrp(fd, pgrp);
	sigclearmask();

	if (err)
		return sh_error(sh, "Cannot set tty process group ({})", strerror(errno));
	return {};
}
#endif
