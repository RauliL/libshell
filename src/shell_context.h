#pragma once

/*
 * One shell. Mutable shell state lives here so the program can be used as a
 * library: the caller owns a ShellContext and runs commands on that object.
 *
 * Signal delivery is process-wide (POSIX handlers only receive the signo), so
 * at most one ShellContext is the async signal target at a time — see
 * claim_signals() / release_signals().
 */

#include <cinttypes>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
	using alias_map_type = std::unordered_map<std::string, std::shared_ptr<alias>>;
	using variable_map_type = std::unordered_map<std::string, var>;
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
	variable_map_type variables;

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

	alias_map_type aliases;
	std::vector<RedirFrame> redirlist;
	unsigned closed_redirs = 0;
	CdState cwd;

	signal_map_type signal_map;

	/**
	 * Process-wide shell that receives async signals (SIGINT, SIGCHLD, …).
	 * Only one context should claim this at a time.
	 */
	inline static ShellContext *signaled = nullptr;

	/** Whether the constructor should become the process signal target. */
	enum class SignalOwnership {
		Claim,
		Leave,
	};

	/**
	 * Constructs a ready-to-use shell: variables, traps, signal names,
	 * and process ids are initialized. Call eval() directly afterward.
	 */
	explicit ShellContext(SignalOwnership signals = SignalOwnership::Claim);

	~ShellContext()
	{
		release_signals();
	}

	ShellContext(const ShellContext &) = delete;
	ShellContext &operator=(const ShellContext &) = delete;

	/** Make this context the process-wide async signal target. */
	void claim_signals();

	/** Drop signal ownership if this context currently holds it. */
	void release_signals();

	bool owns_signals() const { return signaled == this; }

	/**
	 * Parse and execute a shell script from a string. One-shot entry
	 * point for embedding and for -c / eval / traps.
	 */
	shell::Result<int> eval(const std::string &script, EvalFlags flags = {});

	/**
	 * Parse and execute a shell script from a file path.
	 */
	shell::Result<int> eval_file(std::string_view path, EvalFlags flags = {});

	/**
	 * Parses and evaluates an arithmetic expression. Meant for `$((...))`
	 * style shell expressions.
	 */
	shell::Result<std::intmax_t> arith(std::string_view expr);

	/**
	 * Returns the value of a variable, or `std::nullopt` if unset.
	 * The view is valid until the variable is modified or destroyed.
	 */
	std::optional<std::string_view> get_var(std::string_view name) const;

	/**
	 * Set the value of a variable. The flags argument is ORed with the flags
	 * of the variable. If value is `std::nullopt`, the variable is unset.
	 */
	shell::Result<void> set_var(
		std::string_view name,
		const std::optional<std::string> &value = std::nullopt,
		VarFlags flags = {}
	);

	shell::Result<std::intmax_t> get_int_var(std::string_view name) const;

	/**
	 * Set the given integer as the value of a variable. The flags argument is
	 * ORed with the flags of the variable.
	 */
	shell::Result<std::intmax_t> set_int_var(
		std::string_view name,
		std::intmax_t value,
		VarFlags flags = {}
	);

	/** Set `$0` (shell/script name). */
	void set_arg0(std::string name);

	/** Replace positional parameters (`$1`, `$2`, …). */
	void set_args(std::span<const std::string_view> args);

	/**
	 * Enable or disable a `set -o` option by long name (`errexit`, …)
	 * or single-letter flag (`e`, `x`, …).
	 */
	shell::Result<void> set_option(std::string_view name, bool enabled);
	shell::Result<void> set_option(char letter, bool enabled);

	/** Current value of a `set -o` option, or nullopt if the name is unknown. */
	std::optional<bool> get_option(std::string_view name) const;

	/**
	 * Redirect shell stdout/stderr to a host callback instead of fds 1/2.
	 * Pass an empty function to restore fd-based output.
	 */
	void set_stdout(std::function<void(std::string_view)> sink);
	void set_stderr(std::function<void(std::string_view)> sink);

	/** Logical working directory tracked by the shell (`PWD` / `cd`). */
	std::string_view get_cwd() const;

	/**
	 * Update the shell's logical cwd and `PWD` without changing the
	 * process working directory (which is process-global).
	 */
	shell::Result<void> set_cwd(std::string_view path);

	std::optional<std::string_view> find_signal_name(int signal) const;

	std::optional<int> find_signal_number(std::string_view name, int minsig) const;
};

/** Fill signal_map from host SIG* names (called during ShellContext init). */
void init_signal_names(ShellContext &sh);

inline const char *bltinlookup(ShellContext &sh, std::string_view name)
{
	if (auto value = sh.get_var(name))
		return value->data();
	return nullptr;
}
