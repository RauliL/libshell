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
#include <stdio.h>	/* defines BUFSIZ */
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * This file implements the input routines used by the parser.
 */

#include "alias.h"
#include "error.h"
#include "eval.h"
#include "input.h"
#include "main.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "redir.h"
#include "shell.h"
#include "shell_context.h"
#include "syntax.h"
#include "system.h"
#include "trap.h"


static void pushfile(ShellContext &sh, InputStack &in);
static void popstring(ShellContext &sh, InputStack &in);
static int preadfd(ShellContext &sh, InputStack &in);
static void setinputfd(ShellContext &sh, InputStack &in, int fd, int push);
static int preadbuffer(ShellContext &sh, InputStack &in);

void
input_reset_interactive(ShellContext &sh)
{
	int c;

	popallfiles(sh, sh.input);

	c = PEOF;
	if (sh.input.top->nextc - sh.input.top->buf > sh.input.top->unget)
		c = sh.input.top->nextc[-sh.input.top->unget - 1];
	while (c != '\n' && c != PEOF && !int_pending(sh))
		c = pgetc(sh, sh.input);
}

void
input_forkreset(ShellContext &sh)
{
	popallfiles(sh, sh.input);
	if (sh.input.current->fd > 0) {
		close(sh.input.current->fd);
		sh.input.current->fd = 0;
	}
	if (sh.input.stdin.pip[0]) {
		close(sh.input.stdin.pip[0]);
		close(sh.input.stdin.pip[1]);
		memset(sh.input.stdin.pip, 0, sizeof(sh.input.stdin.pip));
	}
}

void
input_init(ShellContext &sh, InputStack &in)
{
	struct termios tios;
	int istty;

	istty = tcgetattr(0, &tios) + 1;
	in.stdin_istty = istty;
	if (istty)
		in.stdin.bufferable = tios.c_lflag & ICANON;
	else {
		in.stdin.seekable = lseek(0, 0, SEEK_CUR) + 1;
		in.stdin.bufferable = !!in.stdin.seekable;
	}
}

static bool
stdin_bufferable(ShellContext &sh, InputStack &in)
{
	if (in.stdin_istty < 0)
		input_init(sh, in);

	return in.stdin.bufferable;
}

static void
flush_tee(ShellContext &sh, void *buf, int nr, int pending)
{
	(void)sh;
	while (pending > 0) {
		int err;

		err = read(0, buf, nr > pending ? pending : nr);
		if (err > 0)
			pending -= err;
	}
}

static int
stdin_tee(ShellContext &sh, InputStack &in, void *buf, int nr)
{
	int err;

	if (!in.stdin.pip[0]) {
		if (auto r = sh_pipe(sh, in.stdin.pip, 0); !r) {
			errno = EIO;
			return -1;
		}
		if (in.stdin.pip[0] < 10) {
			auto r = savefd(sh, in.stdin.pip[0], in.stdin.pip[0]);
			if (!r) {
				errno = EIO;
				return -1;
			}
			in.stdin.pip[0] = *r;
		}
		if (in.stdin.pip[1] < 10) {
			auto r = savefd(sh, in.stdin.pip[1], in.stdin.pip[1]);
			if (!r) {
				errno = EIO;
				return -1;
			}
			in.stdin.pip[1] = *r;
		}
	}

	flush_tee(sh, buf, nr, in.stdin.pending);

	if (USE_TEE)
		err = tee(0, in.stdin.pip[1], nr, 0);
	else {
		errno = EINVAL;
		err = -1;
	}
	in.stdin.pending = err;
	return err;
}

static void
release_pending_aliases(ShellContext &sh, parsefile &pf)
{
	INTOFF(sh);
	for (auto &ap : pf.pending_alias_release)
		ap->in_use = false;
	/* Dropping the shared_ptrs may destroy aliases already removed
	 * from ShellContext::aliases via unalias.
	 */
	pf.pending_alias_release.clear();
	INTON_DEFER(sh);
}


/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int __attribute__((noinline))
pgetc(ShellContext &sh, InputStack &in)
{
	int c;

	if (unlikely(!in.current->pending_alias_release.empty()))
		release_pending_aliases(sh, *in.current);

again:
	if (in.current->unget) {
		long unget = -static_cast<long>(static_cast<unsigned>(in.current->unget--));

		return (signed char)in.current->nextc[unget];
	}

nextc:
	if (likely(in.current->nleft > 0)) {
		in.current->nleft--;
		c = (signed char)*in.current->nextc++;
	} else if (unlikely(!in.current->string_stack.empty())) {
		popstring(sh, in);
		/* Alias release must be delayed until the next pgetc
		 * so pgetc_eoa can still observe the alias frame.
		 */
		goto again;
	} else
		c = preadbuffer(sh, in);

	/* delete nul characters */
	if (unlikely(!c)) {
		in.current->nextc = static_cast<char *>(memmove(
			in.current->nextc - 1, in.current->nextc, in.current->nleft));
		goto nextc;
	}

	return c;
}

int
pgetc_eoa(ShellContext &sh, InputStack &in)
{
	/* After the last alias character is consumed, nleft is 0 while
	 * the StringPush frame is still on the stack.
	 */
	return !in.current->string_stack.empty() && in.current->nleft == 0 &&
	       in.current->string_stack.back().ap ? PEOA : pgetc(sh, in);
}

static int
stdin_clear_nonblock(ShellContext &sh)
{
	(void)sh;
	int flags = fcntl(0, F_GETFL, 0);

	if (flags >= 0) {
		flags &=~ O_NONBLOCK;
		flags = fcntl(0, F_SETFL, flags);
	}

	return flags;
}

static int
preadfd(ShellContext &sh, InputStack &in)
{
	char *buf = in.current->buf;
	int fd = in.current->fd;
	bool use_tee;
	int unget;
	int pnr;
	int nr;

	unget = in.current->nextc - buf;
	if (unget > PUNGETC_MAX)
		unget = PUNGETC_MAX;

	memmove(buf, in.current->nextc - unget, unget);
	buf += unget;
	in.current->nextc = buf;

	nr = BUFSIZ;
	use_tee = likely(!fd) && !stdin_bufferable(sh, in);

	pnr = nr;
retry:
	nr = pnr;

	if (likely(use_tee)) {
		nr = stdin_tee(sh, in, buf, nr);
		if (nr >= 0)
			fd = in.stdin.pip[0];
		else if (errno == EINVAL) {
			use_tee = false;
			nr = pnr = 1;
		}
	}

	if (nr > 0)
		nr = read(fd, buf, nr);

	if (nr < 0) {
		if (errno == EINTR && !(in.base.prev && sh.pending_sig))
			goto retry;
		if (fd == 0 && errno == EWOULDBLOCK &&
		    stdin_clear_nonblock(sh) >= 0) {
			out2str("sh: turning off NDELAY mode\n");
			goto retry;
		}
	}
	return nr;
}

/*
 * Refill the input buffer and return the next input character:
 *
 * 1) If a string was pushed back on the input, pop it;
 * 2) If we are reading from a string we can't refill the buffer, return EOF.
 * 3) If there is more stuff in this buffer, use it else call read to fill it.
 * 4) Process input up to the next newline, deleting nul characters.
 */

static int
preadbuffer(ShellContext &sh, InputStack &in)
{
	int more;
	char *q;

	if (in.current->eof & 2) {
eof:
		in.current->eof = 3;
		return PEOF;
	}
	flushall(sh);

	q = in.current->nextc;

	INTOFF(sh);
	{
		int nr = q - in.current->nextc;
		more = preadfd(sh, in);
		q = in.current->nextc + nr;
		if (more <= 0) {
			in.current->nleft = 0;
			INTON_DEFER(sh);
			goto eof;
		}
	}

	q += more;
	in.current->nleft = q - in.current->nextc - 1;
	*q = '\0';
	INTON_DEFER(sh);

	if (sh.opts.verbose()) {
		out2str(in.current->nextc);
#ifdef FLUSHERR
		sh.out2->flush(sh);
#endif
	}

	return (signed char)*in.current->nextc++;
}

void
pungetn(InputStack &in, int n)
{
	in.current->unget += n;
}

/*
 * Undo a call to pgetc.  Only two characters may be pushed back.
 * PEOF may be pushed back.
 */

void
pungetc(ShellContext &sh, InputStack &in)
{
	(void)sh;
	pungetn(in, 1 - (in.current->eof & 1));
	in.current->eof &= ~1;
}

/*
 * Push a string back onto the input at this current parsefile level.
 * We handle aliases this way.
 */
void
pushstring(ShellContext &sh, InputStack &in, char *s, std::shared_ptr<alias> ap)
{
	const auto len = std::strlen(s);

	INTOFF(sh);
	in.current->string_stack.push_back(StringPush{
		.prev_string = in.current->nextc,
		.prev_nleft = in.current->nleft,
		.ap = ap,
		.string = s,
		.unget = in.current->unget,
	});
	if (ap)
		ap->in_use = true;
	in.current->nextc = s;
	in.current->nleft = static_cast<int>(len);
	in.current->unget = 0;
	INTON_DEFER(sh);
}

static void
popstring(ShellContext &sh, InputStack &in)
{
	auto sp = in.current->string_stack.back();

	INTOFF(sh);
	if (sp.ap && in.current->nextc > sp.string) {
		if (in.current->nextc[-1] == ' ' ||
		    in.current->nextc[-1] == '\t') {
			sh.parse.checkkwd |= CHKALIAS;
		}
	}
	in.current->nextc = sp.prev_string;
	in.current->nleft = sp.prev_nleft;
	in.current->unget = sp.unget;
	in.current->string_stack.pop_back();
	if (sp.ap)
		in.current->pending_alias_release.push_back(std::move(sp.ap));
	INTON_DEFER(sh);
}

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

shell::Result<int>
setinputfile(ShellContext &sh, InputStack &in, std::string_view fname, int flags)
{
	int fd;
	std::string path(fname);

	INTOFF(sh);
	fd = TRY_SH(sh_open(sh, path.c_str(), O_RDONLY, flags & INPUT_NOFILE_OK));
	if (fd < 0)
		goto out;
	if (fd < 10)
		fd = TRY_SH(savefd(sh, fd, fd));
	setinputfd(sh, in, fd, flags & INPUT_PUSH_FILE);
out:
	INTON(sh);

	return fd;
}


/*
 * Like setinputfile, but takes an open file descriptor.  Call this with
 * interrupts off.
 */

static void
setinputfd(ShellContext &sh, InputStack &in, int fd, int push)
{
	pushfile(sh, in);
	if (!push)
		in.top = in.current;
	in.current->fd = fd;
	in.current->owned_buf = std::make_unique<char[]>(IBUFSIZ);
	in.current->nextc = in.current->buf = in.current->owned_buf.get();
}


/*
 * Like setinputfile, but takes input from a string.
 */

void
setinputstring(ShellContext &sh, InputStack &in, char *string)
{
	INTOFF(sh);
	pushfile(sh, in);
	in.current->nextc = string;
	in.current->nleft = std::strlen(string);
	in.current->eof = 2;
	INTON_DEFER(sh);
}



/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

static void
pushfile(ShellContext &, InputStack &in)
{
	auto *pf = new parsefile{};

	pf->prev = in.current;
	pf->linno = 1;
	pf->fd = -1;
	in.current = pf;
}

void
pushstdin(ShellContext &sh, InputStack &in)
{
	INTOFF(sh);
	in.base.prev = in.current;
	in.current = &in.base;
	INTON_DEFER(sh);
}

void
popfile(ShellContext &sh, InputStack &in)
{
	parsefile *pf = in.current;

	INTOFF(sh);
	if (pf == &in.base) {
		in.current = pf->prev;
		pf->prev = nullptr;
		goto out;
	}

	if (pf->fd >= 0)
		close(pf->fd);
	pf->owned_buf.reset();
	pf->buf = nullptr;
	if (!pf->pending_alias_release.empty())
		release_pending_aliases(sh, *pf);
	while (!pf->string_stack.empty()) {
		popstring(sh, in);
		release_pending_aliases(sh, *pf);
	}
	in.current = pf->prev;
	pf->prev = nullptr;
	delete pf;

out:
	INTON_DEFER(sh);
}


void __attribute__((noinline))
unwindfiles(ShellContext &sh, InputStack &in, struct parsefile *stop)
{
	while (in.base.prev || in.current != stop)
		popfile(sh, in);
}


/*
 * Return to top level.
 */

void
popallfiles(ShellContext &sh, InputStack &in)
{
	unwindfiles(sh, in, in.top);
}

void __attribute__((noinline))
flush_input(ShellContext &sh, InputStack &in)
{
	int left = in.base.nleft;

	INTOFF(sh);
	if (in.stdin.seekable && left)
		lseek(0, -left, SEEK_CUR);
	else if (in.stdin.pending > left) {
		flush_tee(sh, in.basebuf, BUFSIZ, in.stdin.pending - left);
		in.stdin.pending = 0;
	}
	in.base.nleft = 0;
	INTON_DEFER(sh);
}

void
reset_input(ShellContext &sh, InputStack &in)
{
	in.stdin_istty = -1;
	in.base.eof = 0;
	flush_input(sh, in);
}

/* Overloads that use the shell's own input stack. */

void
input_init(ShellContext &sh)
{
	input_init(sh, sh.input);
}

int
pgetc(ShellContext &sh)
{
	return pgetc(sh, sh.input);
}

int
pgetc_eoa(ShellContext &sh)
{
	return pgetc_eoa(sh, sh.input);
}

void
pungetc(ShellContext &sh)
{
	pungetc(sh, sh.input);
}

void
pungetn(ShellContext &sh, int n)
{
	pungetn(sh.input, n);
}

void
pushstring(ShellContext &sh, char *s, std::shared_ptr<alias> ap)
{
	pushstring(sh, sh.input, s, std::move(ap));
}

shell::Result<int>
setinputfile(ShellContext &sh, std::string_view fname, int flags)
{
	return setinputfile(sh, sh.input, fname, flags);
}

void
setinputstring(ShellContext &sh, char *string)
{
	setinputstring(sh, sh.input, string);
}

void
pushstdin(ShellContext &sh)
{
	pushstdin(sh, sh.input);
}

void
popfile(ShellContext &sh)
{
	popfile(sh, sh.input);
}

void
unwindfiles(ShellContext &sh, struct parsefile *stop)
{
	unwindfiles(sh, sh.input, stop);
}

void
flush_input(ShellContext &sh)
{
	flush_input(sh, sh.input);
}

void
reset_input(ShellContext &sh)
{
	reset_input(sh, sh.input);
}
