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

#include <cerrno>
#include <cstdio>	/* defines BUFSIZ */
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

/*
 * This file implements the input routines used by the parser.
 */

#include "error.h"
#include "input.h"
#include "output.h"
#include "parser.h"
#include "redir.h"
#include "shell.h"
#include "syntax.h"

using namespace shell;

namespace shell {

namespace {
void pushfile(Context &sh, InputStack &in);
void popstring(Context &sh, InputStack &in);
int preadfd(Context &sh, InputStack &in);
void setinputfd(Context &sh, InputStack &in, int fd, int push);
int preadbuffer(Context &sh, InputStack &in);

int &
active_unget(ParseFile &pf)
{
	return pf.string_stack.empty() ? pf.unget : pf.string_stack.back().unget;
}

int
get_unget_char(ParseFile &pf)
{
	int &ug = active_unget(pf);
	if (pf.string_stack.empty())
		return static_cast<signed char>(pf.buffer[pf.pos - ug--]);
	auto &sp = pf.string_stack.back();
	return static_cast<signed char>(sp.text[sp.pos - ug--]);
}
} // namespace

void
input_reset_interactive(Context &sh)
{
	int c;

	popallfiles(sh, sh.input);

	auto &top = sh.input.frames[sh.input.top];
	c = PEOF;
	if (top.pos > static_cast<std::size_t>(top.unget))
		c = static_cast<signed char>(top.buffer[top.pos - top.unget - 1]);
	while (c != '\n' && c != PEOF && !int_pending(sh))
		c = pgetc(sh, sh.input);
}

void
input_forkreset(Context &sh)
{
	popallfiles(sh, sh.input);
	if (sh.input.cur().fd > 0) {
		close(sh.input.cur().fd);
		sh.input.cur().fd = 0;
	}
	if (sh.input.stdin.pip[0]) {
		close(sh.input.stdin.pip[0]);
		close(sh.input.stdin.pip[1]);
		memset(sh.input.stdin.pip, 0, sizeof(sh.input.stdin.pip));
	}
}

void
input_init(Context &sh, InputStack &in)
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

namespace {
bool
stdin_bufferable(Context &sh, InputStack &in)
{
	if (in.stdin_istty < 0)
		input_init(sh, in);

	return in.stdin.bufferable;
}

void
flush_tee(Context &sh, void *buf, int nr, int pending)
{
	(void)sh;
	while (pending > 0) {
		int err;

		err = read(0, buf, nr > pending ? pending : nr);
		if (err > 0)
			pending -= err;
	}
}

int
stdin_tee(Context &sh, InputStack &in, void *buf, int nr)
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

void
release_pending_aliases(Context &sh, ParseFile &pf)
{
	INTOFF(sh);
	for (auto &ap : pf.pending_alias_release)
		ap->in_use = false;
	/* Dropping the shared_ptrs may destroy aliases already removed
	 * from Context::aliases via unalias.
	 */
	pf.pending_alias_release.clear();
	INTON_DEFER(sh);
}
} // namespace

/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int __attribute__((noinline))
pgetc(Context &sh, InputStack &in)
{
	int c;
	auto &pf = in.cur();

	if (unlikely(!pf.pending_alias_release.empty()))
		release_pending_aliases(sh, pf);

again:
	if (active_unget(pf))
		return get_unget_char(pf);

	if (!pf.string_stack.empty()) {
		auto &sp = pf.string_stack.back();
		if (sp.pos < sp.text.size()) {
			c = static_cast<signed char>(sp.text[sp.pos++]);
			if (unlikely(!c))
				goto again;
			return c;
		}
		popstring(sh, in);
		goto again;
	}

	if (likely(pf.nleft() > 0)) {
		c = static_cast<signed char>(pf.buffer[pf.pos++]);
	} else
		c = preadbuffer(sh, in);

	/* delete nul characters */
	if (unlikely(!c)) {
		if (pf.pos <= pf.end) {
			std::move(pf.buffer.begin() + static_cast<std::ptrdiff_t>(pf.pos),
				  pf.buffer.begin() + static_cast<std::ptrdiff_t>(pf.end),
				  pf.buffer.begin() + static_cast<std::ptrdiff_t>(pf.pos - 1));
			--pf.end;
			--pf.pos;
		}
		goto again;
	}

	return c;
}

int
pgetc_eoa(Context &sh, InputStack &in)
{
	/* After the last alias character is consumed, the StringPush frame
	 * is still on the stack with pos at end.
	 */
	auto &pf = in.cur();
	return !pf.string_stack.empty() &&
			       pf.string_stack.back().pos >= pf.string_stack.back().text.size() &&
			       pf.string_stack.back().ap && !pf.string_stack.back().unget
		       ? PEOA
		       : pgetc(sh, in);
}

namespace {
int
stdin_clear_nonblock(Context &sh)
{
	(void)sh;
	int flags = fcntl(0, F_GETFL, 0);

	if (flags >= 0) {
		flags &=~ O_NONBLOCK;
		flags = fcntl(0, F_SETFL, flags);
	}

	return flags;
}

int
preadfd(Context &sh, InputStack &in)
{
	auto &pf = in.cur();
	std::size_t keep = pf.pos;
	if (keep > PUNGETC_MAX)
		keep = PUNGETC_MAX;

	if (keep > 0)
		std::memmove(pf.buffer.data(),
			     pf.buffer.data() + pf.pos - keep, keep);
	pf.pos = keep;
	pf.end = keep;

	char *buf = pf.buffer.data() + pf.pos;
	int fd = pf.fd;
	bool use_tee;
	int pnr;
	int nr;

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
		if (errno == EINTR && !(in.stdin_saved && sh.pending_sig))
			goto retry;
		if (fd == 0 && errno == EWOULDBLOCK &&
		    stdin_clear_nonblock(sh) >= 0) {
			out2str("sh: turning off NDELAY mode\n");
			goto retry;
		}
	}
	return nr;
}
} // namespace

/*
 * Refill the input buffer and return the next input character:
 *
 * 1) If a string was pushed back on the input, pop it;
 * 2) If we are reading from a string we can't refill the buffer, return EOF.
 * 3) If there is more stuff in this buffer, use it else call read to fill it.
 * 4) Process input up to the next newline, deleting nul characters.
 */

namespace {
int
preadbuffer(Context &sh, InputStack &in)
{
	auto &pf = in.cur();
	int more;

	if (pf.eof & 2) {
eof:
		pf.eof = 3;
		return PEOF;
	}
	flushall(sh);

	INTOFF(sh);
	more = preadfd(sh, in);
	if (more <= 0) {
		pf.end = pf.pos;
		INTON_DEFER(sh);
		goto eof;
	}

	pf.end = pf.pos + static_cast<std::size_t>(more);
	if (pf.end >= pf.buffer.size())
		pf.buffer.resize(pf.end + 1);
	pf.buffer[pf.end] = '\0';
	INTON_DEFER(sh);

	if (sh.opts.verbose()) {
		out2str(pf.buffer.c_str() + pf.pos);
	}

	return static_cast<signed char>(pf.buffer[pf.pos++]);
}
} // namespace

void
pungetn(InputStack &in, int n)
{
	active_unget(in.cur()) += n;
}

/*
 * Undo a call to pgetc.  Only two characters may be pushed back.
 * PEOF may be pushed back.
 */

void
pungetc(Context &sh, InputStack &in)
{
	(void)sh;
	pungetn(in, 1 - (in.cur().eof & 1));
	in.cur().eof &= ~1;
}

/*
 * Push a string back onto the input at this current ParseFile level.
 * We handle aliases this way.
 */
void
pushstring(
	Context &sh,
	InputStack &in,
	std::string text,
	const std::shared_ptr<Alias> &ap
)
{
	auto &pf = in.cur();

	INTOFF(sh);
	StringPush sp;
	sp.prev_pos = pf.pos;
	sp.prev_end = pf.end;
	sp.prev_unget = pf.unget;
	sp.ap = ap;
	sp.owned = std::move(text);
	sp.text = sp.owned;
	sp.pos = 0;
	sp.unget = 0;
	if (ap)
		ap->in_use = true;
	pf.unget = 0;
	pf.string_stack.push_back(std::move(sp));
	INTON_DEFER(sh);
}

void
pushstring(
	Context &sh,
	InputStack &in,
	std::string_view text,
	const std::shared_ptr<Alias> &ap
)
{
	/*
	 * Alias overlays view the alias value directly (kept alive by
	 * shared_ptr). All other overlays copy into owned storage.
	 */
	if (ap && text.data() == ap->value.data() &&
	    text.size() == ap->value.size()) {
		auto &pf = in.cur();
		INTOFF(sh);
		StringPush sp;
		sp.prev_pos = pf.pos;
		sp.prev_end = pf.end;
		sp.prev_unget = pf.unget;
		sp.ap = ap;
		sp.text = ap->value;
		sp.pos = 0;
		sp.unget = 0;
		ap->in_use = true;
		pf.unget = 0;
		pf.string_stack.push_back(std::move(sp));
		INTON_DEFER(sh);
		return;
	}
	pushstring(sh, in, std::string(text), ap);
}

namespace {
void
popstring(Context &sh, InputStack &in)
{
	auto &pf = in.cur();
	auto sp = std::move(pf.string_stack.back());

	INTOFF(sh);
	if (sp.ap && sp.pos > 0) {
		char last = sp.text[sp.pos - 1];
		if (last == ' ' || last == '\t')
			sh.parse.checkkwd |= CHKALIAS;
	}
	pf.pos = sp.prev_pos;
	pf.end = sp.prev_end;
	pf.unget = sp.prev_unget;
	pf.string_stack.pop_back();
	if (sp.ap)
		pf.pending_alias_release.push_back(std::move(sp.ap));
	INTON_DEFER(sh);
}
} // namespace

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

Result<int>
setinputfile(Context &sh, InputStack &in, std::string_view fname, int flags)
{
	int fd;
	std::string path(fname);

	INTOFF(sh);
	TRY_SH_SET(fd, sh_open(sh, path.c_str(), O_RDONLY, flags & INPUT_NOFILE_OK));
	if (fd < 0)
		goto out;
	if (fd < 10)
		TRY_SH_SET(fd, savefd(sh, fd, fd));
	setinputfd(sh, in, fd, flags & INPUT_PUSH_FILE);
out:
	INTON(sh);

	return fd;
}

/*
 * Like setinputfile, but takes an open file descriptor.  Call this with
 * interrupts off.
 */

namespace {
void
setinputfd(Context &sh, InputStack &in, int fd, int push)
{
	pushfile(sh, in);
	if (!push)
		in.top = in.current;
	auto &pf = in.cur();
	pf.fd = fd;
	pf.buffer.assign(IBUFSIZ, '\0');
	pf.pos = 0;
	pf.end = 0;
}
} // namespace

/*
 * Like setinputfile, but takes input from an owned string.
 */

void
setinputstring(Context &sh, InputStack &in, std::string string)
{
	INTOFF(sh);
	pushfile(sh, in);
	auto &pf = in.cur();
	pf.buffer = std::move(string);
	pf.pos = 0;
	pf.end = pf.buffer.size();
	pf.fd = -1;
	pf.eof = 2;
	INTON_DEFER(sh);
}

void
setinputstring(Context &sh, InputStack &in, std::string_view string)
{
	setinputstring(sh, in, std::string(string));
}

/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

namespace {
void
pushfile(Context &, InputStack &in)
{
	in.frames.push_back(ParseFile{});
	in.current = in.frames.size() - 1;
}
} // namespace

void
pushstdin(Context &sh, InputStack &in)
{
	INTOFF(sh);
	if (in.current != 0) {
		in.stdin_saved = in.current;
		in.current = 0;
	}
	INTON_DEFER(sh);
}

void
popfile(Context &sh, InputStack &in)
{
	INTOFF(sh);
	if (in.current == 0 && in.stdin_saved) {
		in.current = *in.stdin_saved;
		in.stdin_saved.reset();
		INTON_DEFER(sh);
		return;
	}

	if (in.current == 0) {
		INTON_DEFER(sh);
		return;
	}

	auto &pf = in.cur();
	if (pf.fd >= 0)
		close(pf.fd);
	if (!pf.pending_alias_release.empty())
		release_pending_aliases(sh, pf);
	while (!pf.string_stack.empty()) {
		popstring(sh, in);
		release_pending_aliases(sh, pf);
	}

	in.frames.pop_back();
	in.current = in.frames.size() - 1;
	if (in.top >= in.frames.size())
		in.top = in.current;

	INTON_DEFER(sh);
}

void __attribute__((noinline))
unwindfiles(Context &sh, InputStack &in, std::size_t stop)
{
	while (in.stdin_saved || in.current != stop)
		popfile(sh, in);
}

/*
 * Return to top level.
 */

void
popallfiles(Context &sh, InputStack &in)
{
	unwindfiles(sh, in, in.top);
}

void __attribute__((noinline))
flush_input(Context &sh, InputStack &in)
{
	auto &base = in.base();
	int left = static_cast<int>(base.nleft());

	INTOFF(sh);
	if (in.stdin.seekable && left)
		lseek(0, -left, SEEK_CUR);
	else if (in.stdin.pending > left) {
		flush_tee(sh, base.buffer.data(), BUFSIZ, in.stdin.pending - left);
		in.stdin.pending = 0;
	}
	base.end = base.pos;
	INTON_DEFER(sh);
}

void
reset_input(Context &sh, InputStack &in)
{
	in.stdin_istty = -1;
	in.base().eof = 0;
	flush_input(sh, in);
}

/* Overloads that use the shell's own input stack. */

void
input_init(Context &sh)
{
	input_init(sh, sh.input);
}

int
pgetc(Context &sh)
{
	return pgetc(sh, sh.input);
}

int
pgetc_eoa(Context &sh)
{
	return pgetc_eoa(sh, sh.input);
}

void
pungetc(Context &sh)
{
	pungetc(sh, sh.input);
}

void
pungetn(Context &sh, int n)
{
	pungetn(sh.input, n);
}

void
pushstring(Context &sh, std::string s, const std::shared_ptr<Alias> &ap)
{
	pushstring(sh, sh.input, std::move(s), ap);
}

void
pushstring(Context &sh, std::string_view s, const std::shared_ptr<Alias> &ap)
{
	pushstring(sh, sh.input, s, ap);
}

Result<int>
setinputfile(Context &sh, std::string_view fname, int flags)
{
	return setinputfile(sh, sh.input, fname, flags);
}

void
setinputstring(Context &sh, std::string string)
{
	setinputstring(sh, sh.input, std::move(string));
}

void
setinputstring(Context &sh, std::string_view string)
{
	setinputstring(sh, sh.input, string);
}

void
pushstdin(Context &sh)
{
	pushstdin(sh, sh.input);
}

void
popfile(Context &sh)
{
	popfile(sh, sh.input);
}

void
unwindfiles(Context &sh, std::size_t stop)
{
	unwindfiles(sh, sh.input, stop);
}

void
flush_input(Context &sh)
{
	flush_input(sh, sh.input);
}

void
reset_input(Context &sh)
{
	reset_input(sh, sh.input);
}

} // namespace shell
