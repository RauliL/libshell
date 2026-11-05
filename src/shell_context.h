#pragma once

/*
 * One shell. Mutable shell state lives here so the program can be used as a
 * library: the caller owns a ShellContext and runs commands on that object.
 * signaled is the process-wide target for asynchronous signals, because a
 * POSIX handler is only passed the signal number.
 */

#include <cinttypes>
#include <climits>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>

#include "alias.h"
#include "cd.h"
#include "error.h"
#include "exec.h"
#include "eval.h"
#include "expand.h"
#include "input.h"
#include "jobs.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "redir.h"
#include "system.h"
#include "var.h"

struct backcmd;
struct synstack;
struct builtincmd;
struct cmdentry;
enum limtype : int;

struct ShellContext {

	using signal_map_type = std::unordered_map<int, std::string_view>;

	struct output output{BUFSIZ, 1};
	struct output errout{0, 2};
	struct output preverrout;
	struct output *out1 = &output;
	struct output *out2 = &errout;

	InputStack input;
	ParseSession parse;

	std::optional<std::string> trap[NSIG] = {};
	int ptrap = 0;
	int trapcnt = 0;
	char sigmode[NSIG - 1] = {};
	char gotsig[NSIG - 1] = {};
	volatile sig_atomic_t pending_sig = 0;
	volatile sig_atomic_t gotsigchld = 0;

	std::vector<std::unique_ptr<Job>> jobs;
	std::vector<Job *> job_order;	/* current job first */
	pid_t backgndpid = 0;
	int vforked = 0;
	int lineno = 0;
	std::vector<LocalVarFrame> localvar_stack;
	std::unordered_map<std::string, var> vars;

	ExpandState expand;

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
	ShellOptions opts;
	NextOptState bopt;

	int rootpid = 0;
	int mypid = 0;
	int shlvl = 0;

	std::optional<shell::Error> exception;
	int suppressint = 0;
	volatile sig_atomic_t intpending = 0;
	int errlinno = 0;

	const char *pathopt = nullptr;
	std::unordered_map<std::string, tblentry> cmdtable;
	int builtinloc = -1;

	std::unordered_map<std::string, std::shared_ptr<alias>> aliases;
	std::vector<RedirFrame> redirlist;
	unsigned closed_redirs = 0;
	CdState cwd;

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
	shell::Result<int> eval(const std::string &script, EvalFlags flags = {});

	/**
	 * Parses and evaluates an arithmetic expression. Meant for `$((...))`
	 * style shell expressions.
	 */
	shell::Result<std::intmax_t> arith(std::string_view expr);

	std::optional<std::string_view> find_signal_name(int signal) const;

	std::optional<int> find_signal_number(std::string_view name, int minsig) const;
};

inline char *bltinlookup(ShellContext &sh, std::string_view name)
{
	return lookupvar(sh, name);
}
