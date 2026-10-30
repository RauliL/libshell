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

#include <ctype.h>
#include <dirent.h>
#ifdef HAVE_FNMATCH
#include <fnmatch.h>
#endif
#ifdef HAVE_GLOB
#include <glob.h>
#endif
#include <inttypes.h>
#include <limits.h>
#ifdef HAVE_GETPWNAM
#include <pwd.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/*
 * Routines to expand arguments to commands.  We have to deal with
 * backquotes, shell variables, and file metacharacters.
 */

#include "error.h"
#include "eval.h"
#include "expand.h"
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
#include "show.h"
#include "syntax.h"
#include "system.h"
#include "var.h"

/*
 * _rmescape() flags
 */
#define RMESCAPE_ALLOC	0x1	/* Allocate a new string */
#define RMESCAPE_GLOB	0x2	/* Add backslashes for glob */
#define RMESCAPE_GROW	0x8	/* Grow strings instead of stalloc */
#define RMESCAPE_HEAP	0x10	/* Malloc strings instead of stalloc */

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







static char * argstr(ShellContext &sh, char *p, int flag);
static char * exptilde(ShellContext &sh, char *startp, int flag);
static char * expari(ShellContext &sh, char *start, int flag);
static void expbackq(ShellContext &sh, union node *, int);
static char * evalvar(ShellContext &sh, char *, int);
static size_t strtodest(ShellContext &sh, const char *p, int flags);
static size_t memtodest(ShellContext &sh, const char *p, size_t len, int flags);
static ssize_t varvalue(ShellContext &sh, char *, int, unsigned);
static void expandmeta(ShellContext &sh, struct strlist *);
static void addglob(ShellContext &sh, const glob64_t *);
static char * expmeta(ShellContext &sh, char *, unsigned, size_t);
static struct strlist * expsort(ShellContext &sh, struct strlist *);
static struct strlist * msort(ShellContext &sh, struct strlist *, int);
static void addfname(ShellContext &sh, char *);
static int patmatch(ShellContext &sh, char *, const char *);
static int pmatch(ShellContext &sh, char *, const char *);
static size_t cvtnum(ShellContext &sh, intmax_t num, int flags);
static void varunset(ShellContext &sh, const char *, const char *, const char *, int)
	__attribute__((__noreturn__));


/*
 * Prepare a pattern for a glob(3) call.
 *
 * Returns an stalloced string.
 */

static inline char *
preglob(ShellContext &sh, const char *pattern, int flag)
{
	if (FNMATCH_IS_ENABLED) {
		if (!flag)
			flag = RMESCAPE_GROW;
		flag |= RMESCAPE_ALLOC;
	}
	flag |= RMESCAPE_GLOB;
	return _rmescapes(sh, (char *)pattern, flag);
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
 * placing the resulting list of arguments in arglist.  If EXP_FULL is true,
 * perform splitting and file name expansion.  When arglist is NULL, perform
 * here document expansion.
 */

void
expandarg(ShellContext &sh, union node *arg, struct arglist *arglist, int flag)
{
	struct strlist *sp;
	char *p;

	sh.argbackq = &arg->narg.backquote;
	sh.argbackq_i = 0;
	STARTSTACKSTR(sh.expdest);
	argstr(sh, arg->narg.text.data(), flag);
	if (arglist == NULL) {
		/* here document expanded */
		goto out;
	}
	p = grabstackstr(sh.expdest);
	sh.exparg.lastp = &sh.exparg.list;
	/*
	 * TODO - EXP_REDIR
	 */
	if (flag & EXP_FULL) {
		ifsbreakup(sh, p, -1, &sh.exparg);
		*sh.exparg.lastp = NULL;
		sh.exparg.lastp = &sh.exparg.list;
		expandmeta(sh, sh.exparg.list);
	} else {
		sp = (struct strlist *)stalloc(sh, sizeof (struct strlist));
		sp->text = p;
		*sh.exparg.lastp = sp;
		sh.exparg.lastp = &sp->next;
	}
	*sh.exparg.lastp = NULL;
	if (sh.exparg.list) {
		*arglist->lastp = sh.exparg.list;
		arglist->lastp = sh.exparg.lastp;
	}

out:
	ifsfree(sh);
}



/*
 * Perform variable and command substitution.  If EXP_FULL is set, sh.output CTLESC
 * characters to allow for further processing.  Otherwise treat
 * $@ like $* since no splitting will be performed.
 */

static char *
argstr(ShellContext &sh, char *p, int flag)
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
			p = exptilde(sh, p, flag);
	}
start:
	startloc = sh.expdest - (char *)stackblock();
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

			q = stnputs(sh, p, length, sh.expdest);
			q[-1] &= end - 1;
			sh.expdest = q - (flag & EXP_WORD ? end : 0);
			newloc = q - (char *)stackblock() - end;
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
				p = evalvar(sh, p + 1, flag | EXP_QUOTED) + 1;
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
				sh.expdest = stnputs(sh, p, ml, sh.expdest);
			p += mb >> 8;
			break;
		case CTLESC:
			startloc++;
			length++;
			goto addquote;
		case CTLVAR:
			p = evalvar(sh, p, flag | inquotes);
			goto start;
		case CTLBACKQ:
			expbackq(sh, (*sh.argbackq)[sh.argbackq_i], flag | inquotes);
			goto start;
		case CTLARI:
			p = expari(sh, p, flag | inquotes);
			goto start;
		}
	}
	return p - 1;
}

static char *
exptilde(ShellContext &sh, char *startp, int flag)
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
		home = lookupvar(sh, homestr);
	} else {
		home = getpwhome(sh, name);
	}
	*p = c;
	if (!home)
		goto lose;
	strtodest(sh, home, flag | EXP_QUOTED);
out:
	return (p);
lose:
	return (startp);
}


void
removerecordregions(ShellContext &sh, int endoff)
{
	if (sh.ifslastp == NULL)
		return;

	if (sh.ifsfirst.endoff > endoff) {
		while (sh.ifsfirst.next != NULL) {
			struct ifsregion *ifsp;
			INTOFF;
			ifsp = sh.ifsfirst.next->next;
			ckfree(sh.ifsfirst.next);
			sh.ifsfirst.next = ifsp;
			INTON;
		}
		if (sh.ifsfirst.begoff > endoff)
			sh.ifslastp = NULL;
		else {
			sh.ifslastp = &sh.ifsfirst;
			sh.ifsfirst.endoff = endoff;
		}
		return;
	}
	
	sh.ifslastp = &sh.ifsfirst;
	while (sh.ifslastp->next && sh.ifslastp->next->begoff < endoff)
		sh.ifslastp=sh.ifslastp->next;
	while (sh.ifslastp->next != NULL) {
		struct ifsregion *ifsp;
		INTOFF;
		ifsp = sh.ifslastp->next->next;
		ckfree(sh.ifslastp->next);
		sh.ifslastp->next = ifsp;
		INTON;
	}
	if (sh.ifslastp->endoff > endoff)
		sh.ifslastp->endoff = endoff;
}


/*
 * Expand arithmetic expression.  Backup to start of expression,
 * evaluate, place result in (backed up) result, adjust string position.
 */
static char *
expari(ShellContext &sh, char *start, int flag)
{
	int begoff;
	int endoff;
	int len;
	intmax_t result;
	char *p;

	p = stackblock();
	begoff = sh.expdest - p;
	p = argstr(sh, start, flag & EXP_DISCARD);

	if (flag & EXP_DISCARD)
		goto out;

	start = stackblock();
	endoff = sh.expdest - start;
	start += begoff;
	STADJUST(start - sh.expdest, sh.expdest);

	removerecordregions(sh, begoff);

	{
		std::size_t su = sh.stackstr.used;
		std::size_t hc = sh.stackheaps.size();
		sh.stackstr.commit(endoff);
		result = arith(sh, start);
		while (sh.stackheaps.size() > hc) {
			ckfree(sh.stackheaps.back());
			sh.stackheaps.pop_back();
		}
		sh.stackstr.rewind(su);
	}

	len = cvtnum(sh, result, flag);

	if (likely(!(flag & EXP_QUOTED)))
		recordregion(sh, begoff, begoff + len, 0);

out:
	return p;
}


/*
 * Expand stuff in backwards quotes.
 */

static void
expbackq(ShellContext &sh, union node *cmd, int flag)
{
	struct backcmd in;
	int i;
	char buf[128];
	char *p;
	char *dest;
	int startloc;

	if (flag & EXP_DISCARD)
		goto out;

	INTOFF;
	startloc = sh.expdest - (char *)stackblock();
	{
		std::size_t su = sh.stackstr.used;
		std::size_t hc = sh.stackheaps.size();
		sh.stackstr.commit(startloc);
		evalbackcmd(sh, cmd, (struct backcmd *) &in);
		while (sh.stackheaps.size() > hc) {
			ckfree(sh.stackheaps.back());
			sh.stackheaps.pop_back();
		}
		sh.stackstr.rewind(su);
	}

	p = in.buf.data();
	i = static_cast<int>(in.buf.size());
	if (i == 0)
		goto read;
	for (;;) {
		memtodest(sh, p, i, flag);
read:
		if (in.fd < 0)
			break;
		do {
			i = read(in.fd, buf, sizeof buf);
		} while (i < 0 && errno == EINTR);
		TRACE(("expbackq: read returns %d\n", i));
		if (i <= 0)
			break;
		p = buf;
	}

	if (in.fd >= 0) {
		close(in.fd);
		sh.back_exitstatus = waitforjob(sh, in.jp);
	}
	INTON;

	/* Eat all trailing newlines */
	dest = sh.expdest;
	for (; dest > ((char *)stackblock() + startloc) && dest[-1] == '\n';)
		STUNPUTC(dest);
	sh.expdest = dest;

	if (!(flag & EXP_QUOTED))
		recordregion(sh, startloc, dest - (char *)stackblock(), 0);
	TRACE(("evalbackq: size=%d: \"%.*s\"\n",
		(dest - (char *)stackblock()) - startloc,
		(dest - (char *)stackblock()) - startloc,
		stackblock() + startloc));

out:
	sh.argbackq_i++;
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

static char *
subevalvar(ShellContext &sh, char *start, char *str, int strloc, int startloc,
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

	p = argstr(sh, start, (flag & EXP_DISCARD) | EXP_TILDE |
			  (str ?  0 : EXP_CASE));
	if (flag & EXP_DISCARD)
		return p;

	startp = stackblock() + startloc;

	switch (subtype) {
	case VSASSIGN:
		setvar(sh, str, startp, 0);

		loc = startp;
		goto out;

	case VSQUESTION:
		varunset(sh, start, str, startp, varflags);
		/* NOTREACHED */
	}

	subtype -= VSTRIMRIGHT;
#ifdef DEBUG
	if (subtype < 0 || subtype > 3)
		abort();
#endif

	rmescend = stackblock() + strloc;
	str = preglob(sh, rmescend, 0);
	if (FNMATCH_IS_ENABLED) {
		startp = stackblock() + startloc;
		rmescend = stackblock() + strloc;
		nstrloc = str - (char *)stackblock();
	}

	rmesc = startp;
	if (FNMATCH_IS_ENABLED || !quotes) {
		rmesc = _rmescapes(sh, startp, RMESCAPE_ALLOC | RMESCAPE_GROW);
		if (rmesc != startp)
			rmescend = sh.expdest;
		startp = stackblock() + startloc;
		str = stackblock() + nstrloc;
	}
	rmescend--;

	/* zero = subtype == VSTRIMLEFT || subtype == VSTRIMLEFTMAX */
	zero = subtype >> 1;
	/* VSTRIMLEFT/VSTRIMRIGHTMAX -> scanleft */
	scan = (subtype & 1) ^ zero ? scanleft : scanright;

	endp = stackblock() + strloc - 1;
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
	amount = loc - sh.expdest;
	STADJUST(amount, sh.expdest);

	/* Remove any recorded regions beyond start of variable */
	removerecordregions(sh, startloc);

	return p;
}


/*
 * Expand a variable, and return a pointer to the next character in the
 * input string.
 */
static char *
evalvar(ShellContext &sh, char *p, int flag)
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
	startloc = sh.expdest - (char *)stackblock();
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
	varlen = varvalue(sh, var, varflags, flag | mbchar);
	if (varflags & VSNUL)
		varlen--;

	discard = varlen < 0 ? EXP_DISCARD : 0;

	switch (subtype) {
	case VSPLUS:
		discard ^= EXP_DISCARD;
		/* fall through */

	case 0:
	case VSMINUS:
		p = argstr(sh, p, flag | EXP_TILDE | EXP_WORD |
			      (discard ^ EXP_DISCARD));
		goto record;

	case VSASSIGN:
	case VSQUESTION:
		p = subevalvar(sh, p, var, 0, startloc, varflags,
			       (flag & ~QUOTES_ESC) |
			       (discard ^ EXP_DISCARD));

		if ((flag | ~discard) & EXP_DISCARD)
			goto record;

		varflags &= ~VSNUL;
		subtype = VSNORMAL;
		goto again;
	}

	if ((discard & ~flag) && uflag)
		varunset(sh, p, var, 0, 0);

	if (subtype == VSLENGTH) {
		p++;
		if (flag & EXP_DISCARD)
			return p;
		cvtnum(sh, varlen > 0 ? varlen : 0, flag);
		goto really_record;
	}

	if (subtype == VSNORMAL)
		goto record;

#ifdef DEBUG
	switch (subtype) {
	case VSTRIMLEFT:
	case VSTRIMLEFTMAX:
	case VSTRIMRIGHT:
	case VSTRIMRIGHTMAX:
		break;
	default:
		abort();
	}
#endif

	flag |= discard;
	if (!(flag & EXP_DISCARD)) {
		/*
		 * Terminate the string and start recording the pattern
		 * right after it
		 */
		STPUTC('\0', sh.expdest);
	}

	patloc = sh.expdest - (char *)stackblock();
	p = subevalvar(sh, p, NULL, patloc, startloc, varflags, flag);

record:
	if ((flag | discard) & EXP_DISCARD)
		return p;

really_record:
	if (quoted) {
		quoted = *var == '@' && !sh.shellparam.p.empty();
		if (!quoted)
			return p;
	}
	recordregion(sh, startloc, sh.expdest - (char *)stackblock(), quoted);
	return p;
}

static char *
chtodest(ShellContext &sh, int c, const char *syntax, char *out)
{
	if (syntax[c] == CCTL)
		USTPUTC(CTLESC, out);
	USTPUTC(c, out);

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
		USTPUTC(CTLMBCHAR, q);
		USTPUTC(ml, q);
	}

	q = static_cast<char *>(mempcpy(q, p, ml));

	if (syntax[CTLMBCHAR] == CCTL) {
		USTPUTC(ml, q);
		USTPUTC(CTLMBCHAR, q);
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
memtodest(ShellContext &sh, const char *p, size_t len, int flags)
{
	const char *syntax;
	size_t count = 0;
	int expq;
	char *q;

	if (unlikely(!len))
		return 0;

	/* CTLMBCHAR, 2, c, c, 2, CTLMBCHAR */
	q = makestrspace(sh, len * 3, sh.expdest);

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

	sh.expdest = q;
	return count;
}


static size_t
strtodest(ShellContext &sh, const char *p, int flags)
{
	size_t len = strlen(p);
	return memtodest(sh, p, len, flags);
}



/*
 * Add the value of a specialized variable to the stack string.
 */

static ssize_t
varvalue(ShellContext &sh, char *name, int varflags, unsigned flags)
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

	(void)0;
	discard = (subtype == VSPLUS || subtype == VSLENGTH) |
		  (flags & EXP_DISCARD);

	if (unlikely(!subtype)) {
		if (discard)
			return -1;

		sh_error(sh, "Bad substitution");
	}

	flags &= discard ? ~QUOTES_ESC : ~0;
	seps = nullstr;
	seplen = flags & EXP_FULL;
	start = sh.expdest - (char *)stackblock();

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
		len = cvtnum(sh, num, flags);
		break;
	case '-':
		p = makestrspace(sh, NOPTS, sh.expdest);
		for (i = NOPTS - 1; i >= 0; i--) {
			if (sh.optlist[i] && optletters[i]) {
				USTPUTC(optletters[i], p);
				len++;
			}
		}
		sh.expdest = p;
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
			seps = sh.ncifs;
		seplen = ((seplen - 1) & (sh.ifsmb0len - 1)) + 1;
param:
		if (sh.shellparam.p.empty())
			return -1;
		{
			bool first = true;
			for (const auto &param : sh.shellparam.p) {
				if (!first)
					len += memtodest(sh, seps, seplen,
							 flags | EXP_KEEPNUL);
				first = false;
				len += strtodest(sh, param.c_str(), flags);
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
		p = lookupvar(sh, name);
value:
		if (!p)
			return -1;

		len = strtodest(sh, p, flags);
		break;
	}

	if (discard)
		sh.expdest = (char *)stackblock() + start;

	return len;
}



/*
 * Record the fact that we have to scan this region of the
 * string for IFS characters.
 */

void
recordregion(ShellContext &sh, int start, int end, int nulonly)
{
	struct ifsregion *ifsp;

	if (sh.ifslastp == NULL) {
		ifsp = &sh.ifsfirst;
	} else {
		INTOFF;
		ifsp = (struct ifsregion *)ckmalloc(sh, sizeof (struct ifsregion));
		ifsp->next = NULL;
		sh.ifslastp->next = ifsp;
		INTON;
	}
	sh.ifslastp = ifsp;
	sh.ifslastp->begoff = start;
	sh.ifslastp->endoff = end;
	sh.ifslastp->nulonly = nulonly;
}

static unsigned
ifsisifs(ShellContext &sh, const char *p, unsigned ml, const char *ifs)
{
	bool isdefifs = false;
	bool isifs = false;
	wchar_t wc = *p;
	wchar_t ifs0;

	if (likely(ifs[0]) && unlikely(!sh.wcifs.empty())) {
		if (wc & 0x80) {
			mbstate_t mbst = {};
			wchar_t wc2;

			if (mbrtowc(&wc2, p, ml, &mbst) != ml)
				goto out;
			wc = wc2;
		}

		isifs = sh.wcifs.find(wc) != std::wstring::npos;
		ifs0 = sh.wcifs[0];
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
ifsbreakup_slow(ShellContext &sh, struct ifs_state *ifst, struct arglist *arglist,
			     int nulonly, char *p)
{
	struct strlist *sp;
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
	 * of the characters to remove, or NULL
	 * if no characters should be removed.
	 */
	if (!ifst->maxargs) {
		if (isdefifs) {
			if (!ifst->r)
				ifst->r = q;
			return p;
		}

		if (!(isifs && ifst->ifsspc))
			ifst->r = NULL;
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
		sp = (struct strlist *)stalloc(sh, sizeof *sp);
		sp->text = ifst->start;
		*arglist->lastp = sp;
		arglist->lastp = &sp->next;
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
ifsbreakup(ShellContext &sh, char *string, int maxargs, struct arglist *arglist)
{
	struct ifsregion *ifsp;
	struct ifs_state ifst;
	const char *realifs;
	struct strlist *sp;
	int nulonly;
	char *p;

	ifst.r = NULL;
	ifst.start = string;
	ifst.maxargs = maxargs;
	if (sh.ifslastp != NULL) {
		ifst.ifsspc = 0;
		nulonly = 0;
		realifs = sh.ncifs;
		ifsp = &sh.ifsfirst;
		do {
			int afternul;

			p = string + ifsp->begoff;
			afternul = nulonly;
			nulonly = ifsp->nulonly;
			ifst.ifs = nulonly ? nullstr : realifs;
			ifst.ifsspc = 0;
			for (;;) {
				char *p0 = p;

				while (string + ifsp->endoff - p >= 8) {
					union {
						uint64_t qw;
						unsigned char b[8];
					} x;

					memcpy(&x.qw, p, sizeof(x.qw));

					if ((x.qw & 0x8080808080808080))
						break;
					if (sh.ifsmap[x.b[0]] |
					    sh.ifsmap[x.b[1]] |
					    sh.ifsmap[x.b[2]] |
					    sh.ifsmap[x.b[3]] |
					    sh.ifsmap[x.b[4]] |
					    sh.ifsmap[x.b[5]] |
					    sh.ifsmap[x.b[6]] |
					    sh.ifsmap[x.b[7]])
						break;
					p += 8;
				}

				if (p != p0) {
					if (!ifst.maxargs)
						ifst.r = NULL;
					else if (ifst.ifsspc)
						ifst.start = p0;
					ifst.ifsspc = 0;
				}

				if (p >= string + ifsp->endoff)
					break;

				p = ifsbreakup_slow(sh, &ifst, arglist,
						    afternul | nulonly, p);
			}
		} while ((ifsp = ifsp->next) != NULL);
		if (nulonly)
			goto add;
		if (ifst.r)
			*ifst.r = '\0';
	}

	if (!*ifst.start)
		return;

add:
	sp = (struct strlist *)stalloc(sh, sizeof *sp);
	sp->text = ifst.start;
	*arglist->lastp = sp;
	arglist->lastp = &sp->next;
}

void
ifsfree(ShellContext &sh)
{
	struct ifsregion *p = sh.ifsfirst.next;

	if (!p)
		goto out;

	INTOFF;
	do {
		struct ifsregion *ifsp;
		ifsp = p->next;
		ckfree(p);
		p = ifsp;
	} while (p);
	sh.ifsfirst.next = NULL;
	INTON;

out:
	sh.ifslastp = NULL;
}

void
changeifs(ShellContext &sh, const char *ifs)
{
	mbstate_t mbs = {};
	std::wstring nwcifs;
	unsigned mb = 0;
	size_t len = 0;
	const char *p;
	size_t ml;

	if (!ifsset())
		ifs = defifs;
	sh.ncifs = ifs;

	memset(sh.ifsmap, 0, sizeof(sh.ifsmap));

	for (p = ifs;; p++) {
		unsigned c = (unsigned char)*p;

		mb |= c >> 7;
		if (!(c >> 7))
			sh.ifsmap[c] = 1;

		if (c == 0)
			break;

		len++;
	}

	sh.ifsmb0len = !!len;

	if (!mb)
		goto out;

	ml = mbrlen(ifs, len, &mbs);
	if (ml == -2 || ml == -1)
		ml = 1;
	sh.ifsmb0len = ml;

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
	sh.wcifs = std::move(nwcifs);
}

/*
 * Expand shell metacharacters.  At this point, the only control characters
 * should be escapes.  The results are stored in the list sh.exparg.
 */

#ifdef __GLIBC__
static void *
opendir_interruptible(ShellContext &sh, const char *pathname)
{
	if (int_pending()) {
		sh.suppressint = 0;
		onint(sh);
	}

	return opendir(pathname);
}
#else
#define GLOB_ALTDIRFUNC 0
#endif

static void
expandmeta_glob(ShellContext &sh, struct strlist *str)
{
	while (str) {
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

		INTOFF;
		p = preglob(sh, str->text, RMESCAPE_HEAP);
		i = glob64(p, GLOB_ALTDIRFUNC | GLOB_NOMAGIC, 0, &pglob);
		if (p != str->text)
			ckfree(p);
		switch (i) {
		case 0:
			if ((pglob.gl_flags & (GLOB_NOMAGIC | GLOB_NOCHECK)) ==
			    (GLOB_NOMAGIC | GLOB_NOCHECK))
				goto nometa2;
			addglob(sh, &pglob);
			globfree64(&pglob);
			INTON;
			break;
		case GLOB_NOMATCH:
nometa2:
			globfree64(&pglob);
			INTON;
nometa:
			*sh.exparg.lastp = str;
			rmescapes(str->text);
			sh.exparg.lastp = &str->next;
			break;
		default:	/* GLOB_NOSPACE */
			sh_error(sh, "Out of space");
		}
		str = str->next;
	}
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

static void
expandmeta(ShellContext &sh, struct strlist *str)
{
	/* TODO - EXP_REDIR */

	if (GLOB_IS_ENABLED)
		return expandmeta_glob(sh, str);

	while (str) {
		struct strlist **savelastp;
		struct strlist *sp;
		char *p;
		unsigned len;

		if (fflag)
			goto nometa;
		if (!strpbrk(str->text, "*?]") || !strcmp(str->text, "]"))
			goto nometa;
		savelastp = sh.exparg.lastp;

		INTOFF;
		p = preglob(sh, str->text, RMESCAPE_ALLOC | RMESCAPE_HEAP);
		len = strlen(p);

		expmeta(sh, p, len, 0);
		if (p != str->text)
			ckfree(p);
		INTON;
		if (sh.exparg.lastp == savelastp) {
			/*
			 * no matches
			 */
nometa:
			*sh.exparg.lastp = str;
			rmescapes(str->text);
			sh.exparg.lastp = &str->next;
		} else {
			*sh.exparg.lastp = NULL;
			*savelastp = sp = expsort(sh, *savelastp);
			while (sp->next != NULL)
				sp = sp->next;
			sh.exparg.lastp = &sp->next;
		}
		str = str->next;
	}
}

static void
addfname_common(ShellContext &sh, char *name)
{
	struct strlist *sp;

	sp = (struct strlist *)stalloc(sh, sizeof *sp);
	sp->text = name;
	*sh.exparg.lastp = sp;
	sh.exparg.lastp = &sp->next;
}

static char *
addfnamealt(ShellContext &sh, char *enddir, size_t expdir_len)
{
	char *name;

	name = grabstackstr(enddir);
	addfname_common(sh, name);

	STARTSTACKSTR(enddir);
	return stnputs(sh, name, expdir_len, enddir) - expdir_len;
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
	return NULL;
}
#endif

/*
 * Do metacharacter (i.e. *, ?, [...]) expansion.
 */

static char *
expmeta(ShellContext &sh, char *name, unsigned name_len, size_t expdir_len)
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

	try {
	len = expdir_len + name_len + 1;
	cp = growstackto(sh, len);
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
			cp = addfnamealt(sh, enddir + 1, expdir_len);
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

	dirp = opendir(expdir_len ? cp : dotdir);
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
			sh.expdest = enddir;
			memtodest(sh, p, len, EXP_MBCHAR | EXP_KEEPNUL);
			cp = stackblock();
			enddir = cp + expdir_len;
			p = enddir;
		}
		if (pmatch(sh, pat, p)) {
			enddir = stnputs(sh, dname, len, enddir);
			if (!c)
				cp = addfnamealt(sh, enddir, expdir_len);
			else {
				enddir[-1] = '/';
				len += expdir_len;
				cp = expmeta(sh, endname, name_len, len);
			}
			enddir = cp + expdir_len;
		}
check_int:
		if (int_pending())
			break;
	}
	*zeroedp = c;

	if (dirp)
		closedir(dirp);
out_opendir:
	return cp;
	} catch (...) {
		if (dirp)
			closedir(dirp);
		throw;
	}
}


/*
 * Add a file name to the list.
 */

static void
addfname(ShellContext &sh, char *name)
{
	addfname_common(sh, sstrdup(sh, name));
}


/*
 * Sort the results of file name expansion.  It calculates the number of
 * strings to sort and then calls msort (short for merge sort) to do the
 * work.
 */

static struct strlist *
expsort(ShellContext &sh, struct strlist *str)
{
	int len;
	struct strlist *sp;

	len = 0;
	for (sp = str ; sp ; sp = sp->next)
		len++;
	return msort(sh, str, len);
}


static struct strlist *
msort(ShellContext &sh, struct strlist *list, int len)
{
	struct strlist *p, *q = NULL;
	struct strlist **lpp;
	int half;
	int n;

	if (len <= 1)
		return list;
	half = len >> 1;
	p = list;
	for (n = half ; --n >= 0 ; ) {
		q = p;
		p = p->next;
	}
	q->next = NULL;			/* terminate first half of list */
	q = msort(sh, list, half);		/* sort first half of list */
	p = msort(sh, p, len - half);		/* sort second half */
	lpp = &list;
	for (;;) {
		if (strcoll(p->text, q->text) < 0) {
			*lpp = p;
			lpp = &p->next;
			if ((p = *lpp) == NULL) {
				*lpp = q;
				break;
			}
		} else {
			*lpp = q;
			lpp = &q->next;
			if ((q = *lpp) == NULL) {
				*lpp = p;
				break;
			}
		}
	}
	return list;
}


/*
 * Returns true if the pattern matches the string.
 */

static inline int
patmatch(ShellContext &sh, char *pattern, const char *string)
{
	return pmatch(sh, preglob(sh, pattern, 0), string);
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
_rmescapes(ShellContext &sh, char *str, int flag)
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

		if (flag & RMESCAPE_GROW) {
			int strloc = str - (char *)stackblock();

			r = makestrspace(sh, fulllen, sh.expdest);
			str = (char *)stackblock() + strloc;
			p = str + len;
		} else if (flag & RMESCAPE_HEAP) {
			r = ckmalloc(sh, fulllen);
		} else {
			r = stalloc(sh, fulllen);
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
	if (flag & (RMESCAPE_ALLOC | RMESCAPE_GROW)) {
		sh.expdest = r;
		STADJUST(q - r + 1, sh.expdest);
	}
	return r;
}



/*
 * See if a pattern matches in a case statement.
 */

int
casematch(ShellContext &sh, union node *pattern, char *val)
{
	int result;
	std::size_t su = sh.stackstr.used;
	std::size_t hc = sh.stackheaps.size();

	sh.argbackq = &pattern->narg.backquote;
	sh.argbackq_i = 0;
	STARTSTACKSTR(sh.expdest);
	argstr(sh, pattern->narg.text.data(), EXP_TILDE | EXP_CASE);
	ifsfree(sh);
	result = patmatch(sh, stackblock(), val);
	while (sh.stackheaps.size() > hc) {
		ckfree(sh.stackheaps.back());
		sh.stackheaps.pop_back();
	}
	sh.stackstr.rewind(su);
	return result;
}

/*
 * Our own itoa().
 */

static size_t
cvtnum(ShellContext &sh, intmax_t num, int flags)
{
	int len = max_int_length(sizeof(num));
	char buf[len];

	len = fmtstr(sh, buf, len, "%" PRIdMAX, num);
	return memtodest(sh, buf, len, flags);
}

static void
varunset(ShellContext &sh, const char *end, const char *var, const char *umsg, int varflags)
{
	const char *msg;
	const char *tail;

	tail = nullstr;
	msg = "parameter not set";
	if (umsg) {
		if (*end == (char)CTLENDVAR) {
			if (varflags & VSNUL)
				tail = " or null";
		} else
			msg = umsg;
	}
	sh_error(sh, "%.*s: %s%s", end - var - 1, var, msg, tail);
}

void
restore_handler_expandarg(ShellContext &sh, int err)
{
	if (err) {
		if (sh.exception != EXERROR)
			exraise(sh, sh.exception);
		ifsfree(sh);
	}
}

#ifdef mkinit

INCLUDE "expand.h"

EXITRESET {
	ifsfree(sh);
}

#endif
