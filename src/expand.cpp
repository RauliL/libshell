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

#include <cctype>
#include <dirent.h>
#ifdef HAVE_FNMATCH
#include <fnmatch.h>
#endif
#ifdef HAVE_GLOB
#include <glob.h>
#endif
#include <format>
#include <cinttypes>
#include <climits>
#ifdef HAVE_GETPWNAM
#include <pwd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <cstring>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cwchar>
#include <cwctype>

/*
 * Routines to expand arguments to commands.  We have to deal with
 * backquotes, shell variables, and file metacharacters.
 */

#include "error.h"
#include "eval.h"
#include "expand.h"
#include "expand_buf.h"
#include "jobs.h"
#include "main.h"
#include "memalloc.h"
#include "mystring.h"
#include "nodes.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "shell.h"
#include "shell_context.h"
#include "syntax.h"
#include "system.h"
#include "var.h"

/*
 * _rmescape() flags
 */
#define RMESCAPE_ALLOC	0x1	/* Allocate a new string */
#define RMESCAPE_GLOB	0x2	/* Add backslashes for glob */
#define RMESCAPE_GROW	0x8	/* Grow ExpandBuf instead of malloc */
#define RMESCAPE_HEAP	0x10	/* Malloc strings */

/* Add CTLESC when necessary. */
#define QUOTES_ESC	(EXP_FULL | EXP_CASE)

/*
 * Structure specifying which parts of the string should be searched
 * for IFS characters.
 */

struct ifs_state {
	const char *ifs;
	char *start;
	char *r;
	int maxargs;
	int ifsspc;
};

/* sh.output of current string */

/* list of back quote expressions */

/* first struct in list of ifs regions */

/* last struct in list */

/* holds expanded arg list */







static shell::Result<char *> argstr(ShellContext &sh, ExpandBuf &eb, char *p, int flag);
static char * exptilde(ShellContext &sh, ExpandBuf &eb, char *startp, int flag);
static shell::Result<char *> expari(ShellContext &sh, ExpandBuf &eb, char *start, int flag);
static shell::Result<void> expbackq(ShellContext &sh, ExpandBuf &eb, Node *, int);
static shell::Result<char *> evalvar(ShellContext &sh, ExpandBuf &eb, char *, int);
static size_t strtodest(ShellContext &sh, ExpandBuf &eb, const char *p, int flags);
static size_t memtodest(ShellContext &sh, ExpandBuf &eb, const char *p, size_t len, int flags);
static shell::Result<ssize_t> varvalue(ShellContext &sh, ExpandBuf &eb, char *, int, unsigned);
static shell::Result<void> expandmeta(ShellContext &sh, ExpandBuf &eb, ArgList &);
static void addglob(ShellContext &sh, const glob64_t *);
static char * expmeta(ShellContext &sh, ExpandBuf &eb, char *, unsigned, size_t);
static void addfname(ShellContext &sh, char *);
static int patmatch(ShellContext &sh, char *, const char *, ExpandBuf *eb = nullptr);
static int pmatch(ShellContext &sh, char *, const char *);
static size_t cvtnum(ShellContext &sh, ExpandBuf &eb, intmax_t num, int flags);
static tl::unexpected<shell::Error> varunset(ShellContext &sh, const char *, const char *, const char *, int);


/*
 * Prepare a pattern for a glob(3) call.
 *
 * Returns a possibly malloc'd string (caller frees if != pattern).
 */

static inline char *
preglob(ShellContext &sh, const char *pattern, int flag, ExpandBuf *eb = nullptr)
{
	if (FNMATCH_IS_ENABLED) {
		if (!flag)
			flag = eb ? RMESCAPE_GROW : 0;
		flag |= RMESCAPE_ALLOC;
	}
	flag |= RMESCAPE_GLOB;
	return _rmescapes(sh, const_cast<char *>(pattern), flag, eb);
}


static size_t
mesclen(ShellContext &sh, const char *start, const char *p, char mesc)
{
	size_t esc = 0;

	while (p > start && *--p == mesc) {
		esc++;
	}
	return esc;
}

static size_t
esclen(ShellContext &sh, const char *start, const char *p)
{
	return mesclen(sh, start, p, CTLESC);
}

static __attribute__((noinline)) unsigned
mbnext(ShellContext &sh, const char *p)
{
	unsigned start = 0;
	unsigned end = 0;
	unsigned ml;
	int c;

	c = (signed char)p[end++];

	switch (__builtin_expect(c, 0)) {
	case CTLMBCHAR:
		if ((signed char)p[end] == CTLESC)
			end++;
		ml = (unsigned char)p[end++];
		start = end;
		end = ml + 2;
		break;
	case CTLESC:
		start++;
		break;
	}

	return start | end << 8;
}

static inline const char *
getpwhome(ShellContext &sh, const char *name)
{
#ifdef HAVE_GETPWNAM
	struct passwd *pw = getpwnam(name);
	return pw ? pw->pw_dir : 0;
#else
	return 0;
#endif
}


/*
 * Perform variable substitution and command substitution on an argument,
 * placing the resulting list of arguments in ArgList.  If EXP_FULL is true,
 * perform splitting and file name expansion.  When ArgList is nullptr, perform
 * here document expansion.
 */

shell::Result<void>
expandarg(ShellContext &sh, Node *arg, ArgList *alist, int flag)
{
	ExpandBuf eb;
	char *p;

	sh.expand.argbackq = &as<Node::Arg>(arg)->backquote;
	sh.expand.argbackq_i = 0;
	eb.start();
	TRY_SHV(argstr(sh, eb, as<Node::Arg>(arg)->text.data(), flag));
	p = (eb.commit_to(eb.dest));
	{
		ArgList words;
		/*
		 * TODO - EXP_REDIR
		 */
		if (flag & EXP_FULL) {
			ifsbreakup(sh, p, -1, &words);
			TRY_SHV(expandmeta(sh, eb, words));
		} else {
			words.emplace_back(p);
		}
		alist->insert(alist->end(),
				std::make_move_iterator(words.begin()),
				std::make_move_iterator(words.end()));
	}

	ifsfree(sh);
	return {};
}

shell::Result<std::string>
expandarg_to_string(ShellContext &sh, Node *arg, int flag)
{
	ExpandBuf eb;

	sh.expand.argbackq = &as<Node::Arg>(arg)->backquote;
	sh.expand.argbackq_i = 0;
	eb.start();
	TRY_SHV(argstr(sh, eb, as<Node::Arg>(arg)->text.data(), flag));
	eb.putc_nul(eb.dest);
	std::string result(eb.base());
	ifsfree(sh);
	return result;
}



/*
 * Perform variable and command substitution.  If EXP_FULL is set, sh.output CTLESC
 * characters to allow for further processing.  Otherwise treat
 * $@ like $* since no splitting will be performed.
 */

static shell::Result<char *>
argstr(ShellContext &sh, ExpandBuf &eb, char *p, int flag)
{
	static const char spclchars[] = {
		'=',
		':',
		CTLQUOTEMARK,
		CTLENDVAR,
		CTLESC,
		CTLVAR,
		CTLBACKQ,
		CTLMBCHAR,
		CTLARI,
		CTLENDARI,
		0
	};
	const char *reject = spclchars;
	int c;
	int breakall = (flag & (EXP_WORD | EXP_QUOTED)) == EXP_WORD;
	int inquotes;
	size_t length;
	int startloc;

	reject += !!(flag & EXP_VARTILDE2);
	reject += flag & EXP_VARTILDE ? 0 : 2;
	inquotes = 0;
	length = 0;
	if (flag & EXP_TILDE) {
		flag &= ~EXP_TILDE;
tilde:
		if (*p == '~')
			p = exptilde(sh, eb, p, flag);
	}
start:
	startloc = eb.dest - eb.base();
	for (;;) {
		unsigned ml;
		unsigned mb;
		int end;

		length += strcspn(p + length, reject);
		end = 0;
		c = (signed char)p[length];
		if (!(c & 0x80) || c == CTLENDARI || c == CTLENDVAR) {
			/*
			 * c == '=' || c == ':' || c == '\0' ||
			 * c == CTLENDARI || c == CTLENDVAR
			 */
			length++;
			/* c == '\0' || c == CTLENDARI || c == CTLENDVAR */
			end = !!((c - 1) & 0x80);
		}
		if (length > 0 && !(flag & EXP_DISCARD)) {
			int newloc;
			char *q;

			q = eb.nputs(p, length, eb.dest);
			q[-1] &= end - 1;
			eb.dest = q - (flag & EXP_WORD ? end : 0);
			newloc = q - eb.base() - end;
			if (breakall && !inquotes && newloc > startloc) {
				recordregion(sh, startloc, newloc, 0);
			}
			startloc = newloc;
		}
		p += length + 1;
		length = 0;

		if (end)
			break;

		switch (c) {
		case '=':
			flag |= EXP_VARTILDE2;
			reject++;
			/* fall through */
		case ':':
			/*
			 * sort of a hack - expand tildes in variable
			 * assignments (after the first '=' and after ':'s).
			 */
			if (*--p == '~') {
				goto tilde;
			}
			continue;
		case CTLQUOTEMARK:
			/* "$@" syntax adherence hack */
			if (!inquotes && !strcmp(p, dolatstr + 1)) {
				p = TRY_SH(evalvar(sh, eb, p + 1, flag | EXP_QUOTED)) + 1;
				goto start;
			}
			inquotes ^= EXP_QUOTED;
addquote:
			if (flag & QUOTES_ESC) {
				p--;
				length++;
				startloc++;
			}
			break;
		case CTLMBCHAR:
			c = (signed char)*p--;
			mb = mbnext(sh, p);
			ml = (mb >> 8) - 2;
			if (flag & (QUOTES_ESC | EXP_MBCHAR)) {
				length = (mb >> 8) + (mb & 0xff);
				if (c == (char)CTLESC)
					startloc += length;
				break;
			}
			if (c == CTLESC)
				startloc += ml;
			p += mb & 0xff;
			if (!(flag & EXP_DISCARD))
				eb.dest = eb.nputs(p, ml, eb.dest);
			p += mb >> 8;
			break;
		case CTLESC:
			startloc++;
			length++;
			goto addquote;
		case CTLVAR:
			p = TRY_SH(evalvar(sh, eb, p, flag | inquotes));
			goto start;
		case CTLBACKQ:
			TRY_SHV(expbackq(sh, eb, (*sh.expand.argbackq)[sh.expand.argbackq_i].get(), flag | inquotes));
			goto start;
		case CTLARI:
			p = TRY_SH(expari(sh, eb, p, flag | inquotes));
			goto start;
		}
	}
	return p - 1;
}

static char *
exptilde(ShellContext &sh, ExpandBuf &eb, char *startp, int flag)
{
	signed char c;
	char *name;
	const char *home;
	char *p;

	p = startp;
	name = p + 1;

	while ((c = *++p) != '\0') {
		switch(c) {
		case CTLESC:
			return (startp);
		case CTLQUOTEMARK:
			return (startp);
		case ':':
			if (flag & EXP_VARTILDE)
				goto done;
			break;
		case '/':
		case CTLENDVAR:
			goto done;
		}
	}
done:
	if (flag & EXP_DISCARD)
		goto out;
	*p = '\0';
	if (*name == '\0') {
		home = sh.get_var("HOME");
	} else {
		home = getpwhome(sh, name);
	}
	*p = c;
	if (!home)
		goto lose;
	strtodest(sh, eb, home, flag | EXP_QUOTED);
out:
	return (p);
lose:
	return (startp);
}


shell::Result<void>
removerecordregions(ShellContext &sh, int endoff)
{
	auto &regs = sh.expand.ifsregions;

	if (regs.empty())
		return {};

	INTOFF(sh);
	if (regs.front().endoff > endoff) {
		if (regs.front().begoff > endoff)
			regs.clear();
		else {
			regs.resize(1);
			regs.front().endoff = endoff;
		}
		INTON(sh);
		return {};
	}

	std::size_t i = 0;
	while (i + 1 < regs.size() && regs[i + 1].begoff < endoff)
		++i;
	regs.resize(i + 1);
	if (regs[i].endoff > endoff)
		regs[i].endoff = endoff;
	INTON(sh);
	return {};
}


/*
 * Expand arithmetic expression.  Backup to start of expression,
 * evaluate, place result in (backed up) result, adjust string position.
 */
static shell::Result<char *>
expari(ShellContext &sh, ExpandBuf &eb, char *start, int flag)
{
	int begoff;
	int endoff;
	int len;
	intmax_t result;
	char *p;

	p = eb.base();
	begoff = eb.dest - p;
	p = TRY_SH(argstr(sh, eb, start, flag & EXP_DISCARD));

	if (flag & EXP_DISCARD)
		goto out;

	start = eb.base();
	endoff = eb.dest - start;
	start += begoff;
	((eb.dest) += (start - eb.dest));

	TRY_SHV(removerecordregions(sh, begoff));

	{
		std::size_t su = eb.used;
		eb.commit(endoff);
		result = TRY_SH(sh.arith(start));
		eb.rewind(su);
	}

	len = cvtnum(sh, eb, result, flag);

	if (likely(!(flag & EXP_QUOTED)))
		recordregion(sh, begoff, begoff + len, 0);

out:
	return p;
}


/*
 * Expand stuff in backwards quotes.
 */

static shell::Result<void>
expbackq(ShellContext &sh, ExpandBuf &eb, Node *cmd, int flag)
{
	struct backcmd in;
	int i;
	char buf[128];
	char *p;
	char *dest;
	int startloc;

	if (flag & EXP_DISCARD)
		goto out;

	INTOFF(sh);
	startloc = eb.dest - eb.base();
	{
		std::size_t su = eb.used;
		eb.commit(startloc);
		TRY_SHV(evalbackcmd(sh, cmd, &in));
		eb.rewind(su);
	}

	p = in.buf.data();
	i = static_cast<int>(in.buf.size());
	if (i == 0)
		goto read;
	for (;;) {
		memtodest(sh, eb, p, i, flag);
read:
		if (in.fd < 0)
			break;
		do {
			i = read(in.fd, buf, sizeof buf);
		} while (i < 0 && errno == EINTR);
		if (i <= 0)
			break;
		p = buf;
	}

	if (in.fd >= 0) {
		close(in.fd);
		sh.back_exitstatus = TRY_SH(waitforjob(sh, in.jp));
	}
	INTON(sh);

	/* Eat all trailing newlines */
	dest = eb.dest;
	for (; dest > (eb.base() + startloc) && dest[-1] == '\n';)
		(--(dest));
	eb.dest = dest;

	if (!(flag & EXP_QUOTED))
		recordregion(sh, startloc, dest - eb.base(), 0);

out:
	sh.expand.argbackq_i++;
	return {};
}


static char *
scanleft(ShellContext &sh, char *startp, char *endp, char *rmesc, char *rmescend,
		      char *str, int quotes, int zero)
{
	char *loc;
	char *loc2;
	char c;

	loc = startp;
	loc2 = rmesc;
	do {
		char *s = FNMATCH_IS_ENABLED ? loc2 : loc;
		unsigned mb;
		unsigned ml;
		int match;

		c = *s;
		if (zero) {
			*s = '\0';
			s = FNMATCH_IS_ENABLED ? rmesc : startp;
		}
		match = pmatch(sh, str, s);
		*(FNMATCH_IS_ENABLED ? loc2 : loc) = c;
		if (match)
			return quotes ? loc : loc2;

		if (!c)
			break;

		mb = mbnext(sh, loc);
		loc += (mb & 0xff) + (mb >> 8);
		ml = (mb >> 8) > 3 ? (mb >> 8) - 2 : 1;
		loc2 += ml;
	} while (1);
	return 0;
}


static char *
scanright(ShellContext &sh, char *startp, char *endp, char *rmesc, char *rmescend,
		       char *str, int quotes, int zero)
{
	size_t esc = 0;
	char *loc;
	char *loc2;

	for (loc = endp, loc2 = rmescend;; loc2--) {
		char *s = FNMATCH_IS_ENABLED ? loc2 : loc;
		char c = *s;
		unsigned ml;
		int match;

		if (zero) {
			*s = '\0';
			s = FNMATCH_IS_ENABLED ? rmesc : startp;
		}
		match = pmatch(sh, str, s);
		*(FNMATCH_IS_ENABLED ? loc2 : loc) = c;
		if (match)
			return quotes ? loc : loc2;
		if (--loc < startp)
			break;
		if (!esc--)
			esc = esclen(sh, startp, loc);
		if (esc % 2) {
			esc--;
			loc--;
			continue;
		}
		if (*loc != (char)CTLMBCHAR)
			continue;

		ml = (unsigned char)*--loc;
		loc -= ml + 2;
		if (*loc == (char)CTLESC)
			loc--;
		loc2 -= ml - 1;
	}
	return 0;
}

static shell::Result<char *>
subevalvar(ShellContext &sh, ExpandBuf &eb, char *start, char *str, int strloc, int startloc,
			int varflags, int flag)
{
	int subtype = varflags & VSTYPE;
	int quotes = flag & QUOTES_ESC;
	char *startp;
	char *loc;
	long amount;
	char *rmesc, *rmescend;
	int zero;
	char *(*scan)(ShellContext &, char *, char *, char *, char *, char *, int, int);
	int nstrloc = strloc;
	char *endp;
	char *p;

	p = TRY_SH(argstr(sh, eb, start, (flag & EXP_DISCARD) | EXP_TILDE |
			  (str ?  0 : EXP_CASE)));
	if (flag & EXP_DISCARD)
		return p;

	startp = eb.base() + startloc;

	switch (subtype) {
	case VSASSIGN:
		TRY_SHV(setvar(sh, str, startp));

		loc = startp;
		goto out;

	case VSQUESTION:
		return varunset(sh, start, str, startp, varflags);
	}

	subtype -= VSTRIMRIGHT;

	rmescend = eb.base() + strloc;
	str = preglob(sh, rmescend, 0, &eb);
	if (FNMATCH_IS_ENABLED) {
		startp = eb.base() + startloc;
		rmescend = eb.base() + strloc;
		nstrloc = str - eb.base();
	}

	rmesc = startp;
	if (FNMATCH_IS_ENABLED || !quotes) {
		rmesc = _rmescapes(sh, startp, RMESCAPE_ALLOC | RMESCAPE_GROW, &eb);
		if (rmesc != startp)
			rmescend = eb.dest;
		startp = eb.base() + startloc;
		str = eb.base() + nstrloc;
	}
	rmescend--;

	/* zero = subtype == VSTRIMLEFT || subtype == VSTRIMLEFTMAX */
	zero = subtype >> 1;
	/* VSTRIMLEFT/VSTRIMRIGHTMAX -> scanleft */
	scan = (subtype & 1) ^ zero ? scanleft : scanright;

	endp = eb.base() + strloc - 1;
	loc = scan(sh, startp, endp, rmesc, rmescend, str, quotes, zero);
	if (!loc) {
		if (quotes) {
			rmesc = startp;
			rmescend = endp;
		}
	} else if (!quotes) {
		if (zero)
			rmesc = loc;
		else
			rmescend = loc;
	} else if (zero) {
		rmesc = loc;
		rmescend = endp;
	} else {
		rmesc = startp;
		rmescend = loc;
	}

	memmove(startp, rmesc, rmescend - rmesc);
	loc = startp + (rmescend - rmesc);

out:
	*loc = '\0';
	amount = loc - eb.dest;
	((eb.dest) += (amount));

	/* Remove any recorded regions beyond start of variable */
	TRY_SHV(removerecordregions(sh, startloc));

	return p;
}


/*
 * Expand a variable, and return a pointer to the next character in the
 * input string.
 */
static shell::Result<char *>
evalvar(ShellContext &sh, ExpandBuf &eb, char *p, int flag)
{
	int subtype;
	int varflags;
	char *var;
	int patloc;
	int startloc;
	ssize_t varlen;
	int discard;
	int quoted;
	int mbchar;

	varflags = *p++ & ~VSBIT;
	subtype = varflags & VSTYPE;

	quoted = flag & EXP_QUOTED;
	var = p;
	startloc = eb.dest - eb.base();
	p = strchr(p, '=') + 1;

	mbchar = 0;
	switch (subtype) {
	case VSTRIMLEFT:
	case VSTRIMLEFTMAX:
	case VSTRIMRIGHT:
	case VSTRIMRIGHTMAX:
		mbchar = EXP_MBCHAR;
		break;
	}

again:
	varlen = TRY_SH(varvalue(sh, eb, var, varflags, flag | mbchar));
	if (varflags & VSNUL)
		varlen--;

	discard = varlen < 0 ? EXP_DISCARD : 0;

	switch (subtype) {
	case VSPLUS:
		discard ^= EXP_DISCARD;
		/* fall through */

	case 0:
	case VSMINUS:
		p = TRY_SH(argstr(sh, eb, p, flag | EXP_TILDE | EXP_WORD |
			      (discard ^ EXP_DISCARD)));
		goto record;

	case VSASSIGN:
	case VSQUESTION:
		p = TRY_SH(subevalvar(sh, eb, p, var, 0, startloc, varflags,
			       (flag & ~QUOTES_ESC) |
			       (discard ^ EXP_DISCARD)));

		if ((flag | ~discard) & EXP_DISCARD)
			goto record;

		varflags &= ~VSNUL;
		subtype = VSNORMAL;
		goto again;
	}

	if ((discard & ~flag) && uflag)
		return varunset(sh, p, var, 0, 0);

	if (subtype == VSLENGTH) {
		p++;
		if (flag & EXP_DISCARD)
			return p;
		cvtnum(sh, eb, varlen > 0 ? varlen : 0, flag);
		goto really_record;
	}

	if (subtype == VSNORMAL)
		goto record;

	flag |= discard;
	if (!(flag & EXP_DISCARD)) {
		/*
		 * Terminate the string and start recording the pattern
		 * right after it
		 */
		((eb.dest) = eb.putc(eb.dest, '\0'));
	}

	patloc = eb.dest - eb.base();
	p = TRY_SH(subevalvar(sh, eb, p, nullptr, patloc, startloc, varflags, flag));

record:
	if ((flag | discard) & EXP_DISCARD)
		return p;

really_record:
	if (quoted) {
		quoted = *var == '@' && !sh.shellparam.p.empty();
		if (!quoted)
			return p;
	}
	recordregion(sh, startloc, eb.dest - eb.base(), quoted);
	return p;
}

static char *
chtodest(ShellContext &sh, int c, const char *syntax, char *out)
{
	if (syntax[c] == CCTL)
		(*(out)++ = (CTLESC));
	(*(out)++ = (c));

	return out;
}

static struct mbpair
mbtodest(ShellContext &sh, const char *p, char *q, const char *syntax,
			      size_t len)
{
	mbstate_t mbs = {};
	struct mbpair mbp;
	char *q0 = q;
	size_t ml;

	ml = mbrlen(--p, len, &mbs);
	if (ml == -2 || ml == -1 || ml < 2) {
		q = chtodest(sh, (signed char)*p, syntax, q);
		ml = 1;
		goto out;
	}

	if (syntax[CTLMBCHAR] == CCTL) {
		(*(q)++ = (CTLMBCHAR));
		(*(q)++ = (ml));
	}

	q = static_cast<char *>(mempcpy(q, p, ml));

	if (syntax[CTLMBCHAR] == CCTL) {
		(*(q)++ = (ml));
		(*(q)++ = (CTLMBCHAR));
	}

out:
	mbp.ml = ml - 1;
	mbp.ql = q - q0;
	return mbp;
}

/*
 * Put a string on the stack.
 */

static size_t
memtodest(ShellContext &sh, ExpandBuf &eb, const char *p, size_t len, int flags)
{
	const char *syntax;
	size_t count = 0;
	int expq;
	char *q;

	if (unlikely(!len))
		return 0;

	/* CTLMBCHAR, 2, c, c, 2, CTLMBCHAR */
	q = eb.makespace(len * 3, eb.dest);

#if QUOTES_ESC != 0x11 || EXP_MBCHAR != 0x20 || EXP_QUOTED != 0x100
#error QUOTES_ESC != 0x11 || EXP_MBCHAR != 0x20 || EXP_QUOTED != 0x100
#endif
	expq = flags & EXP_QUOTED;
	if (likely(!(flags & (expq >> 3 | expq >> 4 | expq >> 8) &
		     (QUOTES_ESC | EXP_MBCHAR)))) {
		while (len >= 8) {
			uint64_t x;

			memcpy(&x, p + count, sizeof(x));

			if ((x | (x - 0x0101010101010101)) &
			    0x8080808080808080)
				break;

			memcpy(q + count, &x, sizeof(x));

			count += 8;
			len -= 8;
		}

		q += count;
		p += count;

		syntax = flags & (QUOTES_ESC | EXP_MBCHAR) ?
			 BASESYNTAX : is_type;
	} else
		syntax = SQSYNTAX;

	for (; len; len--) {
		int c = (signed char)*p++;

		if (unlikely(!c && !(flags & EXP_KEEPNUL)))
			continue;

		count++;

		if (unlikely(c < 0)) {
			struct mbpair mbp = mbtodest(sh, p, q, syntax, len);
			unsigned mlm;

			q += mbp.ql;
			mlm = mbp.ml;
			p += mlm;
			len -= mlm;
			continue;
		}

		q = chtodest(sh, c, syntax, q);
	}

	eb.dest = q;
	return count;
}


static size_t
strtodest(ShellContext &sh, ExpandBuf &eb, const char *p, int flags)
{
	size_t len = strlen(p);
	return memtodest(sh, eb, p, len, flags);
}



/*
 * Add the value of a specialized variable to the stack string.
 */

static shell::Result<ssize_t>
varvalue(ShellContext &sh, ExpandBuf &eb, char *name, int varflags, unsigned flags)
{
	int subtype = varflags & VSTYPE;
	unsigned long seplen;
	const char *seps;
	ssize_t len = 0;
	size_t start;
	int discard;
	int num;
	char *p;
	int i;

	discard = (subtype == VSPLUS || subtype == VSLENGTH) |
		  (flags & EXP_DISCARD);

	if (unlikely(!subtype)) {
		if (discard)
			return -1;

		return sh_error(sh, "Bad substitution");
	}

	flags &= discard ? ~QUOTES_ESC : ~0;
	seps = "";
	seplen = flags & EXP_FULL;
	start = eb.dest - eb.base();

	switch (*name) {
	case '$':
		num = sh.rootpid;
		goto numvar;
	case '?':
		num = sh.exitstatus;
		goto numvar;
	case '#':
		num = static_cast<int>(sh.shellparam.p.size());
		goto numvar;
	case '!':
		num = sh.backgndpid;
		if (num == 0)
			return -1;
numvar:
		len = cvtnum(sh, eb, num, flags);
		break;
	case '-':
		p = eb.makespace(NOPTS, eb.dest);
		for (i = NOPTS - 1; i >= 0; i--) {
			if (sh.opts.list[i] && optletters[i]) {
				(*(p)++ = (optletters[i]));
				len++;
			}
		}
		eb.dest = p;
		break;
	case '@':
		if ((flags & (EXP_QUOTED | EXP_FULL)) ==
		    (EXP_QUOTED | EXP_FULL))
			goto param;
		/* fall through */
	case '*':
		/* We will set seplen to 0 or !0 depending on whether
		 * we're doing field splitting.  We won't do field
		 * splitting if either we're quoted or seplen is zero.
		 *
		 * Instead of testing (quoted || !sep) the following
		 * trick optimises away any branches by using the
		 * fact that EXP_QUOTED (which is the only bit that
		 * can be set in quoted) is the same as EXP_FULL <<
		 * CHAR_BIT (which is the only bit that can be set
		 * in sep).
		 */
#if EXP_QUOTED >> CHAR_BIT != EXP_FULL
#error The following two lines expect EXP_QUOTED == EXP_FULL << CHAR_BIT
#endif
		seplen &= ~(flags >> CHAR_BIT);
		if (!seplen)
			seps = sh.expand.ncifs;
		seplen = ((seplen - 1) & (sh.expand.ifsmb0len - 1)) + 1;
param:
		if (sh.shellparam.p.empty())
			return -1;
		{
			bool first = true;
			for (const auto &param : sh.shellparam.p) {
				if (!first)
					len += memtodest(sh, eb, seps, seplen,
							 flags | EXP_KEEPNUL);
				first = false;
				len += strtodest(sh, eb, param.c_str(), flags);
			}
		}
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		num = atoi(name);
		if (num < 0 ||
		    static_cast<size_t>(num) > sh.shellparam.p.size())
			return -1;
		p = const_cast<char *>(num ? sh.shellparam.p[num - 1].c_str()
					   : sh.arg0.c_str());
		goto value;
	default:
		p = sh.get_var(name);
value:
		if (!p)
			return -1;

		len = strtodest(sh, eb, p, flags);
		break;
	}

	if (discard)
		eb.dest = eb.base() + start;

	return len;
}



/*
 * Record the fact that we have to scan this region of the
 * string for IFS characters.
 */

void
recordregion(ShellContext &sh, int start, int end, int nulonly)
{
	INTOFF(sh);
	sh.expand.ifsregions.push_back({start, end, nulonly});
	INTON_DEFER(sh);
}

static unsigned
ifsisifs(ShellContext &sh, const char *p, unsigned ml, const char *ifs)
{
	bool isdefifs = false;
	bool isifs = false;
	wchar_t wc = *p;
	wchar_t ifs0;

	if (likely(ifs[0]) && unlikely(!sh.expand.wcifs.empty())) {
		if (wc & 0x80) {
			mbstate_t mbst = {};
			wchar_t wc2;

			if (mbrtowc(&wc2, p, ml, &mbst) != ml)
				goto out;
			wc = wc2;
		}

		isifs = sh.expand.wcifs.find(wc) != std::wstring::npos;
		ifs0 = sh.expand.wcifs[0];
	} else if (likely(!ml)) {
		isifs = strchr(ifs, wc);
		ifs0 = ifs[0];
	}

	if (isifs)
		isdefifs = iswspace(wc ?: ifs0);

out:
	return isifs << 1 | isdefifs;
}

static char *
ifsbreakup_slow(ShellContext &sh, struct ifs_state *ifst, ArgList *alist,
			     int nulonly, char *p)
{
	unsigned ifschar;
	unsigned sisifs;
	bool isdefifs;
	unsigned ml;
	bool isifs;
	char *q;

	q = p;

	ifschar = mbnext(sh, p);
	p += ifschar & 0xff;
	ml = (ifschar >> 8) > 3 ?
	     (ifschar >> 8) - 2 : 0;

	sisifs = ifsisifs(sh, p, ml, ifst->ifs);
	p += ifschar >> 8;

	isifs = sisifs >> 1;
	isdefifs = sisifs & 1;

	/* If only reading one more argument:
	 * If we have exactly one field,
	 * read that field without its terminator.
	 * If we have more than one field,
	 * read all fields including their terminators,
	 * except for trailing IFS whitespace.
	 *
	 * This means that if we have only IFS
	 * characters left, and at most one
	 * of them is non-whitespace, we stop
	 * reading here.
	 * Otherwise, we read all the remaining
	 * characters except for trailing
	 * IFS whitespace.
	 *
	 * In any case, r indicates the start
	 * of the characters to remove, or nullptr
	 * if no characters should be removed.
	 */
	if (!ifst->maxargs) {
		if (isdefifs) {
			if (!ifst->r)
				ifst->r = q;
			return p;
		}

		if (!(isifs && ifst->ifsspc))
			ifst->r = nullptr;
	} else if (ifst->ifsspc) {
		if (isifs)
			q = p;

		ifst->start = q;

		if (isdefifs)
			return p;
	} else if (isifs) {
		int ifsspc = ifst->ifsspc;

		if (!nulonly) {
			ifsspc = isdefifs;
			ifst->ifsspc = ifsspc;
		}

		/* Ignore IFS whitespace at start */
		if (q == ifst->start && ifsspc) {
			ifst->start = p;
			goto out_zero_ifsspc;
		}
		if (ifst->maxargs > 0 && !--ifst->maxargs) {
			ifst->r = q;
			return p;
		}
		*q = '\0';
		alist->emplace_back(ifst->start);
		ifst->start = p;
		return p;
	}

out_zero_ifsspc:
	ifst->ifsspc = 0;
	return p;
}

/*
 * Break the argument string into pieces based upon IFS and add the
 * strings to the argument list.  The regions of the string to be
 * searched for IFS characters have been stored by recordregion.
 * If maxargs is non-negative, at most maxargs arguments will be created, by
 * joining together the last arguments.
 */
void
ifsbreakup(ShellContext &sh, char *string, int maxargs, ArgList *alist)
{
	struct ifs_state ifst;
	const char *realifs;
	int nulonly;
	char *p;

	ifst.r = nullptr;
	ifst.start = string;
	ifst.maxargs = maxargs;
	if (!sh.expand.ifsregions.empty()) {
		ifst.ifsspc = 0;
		nulonly = 0;
		realifs = sh.expand.ncifs;
		for (const auto &reg : sh.expand.ifsregions) {
			int afternul;

			p = string + reg.begoff;
			afternul = nulonly;
			nulonly = reg.nulonly;
			ifst.ifs = nulonly ? "" : realifs;
			ifst.ifsspc = 0;
			for (;;) {
				char *p0 = p;

				while (string + reg.endoff - p >= 8) {
					union {
						uint64_t qw;
						unsigned char b[8];
					} x;

					memcpy(&x.qw, p, sizeof(x.qw));

					if ((x.qw & 0x8080808080808080))
						break;
					if (sh.expand.ifsmap[x.b[0]] |
					    sh.expand.ifsmap[x.b[1]] |
					    sh.expand.ifsmap[x.b[2]] |
					    sh.expand.ifsmap[x.b[3]] |
					    sh.expand.ifsmap[x.b[4]] |
					    sh.expand.ifsmap[x.b[5]] |
					    sh.expand.ifsmap[x.b[6]] |
					    sh.expand.ifsmap[x.b[7]])
						break;
					p += 8;
				}

				if (p != p0) {
					if (!ifst.maxargs)
						ifst.r = nullptr;
					else if (ifst.ifsspc)
						ifst.start = p0;
					ifst.ifsspc = 0;
				}

				if (p >= string + reg.endoff)
					break;

				p = ifsbreakup_slow(sh, &ifst, alist,
						    afternul | nulonly, p);
			}
		}
		if (nulonly)
			goto add;
		if (ifst.r)
			*ifst.r = '\0';
	}

	if (!*ifst.start)
		return;

add:
	alist->emplace_back(ifst.start);
}

void
ifsfree(ShellContext &sh)
{
	if (sh.expand.ifsregions.empty())
		return;

	INTOFF(sh);
	sh.expand.ifsregions.clear();
	INTON_DEFER(sh);
}

void
changeifs(ShellContext &sh, std::string_view ifs_in)
{
	mbstate_t mbs = {};
	std::wstring nwcifs;
	unsigned mb = 0;
	size_t len = 0;
	const char *p;
	size_t ml;
	const char *ifs = ifs_in.data();

	if (!ifsset(sh))
		ifs = defifs;
	sh.expand.ncifs = ifs;

	memset(sh.expand.ifsmap, 0, sizeof(sh.expand.ifsmap));

	for (p = ifs;; p++) {
		unsigned c = (unsigned char)*p;

		mb |= c >> 7;
		if (!(c >> 7))
			sh.expand.ifsmap[c] = 1;

		if (c == 0)
			break;

		len++;
	}

	sh.expand.ifsmb0len = !!len;

	if (!mb)
		goto out;

	ml = mbrlen(ifs, len, &mbs);
	if (ml == -2 || ml == -1)
		ml = 1;
	sh.expand.ifsmb0len = ml;

	nwcifs.assign(len + 1, L'\0');
	p = ifs;
	{
		size_t n = mbsrtowcs(nwcifs.data(), &p, len + 1, &mbs);
		if (n == static_cast<size_t>(-1))
			nwcifs.clear();
		else
			nwcifs.resize(n);
	}

out:
	sh.expand.wcifs = std::move(nwcifs);
}

/*
 * Expand shell metacharacters.  At this point, the only control characters
 * should be escapes.  The results are stored in the list sh.expand.exparg.
 */

#ifdef __GLIBC__
static void *
opendir_interruptible(ShellContext &sh, const char *pathname)
{
	if (int_pending(sh)) {
		sh.suppressint = 0;
		onint(sh);
	}

	return opendir(pathname);
}
#else
#define GLOB_ALTDIRFUNC 0
#endif

static shell::Result<void>
expandmeta_glob(ShellContext &sh, ArgList &strs)
{
	ArgList out;

	for (std::string &s : strs) {
		const char *p;
		glob64_t pglob;
		int i;

		if (fflag)
			goto nometa;

#ifdef __GLIBC__
		pglob.gl_closedir = reinterpret_cast<void (*)(void *)>(closedir);
		pglob.gl_readdir = reinterpret_cast<struct dirent64 *(*)(void *)>(
			readdir64);
		pglob.gl_opendir = [](const char *pathname) -> void * {
			return opendir_interruptible(*ShellContext::signaled, pathname);
		};
		pglob.gl_lstat = lstat64;
		pglob.gl_stat = stat64;
#endif

		INTOFF(sh);
		p = preglob(sh, s.c_str(), RMESCAPE_HEAP);
		i = glob64(p, GLOB_ALTDIRFUNC | GLOB_NOMAGIC, 0, &pglob);
		if (p != s.c_str())
			ckfree(p);
		switch (i) {
		case 0:
			if ((pglob.gl_flags & (GLOB_NOMAGIC | GLOB_NOCHECK)) ==
			    (GLOB_NOMAGIC | GLOB_NOCHECK))
				goto nometa2;
			sh.expand.exparg.clear();
			addglob(sh, &pglob);
			out.insert(out.end(),
				   std::make_move_iterator(sh.expand.exparg.begin()),
				   std::make_move_iterator(sh.expand.exparg.end()));
			sh.expand.exparg.clear();
			globfree64(&pglob);
			INTON_DEFER(sh);
			break;
		case GLOB_NOMATCH:
nometa2:
			globfree64(&pglob);
			INTON_DEFER(sh);
nometa:
			rmescapes(s.data());
			out.push_back(std::move(s));
			break;
		default:	/* GLOB_NOSPACE */
			return sh_error(sh, "Out of space");
		}
	}
	strs = std::move(out);
	return {};
}


/*
 * Add the result of glob(3) to the list.
 */

static void
addglob(ShellContext &sh, const glob64_t *pglob)
{
	char **p = pglob->gl_pathv;

	do {
		addfname(sh, *p);
	} while (*++p);
}

static shell::Result<void>
expandmeta(ShellContext &sh, ExpandBuf &eb, ArgList &strs)
{
	/* TODO - EXP_REDIR */

	if (GLOB_IS_ENABLED)
		return expandmeta_glob(sh, strs);

	ArgList out;
	for (std::string &s : strs) {
		char *p;
		unsigned len;

		if (fflag)
			goto nometa;
		if (!strpbrk(s.c_str(), "*?]") || !strcmp(s.c_str(), "]"))
			goto nometa;

		INTOFF(sh);
		p = preglob(sh, s.c_str(), RMESCAPE_ALLOC | RMESCAPE_HEAP);
		len = strlen(p);

		sh.expand.exparg.clear();
		expmeta(sh, eb, p, len, 0);
		if (p != s.c_str())
			ckfree(p);
		INTON(sh);
		if (sh.expand.exparg.empty()) {
			/*
			 * no matches
			 */
nometa:
			rmescapes(s.data());
			out.push_back(std::move(s));
		} else {
			std::sort(sh.expand.exparg.begin(), sh.expand.exparg.end(),
				  [](const std::string &a, const std::string &b) {
					  return strcoll(a.c_str(), b.c_str()) < 0;
				  });
			out.insert(out.end(),
				   std::make_move_iterator(sh.expand.exparg.begin()),
				   std::make_move_iterator(sh.expand.exparg.end()));
			sh.expand.exparg.clear();
		}
	}
	strs = std::move(out);
	return {};
}

static void
addfname_common(ShellContext &sh, char *name)
{
	sh.expand.exparg.emplace_back(name);
}

static char *
addfnamealt(ShellContext &sh, ExpandBuf &eb, char *enddir, size_t expdir_len)
{
	char *name;

	name = (eb.commit_to(enddir));
	addfname_common(sh, name);

	enddir = eb.base();
	return eb.nputs(name, expdir_len, enddir) - expdir_len;
}

static char *
expmeta_rmescapes(ShellContext &sh, char *enddir, const char *name)
{
	const char *p;

	if (!FNMATCH_IS_ENABLED)
		return strchrnul(rmescapes(strcpy(enddir, name)), 0);

	p = name;
	do {
		const char *q = strchrnul(p, '\\');

		enddir = static_cast<char *>(mempcpy(enddir, p, q - p + 1));
		p = q;
		if (!*p)
			break;
		if (*++p)
			enddir[-1] = *p++;
	} while (1);

	return enddir - 1;
}

#ifndef HAVE_MEMRCHR
static void *
memrchr(ShellContext &sh, const void *s, int c, size_t n)
{
	const unsigned char *str = s;
	const unsigned char *cp;

	for (cp = str + n - 1; cp >= str; cp--)
		if (*cp == c)
			return cp;
	return nullptr;
}
#endif

/*
 * Do metacharacter (i.e. *, ?, [...]) expansion.
 */

static char *
expmeta(ShellContext &sh, ExpandBuf &eb, char *name, unsigned name_len, size_t expdir_len)
{
	const char mesc = FNMATCH_IS_ENABLED ? '\\' : CTLESC;
	struct stat64 statb;
	struct dirent64 *dp;
	char *endname;
	char *zeroedp;
	char *enddir;
	int matchdot;
	unsigned esc;
	char *start;
	size_t len;
	DIR *dirp = nullptr;
	char *pat;
	char *cp = nullptr;
	char *p;
	int c;

	len = expdir_len + name_len + 1;
	eb.ensure_avail(len);
	cp = eb.base();
	enddir = cp + expdir_len;

	p = name;
	esc = 0;
	do {
		p = strpbrk(p + esc, "*?]");
		if (!p)
			break;
		esc = mesclen(sh, name, p, mesc) & 1;
	} while (esc);
	/* No meta characters */
	if (likely(!p)) {
		if (!expdir_len)
			goto out_opendir;
		enddir = expmeta_rmescapes(sh, enddir, name);
		if (lstat64(cp, &statb) >= 0)
			cp = addfnamealt(sh, eb, enddir + 1, expdir_len);
		goto out_opendir;
	}
	start = static_cast<char *>(memrchr(name, '/', static_cast<size_t>(p - name)));
	if (start) {
		c = *++start;
		*start = 0;
		enddir = expmeta_rmescapes(sh, enddir, name);
		*start = c;
		expdir_len = enddir - cp;
	} else
		start = name;
	*enddir = 0;

	dirp = opendir(expdir_len ? cp : ".");
	if (!dirp)
		goto out_opendir;
	esc = 0;
	p = strchrnul(p + 1, '/');
	zeroedp = p;
	endname = p;
	if (*p) {
		esc = mesclen(sh, name, p, mesc) & 1;
		zeroedp -= esc;
		endname++;
	}
	c = *zeroedp;
	*zeroedp = '\0';
	name_len -= endname - name;
	matchdot = 0;
	pat = start;
	p = pat;
	if (*p == mesc)
		p++;
	if (*p == '.')
		matchdot++;
	while ((dp = readdir64(dirp))) {
		char *dname = dp->d_name;

		if (*dname == '.' && !matchdot)
			goto check_int;
		if (c && dp->d_type != DT_DIR && dp->d_type != DT_LNK &&
		    dp->d_type != DT_UNKNOWN)
			goto check_int;
		len = strlen(dname) + 1;
		p = dname;
		if (!FNMATCH_IS_ENABLED) {
			eb.dest = enddir;
			memtodest(sh, eb, p, len, EXP_MBCHAR | EXP_KEEPNUL);
			cp = eb.base();
			enddir = cp + expdir_len;
			p = enddir;
		}
		if (pmatch(sh, pat, p)) {
			enddir = eb.nputs(dname, len, enddir);
			if (!c)
				cp = addfnamealt(sh, eb, enddir, expdir_len);
			else {
				enddir[-1] = '/';
				len += expdir_len;
				cp = expmeta(sh, eb, endname, name_len, len);
			}
			enddir = cp + expdir_len;
		}
check_int:
		if (int_pending(sh))
			break;
	}
	*zeroedp = c;

	if (dirp)
		closedir(dirp);
out_opendir:
	return cp;
}


/*
 * Add a file name to the list.
 */

static void
addfname(ShellContext &sh, char *name)
{
	addfname_common(sh, name);
}




/*
 * Returns true if the pattern matches the string.
 */

static inline int
patmatch(ShellContext &sh, char *pattern, const char *string, ExpandBuf *eb)
{
	return pmatch(sh, preglob(sh, pattern, 0, eb), string);
}


static __attribute__((noinline)) int
ccmatch(ShellContext &sh, char *p, const char *mbc, int ml,
					     char **r)
{
	mbstate_t mbst = {};
	wctype_t type;
	wchar_t wc;
	char *q;

	*r = 0;

	if (*p++ != ':')
		return 0;

	q = strstr(p, ":]");
	if (!q)
		return 0;

	*q = 0;
	type = wctype(p);
	*q = ':';

	if (!type)
		return 0;

	*r = q + 2;

	if (mbrtowc(&wc, mbc, ml, &mbst) != ml)
		return 0;

	return iswctype(wc, type);
}

static int
pmatch(ShellContext &sh, char *pattern, const char *string)
{
	const char *q;
	unsigned mb;
	char *p;
	char c;

	if (FNMATCH_IS_ENABLED)
		return !fnmatch(pattern, string, 0);

	p = pattern;
	q = string;
	for (;;) {
		switch ((signed char)(c = *p++)) {
		case '\0':
			goto breakloop;
		case CTLESC:
			c = *p++;
			break;
		case '?':
			if (*q == '\0')
				return 0;
			mb = mbnext(sh, q);
			q += (mb >> 8) + (mb & 0xff);
			continue;
		case '*':
			c = *p;
			while (c == '*')
				c = *++p;
			if (!c)
				return 1;
			if (c == '?' || c == '[')
				c = CTLESC;
			for (;;) {
				if (c != (char)CTLESC) {
					/* Stop should be null-terminated
					 * as it is passed as a string to
					 * strpbrk(3).
					 */
					const char stop[] = {
						c, CTLESC, CTLMBCHAR, 0,
					};
					q = strpbrk(q, stop);
					if (!q)
						return 0;
				}
				if (pmatch(sh, p, q))
					return 1;
				if (!*q)
					break;
				mb = mbnext(sh, q);
				q += (mb >> 8) + (mb & 0xff);
			}
			return 0;
		case '[': {
			char *startp;
			int invert, found;
			char chr;

			startp = p;
			invert = 0;
			if (*p == '!' || *p == '^') {
				invert++;
				p++;
			}
			found = 0;
			mb = mbnext(sh, q);
			q += mb & 0xff;
			mb >>= 8;
			chr = *q;
			if (chr == '\0')
				return 0;
			c = *p++;
			do {
				unsigned mbp = 0;
				const char *mbs = &c;

				if (!c) {
					p = startp;
					c = '[';
					goto dft;
				}
				if (c == '[') {
					char *r;

					found |= !!ccmatch(sh, p, q, mb > 1 ?
								 mb - 2 : mb,
							   &r);
					if (r) {
						p = r;
						continue;
					}
				} else if (c == (char)CTLESC)
					c = *p++;
				else if (c == (char)CTLMBCHAR) {
					mbp = mbnext(sh, --p);
					p += mbp & 0xff;
					mbs = p;
					mbp >>= 8;
					p += mbp;
				}
				if (*p == '-' && p[1] && p[1] != ']') {
					p++;
					if (*p == (char)CTLESC)
						p++;
					else if (*p == (char)CTLMBCHAR) {
						mbp = mbnext(sh, p);
						p += mbp & 0xff;
						p += mbp >> 8;
						continue;
					}
					if (!(mbp | (mb - 1)) &&
					    chr >= c && chr <= *p)
						found = 1;
					p++;
				} else if (!strncmp(mbs, q, mb))
					found = 1;
			} while ((c = *p++) != ']');
			if (found == invert)
				return 0;
			q += mb;
			continue;
		}
		case CTLMBCHAR:
			mb = mbnext(sh, --p);
			p += mb & 0xff;
			mb = mbnext(sh, q);
			q += mb & 0xff;
			mb >>= 8;

			if (strncmp(p - 1, q - 1, mb + 1))
				return 0;

			p += mb;
			q += mb;
			continue;
		}
dft:
		mb = mbnext(sh, q);
		if ((mb >> 8) > 1)
			return 0;
		q += mb & 0xff;
		if (*q != c)
			return 0;
		q += mb >> 8;
	}
breakloop:
	if (*q != '\0')
		return 0;
	return 1;
}



/*
 * Remove any CTLESC characters from a string.
 */

char *
_rmescapes(ShellContext &sh, char *str, int flag, ExpandBuf *eb)
{
	char *p, *q, *r;
	int notescaped;
	int globbing;
	int inquotes;

	p = strpbrk(str, cqchars);
	if (!p) {
		return str;
	}
	q = p;
	r = str;
	globbing = flag & RMESCAPE_GLOB;

	if (flag & RMESCAPE_ALLOC) {
		size_t len = p - str;
		size_t fulllen = strlen(p);

		if (FNMATCH_IS_ENABLED && globbing)
			fulllen *= 2;

		fulllen += len + 1;

		if ((flag & RMESCAPE_GROW) && eb) {
			int strloc = str - eb->base();

			r = eb->makespace(fulllen, eb->dest);
			str = eb->base() + strloc;
			p = str + len;
		} else {
			r = ckmalloc(fulllen);
		}
		q = r;
		if (len > 0) {
			q = static_cast<char *>(mempcpy(q, str, len));
		}
	}
	inquotes = 0;
	notescaped = globbing;
	while (*p) {
		int c = (signed char)*p;
		int newnesc = globbing;
		unsigned mb;
		unsigned ml;

		if (c == CTLQUOTEMARK) {
			p++;
			inquotes ^= globbing;
			continue;
		} else if (c == '\\') {
			/* naked back slash */
			newnesc ^= notescaped;
			/* naked backslashes can only occur outside quotes */
			inquotes = 0;
			if (!FNMATCH_IS_ENABLED && notescaped)
				c = CTLESC;
		} else if (c == CTLESC) {
			if ((notescaped ^ inquotes) & inquotes) {
				if (FNMATCH_IS_ENABLED)
					*q++ = '\\';
				else
					q[-1] = '\\';
			}
			if (globbing)
				*q++ = FNMATCH_IS_ENABLED ? '\\' : CTLESC;

			c = *++p;
		} else if (c == CTLMBCHAR) {
			unsigned tail = 2;

			if (!FNMATCH_IS_ENABLED && (globbing ^ notescaped))
				q--;

			mb = mbnext(sh, p);
			ml = mb >> 8;

			if (!globbing || FNMATCH_IS_ENABLED) {
				p += mb & 0xff;
				ml -= 2;
			} else {
				ml += mb & 0xff;
				tail = 0;
			}

			memmove(q, p, ml);
			q += ml;
			p += ml + tail;
			goto setnesc;
		}

		*q++ = c;
		p++;
setnesc:
		notescaped = newnesc;
	}
	if (!FNMATCH_IS_ENABLED && (globbing ^ notescaped))
		q[-1] = '\\';
	*q = '\0';
	if ((flag & RMESCAPE_GROW) && eb) {
		eb->dest = r;
		eb->dest += (q - r + 1);
	}
	return r;
}



/*
 * See if a pattern matches in a case statement.
 */

shell::Result<int>
casematch(ShellContext &sh, Node *pattern, char *val)
{
	ExpandBuf eb;
	int result;

	sh.expand.argbackq = &as<Node::Arg>(pattern)->backquote;
	sh.expand.argbackq_i = 0;
	eb.start();
	TRY_SHV(argstr(sh, eb, as<Node::Arg>(pattern)->text.data(), EXP_TILDE | EXP_CASE));
	ifsfree(sh);
	result = patmatch(sh, eb.base(), val, &eb);
	return result;
}

/**
 * Our own itoa().
 */
static std::size_t
cvtnum(ShellContext &sh, ExpandBuf &eb, std::intmax_t num, int flags)
{
	auto buf = std::format("{}", num);

	return memtodest(sh, eb, buf.c_str(), buf.size(), flags);
}

static tl::unexpected<shell::Error>
varunset(ShellContext &sh, const char *end, const char *var, const char *umsg, int varflags)
{
	const char *msg = "parameter not set";
	const char *tail = "";

	if (umsg) {
		if (*end == (char)CTLENDVAR) {
			if (varflags & VSNUL)
				tail = " or null";
		} else
			msg = umsg;
	}
	return sh_error(sh, "{}: {}{}", std::string_view(var, end - var - 1), msg, tail);
}

shell::Result<void>
restore_handler_expandarg(ShellContext &sh, int err)
{
	if (err) {
		if (sh.exception != shell::Error::Generic)
			return exraise(sh, sh.exception.value_or(shell::Error::Interrupt));
		ifsfree(sh);
	}
	return {};
}

#ifdef mkinit

INCLUDE "expand.h"

EXITRESET {
	ifsfree(sh);
}

#endif
