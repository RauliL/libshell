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
#include "./config.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif
#include <sys/types.h>
#ifdef BSD
#include <sys/wait.h>
#endif

#include "exec.h"
#include "init.h"
#include "redir.h"
#include "parser.h"
#include "nodes.h"
#include "jobs.h"
#include "options.h"
#include "trap.h"
#include "input.h"
#include "error.h"
#include "mystring.h"
#include "system.h"
#include <cerrno>
#include <utility>

using namespace shell;

namespace shell {

/* mode flags for set_curjob */
#define CUR_DELETE 2
#define CUR_RUNNING 1

/* mode flags for dowait */
#define DOWAIT_NONBLOCK 0
#define DOWAIT_BLOCK 1
#define DOWAIT_WAITCMD 2
#define DOWAIT_WAITCMD_ALL 4

namespace {
void set_curjob(Context &sh, Job *, unsigned);
std::string sprint_status(int);
void freejob(Context &sh, Job *);
Result<Job *> getjob(Context &sh, const char *);
void forkchild(Context &sh, Job *, const Node *, int);
Result<void> forkparent(Context &sh, Job *, const Node *, int, pid_t);
int dowait(Context &sh, int, Job *);
} // namespace

#ifdef SYSV
namespace {
int onsigchild();
} // namespace

#endif
namespace {
int waitproc(Context &sh, int, int *);
std::string commandtext(Context &sh, const Node *);
void cmdtxt(Context &sh, const Node *, std::string &);
void cmdlist(Context &sh, Node *, int, std::string &);
void cmdputs(std::string &, std::string_view);
} // namespace

Job::Process &
Job::add_process(pid_t pid, const std::string &command)
{
	processes.push_back(Process{pid, -1, std::move(command)});

	return processes.back();
}

int
Job::calc_status(bool pipefail_on)
{
	if (processes.empty())
		return 0;

	auto it = processes.end() - 1;
	int status = it->status;
	if (pipefail_on) {
		while (status == 0 && it != processes.begin()) {
			--it;
			status = it->status;
		}
	}

	int retval = WEXITSTATUS(status);
	if (!WIFEXITED(status)) {
		retval = WTERMSIG(status);
		retval += 128;
	}

	return retval;
}

namespace {
void
set_curjob(Context &sh, Job *jp, unsigned mode)
{
	auto &order = sh.job_order;
	order.erase(std::remove(order.begin(), order.end(), jp), order.end());

	switch (mode) {
	default:
	case CUR_DELETE:
		break;
	case CUR_RUNNING:
		order.insert(order.begin(), jp);
		break;
	}
}
} // namespace

Result<int>
killcmd(Context &sh, int argc, char **argv)
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
				case 'l':
					list = 1;
					break;
				case 's':
					signo = decode_signal(sh, sh.bopt.optionarg, 1);
					if (signo < 0) {
						return sh_error(sh, "invalid signal number or name: {}",
							sh.bopt.optionarg);
					}
		                        break;
				}
			argv = sh.bopt.argptr;
		} else
			argv++;
	}

	if (!list && signo < 0)
		signo = SIGTERM;

	if ((signo < 0 || !*argv) ^ list) {
		goto usage;
	}

	if (list) {
		Output *out;

		out = sh.out1;
		if (!*argv) {
			out->write(sh, "0\n");
			for (i = 1; i < NSIG; i++) {
				out->write(sh, std::format("{}\n", sh.find_signal_name(i).value_or("")));
			}
			return 0;
		}
		signo = TRY_SH(number(sh, *argv));
		if (signo > 128)
			signo -= 128;
		if (0 < signo && signo < NSIG)
			out->write(sh, std::format("{}\n", sh.find_signal_name(signo).value_or("")));
		else
			return sh_error(sh, "invalid signal number or exit status: {}",
				 *argv);
		return 0;
	}

	i = 0;
	do {
		if (**argv == '%') {
			jp = TRY_SH(getjob(sh, *argv));
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

/*
 * Text for a signal that killed the job waited on.  SIGINT and SIGPIPE
 * are not reported.  Returns an empty string when there is nothing to print.
 */
namespace {
std::string
sprint_status(int status)
{
	if (WIFEXITED(status))
		return {};

	int st = WTERMSIG(status);
	if (st == SIGINT || st == SIGPIPE)
		return {};

	std::string s(strsignal(st));
#ifdef WCOREDUMP
	if (WCOREDUMP(status))
		s += " (core dumped)";
#endif
	return s;
}
} // namespace

/*
 * Mark a job structure as unused.
 */

namespace {
void
freejob(Context &sh, Job *jp)
{
	INTOFF(sh);
	set_curjob(sh, jp, CUR_DELETE);
	if (jp->slot < sh.jobs.size())
		sh.jobs[jp->slot].reset();
	INTON_DEFER(sh);
}
} // namespace

Result<int>
waitcmd(Context &sh, int argc, char **argv)
{
	Job *job;
	int retval;
	Job *jp;

	TRY_SHV(nextopt(sh, ""));
	retval = 0;

	argv = sh.bopt.argptr;
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
				if (!j->processes.empty() && j->processes.back().pid == pid) {
					job = j;
					break;
				}
			}
			if (!job)
				goto repeat;
		} else
			job = TRY_SH(getjob(sh, *argv));
		/* loop until process terminated */
		if (!dowait(sh, DOWAIT_WAITCMD, job))
			goto sigout;
		job->waited = true;
		retval = job->calc_status(sh.opts.pipefail());
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

namespace {
Result<Job *>
getjob(Context &sh, const char *name)
{
	enum class JobErr {
		NoSuch,
		NoCurrent,
		NoPrevious,
		Ambiguous,
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
		if (j->processes.empty())
			continue;
		if (match(j->processes[0].command.c_str(), p)) {
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
	return jp;
err:
	switch (err_kind) {
	case JobErr::NoCurrent:
		return sh_error(sh, "No current job");
	case JobErr::NoPrevious:
		return sh_error(sh, "No previous job");
	case JobErr::Ambiguous:
		return sh_error(sh, "{}: ambiguous", name);
	case JobErr::NoSuch:
	default:
		return sh_error(sh, "No such job: {}", name ? name : "");
	}
}
} // namespace

/*
 * Return a new job structure.
 * Called with interrupts off.
 */

Job *
makejob(Context &sh, int nprocs)
{
	std::size_t slot = 0;
	for (; slot < sh.jobs.size(); ++slot) {
		Job *jp = sh.jobs[slot].get();
		if (!jp)
			break;
		if (jp->state != Job::State::Done || !jp->waited)
			continue;
		freejob(sh, jp);
		break;
	}
	if (slot == sh.jobs.size())
		sh.jobs.emplace_back();

	auto owned = std::make_unique<Job>();
	owned->slot = slot;
	owned->processes.reserve(static_cast<std::size_t>(nprocs > 0 ? nprocs : 0));
	Job *jp = owned.get();
	sh.jobs[slot] = std::move(owned);
	sh.job_order.insert(sh.job_order.begin(), jp);

	return jp;
}

/*
 * Fork off a subshell.  Jp is a job structure that the job is to be added to.
 * N is the command that will be evaluated by the child.  Both jp and n may
 * be nullptr.  The mode parameter can be one of the following:
 *	FORK_FG - Fork off a foreground process.
 *	FORK_BG - Fork off a background process.
 *	FORK_NOJOB - Like FORK_FG, but keep the command node across fork reset
 *		     (command substitution).
 *
 * Background processes have their standard input redirected to /dev/null
 * (except for the second and later processes in a pipeline).
 *
 * Called with interrupts off.
 */

namespace {
void
forkchild(Context &sh, Job *jp, const Node *n, int mode)
{
	int lvforked = sh.vforked;

	if (!lvforked) {
		sh.mypid = 0;
		sh.shlvl++;

		forkreset(sh, mode == FORK_NOJOB ? n : nullptr);
	}

	if (mode == FORK_BG) {
		ignoresig(sh, SIGINT);
		ignoresig(sh, SIGQUIT);
		if (jp && jp->processes.empty()) {
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

	while (!sh.job_order.empty())
		freejob(sh, sh.job_order.front());
}

Result<void>
forkparent(Context &sh, Job *jp, const Node *n, int mode, pid_t pid)
{
	if (pid < 0) {
		if (jp)
			freejob(sh, jp);
		return sh_error(sh, "Cannot fork");
	}

	if (!jp)
		return {};
	if (mode == FORK_BG) {
		sh.backgndpid = pid;		/* set $! */
		set_curjob(sh, jp, CUR_RUNNING);
	}
	jp->add_process(pid, n ? commandtext(sh, n) : std::string{});
	return {};
}
} // namespace

Result<int>
forkshell(Context &sh, Job *jp, Node *n, int mode)
{
	pid_t pid;

	flush_input(sh);

	pid = fork();
	if (pid == 0)
		forkchild(sh, jp, n, mode);
	else
		TRY_SHV(forkparent(sh, jp, n, mode, pid));

	return pid;
}

Result<Job *>
vforkexec(Context &sh, const Node *n, char **argv, const char *path, int idx)
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
 * Wait for a job to finish and return its status.
 * Called with interrupts off.
 */

Result<int>
waitforjob(Context &sh, Job *jp)
{
	int st;

	dowait(sh, jp ? DOWAIT_BLOCK : DOWAIT_NONBLOCK, jp);
	if (!jp)
		return sh.exitstatus;

	st = jp->calc_status(sh.opts.pipefail());
	if (jp->state == Job::State::Done)
		freejob(sh, jp);
	return st;
}

/*
 * Wait for a process to terminate.
 */

namespace {
int
waitone(Context &sh, int block, Job *job)
{
	int pid;
	int status;
	Job *thisjob = nullptr;
	Job::State state = Job::State::Done;

	INTOFF(sh);
	pid = waitproc(sh, block, &status);
	if (pid <= 0)
		goto out;

	for (Job *jp : sh.job_order) {
		if (jp->state == Job::State::Done)
			continue;
		state = Job::State::Done;
		for (auto &sp : jp->processes) {
			if (sp.pid == pid) {
				sp.status = status;
				thisjob = jp;
			}
			if (sp.status == -1)
				state = Job::State::Running;
		}
		if (thisjob)
			goto gotjob;
	}
	goto out;

gotjob:
	if (state != Job::State::Running && thisjob->state != state) {
		thisjob->state = state;
	}
out:
	INTON_DEFER(sh);

	if (thisjob && thisjob == job) {
		auto s = sprint_status(status);

		if (!s.empty()) {
			s.push_back('\n');
			sh.out2->write(sh, s);
		}
	}
	return pid;
}

int
dowait(Context &sh, int block, Job *jp)
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
} // namespace

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

namespace {
int
waitproc(Context &sh, int block, int *status)
{
	sigset_t oldmask;
	int flags = block == DOWAIT_BLOCK ? 0 : WNOHANG;
	int err;

	do {
		sh.gotsigchld = 0;
		do
#ifdef HAVE_WAIT3
			err = wait3(status, flags, nullptr);
#else
			err = waitpid(static_cast<pid_t>(-1), status, flags, nullptr);
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
} // namespace

/*
 * Return a string identifying a command, so kill and wait can match %job
 * names.
 */

namespace {
std::string
commandtext(Context &sh, const Node *n)
{
	std::string out;
	cmdtxt(sh, n, out);
	return out;
}

void
cmdtxt(Context &sh, const Node *n, std::string &out)
{
	Node *np;
	const char *p;
	char s[2];

	if (!n)
		return;
	switch (n->type) {
	default:
	case Node::Type::Pipe:
		for (std::size_t i = 0; i < as<PipeNode>(n)->cmdlist.size(); i++) {
			if (i)
				cmdputs(out, " | ");
			cmdtxt(sh, as<PipeNode>(n)->cmdlist[i].get(), out);
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
		cmdtxt(sh, as<BinaryNode>(n)->ch1.get(), out);
		cmdputs(out, p);
		n = as<BinaryNode>(n)->ch2.get();
		goto donode;
	case Node::Type::Redir:
	case Node::Type::Backgnd:
		n = as<RedirNode>(n)->n.get();
		goto donode;
	case Node::Type::Not:
		cmdputs(out, "!");
		n = as<NotNode>(n)->com.get();
donode:
		cmdtxt(sh, n, out);
		break;
	case Node::Type::If:
		cmdputs(out, "if ");
		cmdtxt(sh, as<IfNode>(n)->test.get(), out);
		cmdputs(out, "; then ");
		if (as<IfNode>(n)->elsepart) {
			cmdtxt(sh, as<IfNode>(n)->ifpart.get(), out);
			cmdputs(out, "; else ");
			n = as<IfNode>(n)->elsepart.get();
		} else {
			n = as<IfNode>(n)->ifpart.get();
		}
		p = "; fi";
		goto dotail;
	case Node::Type::Subshell:
		cmdputs(out, "(");
		n = as<RedirNode>(n)->n.get();
		p = ")";
		goto dotail;
	case Node::Type::While:
		p = "while ";
		goto until;
	case Node::Type::Until:
		p = "until ";
until:
		cmdputs(out, p);
		cmdtxt(sh, as<BinaryNode>(n)->ch1.get(), out);
		n = as<BinaryNode>(n)->ch2.get();
		p = "; done";
dodo:
		cmdputs(out, "; do ");
dotail:
		cmdtxt(sh, n, out);
		goto dotail2;
	case Node::Type::For:
		cmdputs(out, "for ");
		cmdputs(out, as<ForNode>(n)->var);
		cmdputs(out, " in ");
		cmdlist(sh, as<ForNode>(n)->args.get(), 1, out);
		n = as<ForNode>(n)->body.get();
		p = "; done";
		goto dodo;
	case Node::Type::Defun:
		cmdputs(out, as<DefunNode>(n)->text);
		p = "() { ... }";
		goto dotail2;
	case Node::Type::Cmd:
		cmdlist(sh, as<CommandNode>(n)->args.get(), 1, out);
		cmdlist(sh, as<CommandNode>(n)->redirect.get(), 0, out);
		break;
	case Node::Type::Arg:
		cmdputs(out, as<ArgNode>(n)->text);
		break;
	case Node::Type::Here:
	case Node::Type::XHere:
		p = "<<...";
dotail2:
		cmdputs(out, p);
		break;
	case Node::Type::Case:
		cmdputs(out, "case ");
		cmdputs(out, as<ArgNode>(as<CaseNode>(n)->expr)->text);
		cmdputs(out, " in ");
		for (np = as<CaseNode>(n)->cases.get(); np; np = as<CaseListNode>(np)->next.get()) {
			cmdtxt(sh, as<CaseListNode>(np)->pattern.get(), out);
			cmdputs(out, ") ");
			cmdtxt(sh, as<CaseListNode>(np)->body.get(), out);
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
		s[0] = as<RedirList>(n)->fd + '0';
		s[1] = '\0';
		cmdputs(out, s);
		cmdputs(out, p);
		if (n->type == Node::Type::ToFd || n->type == Node::Type::FromFd) {
			s[0] = as<DupNode>(n)->dupfd + '0';
			p = s;
			goto dotail2;
		} else {
			n = as<FileNode>(n)->fname.get();
			goto donode;
		}
	}
}

void
cmdlist(Context &sh, Node *np, int sep, std::string &out)
{
	for (; np; np = as<ArgNode>(np)->next.get()) {
		if (!sep)
			cmdputs(out, " ");
		cmdtxt(sh, np, out);
		if (sep && as<ArgNode>(np)->next)
			cmdputs(out, " ");
	}
}

void
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
} // namespace

} // namespace shell
