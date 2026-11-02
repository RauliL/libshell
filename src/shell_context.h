#pragma once

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
#include <memory>
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
#include "jobs.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "system.h"
#include "var.h"

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
enum limtype : int;

struct ShellContext;

void changeifs(ShellContext &sh, std::string_view val);
void changepath(ShellContext &sh, std::string_view val);
void getoptsreset(ShellContext &sh, std::string_view val);
void changelocale(ShellContext &sh, std::string_view val);

struct ShellContext {
	using signal_map_type = std::unordered_map<int, std::string_view>;

	struct output output = {
		nullptr, nullptr, nullptr, BUFSIZ, 1, 0
	};
	struct output errout = {
		nullptr, nullptr, nullptr, 0, 2, 0
	};
	struct output preverrout = {};
	struct output *out1 = &output;
	struct output *out2 = &errout;

	InputStack input;
	int whichprompt = 0;

	std::optional<std::string> trap[NSIG] = {};
	int ptrap = 0;
	int trapcnt = 0;
	char sigmode[NSIG - 1] = {};
	char gotsig[NSIG - 1] = {};
	volatile sig_atomic_t pending_sig = 0;
	volatile sig_atomic_t gotsigchld = 0;

	std::vector<std::unique_ptr<Job>> jobs;
	std::vector<Job *> job_order;	/* job-control order (stopped first) */
	pid_t backgndpid = 0;
#if JOBS
	int initialpgrp = 0;
	int ttyfd = -1;
#endif
	int vforked = 0;
	int jobctl = 0;
	int job_warning = 0;
	char defifsvar[sizeof("IFS= \t\n")] = "IFS= \t\n";
	char defoptindvar[sizeof("OPTIND=1")] = "OPTIND=1";
	int lineno = 0;
	struct localvar_list *localvar_stack = nullptr;

	static constexpr int varinit_len =
		6
#ifdef WITH_LINENO
		+ 1
#endif
		+ 5;
	struct var varinit[varinit_len] = {
		{ nullptr, VarFlag::str_fixed, defifsvar, changeifs },
		{ nullptr, VarFlag::str_fixed, defpathvar, changepath },
		{ nullptr, VarFlag::str_fixed, "PS1=$ ", nullptr },
		{ nullptr, VarFlag::str_fixed, "PS2=> ", nullptr },
		{ nullptr, VarFlag::str_fixed, "PS4=+ ", nullptr },
		{ nullptr, VarFlag::str_fixed | VarFlag::nofunc, defoptindvar, getoptsreset },
#ifdef WITH_LINENO
		{ nullptr, VarFlag::str_fixed, "LINENO=", nullptr },
#endif
		{ nullptr, VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_ALL=", changelocale },
		{ nullptr, VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_COLLATE=", changelocale },
		{ nullptr, VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_CTYPE=", changelocale },
		{ nullptr, VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LC_NUMERIC=", changelocale },
		{ nullptr, VarFlag::str_fixed | VarFlag::full | VarFlag::unset, "LANG=", changelocale },
	};
	struct var *vartab[39] = {};

	const std::vector<Node::ptr> *argbackq = nullptr;
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
	Token lasttoken = Token::Eof;
	int tokpushback = 0;
	std::string wordtext;
	int checkkwd = 0;
	std::vector<Node::ptr> backquotelist;
	Node::ptr redirnode;
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

	signal_map_type signal_map;

	inline static ShellContext *signaled = nullptr;

	ShellContext()
	{
		signaled = this;
	}

	~ShellContext()
	{
		if (signaled == this)
			signaled = nullptr;
	}

	/*
	 * Parse and execute a shell script from a string. One-shot entry
	 * point for embedding and for -c / eval / traps.
	 */
	dash::ShellResult<int> eval(const std::string &script, EvalFlags flags = {});

	/*
	 * Interactive parse/eval loop over the current input (prompting,
	 * ignoreeof). Used by the dash executable's main loop.
	 */
	dash::ShellResult<int> run_interactive();

	std::optional<std::string_view> find_signal_name(int signal) const;

	std::optional<int> find_signal_number(std::string_view name, int minsig) const;
};

inline char *bltinlookup(ShellContext &sh, std::string_view name)
{
	return lookupvar(sh, name);
}
