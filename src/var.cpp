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

#include <cstring>
#include <locale.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

/*
 * Shell variables.
 */

#include "shell.h"
#include "shell_context.h"
#include "output.h"
#include "expand.h"
#include "nodes.h"	/* for other headers */
#include "exec.h"
#include "syntax.h"
#include "options.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "parser.h"
#include "show.h"
#include "system.h"


#define VTABSIZE 39


struct localvar_list {
	struct localvar_list *next;
	struct localvar *lv;
};



const char defpathvar[] =
	"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";






void
changelocale(ShellContext &, const char *val)
{
	const auto eq = std::strchr(val, '=');

	if (eq) {
		std::string name(val, static_cast<std::size_t>(eq - val));

		if (eq[1] == '\0')
			unsetenv(name.c_str());
		else
			setenv(name.c_str(), eq + 1, 1);
	} else {
		unsetenv(val);
	}
	setlocale(LC_ALL, nullstr);
}

/* Some macros in var.h depend on the order, add new variables to the end. */




static struct var ** hashvar(ShellContext &sh, const char *);
STATIC int vpcmp(const void *, const void *);
static struct var ** findvar(ShellContext &sh, const char *);

/*
 * Initialize the varable symbol tables and import the environment
 */

#ifdef mkinit
INCLUDE <unistd.h>
INCLUDE <sys/types.h>
INCLUDE <sys/stat.h>
INCLUDE "cd.h"
INCLUDE "output.h"
INCLUDE "var.h"

INIT {
	char **envp;
	const char *p;
	struct stat64 st1, st2;

	initvar(sh);
	for (envp = environ ; *envp ; envp++) {
		p = endofname(*envp);
		if (p != *envp && *p == '=') {
			setvareq(sh, *envp, VarFlag::exported);
		}
	}

	setvareq(sh, sh.defifsvar);
	setvareq(sh, sh.defoptindvar);

	fmtstr(sh, sh.ppidbuf + 5, sizeof(sh.ppidbuf) - 5, "%ld", (long) getppid());
	setvareq(sh, sh.ppidbuf);

	p = lookupvar(sh, "PWD");
	if (p)
		if (*p != '/' || stat64(p, &st1) || stat64(dotdir, &st2) ||
		    st1.st_dev != st2.st_dev || st1.st_ino != st2.st_ino)
			p = 0;
	setpwd(sh, p, 0);
}

RESET {
	unwindlocalvars(sh, 0);
}
#endif

static inline const char *
varnull(const std::string &s)
{
	/* Unset variables end with an extra NUL (name\0\0) or name=. */
	return strchrnul(s.c_str(), '=') + 1;
}

static void
varfunc(ShellContext &sh, struct var *vp)
{
	const char *s;

	if (!vp->func)
		return;

	s = vp->text.c_str();
	if (!vp->flags.test(VarFlag::full))
		s = varnull(vp->text);
	vp->func(sh, s);
}

/*
 * This routine initializes the builtin variables.  It is called when the
 * shell is initialized.
 */

void
initvar(ShellContext &sh)
{
	struct var *vp;
	struct var *end;
	struct var **vpp;

	vp = sh.varinit;
	end = vp + sizeof(sh.varinit) / sizeof(sh.varinit[0]);
	do {
		vpp = hashvar(sh, vp->text.c_str());
		vp->next = *vpp;
		*vpp = vp;
	} while (++vp < end);
	/*
	 * PS1 depends on uid
	 */
	if (!geteuid())
		vps1.text = "PS1=# ";
}

/*
 * Set the value of a variable.  The flags argument is ored with the
 * flags of the variable.  If val is nullopt, the variable is unset.
 */

dash::ShellResult<struct var *>
setvar(ShellContext &sh, const std::string &name, std::optional<std::string> val,
       VarFlags flags)
{
	const char *q = endofname(name.c_str());
	const char *eq = strchrnul(q, '=');
	size_t namelen = static_cast<size_t>(eq - name.c_str());
	if (!namelen || eq != q)
		return sh_error(sh, "%.*s: bad variable name",
				static_cast<int>(namelen), name.c_str());

	if (!val)
		flags |= VarFlag::unset;

	/* Always store name=value (empty value when unset). */
	std::string nameeq;
	nameeq.reserve(namelen + 1 + (val ? val->size() : 0));
	nameeq.assign(name, 0, namelen);
	nameeq.push_back('=');
	if (val)
		nameeq += *val;

	INTOFF;
	struct var *vp = TRY_SH(setvareq(sh, std::move(nameeq), flags));
	INTON;

	return vp;
}

/*
 * Set the given integer as the value of a variable.  The flags argument is
 * ored with the flags of the variable.
 */

dash::ShellResult<intmax_t>
setvarint(ShellContext &sh, const std::string &name, intmax_t val, VarFlags flags)
{
	int len = max_int_length(sizeof(val));
	char buf[len];

	fmtstr(sh, buf, len, "%" PRIdMAX, val);
	TRY_SHV(setvar(sh, name, buf, flags));
	return val;
}



/*
 * Same as setvar except that the variable and value are passed in
 * the first argument as name=value.
 * Called with interrupts off.
 */

dash::ShellResult<struct var *>
setvareq(ShellContext &sh, std::string s, VarFlags flags)
{
	static const VarFlags special =
		VarFlag::exported | VarFlag::readonly | VarFlag::str_fixed |
		VarFlag::unset;

	struct var *vp, **vpp;

	if (aflag)
		flags |= VarFlag::exported;
	vpp = findvar(sh, s.c_str());
	vp = *vpp;
	if (vp) {
		VarFlags keep;

		if (vp->flags.test(VarFlag::readonly)) {
			const char *n = vp->text.c_str();
			return sh_error(sh, "%.*s: is read only", strchrnul(n, '=') - n,
				 n);
		}

		if ((flags & special) != VarFlag::unset)
			keep = ~VarFlag::unset;
		else if (vp->flags.test(VarFlag::str_fixed))
			keep = VarFlag::str_fixed;
		else {
			*vpp = vp->next;
			delete vp;
			vp = nullptr;
			goto out;
		}

		flags |= vp->flags & keep;
	} else {
		if ((flags & special) == VarFlag::unset)
			goto out;
		/* not found */
		vp = new var;
		vp->next = *vpp;
		vp->func = NULL;
		*vpp = vp;
	}
	vp->text = std::move(s);
	vp->flags = flags;

	if (!flags.test(VarFlag::nofunc))
		varfunc(sh, vp);

out:
	return vp;
}

/*
 * Find the value of a variable.  Returns NULL if not set.
 */

char *
lookupvar(ShellContext &sh, const char *name)
{
	struct var *v;

	if ((v = *findvar(sh, name)) && !v->flags.test(VarFlag::unset)) {
#ifdef WITH_LINENO
		if (v == &vlineno) {
			fmtstr(sh, sh.linenovar + 7, sizeof(sh.linenovar) - 7,
			       "%d", sh.lineno);
			v->text = sh.linenovar;
		}
#endif
		return const_cast<char *>(strchrnul(v->text.c_str(), '=')) + 1;
	}
	return NULL;
}

dash::ShellResult<intmax_t>
lookupvarint(ShellContext &sh, const char *name)
{
	return atomax(sh, lookupvar(sh, name) ?: nullstr, 0);
}



/*
 * Generate a list of variables satisfying the given conditions.
 */

std::vector<char *>
listvars(ShellContext &sh, VarFlags on, VarFlags off)
{
	struct var **vpp;
	struct var *vp;
	VarFlags mask;
	std::vector<char *> list;

	vpp = sh.vartab;
	mask = on | off;
	do {
		for (vp = *vpp ; vp ; vp = vp->next)
			if ((vp->flags & mask) == on)
				list.push_back(const_cast<char *>(vp->text.c_str()));
	} while (++vpp < sh.vartab + VTABSIZE);
	list.push_back(nullptr);
	return list;
}



/*
 * POSIX requires that 'set' (but not export or readonly) sh.output the
 * variables in lexicographic order - by the locale's collating order (sigh).
 * Maybe we could keep them in an ordered balanced binary tree
 * instead of hashed lists.
 * For now just roll 'em through qsort for printing...
 */

dash::ShellResult<int>
showvars(ShellContext &sh, const char *prefix, VarFlags on, VarFlags off)
{
	const char *sep;
	std::vector<char *> list = listvars(sh, on, off);
	char **ep = list.data();
	char **epend = list.data() + list.size() - 1;

	qsort(ep, static_cast<size_t>(epend - ep), sizeof(char *), vpcmp);

	sep = *prefix ? spcstr : prefix;

	for (; ep < epend; ep++) {
		const char *p;
		const char *q;

		p = strchrnul(*ep, '=');
		q = nullstr;
		if (*p)
			q = single_quote(sh, ++p);

		out1fmt(sh, "%s%s%.*s%s\n", prefix, sep, (int)(p - *ep), *ep, q);
	}

	return 0;
}



/*
 * The export and readonly commands.
 */

dash::ShellResult<int>
exportcmd(ShellContext &sh, int argc, char **argv)
{
	struct var *vp;
	char *name;
	const char *p;
	char **aptr;
	VarFlags flag = argv[0][0] == 'r' ? VarFlag::readonly : VarFlag::exported;
	int notp;

	notp = TRY_SH(nextopt(sh, "p")) - 'p';
	if (notp && ((name = *(aptr = sh.argptr)))) {
		do {
			if ((p = strchr(name, '=')) != NULL) {
				p++;
			} else {
				if ((vp = *findvar(sh, name))) {
					vp->flags |= flag;
					continue;
				}
			}
			TRY_SHV(setvar(sh, name,
				       p ? std::optional<std::string>(p)
					 : std::nullopt,
				       flag));
		} while ((name = *++aptr) != NULL);
	} else {
		TRY_SHV(showvars(sh, argv[0], flag, {}));
	}
	return 0;
}


/*
 * The "local" command.
 */

dash::ShellResult<int>
localcmd(ShellContext &sh, int argc, char **argv)
{
	char *name;

	if (!sh.localvar_stack)
		return sh_error(sh, "not in a function");

	argv = sh.argptr;
	while ((name = *argv++) != NULL) {
		TRY_SHV(mklocal(sh, name));
	}
	return 0;
}


/*
 * Make a variable a local variable.  When a variable is made local, it's
 * value and flags are saved in a localvar structure.  The saved values
 * will be restored when the shell function returns.  We handle the name
 * "-" as a special case.
 */

dash::ShellResult<void>
mklocal(ShellContext &sh, char *name, VarFlags flags)
{
	struct localvar *lvp;
	struct var *vp;

	INTOFF;
	lvp = new localvar;
	if (name[0] == '-' && name[1] == '\0') {
		lvp->text.assign(sh.optlist, sizeof(sh.optlist));
		vp = NULL;
	} else {
		char *eq;

		vp = *findvar(sh, name);
		eq = strchr(name, '=');
		if (vp == NULL) {
			if (eq)
				vp = TRY_SH(setvareq(sh, name, VarFlag::str_fixed | flags));
			else
				vp = TRY_SH(setvar(sh, name, std::nullopt,
						   VarFlag::str_fixed | flags));
			lvp->flags = VarFlag::unset;
		} else {
			lvp->text = vp->text;
			lvp->flags = vp->flags;
			vp->flags |= VarFlag::str_fixed;
			if (eq)
				TRY_SHV(setvareq(sh, name, flags));
		}
	}
	lvp->vp = vp;
	lvp->next = sh.localvar_stack->lv;
	sh.localvar_stack->lv = lvp;
	INTON;
	return {};
}


/*
 * Called after a function returns.
 * Interrupts must be off.
 */

static dash::ShellResult<void>
poplocalvars(ShellContext &sh)
{
	struct localvar_list *ll;
	struct localvar *lvp, *next;
	struct var *vp;

	INTOFF;
	ll = sh.localvar_stack;
	sh.localvar_stack = ll->next;

	next = ll->lv;
	ckfree(ll);

	while ((lvp = next) != NULL) {
		next = lvp->next;
		vp = lvp->vp;
		TRACE(("poplocalvar %s\n", vp ? vp->text.c_str() : "-"));
		if (vp == NULL) {	/* $- saved */
			memcpy(sh.optlist, lvp->text.data(), sizeof(sh.optlist));
			optschanged(sh);
		} else if (lvp->flags == VarFlag::unset) {
			vp->flags &= ~(VarFlag::str_fixed | VarFlag::readonly);
			unsetvar(sh, vp->text);
		} else {
			vp->flags = lvp->flags;
			vp->text = std::move(lvp->text);
			if (!vp->flags.test(VarFlag::nofunc))
				varfunc(sh, vp);
		}
		delete lvp;
	}
	INTON;
	return {};
}


/*
 * Create a new localvar environment.
 */
dash::ShellResult<struct localvar_list *>
pushlocalvars(ShellContext &sh, int push)
{
	struct localvar_list *ll;
	struct localvar_list *top;

	top = sh.localvar_stack;
	if (!push)
		goto out;

	INTOFF;
	ll = ckmalloc(sh, sizeof(*ll));
	ll->lv = NULL;
	ll->next = top;
	sh.localvar_stack = ll;
	INTON;

out:
	return top;
}


dash::ShellResult<void>
unwindlocalvars(ShellContext &sh, struct localvar_list *stop)
{
	while (sh.localvar_stack != stop)
		TRY_SHV(poplocalvars(sh));
	return {};
}


/*
 * The unset builtin command.  We unset the function before we unset the
 * variable to allow a function to be unset when there is a readonly variable
 * with the same name.
 */

dash::ShellResult<int>
unsetcmd(ShellContext &sh, int argc, char **argv)
{
	char **ap;
	int i;
	int flag = 0;

	while ((i = TRY_SH(nextopt(sh, "vf"))) != '\0') {
		flag = i;
	}

	for (ap = sh.argptr; *ap ; ap++) {
		if (flag != 'f') {
			TRY_SHV(unsetvar(sh, *ap));
			continue;
		}
		if (flag != 'v')
			unsetfunc(sh, *ap);
	}
	return 0;
}


/*
 * Unset the specified variable.
 */

dash::ShellResult<void>
unsetvar(ShellContext &sh, const std::string &s)
{
	TRY_SHV(setvar(sh, s, std::nullopt));
	return {};
}



/*
 * Find the appropriate entry in the hash table from the name.
 */

static struct var **
hashvar(ShellContext &sh, const char *p)
{
	return &sh.vartab[hashval(p) % VTABSIZE];
}



/*
 * Compares two strings up to the first = or '\0'.  The first
 * string must be terminated by '='; the second may be terminated by
 * either '=' or '\0'.
 */

int
varcmp(const char *p, const char *q)
{
	int c = *p, d = *q;
	while (c == d) {
		if (!c)
			break;
		p++;
		q++;
		c = *p;
		d = *q;
		if (c == '=')
			c = '\0';
		if (d == '=')
			d = '\0';
	}
	return c - d;
}

STATIC int
vpcmp(const void *a, const void *b)
{
	return varcmp(*(const char **)a, *(const char **)b);
}

static struct var **
findvar(ShellContext &sh, const char *name)
{
	struct var **vpp;

	for (vpp = hashvar(sh, name); *vpp; vpp = &(*vpp)->next) {
		if (varequal((*vpp)->text.c_str(), name)) {
			break;
		}
	}
	return vpp;
}
