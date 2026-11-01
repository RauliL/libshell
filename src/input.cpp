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

#ifdef mkinit
INCLUDE <stdio.h>
INCLUDE <string.h>
INCLUDE <termios.h>
INCLUDE <unistd.h>
INCLUDE "input.h"
INCLUDE "error.h"
INCLUDE "syntax.h"

INIT {
	sh.input.base.nextc = sh.input.base.buf = sh.input.basebuf;
	sh.input.base.linno = 1;
}

RESET {
	int c;

	/* clear input buffer */
	popallfiles(sh, sh.input);

	c = PEOF;
	if (sh.input.top->nextc - sh.input.top->buf > sh.input.top->unget)
		c = sh.input.top->nextc[-sh.input.top->unget - 1];
	while (c != '\n' && c != PEOF && !int_pending())
		c = pgetc(sh, sh.input);
}

FORKRESET {
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

POSTEXITRESET {
	flush_input(sh, sh.input);
}
#endif

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
freestrings(ShellContext &sh, InputStack &in, struct strpush *sp)
{
	INTOFF;
	do {
		struct strpush *psp;

		if (sp->ap) {
			sp->ap->flag &= ~ALIASINUSE;
			if (sp->ap->flag & ALIASDEAD) {
				unalias(sh, sp->ap->name);
			}
		}

		psp = sp;
		sp = sp->spfree;

		if (psp != &(in.current->basestrpush))
			ckfree(psp);
	} while (sp);

	in.current->spfree = NULL;
	INTON_DEFER;
}


/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int __attribute__((noinline))
pgetc(ShellContext &sh, InputStack &in)
{
	struct strpush *sp = in.current->spfree;
	int c;

	if (unlikely(sp))
		freestrings(sh, in, sp);

again:
	if (in.current->unget) {
		long unget = -(long)(unsigned)in.current->unget--;

		return (signed char)in.current->nextc[unget];
	}

nextc:
	if (likely(in.current->nleft > 0)) {
		in.current->nleft--;
		c = (signed char)*in.current->nextc++;
	} else if (unlikely(in.current->strpush)) {
		popstring(sh, in);
		/* The freestrings call must be delayed til the next
		 * pgetc call for PEOA to work properly.
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
	return in.current->strpush && in.current->nleft == -1 &&
	       in.current->strpush->ap ? PEOA : pgetc(sh, in);
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

	INTOFF;
	{
		int nr = q - in.current->nextc;
		more = preadfd(sh, in);
		q = in.current->nextc + nr;
		if (more <= 0) {
			in.current->nleft = 0;
			INTON_DEFER;
			goto eof;
		}
	}

	q += more;
	in.current->nleft = q - in.current->nextc - 1;
	*q = '\0';
	INTON_DEFER;

	if (vflag) {
		out2str(in.current->nextc);
#ifdef FLUSHERR
		flushout(sh, sh.out2);
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
pushstring(ShellContext &sh, InputStack &in, char *s, void *ap)
{
	struct strpush *sp;
	size_t len;

	len = strlen(s);
	INTOFF;
	if ((unsigned long)in.current->strpush |
	    (unsigned long)in.current->spfree) {
		sp = ckmalloc(sh, sizeof (struct strpush));
		sp->prev = in.current->strpush;
		in.current->strpush = sp;
	} else
		sp = in.current->strpush = &(in.current->basestrpush);
	sp->prevstring = in.current->nextc;
	sp->prevnleft = in.current->nleft;
	sp->unget = in.current->unget;
	sp->spfree = in.current->spfree;
	sp->ap = (struct alias *)ap;
	if (ap) {
		((struct alias *)ap)->flag |= ALIASINUSE;
		sp->string = s; /* start of alias value being scanned */
	}
	in.current->nextc = s;
	in.current->nleft = len;
	in.current->unget = 0;
	in.current->spfree = NULL;
	INTON_DEFER;
}

static void
popstring(ShellContext &sh, InputStack &in)
{
	struct strpush *sp = in.current->strpush;

	INTOFF;
	if (sp->ap && in.current->nextc > sp->string) {
		if (in.current->nextc[-1] == ' ' ||
		    in.current->nextc[-1] == '\t') {
			sh.checkkwd |= CHKALIAS;
		}
		/* Alias values are owned by the alias; do not free here. */
	}
	in.current->nextc = sp->prevstring;
	in.current->nleft = sp->prevnleft;
	in.current->unget = sp->unget;
	in.current->strpush = sp->prev;
	in.current->spfree = sp;
	INTON_DEFER;
}

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

dash::ShellResult<int>
setinputfile(ShellContext &sh, InputStack &in, std::string_view fname, int flags)
{
	int fd;
	std::string path(fname);

	INTOFF;
	fd = TRY_SH(sh_open(sh, path.c_str(), O_RDONLY, flags & INPUT_NOFILE_OK));
	if (fd < 0)
		goto out;
	if (fd < 10)
		fd = TRY_SH(savefd(sh, fd, fd));
	setinputfd(sh, in, fd, flags & INPUT_PUSH_FILE);
out:
	INTON;
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
	in.current->nextc = in.current->buf = ckmalloc(sh, IBUFSIZ);
}


/*
 * Like setinputfile, but takes input from a string.
 */

void
setinputstring(ShellContext &sh, InputStack &in, char *string)
{
	INTOFF;
	pushfile(sh, in);
	in.current->nextc = string;
	in.current->nleft = strlen(string);
	in.current->eof = 2;
	INTON_DEFER;
}



/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

static void
pushfile(ShellContext &sh, InputStack &in)
{
	struct parsefile *pf;

	pf = (struct parsefile *)ckmalloc(sh, sizeof (struct parsefile));
	memset(pf, 0, sizeof(*pf));
	pf->prev = in.current;
	pf->linno = 1;
	pf->fd = -1;
	in.current = pf;
}

void
pushstdin(ShellContext &sh, InputStack &in)
{
	(void)sh;
	INTOFF;
	in.base.prev = in.current;
	in.current = &in.base;
	INTON_DEFER;
}

void
popfile(ShellContext &sh, InputStack &in)
{
	struct parsefile *pf = in.current;

	INTOFF;
	in.current = pf->prev;
	pf->prev = NULL;
	if (pf == &in.base)
		goto out;

	if (pf->fd >= 0)
		close(pf->fd);
	ckfree(pf->buf);
	if (in.current->spfree)
		freestrings(sh, in, in.current->spfree);
	while (pf->strpush) {
		popstring(sh, in);
		freestrings(sh, in, in.current->spfree);
	}
	ckfree(pf);

out:
	INTON_DEFER;
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

	INTOFF;
	if (in.stdin.seekable && left)
		lseek(0, -left, SEEK_CUR);
	else if (in.stdin.pending > left) {
		flush_tee(sh, in.basebuf, BUFSIZ, in.stdin.pending - left);
		in.stdin.pending = 0;
	}
	in.base.nleft = 0;
	INTON_DEFER;
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
pushstring(ShellContext &sh, char *s, void *ap)
{
	pushstring(sh, sh.input, s, ap);
}

dash::ShellResult<int>
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
