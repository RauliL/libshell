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

#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

/*
 * When commands are first encountered, they are entered in a hash table.
 * This ensures that a full path search will not have to be done for them
 * on each invocation.
 */

#include "shell.h"
#include "main.h"
#include "nodes.h"
#include "parser.h"
#include "exec.h"
#include "builtins.h"
#include "var.h"
#include "options.h"
#include "output.h"
#include "error.h"
#include "mystring.h"
#include "alias.h"
#include "system.h"
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>

		/* index in path of %builtin, or -1 */

using namespace shell;

namespace shell {

namespace {
void tryexec(Context &sh, char *, char **, char **);
void printentry(Context &sh, const std::string &, const TableEntry &);
bool hash_affected(const TableEntry &, int);
void clearcmdentry(Context &sh);
TableEntry *cmdlookup(Context &sh, const char *, int);
void delete_cmd_entry(Context &sh, const char *);
void addcmdentry(Context &sh, const char *, const struct cmdentry *);
Result<int> describe_command(Context &sh, Output *, char *, const char *, int);
} // namespace

/*
 * Exec a program.  If you change this routine, you may have to change
 * the find_command routine as well.
 */

void
shellexec(Context &sh, char **argv, const char *path, int idx)
{
	int e;
	std::vector<char *> env = environment(sh);
	char **envp = env.data();
	int exerrno;

	if (std::strchr(argv[0], '/')) {
		tryexec(sh, argv[0], argv, envp);
		e = errno;
	} else {
		e = ENOENT;
		std::string candidate;
		while (padvance(sh, &path, argv[0], candidate) >= 0) {
			if (--idx < 0 && !sh.pathopt) {
				tryexec(sh, candidate.data(), argv, envp);
				if (errno != ENOENT && errno != ENOTDIR)
					e = errno;
			}
		}
	}

	/* Map to POSIX errors */
	switch (e) {
	default:
		exerrno = 126;
		break;
	case ELOOP:
	case ENAMETOOLONG:
	case ENOENT:
	case ENOTDIR:
		exerrno = 127;
		break;
	}
	sh.exitstatus = exerrno;
	exerror(sh, Error::End, "{}: {}", argv[0], errmsg(e, E_EXEC));
	/* NOTREACHED */
}

namespace {
void
tryexec(Context &sh, char *cmd, char **argv, char **envp)
{
	char path_bshell[] = _PATH_BSHELL;

repeat:
#ifdef SYSV
	do {
		execve(cmd, argv, envp);
	} while (errno == EINTR);
#else
	execve(cmd, argv, envp);
#endif
	if (cmd != path_bshell && errno == ENOEXEC) {
		*argv-- = cmd;
		*argv = cmd = path_bshell;
		goto repeat;
	}
}

const char *
legal_pathopt(Context &sh, const char *opt, const char *term, int magic)
{
	switch (magic) {
	case 0:
		opt = nullptr;
		break;

	case 1:
		opt = prefix(opt, "builtin") ?: prefix(opt, "func");
		break;

	default:
		opt += strcspn(opt, term);
		break;
	}

	if (opt && *opt == '%')
		opt++;

	return opt;
}
} // namespace

/*
 * Do a path search.  The variable path (passed by reference) should be
 * set to the start of the path before the first call; padvance will update
 * this value as it proceeds.  Successive calls to padvance will return
 * the possible path expansions in sequence.  If an option (indicated by
 * a percent sign) appears in the path entry then the global variable
 * sh.pathopt will be set to point to it; otherwise sh.pathopt will be set to
 * nullptr.
 *
 * If magic is 0 then sh.pathopt recognition will be disabled.  If magic is
 * 1 we shall recognise %builtin/%func.  Otherwise we shall accept any
 * sh.pathopt.
 */

int
padvance_magic(Context &sh, const char **path, const char *name, int magic,
	       std::string &out)
{
	const char *term = "%:";
	const char *lpathopt;
	const char *p;
	const char *start;
	size_t len;

	if (!*path)
		return -1;

	lpathopt = nullptr;
	start = *path;

	if (*start == '%' && (p = legal_pathopt(sh, start + 1, term, magic))) {
		lpathopt = start + 1;
		start = p;
		term = ":";
	}

	len = strcspn(start, term);
	p = start + len;

	if (*p == '%') {
		size_t extra = strchrnul(p, ':') - p;

		if (legal_pathopt(sh, p + 1, term, magic))
			lpathopt = p + 1;
		else
			len += extra;

		p += extra;
	}

	sh.pathopt = lpathopt;
	*path = *p == ':' ? p + 1 : nullptr;

	out.clear();
	if (likely(len)) {
		out.assign(start, len);
		out.push_back('/');
	}
	out.append(name);

	return static_cast<int>(out.size());
}

/*** Command hashing code ***/

/*
 * True for entries that a cd or a PATH change must invalidate.
 */
namespace {
inline bool
hash_affected(const TableEntry &cmdp, int builtinloc)
{
	return cmdp.type == TableEntry::Type::Normal || (
		cmdp.type == TableEntry::Type::Builtin &&
		!(cmdp.param.cmd->flags & BuiltinCommand::Flag::Regular) &&
		builtinloc > 0
	);
}
} // namespace

Result<int>
hashcmd(Context &sh, int argc, char **argv)
{
	int c;
	struct cmdentry entry;
	char *name;
	bool clear;

	clear = false;
	while ((c = TRY_SH(nextopt(sh, "r"))) != '\0')
		clear = true;
	if(clear) {
		clearcmdentry(sh);
		return 0;
	}

	if (!*sh.bopt.argptr) {
		for (const auto &cmd : sh.cmdtable) {
			if (cmd.second.type == TableEntry::Type::Normal)
				printentry(sh, cmd.first, cmd.second);
		}
		return 0;
	}
	c = 0;
	while ((name = *sh.bopt.argptr)) {
		TableEntry *cmdp = cmdlookup(sh, name, 0);
		if (cmdp && hash_affected(*cmdp, sh.builtinloc))
			delete_cmd_entry(sh, name);
		TRY_SHV(find_command(sh, name, &entry, DO_ERR, pathval(sh)));
		if (entry.type == TableEntry::Type::Unknown)
			c = 1;
		sh.bopt.argptr++;
	}
	return c;
}

namespace {
void
printentry(Context &sh, const std::string &cmdname, const TableEntry &cmdp)
{
	int idx;
	const char *path;
	std::string name;

	idx = cmdp.param.index;
	path = pathval(sh);
	do {
		padvance(sh, &path, cmdname.c_str(), name);
	} while (--idx >= 0);
	out1str(std::format("{}{}\n", name, cmdp.rehash ? "*" : ""));
}

int
test_exec(Context &sh, const char *fullname, struct stat64 *statb)
{
	if (!S_ISREG(statb->st_mode))
		return 0;

	if ((statb->st_mode & 0111) != 0111 &&
#ifdef HAVE_FACCESSAT
	    !test_file_access(sh, fullname, X_OK)
#else
	    !test_access(statb, X_OK)
#endif
	   )
		return 0;

	return 1;
}
} // namespace

/*
 * Resolve a command name.  If you change this routine, you may have to
 * change the shellexec routine as well.
 */

Result<void>
find_command(Context &sh, char *name, struct cmdentry *entry, int act, const char *path)
{
	TableEntry *cmdp;
	int idx;
	int prev;
	std::string fullname;
	struct stat64 statb;
	int e;
	int updatetbl;
	const BuiltinCommand *bcmd;

	// If name contains a slash, don't use PATH or hash table
	if (std::strchr(name, '/')) {
		entry->u.index = -1;
		if (act & DO_ABS) {
			while (stat64(name, &statb) < 0) {
#ifdef SYSV
				if (errno == EINTR)
					continue;
#endif
absfail:
				entry->type = TableEntry::Type::Unknown;
				return {};
			}
			if (!test_exec(sh, name, &statb))
				goto absfail;
		}
		entry->type = TableEntry::Type::Normal;
		return {};
	}

	updatetbl = (path == pathval(sh));
	if (!updatetbl)
		act |= DO_ALTPATH;

	// If name is in the table, check answer will be ok
	if ((cmdp = cmdlookup(sh, name, 0))) {
		int bit;

		switch (cmdp->type) {
		default:
		case TableEntry::Type::Normal:
			bit = DO_ALTPATH | DO_REGBLTIN;
			break;
		case TableEntry::Type::Function:
			bit = DO_NOFUNC;
			break;
		case TableEntry::Type::Builtin:
			bit = cmdp->param.cmd->flags & BuiltinCommand::Flag::Regular ?
			      0 : DO_REGBLTIN;
			break;
		}
		if (act & bit) {
			if (act & bit & DO_REGBLTIN)
				goto fail;

			updatetbl = 0;
			cmdp = nullptr;
		} else if (!cmdp->rehash)
			/* if not invalidated by cd, we're done */
			goto success;
	}

	/* If %builtin not in path, check for builtin next */
	bcmd = find_builtin(name);
	if (bcmd && ((bcmd->flags & BuiltinCommand::Flag::Regular) | (act & DO_ALTPATH) |
		     (sh.builtinloc <= 0)))
		goto builtin_success;

	if (act & DO_REGBLTIN)
		goto fail;

	/* We have to search path. */
	prev = -1;		/* where to start */
	if (cmdp && cmdp->rehash) {	/* doing a rehash */
		if (cmdp->type == TableEntry::Type::Builtin)
			prev = sh.builtinloc;
		else
			prev = cmdp->param.index;
	}

	e = ENOENT;
	idx = -1;
loop:
	while (padvance(sh, &path, name, fullname) >= 0) {
		const char *lpathopt = sh.pathopt;

		idx++;
		if (lpathopt) {
			if (*lpathopt == 'b') {
				if (bcmd)
					goto builtin_success;
				continue;
			} else if (!(act & DO_NOFUNC)) {
				/* handled below */
			} else {
				/* ignore unimplemented options */
				continue;
			}
		}
		/* if rehash, don't redo absolute path names */
		if (fullname[0] == '/' && idx <= prev) {
			if (idx < prev)
				continue;
			goto success;
		}
		while (stat64(fullname.c_str(), &statb) < 0) {
#ifdef SYSV
			if (errno == EINTR)
				continue;
#endif
			if (errno != ENOENT && errno != ENOTDIR)
				e = errno;
			goto loop;
		}
		if (lpathopt) {		/* this is a %func directory */
			std::string pathcopy(fullname);
			TRY_SHV(readcmdfile(sh, pathcopy.data()));
			if (!(cmdp = cmdlookup(sh, name, 0)) ||
			    cmdp->type != TableEntry::Type::Function)
				return sh_error(sh, "{} not defined in {}", name,
					 pathcopy);
			goto success;
		}
		e = EACCES;	/* if we fail, this will be the error */
		if (!test_exec(sh, fullname.c_str(), &statb))
			continue;
		if (!updatetbl) {
			entry->type = TableEntry::Type::Normal;
			entry->u.index = idx;
			return {};
		}
		INTOFF(sh);
		cmdp = cmdlookup(sh, name, 1);
		cmdp->type = TableEntry::Type::Normal;
		cmdp->param.index = idx;
		INTON(sh);
		goto success;
	}

	/* We failed.  If there was an entry for this command, delete it */
	if (cmdp && updatetbl)
		delete_cmd_entry(sh, name);
	if (act & DO_ERR)
		sh_warnx(sh, "{}: {}", name, errmsg(e, E_EXEC));
fail:
	entry->type = TableEntry::Type::Unknown;
	return {};

builtin_success:
	if (!updatetbl) {
		entry->type = TableEntry::Type::Builtin;
		entry->u.cmd = bcmd;
		return {};
	}
	INTOFF(sh);
	cmdp = cmdlookup(sh, name, 1);
	cmdp->type = TableEntry::Type::Builtin;
	cmdp->param.cmd = bcmd;
	INTON(sh);
success:
	cmdp->rehash = false;
	entry->type = cmdp->type;
	entry->u = cmdp->param;
	return {};
}

/*
 * Search the table of builtin commands.
 */

/*
 * Called when a cd is done.  Marks all commands so the next time they
 * are executed they will be rehashed.
 */

void
hashcd(Context &sh)
{
	for (auto &entry : sh.cmdtable) {
		if (hash_affected(entry.second, sh.builtinloc))
			entry.second.rehash = true;
	}
}

/*
 * Fix command hash table when PATH sh.changed.
 * Called before PATH is sh.changed.  The argument is the new value of PATH;
 * pathval() still returns the old value at this point.
 * Called with interrupts off.
 */

void
changepath(Context &sh, std::string_view newval)
{
	std::string path(newval);
	const char *scan;
	int idx;
	int bltin;

	scan = path.c_str();
	idx = 0;
	bltin = -1;
	for (;;) {
		if (*scan == '%' && prefix(scan + 1, "builtin")) {
			bltin = idx;
			break;
		}
		scan = std::strchr(scan, ':');
		if (!scan)
			break;
		idx++;
		scan++;
	}
	sh.builtinloc = bltin;
	clearcmdentry(sh);
}

/*
 * Clear out command entries.  The argument specifies the first entry in
 * PATH which has sh.changed.
 */

namespace {
void
clearcmdentry(Context &sh)
{
	INTOFF(sh);
	std::erase_if(
		sh.cmdtable,
		[&](const auto &entry) -> bool {
			return hash_affected(entry.second, sh.builtinloc);
		}
	);
	INTON_DEFER(sh);
}
} // namespace

/*
 * Locate a command in the command hash table.  If "add" is nonzero,
 * add the command to the table if it is not already present.
 *
 * Interrupts must be off if called with add != 0.
 * The returned pointer is invalidated by a later insertion.
 */

namespace {
TableEntry *
cmdlookup(Context &sh, const char *name, int add)
{
	auto it = sh.cmdtable.find(name);
	if (it == sh.cmdtable.end()) {
		if (!add)
			return nullptr;
		it = sh.cmdtable.try_emplace(std::string(name)).first;
	}
	return &it->second;
}
} // namespace

/*
 * Delete the command entry for this name, if one exists.
 */

namespace {
void
delete_cmd_entry(Context &sh, const char *name)
{
	INTOFF(sh);
	sh.cmdtable.erase(name);
	INTON_DEFER(sh);
}
} // namespace

/*
 * Add a new command entry, replacing any existing command entry for
 * the same name - except special builtins.
 */

namespace {
void
addcmdentry(Context &sh, const char *name, const struct cmdentry *entry)
{
	TableEntry *cmdp;

	cmdp = cmdlookup(sh, name, 1);
	cmdp->type = entry->type;
	cmdp->param = entry->u;
	cmdp->rehash = false;
}
} // namespace

/*
 * Define a shell function.
 */

void
defun(Context &sh, const DefunNode *func)
{
	struct cmdentry entry;

	INTOFF(sh);
	entry.type = TableEntry::Type::Function;
	entry.u.func = std::shared_ptr<Node>(func->clone());
	addcmdentry(sh, func->text.c_str(), &entry);
	INTON_DEFER(sh);
}

/*
 * Delete a function if it exists.
 */

void
unsetfunc(Context &sh, std::string_view name)
{
	std::string key(name);
	TableEntry *cmdp = cmdlookup(sh, key.c_str(), 0);

	if (cmdp && cmdp->type == TableEntry::Type::Function)
		delete_cmd_entry(sh, key.c_str());
}

/*
 * Locate and print what a word is...
 */

Result<int>
typecmd(Context &sh, int argc, char **argv)
{
	int err = 0;

	TRY_SHV(nextopt(sh, ""));
	while (*sh.bopt.argptr) {
		err |= TRY_SH(describe_command(sh, sh.out1, *sh.bopt.argptr++, nullptr, 1));
	}
	return err;
}

namespace {
Result<int>
describe_command(
	Context &sh,
	Output *out,
	char *command,
    const char *path,
	int verbose
)
{
	struct cmdentry entry;
	TableEntry *cmdp;
	if (verbose) {
		out->write(sh, command);
	}

	/* First look at the keywords */
	if (findkwd(command)) {
		out->write(sh, verbose ? " is a shell keyword" : command);
		goto out;
	}

	// Then look at the aliases
	if (const auto ap = lookupalias(sh, command, false)) {
		if (verbose) {
			out->write(sh, std::format(" is an alias for {}", ap->value));
		} else {
			out->write(sh, "alias ");
			printalias(sh, *ap);

			return 0;
		}
		goto out;
	}

	/* Then if the standard search path is used, check if it is
	 * a tracked alias.
	 */
	if (!path) {
		path = pathval(sh);
		cmdp = cmdlookup(sh, command, 0);
	} else {
		cmdp = nullptr;
	}

	if (cmdp) {
		entry.type = cmdp->type;
		entry.u = cmdp->param;
	} else {
		/* Finally use brute force */
		TRY_SHV(find_command(sh, command, &entry, DO_ABS, path));
	}

	switch (entry.type) {
	case TableEntry::Type::Normal: {
		int j = entry.u.index;
		std::string pathbuf;
		const char *p;
		if (j == -1) {
			p = command;
		} else {
			do {
				padvance(sh, &path, command, pathbuf);
			} while (--j >= 0);
			p = pathbuf.c_str();
		}
		if (verbose) {
			out->write(
				sh,
				std::format(
					" is{} {}",
					cmdp ? " a tracked alias for" : "",
					p
				)
			);
		} else {
			out->write(sh, p);
		}
		break;
	}

	case TableEntry::Type::Function:
		if (verbose) {
			out->write(sh, " is a shell function");
		} else {
			out->write(sh, command);
		}
		break;

	case TableEntry::Type::Builtin:
		if (verbose) {
			out->write(
				sh,
				std::format(
					" is a {}shell builtin",
					entry.u.cmd->flags & BuiltinCommand::Flag::Special ? "special " : ""
				)
			);
		} else {
			out->write(sh, command);
		}
		break;

	default:
		if (verbose) {
			out->write(sh, ": not found\n");
		}
		return 127;
	}

out:
	outc(sh, '\n', out);
	return 0;
}
} // namespace

Result<int>
commandcmd(Context &sh, int argc, char **argv)
{
	char *cmd;
	int c;
	enum {
		VERIFY_BRIEF = 1,
		VERIFY_VERBOSE = 2,
	};
	int verify = 0;
	const char *path = nullptr;

	while ((c = TRY_SH(nextopt(sh, "pvV"))) != '\0')
		if (c == 'V')
			verify |= VERIFY_VERBOSE;
		else if (c == 'v')
			verify |= VERIFY_BRIEF;
		else
			path = defpath;

	cmd = *sh.bopt.argptr;
	if (verify && cmd)
		return describe_command(sh, sh.out1, cmd, path, verify - VERIFY_BRIEF);

	return 0;
}

} // namespace shell
