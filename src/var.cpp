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
#include <format>
#include <locale.h>
#include <string>
#include <string_view>
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
#include "error.h"
#include "mystring.h"
#include "parser.h"
#include "show.h"
#include "system.h"


const char defpathvar[] =
	"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";


void
changelocale(ShellContext &, std::string_view val)
{
	auto eq = val.find('=');

	if (eq != std::string_view::npos) {
		std::string name(val.substr(0, eq));
		std::string_view value = val.substr(eq + 1);

		if (value.empty())
			unsetenv(name.c_str());
		else {
			std::string value_str(value);
			setenv(name.c_str(), value_str.c_str(), 1);
		}
	} else {
		std::string name(val);
		unsetenv(name.c_str());
	}
	setlocale(LC_ALL, nullstr);
}

static int vpcmp(const void *, const void *);
static struct var *findvar(ShellContext &sh, std::string_view name);

/* Name part of "name" or "name=value". */
static std::string_view
varname(std::string_view s)
{
	return s.substr(0, s.find('='));
}

/*
 * Initialize the varable symbol tables and import the environment
 */

#ifdef mkinit
INCLUDE <unistd.h>
INCLUDE <sys/types.h>
INCLUDE <sys/stat.h>
INCLUDE <format>
INCLUDE "cd.h"
INCLUDE "var.h"

INIT {
	char **envp;
	const char *p;
	struct stat64 st1, st2;

	initvar(sh);
	for (envp = environ ; *envp ; envp++) {
		std::string_view ev(*envp);
		auto n = endofname(ev);
		if (n > 0 && n < ev.size() && ev[n] == '=') {
			setvareq(sh, *envp, VarFlag::exported);
		}
	}

	setvareq(sh, "IFS= \t\n");
	setvareq(sh, "OPTIND=1");

	setvareq(sh, std::format("PPID={}", getppid()));

	p = lookupvar(sh, "PWD");
	if (p)
		if (*p != '/' || stat64(p, &st1) || stat64(".", &st2) ||
		    st1.st_dev != st2.st_dev || st1.st_ino != st2.st_ino)
			p = 0;
	setpwd(sh, p ? std::optional<std::string_view>(p) : std::nullopt, 0);
}

RESET {
	unwindlocalvars(sh, 0);
}
#endif

static std::string_view
varvalue(std::string_view s)
{
	auto pos = s.find('=');
	if (pos == std::string_view::npos)
		return {};
	return s.substr(pos + 1);
}

static void
varfunc(ShellContext &sh, struct var *vp)
{
	if (!vp->func)
		return;

	std::string_view s = vp->text;
	if (!vp->flags.test(VarFlag::full))
		s = varvalue(vp->text);
	vp->func(sh, s);
}

/*
 * Seed permanent builtin variables.  Called when the shell is initialized.
 */

void
initvar(ShellContext &sh)
{
	auto add = [&](VarFlags flags, const char *text,
		       void (*func)(ShellContext &, std::string_view)) {
		std::string nameeq(text);
		std::string name(varname(nameeq));
		var v;
		v.flags = flags;
		v.text = std::move(nameeq);
		v.func = func;
		sh.vars.emplace(std::move(name), std::move(v));
	};

	add(VarFlag::str_fixed, "IFS= \t\n", changeifs);
	add(VarFlag::str_fixed, defpathvar, changepath);
	add(VarFlag::str_fixed, "PS1=$ ", nullptr);
	add(VarFlag::str_fixed, "PS2=> ", nullptr);
	add(VarFlag::str_fixed, "PS4=+ ", nullptr);
	add(VarFlag::str_fixed | VarFlag::nofunc, "OPTIND=1", getoptsreset);
#ifdef WITH_LINENO
	add(VarFlag::str_fixed, "LINENO=", nullptr);
#endif
	add(VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_ALL=",
	    changelocale);
	add(VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_COLLATE=",
	    changelocale);
	add(VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_CTYPE=",
	    changelocale);
	add(VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_NUMERIC=",
	    changelocale);
	add(VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LANG=",
	    changelocale);

	/*
	 * PS1 depends on uid
	 */
	if (!geteuid())
		sh.vars.at("PS1").text = "PS1=# ";
}

/*
 * Set the value of a variable.  The flags argument is ored with the
 * flags of the variable.  If val is nullopt, the variable is unset.
 */

shell::Result<struct var *>
setvar(ShellContext &sh, const std::string &name, std::optional<std::string> val,
       VarFlags flags)
{
	size_t namelen = endofname(name);
	if (!namelen || (namelen < name.size() && name[namelen] != '='))
		return sh_error(sh, "{}: bad variable name",
				std::string_view(name.c_str(), namelen ? namelen : name.size()));

	if (!val)
		flags |= VarFlag::unset;

	/* Always store name=value (empty value when unset). */
	std::string nameeq;
	nameeq.reserve(namelen + 1 + (val ? val->size() : 0));
	nameeq.assign(name, 0, namelen);
	nameeq.push_back('=');
	if (val)
		nameeq += *val;

	INTOFF(sh);
	struct var *vp = TRY_SH(setvareq(sh, std::move(nameeq), flags));
	INTON(sh);

	return vp;
}

/*
 * Set the given integer as the value of a variable.  The flags argument is
 * ored with the flags of the variable.
 */

shell::Result<intmax_t>
setvarint(ShellContext &sh, const std::string &name, intmax_t val, VarFlags flags)
{
	TRY_SHV(setvar(sh, name, std::format("{}", val), flags));
	return val;
}



/*
 * Same as setvar except that the variable and value are passed in
 * the first argument as name=value.
 * Called with interrupts off.
 */

shell::Result<struct var *>
setvareq(ShellContext &sh, std::string s, VarFlags flags)
{
	static const VarFlags special =
		VarFlag::exported | VarFlag::readonly | VarFlag::str_fixed |
		VarFlag::unset;

	if (aflag)
		flags |= VarFlag::exported;

	std::string name(varname(s));
	auto it = sh.vars.find(name);
	struct var *vp = nullptr;

	if (it != sh.vars.end()) {
		VarFlags keep;

		vp = &it->second;
		if (vp->flags.test(VarFlag::readonly)) {
			return sh_error(sh, "{}: is read only",
					std::string_view(name));
		}

		if ((flags & special) != VarFlag::unset)
			keep = ~VarFlag::unset;
		else if (vp->flags.test(VarFlag::str_fixed))
			keep = VarFlag::str_fixed;
		else {
			sh.vars.erase(it);
			return nullptr;
		}

		flags |= vp->flags & keep;
		vp->text = std::move(s);
		vp->flags = flags;
	} else {
		if ((flags & special) == VarFlag::unset)
			return nullptr;
		var v;
		v.text = std::move(s);
		v.flags = flags;
		auto [placed, inserted] = sh.vars.emplace(std::move(name),
							  std::move(v));
		(void)inserted;
		vp = &placed->second;
	}

	if (!flags.test(VarFlag::nofunc))
		varfunc(sh, vp);

	return vp;
}

/*
 * Find the value of a variable.  Returns NULL if not set.
 */

char *
lookupvar(ShellContext &sh, std::string_view name)
{
	struct var *v = findvar(sh, name);

	if (v && !v->flags.test(VarFlag::unset)) {
#ifdef WITH_LINENO
		if (name == "LINENO")
			v->text = std::format("LINENO={}", sh.lineno);
#endif
		return const_cast<char *>(strchrnul(v->text.c_str(), '=')) + 1;
	}
	return NULL;
}

shell::Result<intmax_t>
lookupvarint(ShellContext &sh, std::string_view name)
{
	return atomax(sh, lookupvar(sh, name) ?: nullstr, 0);
}



/*
 * Generate a list of variables satisfying the given conditions.
 */

std::vector<char *>
listvars(ShellContext &sh, VarFlags on, VarFlags off)
{
	VarFlags mask = on | off;
	std::vector<char *> list;

	for (auto &[name, vp] : sh.vars) {
		(void)name;
		if ((vp.flags & mask) == on)
			list.push_back(const_cast<char *>(vp.text.c_str()));
	}
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

shell::Result<int>
showvars(ShellContext &sh, std::string_view prefix, VarFlags on, VarFlags off)
{
	const char *sep;
	std::vector<char *> list = listvars(sh, on, off);
	char **ep = list.data();
	char **epend = list.data() + list.size() - 1;
	std::string prefix_str(prefix);

	qsort(ep, static_cast<size_t>(epend - ep), sizeof(char *), vpcmp);

	sep = prefix.empty() ? "" : " ";

	for (; ep < epend; ep++) {
		const char *p;
		const char *q;

		p = strchrnul(*ep, '=');
		std::string quoted;
		q = nullstr;
		if (*p) {
			quoted = single_quote(++p);
			q = quoted.c_str();
		}

		out1fmt(sh, "%s%s%.*s%s\n", prefix_str.c_str(), sep,
			(int)(p - *ep), *ep, q);
	}

	return 0;
}



/*
 * The export and readonly commands.
 */

shell::Result<int>
exportcmd(ShellContext &sh, int argc, char **argv)
{
	struct var *vp;
	char *name;
	const char *p;
	char **aptr;
	VarFlags flag = argv[0][0] == 'r' ? VarFlag::readonly : VarFlag::exported;
	int notp;

	notp = TRY_SH(nextopt(sh, "p")) - 'p';
	if (notp && ((name = *(aptr = sh.bopt.argptr)))) {
		do {
			if ((p = strchr(name, '=')) != NULL) {
				p++;
			} else {
				if ((vp = findvar(sh, name))) {
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

shell::Result<int>
localcmd(ShellContext &sh, int argc, char **argv)
{
	char *name;

	if (sh.localvar_stack.empty())
		return sh_error(sh, "not in a function");

	argv = sh.bopt.argptr;
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

shell::Result<void>
mklocal(ShellContext &sh, std::string_view name, VarFlags flags)
{
	localvar lv;
	struct var *vp;

	INTOFF(sh);
	if (name == "-") {
		lv.text.assign(sh.opts.list, sizeof(sh.opts.list));
		vp = nullptr;
	} else {
		auto eq = name.find('=');

		vp = findvar(sh, name);
		if (vp == nullptr) {
			if (eq != std::string_view::npos)
				vp = TRY_SH(setvareq(sh, std::string(name),
						     VarFlag::str_fixed | flags));
			else
				vp = TRY_SH(setvar(sh, std::string(name), std::nullopt,
						   VarFlag::str_fixed | flags));
			lv.flags = VarFlag::unset;
		} else {
			lv.text = vp->text;
			lv.flags = vp->flags;
			vp->flags |= VarFlag::str_fixed;
			if (eq != std::string_view::npos)
				TRY_SHV(setvareq(sh, std::string(name), flags));
		}
	}
	lv.vp = vp;
	sh.localvar_stack.back().vars.push_back(std::move(lv));
	INTON(sh);

	return {};
}


/*
 * Called after a function returns.
 * Interrupts must be off.
 */

static shell::Result<void>
poplocalvars(ShellContext &sh)
{
	INTOFF(sh);
	LocalVarFrame frame = std::move(sh.localvar_stack.back());
	sh.localvar_stack.pop_back();

	for (auto it = frame.vars.rbegin(); it != frame.vars.rend(); ++it) {
		struct var *vp = it->vp;
		TRACE(("poplocalvar %s\n", vp ? vp->text.c_str() : "-"));
		if (vp == nullptr) {	/* $- saved */
			memcpy(sh.opts.list, it->text.data(), sizeof(sh.opts.list));
			optschanged(sh);
		} else if (it->flags == VarFlag::unset) {
			vp->flags &= ~(VarFlag::str_fixed | VarFlag::readonly);
			unsetvar(sh, vp->text);
		} else {
			vp->flags = it->flags;
			vp->text = std::move(it->text);
			if (!vp->flags.test(VarFlag::nofunc))
				varfunc(sh, vp);
		}
	}
	INTON(sh);

	return {};
}


/*
 * Create a new localvar environment.
 */
shell::Result<std::size_t>
pushlocalvars(ShellContext &sh, int push)
{
	const auto stop = sh.localvar_stack.size();

	if (!push)
		return stop;

	INTOFF(sh);
	sh.localvar_stack.emplace_back();
	INTON(sh);

	return stop;
}


shell::Result<void>
unwindlocalvars(ShellContext &sh, std::size_t stop)
{
	while (sh.localvar_stack.size() > stop)
		TRY_SHV(poplocalvars(sh));
	return {};
}


/*
 * The unset builtin command.  We unset the function before we unset the
 * variable to allow a function to be unset when there is a readonly variable
 * with the same name.
 */

shell::Result<int>
unsetcmd(ShellContext &sh, int argc, char **argv)
{
	char **ap;
	int i;
	int flag = 0;

	while ((i = TRY_SH(nextopt(sh, "vf"))) != '\0') {
		flag = i;
	}

	for (ap = sh.bopt.argptr; *ap ; ap++) {
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

shell::Result<void>
unsetvar(ShellContext &sh, const std::string &s)
{
	TRY_SHV(setvar(sh, s, std::nullopt));
	return {};
}



/*
 * Compares two strings up to the first = or end.  Either side may use
 * '=' as the name terminator.
 */

int
varcmp(std::string_view p, std::string_view q)
{
	auto at = [](std::string_view s, std::size_t i, bool map_eq) {
		if (i >= s.size())
			return 0;
		int c = static_cast<unsigned char>(s[i]);
		if (map_eq && c == '=')
			return 0;
		return c;
	};

	std::size_t i = 0;
	std::size_t j = 0;
	int c = at(p, 0, false);
	int d = at(q, 0, false);
	while (c == d) {
		if (!c)
			return 0;
		++i;
		++j;
		c = at(p, i, true);
		d = at(q, j, true);
	}
	return c - d;
}

static int
vpcmp(const void *a, const void *b)
{
	return varcmp(*(const char **)a, *(const char **)b);
}

static struct var *
findvar(ShellContext &sh, std::string_view name)
{
	auto it = sh.vars.find(std::string(varname(name)));
	return it != sh.vars.end() ? &it->second : nullptr;
}
