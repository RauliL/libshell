#ifndef SHELL_CONTEXT_H
#define SHELL_CONTEXT_H

/*
 * One shell. Mutable shell state lives here so the program can be used as a
 * library: the caller owns a ShellContext and runs commands on that object.
 * signaled is the process-wide target for asynchronous signals, because a
 * POSIX handler is only passed the signal number.
 */

#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <inttypes.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <termios.h>
#include <vector>

#include "error.h"
#include "eval.h"
#include "expand.h"
#include "input.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "stackstr.h"
#include "system.h"
#include "var.h"

union node;
struct nodelist;
struct funcnode;
struct job;
struct backcmd;
struct alias;
struct redirtab;
struct heredoc;
struct synstack;
struct localvar_list;
struct tblentry;
struct t_op;
struct ifs_state;
struct builtincmd;
struct cmdentry;
enum token : int;
enum limtype : int;

struct stdin_state {
	off_t seekable;
	int pip[2];
	int pending;
	tcflag_t bufferable;
};

struct ShellContext;

void changeifs(ShellContext &sh, const char *val);
void changemail(ShellContext &sh, const char *val);
void changepath(ShellContext &sh, const char *val);
void getoptsreset(ShellContext &sh, const char *val);
void changelocale(ShellContext &sh, const char *val);

struct ShellContext {
	StackStr stackstr;
	std::vector<void *> stackheaps;

	struct output output = {
#ifdef USE_GLIBC_STDIO
		nullptr,
#endif
		nullptr, nullptr, nullptr,
#ifdef USE_GLIBC_STDIO
		0,
#else
		BUFSIZ,
#endif
		1, 0
	};
	struct output errout = {
#ifdef USE_GLIBC_STDIO
		nullptr,
#endif
		nullptr, nullptr, nullptr, 0, 2, 0
	};
	struct output preverrout = {};
	struct output *out1 = &output;
	struct output *out2 = &errout;

	struct parsefile basepf = {};
	char basebuf[IBUFSIZ] = {};
	struct parsefile *toppf = &basepf;
	struct parsefile *parsefile = &basepf;
	struct stdin_state stdin_state = {};
	int whichprompt = 0;
	int stdin_istty = -1;

	char *trap[NSIG] = {};
	int ptrap = 0;
	int trapcnt = 0;
	char sigmode[NSIG - 1] = {};
	char gotsig[NSIG - 1] = {};
	volatile sig_atomic_t pending_sig = 0;
	volatile sig_atomic_t gotsigchld = 0;
	int is_interactive = 0;

	struct job *jobtab = nullptr;
	unsigned njobs = 0;
	pid_t backgndpid = 0;
#if JOBS
	int initialpgrp = 0;
	int ttyfd = -1;
#endif
	struct job *curjob = nullptr;
	int vforked = 0;
	int jobctl = 0;
	int job_warning = 0;
	char *cmdnextc = nullptr;

	char defifsvar[sizeof("IFS= \t\n")] = "IFS= \t\n";
	char defoptindvar[sizeof("OPTIND=1")] = "OPTIND=1";
	int lineno = 0;
	char linenovar[sizeof("LINENO=") + sizeof(int) * CHAR_BIT / 3 + 1] = "LINENO=";
	char ppidbuf[32] = "PPID=";
	struct localvar_list *localvar_stack = nullptr;

	static constexpr int varinit_len =
#if ATTY
		1 +
#endif
		8
#ifdef WITH_LINENO
		+ 1
#endif
		+ 5;
	struct var varinit[varinit_len] = {
#if ATTY
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "ATTY\0", nullptr },
#endif
		{ nullptr, VSTRFIXED|VTEXTFIXED, defifsvar, changeifs },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "MAIL\0", changemail },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "MAILPATH\0", changemail },
		{ nullptr, VSTRFIXED|VTEXTFIXED, defpathvar, changepath },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS1=$ ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS2=> ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS4=+ ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VNOFUNC, defoptindvar, getoptsreset },
#ifdef WITH_LINENO
		{ nullptr, VSTRFIXED|VTEXTFIXED, linenovar, nullptr },
#endif
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_ALL\0", changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_COLLATE\0", changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_CTYPE\0", changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_NUMERIC\0", changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LANG\0", changelocale },
	};
	struct var *vartab[39] = {};

	char *expdest = nullptr;
	struct nodelist *argbackq = nullptr;
	struct ifsregion ifsfirst = {};
	struct ifsregion *ifslastp = nullptr;
	struct arglist exparg = {};
	char ifsmap[128] = {};
	const char *ncifs = nullptr;
	size_t ifsmb0len = 0;
	wchar_t *wcifs = nullptr;

	struct heredoc *heredoclist = nullptr;
	int doprompt = 0;
	int needprompt = 0;
	int lasttoken = 0;
	int tokpushback = 0;
	char *wordtext = nullptr;
	int checkkwd = 0;
	struct nodelist *backquotelist = nullptr;
	union node *redirnode = nullptr;
	struct heredoc *heredoc = nullptr;
	int quoteflag = 0;

	int evalskip = 0;
	int skipcount = 0;
	int loopnest = 0;
	int funcline = 0;
	char *commandname = nullptr;
	int exitstatus = 0;
	int back_exitstatus = 0;
	int savestatus = -1;
	int inps4 = 0;
	int tpip[2] = { -1, -1 };

	char *arg0 = nullptr;
	struct shparam shellparam = {};
	char **argptr = nullptr;
	char *optionarg = nullptr;
	char *optptr = nullptr;
	char *minusc = nullptr;
	char optlist[NOPTS] = {};

	int rootpid = 0;
	int mypid = 0;
	int shlvl = 0;
	struct jmploc main_handler = {};

	struct jmploc *handler = nullptr;
	int exception = 0;
	int suppressint = 0;
	volatile sig_atomic_t intpending = 0;
	int errlinno = 0;

	const char *pathopt = nullptr;
	struct tblentry *cmdtable[31] = {};
	int builtinloc = -1;
	struct tblentry **lastcmdentry = nullptr;

	struct alias *atab[39] = {};
	struct redirtab *redirlist = nullptr;
	unsigned closed_redirs = 0;
	char *curdir = nullstr;
	char *physdir = nullstr;

	time_t mailtime[10] = {};
	int changed = 0;

	char **t_wp = nullptr;
	const struct t_op *t_wp_op = nullptr;
	int rval = 0;
	char **gargv = nullptr;

	int funcblocksize = 0;
	int funcstringsize = 0;
	pointer funcblock = nullptr;
	char *funcstring = nullptr;

#ifdef DEBUG
	FILE *tracefile = nullptr;
#endif

	inline static ShellContext *signaled = nullptr;

	ShellContext()
	{
		signaled = this;
		basepf.nextc = basepf.buf = basebuf;
		basepf.linno = 1;
	}

	~ShellContext()
	{
		if (signaled == this)
			signaled = nullptr;
	}
};

#include "shell_context_methods.inc"

inline void freestdout(ShellContext &sh)
{
	sh.output.nextc = sh.output.buf;
	sh.output.flags = 0;
}

#ifdef USE_GLIBC_STDIO
inline void outc(ShellContext &sh, int ch, struct output *file)
{
	(void)sh;
	putc(ch, file->stream);
}
#else
inline void outc(ShellContext &sh, int ch, struct output *file)
{
	if (file->nextc == file->end)
		outcslow(sh, ch, file);
	else {
		*file->nextc = ch;
		file->nextc++;
	}
}
#endif

inline int padvance(ShellContext &sh, const char **path, const char *name)
{
	return padvance_magic(sh, path, name, 1);
}

inline int have_traps(ShellContext &sh)
{
	return sh.trapcnt;
}

inline char *bltinlookup(ShellContext &sh, const char *name)
{
	return lookupvar(sh, name);
}

inline int goodname(ShellContext &sh, const char *p)
{
	return !*endofname(sh, p);
}

inline int parser_eof(ShellContext &sh)
{
	return sh.tokpushback && sh.lasttoken == TEOF;
}

inline int execute(ShellContext &sh, char *command, EvalFlags flags)
{
	return evalstring(sh, command, flags);
}

#endif
