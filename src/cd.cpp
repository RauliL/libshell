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
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>
#ifdef __CYGWIN__
#include <sys/cygwin.h>
#endif

/*
 * The cd and pwd commands.
 */

#include "cd.h"
#include "error.h"
#include "exec.h"
#include "options.h"
#include "output.h"
#include "shell.h"
#include "var.h"

#define CD_PHYSICAL 1
#define CD_PRINT 2

using namespace shell;

namespace shell {

namespace {
Result<int> docd(Context &sh, const char *, int);
Result<std::optional<std::string>> updatepwd(Context &sh, const char *);
std::string getpwd(Context &sh);
Result<int> cdopt(Context &sh);

Result<int>
cdopt(Context &sh)
{
	int flags = 0;
	int i, j;

	j = 'L';
	for (;;) {
		TRY_SH_SET(i, nextopt(sh, "LP"));
		if (!i)
			break;
		if (i != j) {
			flags ^= CD_PHYSICAL;
			j = i;
		}
	}

	return flags;
}
} // namespace

Result<int>
cdcmd(Context &sh, int argc, char **argv)
{
	const char *dest;
	const char *path;
	const char *p;
	char c;
	struct stat64 statb;
	int flags;
	int len;
	std::string candidate;

	TRY_SH_SET(flags, cdopt(sh));
	dest = *sh.bopt.argptr;
	if (!dest)
		dest = bltinlookup(sh, "HOME");
	else if (dest[0] == '-' && dest[1] == '\0') {
		dest = bltinlookup(sh, "OLDPWD");
		flags |= CD_PRINT;
	}
	if (!dest)
		dest = "";
	if (*dest == '/')
		goto step6;
	if (*dest == '.') {
		c = dest[1];
dotdot:
		switch (c) {
		case '\0':
		case '/':
			goto step6;
		case '.':
			c = dest[2];
			if (c != '.')
				goto dotdot;
		}
	}
	if (!*dest)
		dest = ".";
	path = bltinlookup(sh, "CDPATH");
	while (p = path, (len = padvance_magic(sh, &path, dest, 0, candidate)) >= 0) {
		c = *p;
		p = candidate.c_str();

		if (stat64(p, &statb) >= 0 && S_ISDIR(statb.st_mode)) {
			if (c && c != ':')
				flags |= CD_PRINT;
docd:
			{
				int ok;
				TRY_SH_SET(ok, docd(sh, p, flags));
				if (!ok)
					goto out;
			}
			goto err;
		}
	}

step6:
	p = dest;
	goto docd;

err:
	return sh_error(sh, "can't cd to {}", dest);
out:
	if (flags & CD_PRINT)
        out1str(std::format("{}\n", sh.cwd.curdir));
	return 0;
}

/*
 * Actually do the chdir.  We also call hashcd to let the routines in exec.c
 * know that the current directory has sh.changed.
 */

namespace {
Result<int>
docd(Context &sh, const char *dest, int flags)
{
	std::optional<std::string> logical;
	const char *dir = nullptr;
	int err;

	INTOFF(sh);
	if (!(flags & CD_PHYSICAL)) {
		auto up = updatepwd(sh, dest);
		if (!up)
			return tl::unexpected(up.error());
		logical = std::move(*up);
		if (logical)
			dest = dir = logical->c_str();
	}
	err = chdir(dest);
	if (err)
		goto out;
	TRY_SHV(setpwd(sh,
			 dir ? std::optional<std::string_view>(dir)
			     : std::nullopt,
			 1));
	hashcd(sh);
out:
	INTON(sh);

	return err;
}
} // namespace

/*
 * Update curdir (the name of the current directory) in response to a
 * cd command.
 */

namespace {
Result<std::optional<std::string>>
updatepwd(Context &sh, const char *dir)
{
	std::string path;
	std::string components;
	std::size_t lim = 1;
	char *tokctx = nullptr;

#ifdef __CYGWIN__
	/* On cygwin, thanks to drive letters, some absolute paths do
	   not begin with slash; but cygwin includes a function that
	   forces normalization to the posix form */
	char pathbuf[PATH_MAX];
	if (cygwin_conv_path(CCP_WIN_A_TO_POSIX | CCP_RELATIVE, dir, pathbuf,
			     sizeof(pathbuf)) < 0)
		return sh_error(sh, "can't normalize {}", dir);
	dir = pathbuf;
#endif

	if (*dir != '/') {
		if (sh.cwd.curdir.empty())
			return std::optional<std::string>{};
		path = sh.cwd.curdir;
	}

	components.assign(dir);
	if (*dir != '/') {
		if (path.empty() || path.back() != '/')
			path.push_back('/');
		if (path.size() > 1 && path[1] == '/')
			lim = 2;
	} else {
		path.assign(1, '/');
		if (dir[1] == '/' && dir[2] != '/') {
			path.push_back('/');
			lim = 2;
		}
	}

	char *comp = strtok_r(components.data(), "/", &tokctx);
	while (comp) {
		switch (*comp) {
		case '.':
			if (comp[1] == '.' && comp[2] == '\0') {
				while (path.size() > lim) {
					path.pop_back();
					if (!path.empty() && path.back() == '/')
						break;
				}
				break;
			} else if (comp[1] == '\0')
				break;
			/* fall through */
		default:
			path.append(comp);
			path.push_back('/');
		}
		comp = strtok_r(nullptr, "/", &tokctx);
	}
	if (path.size() > lim && !path.empty() && path.back() == '/')
		path.pop_back();
	return std::optional<std::string>{std::move(path)};
}
} // namespace

/*
 * Find out what the current directory is. If we already know the current
 * directory, this routine returns immediately.
 */
namespace {
std::string
getpwd(Context &sh)
{
#ifdef __GLIBC__
	char *dir = getcwd(0, 0);

	if (dir) {
		std::string s(dir);
		free(dir);
		return s;
	}
#else
	char buf[PATH_MAX];

	if (getcwd(buf, sizeof(buf)))
		return buf;
#endif

	sh_warnx(sh, "getcwd() failed: {}", strerror(errno));
	return {};
}
} // namespace

Result<int>
pwdcmd(Context &sh, int argc, char **argv)
{
	int flags;
	const char *dir = sh.cwd.curdir.c_str();

	TRY_SH_SET(flags, cdopt(sh));
	if (flags) {
		if (sh.cwd.physdir.empty())
			TRY_SHV(setpwd(sh, std::nullopt, 0, true));
		dir = sh.cwd.physdir.c_str();
	}
    out1str(std::format("{}\n", dir));
	return 0;
}

Result<void>
setpwd(
	Context &sh,
	std::optional<std::string_view> val,
	int setold,
	bool refresh_phys
)
{
	if (setold)
		TRY_SHV(setvar(sh, "OLDPWD", sh.cwd.curdir, Variable::Flag::Exported));

	INTOFF(sh);
	if (refresh_phys) {
		sh.cwd.physdir = getpwd(sh);
	} else {
		sh.cwd.physdir.clear();
		if (!val) {
			sh.cwd.curdir = getpwd(sh);
			sh.cwd.physdir = sh.cwd.curdir;
		} else {
			sh.cwd.curdir = *val;
		}
	}
	INTON(sh);
	TRY_SHV(setvar(sh, "PWD", sh.cwd.curdir, Variable::Flag::Exported));

	return {};
}

} // namespace shell
