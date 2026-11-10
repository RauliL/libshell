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

#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/*
 * Code for dealing with input/sh.output redirection.
 */

#include "error.h"
#include "expand.h"
#include "input.h"
#include "jobs.h"
#include "nodes.h"
#include "output.h"
#include "redir.h"
#include "shell.h"
#include "system.h"
#include <cerrno>
#include <cstddef>
#include <utility>

#define EMPTY -2		/* marks an unused slot in redirtab */
#define CLOSED -1		/* fd opened for redir needs to be closed */

#ifndef PIPE_BUF
# define PIPESIZE 4096		/* amount of buffering in a pipe */
#else
# define PIPESIZE PIPE_BUF
#endif

using namespace shell;

namespace shell {

/* Bit map of currently closed file descriptors. */

namespace {
Result<int> openredirect(Context &sh, Node *);
Result<void> dupredirect(Context &sh, Node *, int);
Result<int> openhere(Context &sh, Node *);

unsigned
update_closed_redirs(Context &sh, int fd, int nfd)
{
	unsigned val = sh.closed_redirs;
	unsigned bit = 1 << fd;

	if (nfd >= 0)
		sh.closed_redirs &= ~bit;
	else
		sh.closed_redirs |= bit;

	return val & bit;
}
} // namespace

/*
 * Process a list of redirection commands.  If the REDIR_PUSH flag is set,
 * old file descriptors are stashed away so that the redirection can be
 * undone by calling popredir.  If the REDIR_BACKQ flag is set, then the
 * standard sh.output, and the standard error if it becomes a duplicate of
 * stdout, is saved in memory.
 */

Result<void>
redirect(Context &sh, const std::vector<Node::ptr> &redir, int flags)
{
	RedirFrame *sv;
	int i;
	int fd;
	int newfd;
	int *p;
	if (redir.empty())
		return {};
	sv = nullptr;
	INTOFF(sh);
	if (likely(flags & REDIR_PUSH))
		sv = &sh.redirlist.back();
	for (const auto &rn : redir) {
		Node *n = rn.get();
		TRY_SH_SET(newfd, openredirect(sh, n));
		if (newfd < -1)
			continue;

		fd = as<RedirList>(n)->fd;
		if (fd == 0)
			reset_input(sh);

		if (sv) {
			int closed;

			p = &sv->renamed[fd];
			i = *p;

			closed = update_closed_redirs(sh, fd, newfd);

			if (likely(i == EMPTY)) {
				i = CLOSED;
				if (fd != newfd && !closed) {
					TRY_SH_SET(i, savefd(sh, fd, fd));
					fd = -1;
				}
			}

			*p = i;
		}

		if (fd == newfd)
			continue;

		TRY_SHV(dupredirect(sh, n, newfd));
	}
	INTON(sh);
	if (flags & REDIR_SAVEFD2 && sv->renamed[2] >= 0)
		sh.preverrout.fd = sv->renamed[2];
	return {};
}

namespace {
tl::unexpected<Error>
sh_open_fail(Context &sh, const std::filesystem::path &pathname, int flags, int e)
{
	const char *word;
	int action;

	word = "open";
	action = E_OPEN;
	if (flags & O_CREAT) {
		word = "create";
		action = E_CREAT;
	}

	return sh_error(sh, "cannot {} {}: {}", word, pathname.string(), errmsg(e, action));
}
} // namespace

Result<int>
sh_open(Context &sh, const std::filesystem::path &pathname, int flags, int mayfail)
{
	int fd;
	int e;

	do {
		fd = open64(pathname.c_str(), flags, 0666);
		e = errno;
	} while (fd < 0 && e == EINTR && !sh.pending_sig);

	if (mayfail || fd >= 0)
		return fd;

	return sh_open_fail(sh, pathname, flags, e);
}

namespace {
Result<int>
openredirect(Context &sh, Node *redir)
{
	struct stat64 sb;
	const char *fname;
	int flags;
	int f;

	switch (redir->type) {
	case Node::Type::From:
		flags = O_RDONLY;
do_open:
		TRY_SH_SET(f, sh_open(sh, as<FileNode>(redir)->expfname, flags, 0));
		break;
	case Node::Type::FromTo:
		flags = O_RDWR|O_CREAT;
		goto do_open;
	case Node::Type::To:
		/* Take care of noclobber mode. */
		if (sh.opts.noclobber()) {
			fname = as<FileNode>(redir)->expfname.c_str();
			if (stat64(fname, &sb) < 0) {
				flags = O_WRONLY|O_CREAT|O_EXCL;
				goto do_open;
			}

			if (S_ISREG(sb.st_mode))
				goto ecreate;

			TRY_SH_SET(f, sh_open(sh, fname, O_WRONLY, 0));
			if (!fstat64(f, &sb) && S_ISREG(sb.st_mode)) {
				close(f);
				goto ecreate;
			}
			break;
		}
		/* FALLTHROUGH */
	case Node::Type::Clobber:
		flags = O_WRONLY|O_CREAT|O_TRUNC;
		goto do_open;
	case Node::Type::Append:
		flags = O_WRONLY|O_CREAT|O_APPEND;
		goto do_open;
	case Node::Type::ToFd:
	case Node::Type::FromFd:
		f = as<DupNode>(redir)->dupfd;
		if (f == as<RedirList>(redir)->fd)
			f = -2;
		break;
	default:
		/* Fall through to eliminate warning. */
	case Node::Type::Here:
	case Node::Type::XHere:
		TRY_SH_SET(f, openhere(sh, redir));
		break;
	}

	return f;
ecreate:
	return sh_open_fail(sh, fname, O_CREAT, EEXIST);
}

Result<int>
sh_dup2(Context &sh, int ofd, int nfd, int cfd)
{
	if (nfd < 0) {
		nfd = dup(ofd);
		if (nfd >= 0)
			cfd = -1;
	} else
		nfd = dup2(ofd, nfd);
	if (likely(cfd >= 0))
		close(cfd);
	if (nfd < 0)
		return sh_error(sh, "{}: {}", ofd, strerror(errno));

	return nfd;
}

Result<void> dupredirect(Context &sh, Node *redir, int f)
{
	int fd = as<RedirList>(redir)->fd;

	if (redir->type == Node::Type::ToFd || redir->type == Node::Type::FromFd) {
		/* if not ">&-" */
		if (f >= 0) {
			TRY_SHV(sh_dup2(sh, f, fd, -1));
			return {};
		}
		close(fd);
	} else
		TRY_SHV(sh_dup2(sh, f, fd, f));
	return {};
}
} // namespace

Result<int>
sh_pipe(Context &sh, int pip[2], int memfd)
{
	if (memfd) {
		pip[0] = USE_MEMFD_CREATE ? memfd_create("dash", 0) : -1;
		if (pip[0] >= 0) {
			TRY_SH_SET(pip[1], sh_dup2(sh, pip[0], -1, pip[0]));
			return 1;
		}
	}

	if (pipe(pip) < 0)
		return sh_error(sh, "Pipe call failed");

	return 0;
}

/*
 * Handle here documents.  Normally we fork off a process to write the
 * data to a pipe.  If the document is short, we can stuff the data in
 * the pipe without forking.
 */

namespace {
Result<int>
openhere(Context &sh, Node *redir)
{
	size_t len = 0;
	int pip[2];
	int memfd;

	std::string expanded;
	const char *text = as<ArgNode>(as<HereNode>(redir)->doc)->text.data();
	if (redir->type == Node::Type::XHere) {
		TRY_SH_SET(expanded, expandarg_to_string(sh, as<HereNode>(redir)->doc.get(), EXP_QUOTED));
		text = expanded.c_str();
	}

	len = strlen(text);
	TRY_SH_SET(memfd, sh_pipe(sh, pip, len > PIPESIZE));

	if (memfd || len <= PIPESIZE) {
		xwrite(sh, pip[1], text, len);
		lseek(pip[1], 0, SEEK_SET);
		goto out;
	}

	{
		int pid;
		TRY_SH_SET(pid, forkshell(sh, nullptr, nullptr, FORK_NOJOB));
		if (pid == 0) {
			close(pip[0]);
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			signal(SIGHUP, SIG_IGN);
#ifdef SIGTSTP
			signal(SIGTSTP, SIG_IGN);
#endif
			signal(SIGPIPE, SIG_DFL);
			xwrite(sh, pip[1], text, len);
			_exit(0);
		}
	}
out:
	close(pip[1]);
	return pip[0];
}
} // namespace

/*
 * Undo the effects of the last redirection.
 */

Result<void>
popredir(Context &sh, int drop)
{
	INTOFF(sh);
	RedirFrame rp = std::move(sh.redirlist.back());
	sh.redirlist.pop_back();
	for (int i = 0; i < 10; i++) {
		int closed;

		if (rp.renamed[i] == EMPTY)
			continue;

		closed = drop ? 1 : update_closed_redirs(sh, i, rp.renamed[i]);

		switch (rp.renamed[i]) {
		case CLOSED:
			if (!closed)
				close(i);
			break;
		default:
			if (!drop) {
				if (i == 0)
					reset_input(sh);
				dup2(rp.renamed[i], i);
			}
			close(rp.renamed[i]);
			break;
		}
	}
	INTON(sh);

	return {};
}

/*
 * Undo all redirections.  Called on error or interrupt.
 */

void
redir_forkreset(Context &sh)
{
	sh.redirlist.clear();
}

/*
 * Move a file descriptor to > 10.  Invokes sh_error on error unless
 * the original file dscriptor is not open.
 */

Result<int>
savefd(Context &sh, int from, int ofd)
{
	int newfd;
	int err;

#if HAVE_F_DUPFD_CLOEXEC
	newfd = fcntl(from, F_DUPFD_CLOEXEC, 10);
#else
	newfd = fcntl(from, F_DUPFD, 10);
#endif

	err = newfd < 0 ? errno : 0;
	if (err != EBADF) {
		close(ofd);
		if (err)
			return sh_error(sh, "{}: {}", from, strerror(err));
		else if(!HAVE_F_DUPFD_CLOEXEC)
			fcntl(newfd, F_SETFD, FD_CLOEXEC);
	}

	return newfd;
}

Result<int>
redirectsafe(Context &sh, const std::vector<Node::ptr> &redir, int flags)
{
	int err = 0;
	int saveint = sh.suppressint;

	if (auto r = redirect(sh, redir, flags); !r) {
		sh.exception = r.error();
		err = r.error() == Error::Generic ? 2 : static_cast<int>(r.error());
	}
	TRY_SHV(restore_handler_expandarg(sh, err));
	barrier();
	if ((sh.suppressint = saveint) == 0 && sh.intpending)
		return onint(sh);

	return err;
}

Result<void>
unwindredir(Context &sh, std::size_t stop)
{
	while (sh.redirlist.size() > stop)
		TRY_SHV(popredir(sh, 0));
	return {};
}

std::size_t
pushredir(Context &sh, const std::vector<Node::ptr> &redir)
{
	std::size_t stop = sh.redirlist.size();
	if (redir.empty())
		return stop;

	RedirFrame sv;
	sv.renamed.fill(EMPTY);
	sh.redirlist.push_back(sv);
	return stop;
}

} // namespace shell
