/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>. All rights reserved.
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
 *
 *	@(#)jobs.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h>

#ifndef JOBS
#define JOBS 1
#endif

/* Mode argument to forkshell.  Don't change FORK_FG or FORK_BG. */
#define FORK_FG 0
#define FORK_BG 1
#define FORK_NOJOB 2

struct ShellContext;
struct Node;

/*
 * A job is either a single process or a pipeline.  Process status lives in
 * procs; job numbers (%N) are slot + 1 in ShellContext::jobs.
 */
struct ProcStat {
	pid_t pid = -1;
	int status = -1;	/* last wait() status; -1 = still running */
	std::string cmd;
};

struct Job {
	enum class State : std::uint8_t {
		Running = 0,	/* at least one proc running */
		Done = 1,	/* all procs are completed */
	};

	std::size_t slot = 0;		/* index in ShellContext::jobs */
	std::vector<ProcStat> procs;
	State state = State::Running;
	bool waited = false;		/* true if this entry has been waited for */

	int number() const { return static_cast<int>(slot + 1); }
	pid_t pgid() const { return procs.empty() ? -1 : procs.front().pid; }

	ProcStat &add_process(pid_t pid, std::string cmd = {});
	int calc_status(bool pipefail_on);
};

shell::Result<int> killcmd(ShellContext &, int, char **);
shell::Result<int> waitcmd(ShellContext &, int, char **);
Job *makejob(ShellContext &, int);
shell::Result<int> forkshell(ShellContext &, Job *, Node *, int);
shell::Result<Job *> vforkexec(ShellContext &, Node *, char **, const char *, int);
shell::Result<int> waitforjob(ShellContext &, Job *);
