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

#include "error.h"
#include "eval.h"
#include "expand.h"
#include "input.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "system.h"
#include "var.h"
#ifndef SMALL
#include "myhistedit.h"
#endif

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

struct ShellContext {
	struct stack_block stackbase = {};
	struct stack_block *stackp = &stackbase;
	char *stacknxt = stackbase.space;
	size_t stacknleft = MINSIZE;
	char *sstrend = stackbase.space + MINSIZE;

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
#ifndef SMALL
		+ 2
#endif
		+ 5;
	struct var varinit[varinit_len] = {
#if ATTY
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "ATTY\0", nullptr },
#endif
		{ nullptr, VSTRFIXED|VTEXTFIXED, defifsvar, &ShellContext::changeifs },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "MAIL\0", &ShellContext::changemail },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "MAILPATH\0", &ShellContext::changemail },
		{ nullptr, VSTRFIXED|VTEXTFIXED, defpathvar, &ShellContext::changepath },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS1=$ ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS2=> ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED, "PS4=+ ", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VNOFUNC, defoptindvar, &ShellContext::getoptsreset },
#ifdef WITH_LINENO
		{ nullptr, VSTRFIXED|VTEXTFIXED, linenovar, nullptr },
#endif
#ifndef SMALL
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "TERM\0", nullptr },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VUNSET, "HISTSIZE\0", &ShellContext::sethistsize },
#endif
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_ALL\0", &ShellContext::changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_COLLATE\0", &ShellContext::changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_CTYPE\0", &ShellContext::changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LC_NUMERIC\0", &ShellContext::changelocale },
		{ nullptr, VSTRFIXED|VTEXTFIXED|VFULL|VUNSET, "LANG\0", &ShellContext::changelocale },
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
#ifdef __GLIBC__
	int *dash_errno = nullptr;
#endif
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

#ifndef SMALL
	History *hist = nullptr;
	EditLine *el = nullptr;
	int displayhist = 0;
	FILE *el_in = nullptr;
	FILE *el_out = nullptr;
	int fc_active = 0;
	const char *rl_cp = nullptr;
	int el_len = 0;
#endif
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

	void init();
	void exitreset();
	void forkreset(union node *);
	void postexitreset();
	void reset();

	void grabstackblock(size_t len)
	{
		stalloc(len);
	}

	char *_STPUTC(int c, char *p)
	{
		if (p == sstrend)
			p = growstackstr();
		*p++ = c;
		return p;
	}

	void freestdout()
	{
		output.nextc = output.buf;
		output.flags = 0;
	}

#ifdef USE_GLIBC_STDIO
	void outc(int ch, struct output *file)
	{
		putc(ch, file->stream);
	}
#else
	void outc(int ch, struct output *file)
	{
		if (file->nextc == file->end)
			outcslow(ch, file);
		else {
			*file->nextc = ch;
			file->nextc++;
		}
	}
#endif

	int padvance(const char **path, const char *name)
	{
		return padvance_magic(path, name, 1);
	}

	int have_traps(void)
	{
		return trapcnt;
	}

	char *bltinlookup(const char *name)
	{
		return lookupvar(name);
	}

	int goodname(const char *p)
	{
		return !*endofname(p);
	}

	int parser_eof(void)
	{
		return tokpushback && lasttoken == TEOF;
	}

#include "shell_context_methods.inc"

	int execute(char *command, EvalFlags flags)
	{
		return evalstring(command, flags);
	}
};

#endif
