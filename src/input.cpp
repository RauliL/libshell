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
#include <stdbool.h>
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

	/* top level input file */
	/* buffer for top level input file */


	/* current input file */
		/* 1 == PS1, 2 == PS2 */


static void pushfile(ShellContext &sh);
static void popstring(ShellContext &sh);
static int preadfd(ShellContext &sh);
static void setinputfd(ShellContext &sh, int fd, int push);
static int preadbuffer(ShellContext &sh);

#ifdef mkinit
INCLUDE <stdio.h>
INCLUDE <string.h>
INCLUDE <termios.h>
INCLUDE <unistd.h>
INCLUDE "input.h"
INCLUDE "error.h"
INCLUDE "syntax.h"

INIT {
	sh.basepf.nextc = sh.basepf.buf = sh.basebuf;
	sh.basepf.linno = 1;
}

RESET {
	int c;

	/* clear input buffer */
	popallfiles(sh);

	c = PEOF;
	if (sh.toppf->nextc - sh.toppf->buf > sh.toppf->unget)
		c = sh.toppf->nextc[-sh.toppf->unget - 1];
	while (c != '\n' && c != PEOF && !int_pending())
		c = pgetc(sh);
}

FORKRESET {
	popallfiles(sh);
	if (sh.parsefile->fd > 0) {
		close(sh.parsefile->fd);
		sh.parsefile->fd = 0;
	}
	if (sh.stdin_state.pip[0]) {
		close(sh.stdin_state.pip[0]);
		close(sh.stdin_state.pip[1]);
		memset(sh.stdin_state.pip, 0, sizeof(sh.stdin_state.pip));
	}
}

POSTEXITRESET {
	flush_input(sh);
}
#endif

void
input_init(ShellContext &sh)
{
	struct stdin_state *st = &sh.stdin_state;
	struct termios tios;
	int istty;

	istty = tcgetattr(0, &tios) + 1;
	sh.stdin_istty = istty;
	if (istty)
		st->bufferable = tios.c_lflag & ICANON;
	else {
		st->seekable = lseek(0, 0, SEEK_CUR) + 1;
		st->bufferable = !!st->seekable;
	}
}

static bool
stdin_bufferable(ShellContext &sh)
{
	struct stdin_state *st = &sh.stdin_state;

	if (sh.stdin_istty < 0)
		input_init(sh);

	return st->bufferable;
}

static void
flush_tee(ShellContext &sh, void *buf, int nr, int pending)
{
	while (pending > 0) {
		int err;

		err = read(0, buf, nr > pending ? pending : nr);
		if (err > 0)
			pending -= err;
	}
}

static int
stdin_tee(ShellContext &sh, void *buf, int nr)
{
	int err;

	if (!sh.stdin_state.pip[0]) {
		if (auto r = sh_pipe(sh, sh.stdin_state.pip, 0); !r) {
			errno = EIO;
			return -1;
		}
		if (sh.stdin_state.pip[0] < 10) {
			auto r = savefd(sh, sh.stdin_state.pip[0],
					sh.stdin_state.pip[0]);
			if (!r) {
				errno = EIO;
				return -1;
			}
			sh.stdin_state.pip[0] = *r;
		}
		if (sh.stdin_state.pip[1] < 10) {
			auto r = savefd(sh, sh.stdin_state.pip[1],
					sh.stdin_state.pip[1]);
			if (!r) {
				errno = EIO;
				return -1;
			}
			sh.stdin_state.pip[1] = *r;
		}
	}

	flush_tee(sh, buf, nr, sh.stdin_state.pending);

	if (USE_TEE)
		err = tee(0, sh.stdin_state.pip[1], nr, 0);
	else {
		errno = EINVAL;
		err = -1;
	}
	sh.stdin_state.pending = err;
	return err;
}

static void
freestrings(ShellContext &sh, struct strpush *sp)
{
	INTOFF;
	do {
		struct strpush *psp;

		if (sp->ap) {
			sp->ap->flag &= ~ALIASINUSE;
			if (sp->ap->flag & ALIASDEAD) {
				unalias(sh, sp->ap->name.c_str());
			}
		}

		psp = sp;
		sp = sp->spfree;

		if (psp != &(sh.parsefile->basestrpush))
			ckfree(psp);
	} while (sp);

	sh.parsefile->spfree = NULL;
	INTON_DEFER;
}


/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int __attribute__((noinline))
pgetc(ShellContext &sh)
{
	struct strpush *sp = sh.parsefile->spfree;
	int c;

	if (unlikely(sp))
		freestrings(sh, sp);

again:
	if (sh.parsefile->unget) {
		long unget = -(long)(unsigned)sh.parsefile->unget--;

		return (signed char)sh.parsefile->nextc[unget];
	}

nextc:
	if (likely(sh.parsefile->nleft > 0)) {
		sh.parsefile->nleft--;
		c = (signed char)*sh.parsefile->nextc++;
	} else if (unlikely(sh.parsefile->strpush)) {
		popstring(sh);
		/* The freestrings call must be delayed til the next
		 * pgetc call for PEOA to work properly.
		 */
		goto again;
	} else
		c = preadbuffer(sh);

	/* delete nul characters */
	if (unlikely(!c)) {
		sh.parsefile->nextc = static_cast<char *>(memmove(
			sh.parsefile->nextc - 1, sh.parsefile->nextc, sh.parsefile->nleft));
		goto nextc;
	}

	return c;
}

int
pgetc_eoa(ShellContext &sh)
{
	return sh.parsefile->strpush && sh.parsefile->nleft == -1 &&
	       sh.parsefile->strpush->ap ? PEOA : pgetc(sh);
}

static int
stdin_clear_nonblock(ShellContext &sh)
{
	int flags = fcntl(0, F_GETFL, 0);

	if (flags >= 0) {
		flags &=~ O_NONBLOCK;
		flags = fcntl(0, F_SETFL, flags);
	}

	return flags;
}

static int
preadfd(ShellContext &sh)
{
	char *buf = sh.parsefile->buf;
	int fd = sh.parsefile->fd;
	bool use_tee;
	int unget;
	int pnr;
	int nr;

	unget = sh.parsefile->nextc - buf;
	if (unget > PUNGETC_MAX)
		unget = PUNGETC_MAX;

	memmove(buf, sh.parsefile->nextc - unget, unget);
	buf += unget;
	sh.parsefile->nextc = buf;

	nr = BUFSIZ;
	use_tee = likely(!fd) && !stdin_bufferable(sh);

	pnr = nr;
retry:
	nr = pnr;

	if (likely(use_tee)) {
		nr = stdin_tee(sh, buf, nr);
		if (nr >= 0)
			fd = sh.stdin_state.pip[0];
		else if (errno == EINVAL) {
			use_tee = false;
			nr = pnr = 1;
		}
	}

	if (nr > 0)
		nr = read(fd, buf, nr);

	if (nr < 0) {
		if (errno == EINTR && !(sh.basepf.prev && sh.pending_sig))
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
preadbuffer(ShellContext &sh)
{
	int more;
	char *q;

	if (sh.parsefile->eof & 2) {
eof:
		sh.parsefile->eof = 3;
		return PEOF;
	}
	flushall(sh);

	q = sh.parsefile->nextc;

	INTOFF;
	{
		int nr = q - sh.parsefile->nextc;
		more = preadfd(sh);
		q = sh.parsefile->nextc + nr;
		if (more <= 0) {
			sh.parsefile->nleft = 0;
			INTON_DEFER;
			goto eof;
		}
	}

	q += more;
	sh.parsefile->nleft = q - sh.parsefile->nextc - 1;
	*q = '\0';
	INTON_DEFER;

	if (vflag) {
		out2str(sh.parsefile->nextc);
#ifdef FLUSHERR
		flushout(sh, sh.out2);
#endif
	}

	return (signed char)*sh.parsefile->nextc++;
}

void
pungetn(ShellContext &sh, int n)
{
	sh.parsefile->unget += n;
}

/*
 * Undo a call to pgetc.  Only two characters may be pushed back.
 * PEOF may be pushed back.
 */

void
pungetc(ShellContext &sh)
{
	pungetn(sh, 1 - (sh.parsefile->eof & 1));
	sh.parsefile->eof &= ~1;
}

/*
 * Push a string back onto the input at this current sh.parsefile level.
 * We handle aliases this way.
 */
void
pushstring(ShellContext &sh, char *s, void *ap)
{
	struct strpush *sp;
	size_t len;

	len = strlen(s);
	INTOFF;
/*dprintf(__STR21__, s, len);*/
	if ((unsigned long)sh.parsefile->strpush |
	    (unsigned long)sh.parsefile->spfree) {
		sp = ckmalloc(sh, sizeof (struct strpush));
		sp->prev = sh.parsefile->strpush;
		sh.parsefile->strpush = sp;
	} else
		sp = sh.parsefile->strpush = &(sh.parsefile->basestrpush);
	sp->prevstring = sh.parsefile->nextc;
	sp->prevnleft = sh.parsefile->nleft;
	sp->unget = sh.parsefile->unget;
	sp->spfree = sh.parsefile->spfree;
	sp->ap = (struct alias *)ap;
	if (ap) {
		((struct alias *)ap)->flag |= ALIASINUSE;
		sp->string = s; /* start of alias value being scanned */
	}
	sh.parsefile->nextc = s;
	sh.parsefile->nleft = len;
	sh.parsefile->unget = 0;
	sh.parsefile->spfree = NULL;
	INTON_DEFER;
}

static void
popstring(ShellContext &sh)
{
	struct strpush *sp = sh.parsefile->strpush;

	INTOFF;
	if (sp->ap && sh.parsefile->nextc > sp->string) {
		if (sh.parsefile->nextc[-1] == ' ' ||
		    sh.parsefile->nextc[-1] == '\t') {
			sh.checkkwd |= CHKALIAS;
		}
		/* Alias values are owned by the alias; do not free here. */
	}
	sh.parsefile->nextc = sp->prevstring;
	sh.parsefile->nleft = sp->prevnleft;
	sh.parsefile->unget = sp->unget;
/*dprintf("*** calling popstring: restoring to '%s'\n", parsenextc);*/
	sh.parsefile->strpush = sp->prev;
	sh.parsefile->spfree = sp;
	INTON_DEFER;
}

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

dash::ShellResult<int>
setinputfile(ShellContext &sh, const char *fname, int flags)
{
	int fd;

	INTOFF;
	fd = TRY_SH(sh_open(sh, fname, O_RDONLY, flags & INPUT_NOFILE_OK));
	if (fd < 0)
		goto out;
	if (fd < 10)
		fd = TRY_SH(savefd(sh, fd, fd));
	setinputfd(sh, fd, flags & INPUT_PUSH_FILE);
out:
	INTON;
	return fd;
}


/*
 * Like setinputfile, but takes an open file descriptor.  Call this with
 * interrupts off.
 */

static void
setinputfd(ShellContext &sh, int fd, int push)
{
	pushfile(sh);
	if (!push)
		sh.toppf = sh.parsefile;
	sh.parsefile->fd = fd;
	sh.parsefile->nextc = sh.parsefile->buf = ckmalloc(sh, IBUFSIZ);
}


/*
 * Like setinputfile, but takes input from a string.
 */

void
setinputstring(ShellContext &sh, char *string)
{
	INTOFF;
	pushfile(sh);
	sh.parsefile->nextc = string;
	sh.parsefile->nleft = strlen(string);
	sh.parsefile->eof = 2;
	INTON_DEFER;
}



/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

static void
pushfile(ShellContext &sh)
{
	struct parsefile *pf;

	pf = (struct parsefile *)ckmalloc(sh, sizeof (struct parsefile));
	memset(pf, 0, sizeof(*pf));
	pf->prev = sh.parsefile;
	pf->linno = 1;
	pf->fd = -1;
	sh.parsefile = pf;
}

void
pushstdin(ShellContext &sh)
{
	INTOFF;
	sh.basepf.prev = sh.parsefile;
	sh.parsefile = &sh.basepf;
	INTON_DEFER;
}

void
popfile(ShellContext &sh)
{
	struct parsefile *pf = sh.parsefile;

	INTOFF;
	sh.parsefile = pf->prev;
	pf->prev = NULL;
	if (pf == &sh.basepf)
		goto out;

	if (pf->fd >= 0)
		close(pf->fd);
	ckfree(pf->buf);
	if (sh.parsefile->spfree)
		freestrings(sh, sh.parsefile->spfree);
	while (pf->strpush) {
		popstring(sh);
		freestrings(sh, sh.parsefile->spfree);
	}
	ckfree(pf);

out:
	INTON_DEFER;
}


void __attribute__((noinline))
unwindfiles(ShellContext &sh, struct parsefile *stop)
{
	while (sh.basepf.prev || sh.parsefile != stop)
		popfile(sh);
}


/*
 * Return to top level.
 */

void
popallfiles(ShellContext &sh)
{
	unwindfiles(sh, sh.toppf);
}

void __attribute__((noinline))
flush_input(ShellContext &sh)
{
	int left = sh.basepf.nleft;

	INTOFF;
	if (sh.stdin_state.seekable && left)
		lseek(0, -left, SEEK_CUR);
	else if (sh.stdin_state.pending > left) {
		flush_tee(sh, sh.basebuf, BUFSIZ, sh.stdin_state.pending - left);
		sh.stdin_state.pending = 0;
	}
	sh.basepf.nleft = 0;
	INTON_DEFER;
}

void
reset_input(ShellContext &sh)
{
	sh.stdin_istty = -1;
	sh.basepf.eof = 0;
	flush_input(sh);
}
