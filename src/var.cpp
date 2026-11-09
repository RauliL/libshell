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

#include <algorithm>
#include <cstring>
#include <format>
#include <locale.h>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

/*
 * Shell variables.
 */

#include "cd.h"
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
#include "system.h"

static int varcmp(std::string_view p, std::string_view q);

const char defpathvar[] =
	"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";


void
changelocale(Context &, std::string_view val)
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
	setlocale(LC_ALL, "");
}

static shell::Variable *findvar(const Context &sh, std::string_view name);

/* Name part of "name" or "name=value". */
static std::string_view
varname(std::string_view s)
{
	return s.substr(0, s.find('='));
}

/*
 * Initialize the varable symbol tables and import the environment
 */

void
init_shell_vars(Context &sh)
{
	char **envp;
	struct stat64 st1, st2;

	initvar(sh);
	for (envp = environ; *envp; envp++) {
		std::string_view ev(*envp);
		auto n = endofname(ev);
		if (n > 0 && n < ev.size() && ev[n] == '=') {
			setvareq(sh, *envp, shell::Variable::Flag::Exported);
		}
	}

	setvareq(sh, "IFS= \t\n");
	setvareq(sh, "OPTIND=1");

	setvareq(sh, std::format("PPID={}", getppid()));

	auto pwd = sh.get_var("PWD");
	if (pwd) {
		if (pwd->empty() || (*pwd)[0] != '/' ||
		    stat64(pwd->data(), &st1) || stat64(".", &st2) ||
		    st1.st_dev != st2.st_dev || st1.st_ino != st2.st_ino)
			pwd = std::nullopt;
	}
	setpwd(sh, pwd, 0);
}

static inline std::string_view
varvalue(std::string_view s)
{
	const auto pos = s.find('=');

	if (pos == std::string_view::npos)
		return {};

	return s.substr(pos + 1);
}

static void
varfunc(Context &sh, shell::Variable *vp)
{
	if (!vp->func)
		return;

	std::string_view s = vp->text;
	if (!vp->flags.test(shell::Variable::Flag::Full))
		s = varvalue(vp->text);
	vp->func(sh, s);
}

/*
 * Seed permanent builtin variables.  Called when the shell is initialized.
 */

void
initvar(Context &sh)
{
	auto add = [&](
		const shell::Variable::Flags &flags,
		const char *text,
		shell::Variable::callback_type func
	) {
		std::string nameeq(text);
		std::string name(varname(nameeq));
		shell::Variable v;

		v.flags = flags;
		v.text = std::move(nameeq);
		v.func = func;
		sh.variables.emplace(std::move(name), std::move(v));
	};

	add(shell::Variable::Flag::StrFixed, "IFS= \t\n", changeifs);
	add(shell::Variable::Flag::StrFixed, defpathvar, changepath);
	add(shell::Variable::Flag::StrFixed, "PS1=$ ", nullptr);
	add(shell::Variable::Flag::StrFixed, "PS2=> ", nullptr);
	add(shell::Variable::Flag::StrFixed, "PS4=+ ", nullptr);
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::NoFunc, "OPTIND=1", getoptsreset);
#ifdef WITH_LINENO
	add(shell::Variable::Flag::StrFixed, "LINENO=", nullptr);
#endif
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::Full | shell::Variable::Flag::Unset, "LC_ALL=",
	    changelocale);
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::Full | shell::Variable::Flag::Unset, "LC_COLLATE=",
	    changelocale);
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::Full | shell::Variable::Flag::Unset, "LC_CTYPE=",
	    changelocale);
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::Full | shell::Variable::Flag::Unset, "LC_NUMERIC=",
	    changelocale);
	add(shell::Variable::Flag::StrFixed | shell::Variable::Flag::Full | shell::Variable::Flag::Unset, "LANG=",
	    changelocale);

	/*
	 * PS1 depends on uid
	 */
	if (!geteuid())
		sh.variables.at("PS1").text = "PS1=# ";
}

shell::Result<shell::Variable *>
setvar(
	Context &sh,
	const std::string &name,
	const std::optional<std::string> &val,
	shell::Variable::Flags flags
)
{
	const auto namelen = endofname(name);
	shell::Variable *variable;

	if (!namelen || (namelen < name.size() && name[namelen] != '='))
		return sh_error(
			sh,
			"{}: bad variable name",
			std::string_view(name.data(), namelen ? namelen : name.size())
		);

	if (!val)
		flags |= shell::Variable::Flag::Unset;

	/* Always store name=value (empty value when unset). */
	std::string nameeq;
	nameeq.reserve(namelen + 1 + (val ? val->size() : 0));
	nameeq.assign(name, 0, namelen);
	nameeq.push_back('=');
	if (val)
		nameeq += *val;

	INTOFF(sh);
	variable = TRY_SH(setvareq(sh, std::move(nameeq), flags));
	INTON(sh);

	return variable;
}

shell::Result<void>
Context::set_var(
	std::string_view name,
	const std::optional<std::string> &value,
	const shell::Variable::Flags &flags
)
{
	TRY_SHV(setvar(*this, std::string(name), value, flags));
	return {};
}

shell::Result<std::intmax_t>
Context::set_int_var(
	std::string_view name,
	std::intmax_t value,
	const shell::Variable::Flags &flags
)
{
	TRY_SHV(set_var(name, std::format("{}", value), flags));

	return value;
}

/*
 * Same as setvar except that the variable and value are passed in
 * the first argument as name=value.
 * Called with interrupts off.
 */

shell::Result<shell::Variable *>
setvareq(Context &sh, std::string s, shell::Variable::Flags flags)
{
	static const shell::Variable::Flags special =
		shell::Variable::Flag::Exported |
		shell::Variable::Flag::ReadOnly |
		shell::Variable::Flag::StrFixed |
		shell::Variable::Flag::Unset;

	if (sh.opts.allexport())
		flags |= shell::Variable::Flag::Exported;

	std::string name(varname(s));
	auto it = sh.variables.find(name);
	shell::Variable *vp = nullptr;

	if (it != std::end(sh.variables)) {
		shell::Variable::Flags keep;

		vp = &it->second;
		if (vp->flags.test(shell::Variable::Flag::ReadOnly)) {
			return sh_error(sh, "{}: is read only", std::string_view(name));
		}

		if ((flags & special) != shell::Variable::Flag::Unset)
			keep = ~shell::Variable::Flag::Unset;
		else if (vp->flags.test(shell::Variable::Flag::StrFixed))
			keep = shell::Variable::Flag::StrFixed;
		else {
			sh.variables.erase(it);

			return nullptr;
		}

		flags |= vp->flags & keep;
		vp->text = std::move(s);
		vp->flags = flags;
	} else {
		if ((flags & special) == shell::Variable::Flag::Unset)
			return nullptr;
		shell::Variable v;
		v.text = std::move(s);
		v.flags = flags;
		auto [placed, inserted] = sh.variables.emplace(std::move(name), std::move(v));
		(void)inserted;
		vp = &placed->second;
	}

	if (!flags.test(shell::Variable::Flag::NoFunc))
		varfunc(sh, vp);

	return vp;
}

std::optional<std::string_view>
Context::get_var(std::string_view name) const
{
	const auto variable = findvar(*this, name);

	if (variable && !variable->flags.test(shell::Variable::Flag::Unset)) {
#ifdef WITH_LINENO
		if (name == "LINENO")
			variable->text = std::format("LINENO={}", lineno);
#endif
		return std::string_view(
			strchrnul(variable->text.c_str(), '=') + 1);
	}

	return std::nullopt;
}

shell::Result<std::intmax_t>
Context::get_int_var(std::string_view name) const
{
	const auto value = get_var(name);

	return atomax(const_cast<Context &>(*this),
		      value.value_or(std::string_view{}), 0);
}

/*
 * Generate a list of variables satisfying the given conditions.
 */

std::vector<std::string_view>
listvars(Context &sh, const shell::Variable::Flags &on, const shell::Variable::Flags &off)
{
	shell::Variable::Flags mask = on | off;
	std::vector<std::string_view> list;

	for (auto &[name, vp] : sh.variables) {
		(void)name;
		if ((vp.flags & mask) == on)
			list.emplace_back(vp.text);
	}

	return list;
}

std::vector<char *>
environment(Context &sh)
{
	auto views = listvars(sh, shell::Variable::Flag::Exported, shell::Variable::Flag::Unset);
	std::vector<char *> env;

	env.reserve(views.size() + 1);
	for (auto view : views)
		env.push_back(const_cast<char *>(view.data()));
	env.push_back(nullptr);

	return env;
}



/*
 * POSIX requires that 'set' (but not export or readonly) output the
 * variables in lexicographic order - by the locale's collating order (sigh).
 */

shell::Result<int>
showvars(Context &sh, std::string_view prefix, const shell::Variable::Flags &on, const shell::Variable::Flags &off)
{
	auto list = listvars(sh, on, off);
	std::string prefix_str(prefix);
	const char *sep = prefix.empty() ? "" : " ";

	std::sort(list.begin(), list.end(), [](std::string_view a, std::string_view b) {
		return varcmp(a, b) < 0;
	});

	for (auto entry : list) {
		const char *ep = entry.data();
		const char *p = strchrnul(ep, '=');
		std::string quoted;
		const char *q = "";

		if (*p) {
			quoted = single_quote(++p);
			q = quoted.c_str();
		}

		out1fmt(sh, "%s%s%.*s%s\n", prefix_str.c_str(), sep,
			static_cast<int>(p - ep), ep, q);
	}

	return 0;
}



/*
 * The export and readonly commands.
 */

shell::Result<int>
exportcmd(Context &sh, int argc, char **argv)
{
	char *name;
	const char *p;
	char **aptr;
	shell::Variable::Flags flag = argv[0][0] == 'r' ? shell::Variable::Flag::ReadOnly : shell::Variable::Flag::Exported;
	int notp;

	notp = TRY_SH(nextopt(sh, "p")) - 'p';
	if (notp && ((name = *(aptr = sh.bopt.argptr)))) {
		do {
			if ((p = std::strchr(name, '='))) {
				p++;
			} else {
				if (auto vp = findvar(sh, name)) {
					vp->flags |= flag;
					continue;
				}
			}
			TRY_SHV(sh.set_var(name, p ? std::optional<std::string>(p) : std::nullopt, flag));
		} while ((name = *++aptr));
	} else {
		TRY_SHV(showvars(sh, argv[0], flag, {}));
	}
	return 0;
}


/*
 * The "local" command.
 */

shell::Result<int>
localcmd(Context &sh, int argc, char **argv)
{
	char *name;

	if (sh.stack.empty())
		return sh_error(sh, "not in a function");

	argv = sh.bopt.argptr;
	while ((name = *argv++)) {
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
mklocal(Context &sh, std::string_view name, const shell::Variable::Flags &flags)
{
	shell::LocalVariable lv;
	shell::Variable *vp;

	INTOFF(sh);
	if (name == "-") {
		lv.text.assign(sh.opts.list.data(), sh.opts.list.size());
		vp = nullptr;
	} else {
		const auto eq = name.find('=');

		if (!(vp = findvar(sh, name))) {
			if (eq != std::string_view::npos)
				vp = TRY_SH(setvareq(sh, std::string(name),
						     shell::Variable::Flag::StrFixed | flags));
			else
				vp = TRY_SH(setvar(sh, std::string(name), std::nullopt,
						   shell::Variable::Flag::StrFixed | flags));
			lv.flags = shell::Variable::Flag::Unset;
		} else {
			lv.text = vp->text;
			lv.flags = vp->flags;
			vp->flags |= shell::Variable::Flag::StrFixed;
			if (eq != std::string_view::npos)
				TRY_SHV(setvareq(sh, std::string(name), flags));
		}
	}
	lv.variable = vp;
	sh.stack.back().variables.push_back(std::move(lv));
	INTON(sh);

	return {};
}


/*
 * Called after a function returns.
 * Interrupts must be off.
 */

static shell::Result<void>
poplocalvars(Context &sh)
{
	INTOFF(sh);

	shell::Frame frame = std::move(sh.stack.back());
	sh.stack.pop_back();

	for (auto it = frame.variables.rbegin(); it != frame.variables.rend(); ++it) {
		auto vp = it->variable;

		if (!vp) {	/* $- saved */
			std::memcpy(sh.opts.list.data(), it->text.data(),
				    sh.opts.list.size());
			optschanged(sh);
		} else if (it->flags == shell::Variable::Flag::Unset) {
			vp->flags &= ~(shell::Variable::Flag::StrFixed | shell::Variable::Flag::ReadOnly);
			unsetvar(sh, vp->text);
		} else {
			vp->flags = it->flags;
			vp->text = std::move(it->text);
			if (!vp->flags.test(shell::Variable::Flag::NoFunc))
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
pushlocalvars(Context &sh, int push)
{
	const auto stop = sh.stack.size();

	if (!push)
		return stop;

	INTOFF(sh);
	sh.stack.emplace_back();
	INTON(sh);

	return stop;
}


shell::Result<void>
unwindlocalvars(Context &sh, std::size_t stop)
{
	while (sh.stack.size() > stop)
		TRY_SHV(poplocalvars(sh));
	return {};
}


/*
 * The unset builtin command.  We unset the function before we unset the
 * variable to allow a function to be unset when there is a readonly variable
 * with the same name.
 */

shell::Result<int>
unsetcmd(Context &sh, int argc, char **argv)
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
unsetvar(Context &sh, const std::string &s)
{
	TRY_SHV(setvar(sh, s, std::nullopt));
	return {};
}



/**
 * Compares two strings up to the first = or end.  Either side may use
 * '=' as the name terminator.
 */
static int
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

static shell::Variable *
findvar(const Context &sh, std::string_view name)
{
	auto it = sh.variables.find(std::string(varname(name)));

	return it != std::end(sh.variables)
		? const_cast<shell::Variable *>(&it->second)
		: nullptr;
}
