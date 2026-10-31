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
#include <cwchar>
#include <inttypes.h>
#include <limits.h>
#include <optional>
#include <string>
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
void changepath(ShellContext &sh, const char *val);
void getoptsreset(ShellContext &sh, const char *val);
void changelocale(ShellContext &sh, const char *val);

struct ShellContext {
	StackStr stackstr;

	struct output output = {
		nullptr, nullptr, nullptr, BUFSIZ, 1, 0
	};
	struct output errout = {
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

	std::optional<std::string> trap[NSIG] = {};
	int ptrap = 0;
	int trapcnt = 0;
	char sigmode[NSIG - 1] = {};
	char gotsig[NSIG - 1] = {};
	volatile sig_atomic_t pending_sig = 0;
	volatile sig_atomic_t gotsigchld = 0;

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
		6
#ifdef WITH_LINENO
		+ 1
#endif
		+ 5;
	struct var varinit[varinit_len] = {
		{ nullptr, VSTRFIXED, defifsvar, changeifs },
		{ nullptr, VSTRFIXED, defpathvar, changepath },
		{ nullptr, VSTRFIXED, "PS1=$ ", nullptr },
		{ nullptr, VSTRFIXED, "PS2=> ", nullptr },
		{ nullptr, VSTRFIXED, "PS4=+ ", nullptr },
		{ nullptr, VSTRFIXED|VNOFUNC, defoptindvar, getoptsreset },
#ifdef WITH_LINENO
		{ nullptr, VSTRFIXED, linenovar, nullptr },
#endif
		{ nullptr, VSTRFIXED|VFULL|VUNSET, "LC_ALL=", changelocale },
		{ nullptr, VSTRFIXED|VFULL|VUNSET, "LC_COLLATE=", changelocale },
		{ nullptr, VSTRFIXED|VFULL|VUNSET, "LC_CTYPE=", changelocale },
		{ nullptr, VSTRFIXED|VFULL|VUNSET, "LC_NUMERIC=", changelocale },
		{ nullptr, VSTRFIXED|VFULL|VUNSET, "LANG=", changelocale },
	};
	struct var *vartab[39] = {};

	char *expdest = nullptr;
	const std::vector<union node *> *argbackq = nullptr;
	std::size_t argbackq_i = 0;
	struct ifsregion ifsfirst = {};
	struct ifsregion *ifslastp = nullptr;
	ArgList exparg;
	char ifsmap[128] = {};
	const char *ncifs = nullptr;
	size_t ifsmb0len = 0;
	std::wstring wcifs;

	struct heredoc *heredoclist = nullptr;
	int doprompt = 0;
	int needprompt = 0;
	int lasttoken = 0;
	int tokpushback = 0;
	std::string wordtext;
	int checkkwd = 0;
	std::vector<union node *> backquotelist;
	union node *redirnode = nullptr;
	struct heredoc *heredoc = nullptr;
	int quoteflag = 0;

	int evalskip = 0;
	int skipcount = 0;
	int loopnest = 0;
	int funcline = 0;
	std::string commandname;
	int exitstatus = 0;
	int back_exitstatus = 0;
	int savestatus = -1;
	int inps4 = 0;
	int tpip[2] = { -1, -1 };

	std::string arg0;
	struct shparam shellparam = {};
	char **argptr = nullptr;
	char *optionarg = nullptr;
	char *optptr = nullptr;
	std::optional<std::string> minusc;	/* nullopt = no -c */
	char optlist[NOPTS] = {};

	int rootpid = 0;
	int mypid = 0;
	int shlvl = 0;

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
	std::string curdir;	/* empty when unknown */
	std::string physdir;	/* empty when unknown */

	char **t_wp = nullptr;
	const struct t_op *t_wp_op = nullptr;
	int rval = 0;
	char **gargv = nullptr;

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

inline char *bltinlookup(ShellContext &sh, const char *name)
{
	return lookupvar(sh, name);
}

#endif
