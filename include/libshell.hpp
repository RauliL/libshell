#pragma once

#include <array>
#include <bitset>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cwchar>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sys/types.h>
#include <termios.h>

#include <tl/expected.hpp>

namespace shell {

struct Context;

/**
 * Bit set over a dense enum class whose enumerators are used as bit indices
 * (0 .. Last inclusive). Shared by variable, eval, and skip flags.
 */
template<typename Enum, Enum Last>
class Flags {
	static_assert(std::is_enum_v<Enum>);

	static constexpr std::size_t nbits =
		static_cast<std::size_t>(Last) + 1;
	std::bitset<nbits> bits{};

	static constexpr std::size_t index(Enum flag)
	{
		return static_cast<std::size_t>(flag);
	}

	explicit Flags(const std::bitset<nbits> &b) : bits(b) {}

public:
	using enum_type = Enum;

	Flags() = default;

	Flags(Enum flag)
	{
		bits.set(index(flag));
	}

	bool test(Enum flag) const
	{
		return bits.test(index(flag));
	}

	bool any() const { return bits.any(); }
	bool none() const { return bits.none(); }

	explicit operator bool() const { return any(); }

	Flags &set(Enum flag)
	{
		bits.set(index(flag));
		return *this;
	}

	Flags &reset(Enum flag)
	{
		bits.reset(index(flag));
		return *this;
	}

	Flags &clear()
	{
		bits.reset();
		return *this;
	}

	Flags &operator|=(Enum flag) { return set(flag); }

	Flags &operator|=(const Flags &other)
	{
		bits |= other.bits;
		return *this;
	}

	Flags operator|(Enum flag) const
	{
		Flags out = *this;
		out.set(flag);
		return out;
	}

	Flags operator|(const Flags &other) const
	{
		Flags out = *this;
		out |= other;
		return out;
	}

	Flags &operator&=(Enum flag) { return *this &= Flags(flag); }

	Flags &operator&=(const Flags &other)
	{
		bits &= other.bits;
		return *this;
	}

	Flags operator&(Enum flag) const { return *this & Flags(flag); }

	Flags operator&(const Flags &other) const
	{
		Flags out = *this;
		out &= other;
		return out;
	}

	Flags operator~() const { return Flags(~bits); }

	unsigned long to_ulong() const { return bits.to_ulong(); }

	friend bool operator==(const Flags &a, const Flags &b)
	{
		return a.bits == b.bits;
	}

	friend bool operator!=(const Flags &a, const Flags &b)
	{
		return !(a == b);
	}

	friend bool operator==(const Flags &a, Enum b)
	{
		return a == Flags(b);
	}

	friend bool operator!=(const Flags &a, Enum b)
	{
		return !(a == b);
	}

	friend bool operator==(Enum a, const Flags &b)
	{
		return Flags(a) == b;
	}

	friend bool operator!=(Enum a, const Flags &b)
	{
		return !(a == b);
	}
};

/**
 * Enumeration of shell error codes.
 */
enum class Error : std::uint8_t {
	/** SIGINT received. */
	Interrupt = 0,
	/** A generic error. */
	Generic = 1,
	/** Exit the shell. */
	End = 3,
	/** Exit the shell via exit command. */
	Exit = 4,
};

template<typename T>
using Result = tl::expected<T, Error>;

/**
 * Alias expansion pushes val onto the input stack, so the alias object must
 * outlive that push. Context::aliases owns shared_ptrs; the input stack
 * holds additional shared_ptrs for the duration of expansion. Unalias only
 * erases the map entry — in-flight expansions keep the object alive.
 *
 * in_use blocks recursive expansion of the same alias (POSIX).
 */
struct Alias {
	std::string name;
	std::string value;
	bool in_use = false;
};

struct Variable {
	using callback_type = void(*)(Context &, std::string_view);

	enum class Flag : std::uint8_t {
		/** Variable is exported. */
		Exported,
		/** Variable cannot be modified. */
		ReadOnly,
		/** Permanent map entry; Do not erase on unset. */
		StrFixed,
		/** The variable is not set. */
		Unset,
		/** Don't call the callback function. */
		NoFunc,
		/** Pass value suitable for `putenv`. */
		Full,
	};

	using Flags = shell::Flags<Flag, Flag::Full>;

	Flags flags;
	/** name=value (owned; empty value when uset) */
	std::string text;
	/** Function to be called when the variable gets set/unset. */
	callback_type func = nullptr;
};

inline Variable::Flags operator|(Variable::Flag a, Variable::Flag b)
{
	Variable::Flags flags(a);

	flags.set(b);

	return flags;
}

inline Variable::Flags operator|(Variable::Flag a, Variable::Flags b)
{
	return b | a;
}

inline Variable::Flags operator~(Variable::Flag flag)
{
	return ~Variable::Flags(flag);
}

static_assert(std::is_trivially_copyable_v<Variable::Flags>);
static_assert(std::is_trivially_destructible_v<Variable::Flags>);

struct LocalVariable {
	/** The variable that was made local. */
	Variable *variable = nullptr;
	/** Saved flags. */
	Variable::Flags flags{};
	/** Saved text or $- optlist bytes. */
	std::string text;
};

/**
 * One functioon/scope frame: Ordered restore records (undo log), not a map.
 */
struct Frame {
	std::vector<LocalVariable> variables;
};

/**
 * Buffered writer for a file descriptor. Owns its buffer via std::vector.
 *
 * bufsize == 0 means unbuffered (writes go straight to the fd / sink).
 * Otherwise the buffer is allocated lazily on first write.
 *
 * If `sink` is set, bytes are delivered there instead of `write(fd, …)`.
 */
struct Output {
	using value_type = char;
	using container_type = std::vector<value_type>;
	using size_type = container_type::size_type;
	using sink_type = std::function<void(std::string_view)>;

	container_type buf;
	/** Bytes pending in buffer. */
	size_type pos = 0;
	/** Capacity once allocated; 0 = unbuffered. */
	size_type bufsize = 0;
	int fd = -1;
	int flags = 0;
	/** Optional host capture; when set, overrides the fd. */
	sink_type sink;

	Output() = default;

	Output(size_type size, int filedes)
		: bufsize(size)
		, fd(filedes) {}

	inline size_type space() const
	{
		return buf.size() >= pos ? buf.size() - pos : 0;
	}

	/**
	 * Drop pending data without writing (e.g. after an interrupted builtin).
	 */
	void discard()
	{
		pos = 0;
		flags = 0;
	}

	void flush(Context &);

	void write(Context &, const char *, std::size_t);

	inline void write(Context &context, std::string_view s)
	{
		write(context, s.data(), s.size());
	}

private:
	void ensure_buf();
	int emit(Context &, const char *, std::size_t);
};

struct Job {
	enum class State : std::uint8_t {
		/** At least one proc running. */
		Running = 0,
		/** All procs are completed. */
		Done = 1,
	};

	/**
	 * A job is either a single process or a pipeline. Process status lives in
	 * procs; job numbers (%N) are slot + 1 in Context::jobs.
	 */
	struct Process {
		pid_t pid = -1;
		/** Last wait() status; -1 = still running. */
		int status = -1;
		std::string command;
	};

	/** Index of Context::jobs. */
	std::size_t slot = 0;
	std::vector<Process> processes;
	State state = State::Running;
	/** True if this entry has been waited for. */
	bool waited = false;

	inline int number() const
	{
		return static_cast<int>(slot + 1);
	}

	inline pid_t pgid() const
	{
		return processes.empty() ? -1 : processes.front().pid;
	}

	Process &add_process(pid_t pid, const std::string &command = {});

	int calc_status(bool pipefail_on);
};

/**
 * Cached logical/physical working directory for `cd` / `pwd` / PWD.
 */
struct CdState {
	/** Empty when unknown. */
	std::string curdir;
	/** Empty when unknown. */
	std::string physdir;
};

/**
 * One redirection save/restore frame (fd 0..9).
 */
struct RedirFrame {
	std::array<int, 10> renamed{};
};

struct ShellParam {
	/** Positional parameters (without $0). */
	std::vector<std::string> p;
	/** Next parameter to be processed by getopts. */
	int optind = 1;
	/** Used by getopts. */
	int optoff = -1;
};

inline constexpr int option_count = 15;

/**
 * Shell `set` / startup option bits (`$-`, `set -e`, …).
 */
struct ShellOptions {
	std::array<char, option_count> list{};
	/** True when the host started the shell with `-c`. */
	bool c_mode = false;

	inline char &errexit() { return list[0]; }
	inline char errexit() const { return list[0]; }
	inline char &noglob() { return list[1]; }
	inline char noglob() const { return list[1]; }
	inline char &ignoreeof() { return list[2]; }
	inline char ignoreeof() const { return list[2]; }
	inline char &interactive() { return list[3]; }
	inline char interactive() const { return list[3]; }
	inline char &monitor() { return list[4]; }
	inline char monitor() const { return list[4]; }
	inline char &noexec() { return list[5]; }
	inline char noexec() const { return list[5]; }
	inline char &stdin_opt() { return list[6]; }
	inline char stdin_opt() const { return list[6]; }
	inline char &xtrace() { return list[7]; }
	inline char xtrace() const { return list[7]; }
	inline char &verbose() { return list[8]; }
	inline char verbose() const { return list[8]; }
	inline char &noclobber() { return list[9]; }
	inline char noclobber() const { return list[9]; }
	inline char &allexport() { return list[10]; }
	inline char allexport() const { return list[10]; }
	inline char &notify() { return list[11]; }
	inline char notify() const { return list[11]; }
	inline char &nounset() { return list[12]; }
	inline char nounset() const { return list[12]; }
	inline char &pipefail() { return list[13]; }
	inline char pipefail() const { return list[13]; }
	inline char &debug() { return list[14]; }
	inline char debug() const { return list[14]; }
};

/**
 * Scratch for builtin `nextopt` / argv walking.
 */
struct NextOptState {
	char **argptr = nullptr;
	char *optionarg = nullptr;
	char *optptr = nullptr;
};

struct BuiltinCommand {
	enum Flag : std::uint8_t {
		Special = 0x1,
		Regular = 0x2,
		Assign = 0x4,
	};

	using callback_type = Result<int>(*)(Context &, int, char **);

	const char *name;
	callback_type builtin;
	unsigned flags;
};

/**
 * Byte range of an expanded word that must be scanned for IFS.
 */
struct IfsRegion {
	int begoff = 0;
	int endoff = 0;
	int nulonly = 0;
};

enum class Token : std::uint8_t {
	Eof = 0,
	Blank = 1,
	Nl = 2,
	Semi = 3,
	Backgnd = 4,
	And = 5,
	Or = 6,
	Pipe = 7,
	Lp = 8,
	Rp = 9,
	Endcase = 10,
	Endbquote = 11,
	Redir = 12,
	Word = 13,
	Not = 14,
	Case = 15,
	Do = 16,
	Done = 17,
	Elif = 18,
	Else = 19,
	Esac = 20,
	Fi = 21,
	For = 22,
	If = 23,
	In = 24,
	Then = 25,
	Until = 26,
	While = 27,
	Begin = 28,
	End = 29,
};

/**
 * How to evaluate a command tree. Passed to execute(), evalstring(), and
 * evaltree().
 */
enum class EvalFlag : std::uint8_t {
	/**
	 * This tree is the last action of the shell. After it runs, exit instead
	 * of returning. Also lets a subshell or external command replace the
	 * current process.
	 */
	Exit,
	/**
	 * The caller inspects the status (if, while, &&, ||, !, a function body).
	 * A failing command does not trigger set -e.
	 */
	Tested,
};

using EvalFlags = Flags<EvalFlag, EvalFlag::Tested>;

inline EvalFlags operator|(EvalFlag a, EvalFlag b)
{
	return EvalFlags(a) | b;
}

static_assert(std::is_trivially_copyable_v<EvalFlags>);
static_assert(std::is_trivially_destructible_v<EvalFlags>);

/**
 * Why command evaluation is skipping ahead (break / continue / return).
 * Stored as a bit set on Context::evalskip; see breakcmd().
 */
enum class EvalSkip : std::uint8_t {
	Break,
	Continue,
	Func,
	FuncDef,
};

using EvalSkipFlags = Flags<EvalSkip, EvalSkip::FuncDef>;

inline EvalSkipFlags operator|(EvalSkip a, EvalSkip b)
{
	return EvalSkipFlags(a) | b;
}

inline EvalSkipFlags operator~(EvalSkip flag)
{
	return ~EvalSkipFlags(flag);
}

static_assert(std::is_trivially_copyable_v<EvalSkipFlags>);
static_assert(std::is_trivially_destructible_v<EvalSkipFlags>);

struct Node {
	using ptr = std::unique_ptr<Node>;

	/*
	 * Node kind tags — values must stay stable (eval uses And/Or/Semi
	 * consecutive order).
	 */
	enum class Type : std::uint8_t {
		Cmd = 0,
		Pipe = 1,
		Redir = 2,
		Backgnd = 3,
		Subshell = 4,
		And = 5,
		Or = 6,
		Semi = 7,
		If = 8,
		While = 9,
		Until = 10,
		For = 11,
		Case = 12,
		CaseList = 13,
		Defun = 14,
		Arg = 15,
		To = 16,
		Clobber = 17,
		From = 18,
		FromTo = 19,
		Append = 20,
		ToFd = 21,
		FromFd = 22,
		Here = 23,
		XHere = 24,
		Not = 25,
	};

	Type type;

	explicit Node(Type t) : type(t) {}
	Node(const Node &) = delete;
	Node &operator=(const Node &) = delete;
	virtual ~Node() = default;

	virtual ptr clone() const = 0;

	/**
	 * Evaluate this node.  Prologue/epilogue (traps, set -e, EvalFlag::Exit)
	 * live in the free evaltree(); overrides implement type-specific work.
	 * Set checkexit when a failing status should trigger set -e.
	 */
	virtual shell::Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const;
};

struct CommandParam {
	int index = 0;
	const BuiltinCommand *cmd = nullptr;
	std::shared_ptr<Node> func;
};

/**
 * One remembered command: a function, a builtin, or a hashed path.
 */
struct TableEntry {
	enum class Type : std::int8_t {
		/** No entry in table for command. */
		Unknown = -1,
		/** Command is an executable program. */
		Normal = 0,
		/** Command is a shell function. */
		Function = 1,
		/** Command is a shell builtin. */
		Builtin = 2,
	};

	CommandParam param{};
	Type type = Type::Unknown;
	bool rehash = false;
};

/**
 * Word-expansion scratch and IFS lookup cache for one shell.
 * Region/argbackq/exparg state is reset around each expandarg;
 * ifsmap/ncifs/wcifs track the current IFS value.
 */
struct ExpandState {
	const std::vector<std::unique_ptr<Node>> *argbackq = nullptr;
	std::size_t argbackq_i = 0;
	std::vector<IfsRegion> ifsregions;
	std::vector<std::string> exparg;
	char ifsmap[128] = {};
	const char *ncifs = nullptr;
	std::size_t ifsmb0len = 0;
	std::wstring wcifs;
};

/**
 * Pending here-document awaiting its body after the next newline.
 */
struct HereDoc {
	/** Redirection node. */
	Node *here = nullptr;
	/** String indicating end of input. */
	std::string eofmark;
	/** If set, strip leading tabs. */
	bool striptabs = false;
};

/**
 * Ephemeral parser state for one parse pass (and nested expandstr /
 * here-doc reads). Not part of the durable script environment.
 *
 * Owns Node unique_ptrs; special members are out-of-line so Node can stay
 * incomplete in this header.
 */
struct ParseSession {
	std::vector<HereDoc> heredoclist;
	/** Here-doc currently being attached to a redirection node. */
	std::optional<HereDoc> pending_here;
	int doprompt = 0;
	int needprompt = 0;
	int whichprompt = 0;
	Token lasttoken = Token::Eof;
	int tokpushback = 0;
	std::string wordtext;
	int checkkwd = 0;
	std::vector<std::unique_ptr<Node>> backquotelist;
	std::unique_ptr<Node> redirnode;
	int quoteflag = 0;

	ParseSession();
	~ParseSession();
	ParseSession(ParseSession &&) noexcept;
	ParseSession &operator=(ParseSession &&) noexcept;
	ParseSession(const ParseSession &) = delete;
	ParseSession &operator=(const ParseSession &) = delete;
};

inline constexpr std::size_t pungetc_max =
	MB_LEN_MAX > 16 ? static_cast<std::size_t>(MB_LEN_MAX) : 16;
inline constexpr std::size_t ibufsiz = BUFSIZ + pungetc_max + 1;

/**
 * One pushed string overlay on a parsefile (alias text or parser
 * re-injection).
 */
struct StringPush {
	char *prev_string = nullptr;
	int prev_nleft = 0;
	std::shared_ptr<Alias> ap;
	char *string = nullptr;
	int unget = 0;
};

/**
 * One level of the input file stack (script, sourced file, or string).
 */
struct ParseFile {
	ParseFile *prev = nullptr;
	int linno = 0;
	int fd = 0;
	int nleft = 0;
	int eof = 0;
	char *nextc = nullptr;
	char *buf = nullptr;
	std::unique_ptr<char[]> owned_buf;
	std::vector<StringPush> string_stack;
	std::vector<std::shared_ptr<Alias>> pending_alias_release;
	int unget = 0;
};

struct StdinState {
	off_t seekable = 0;
	int pip[2] = {};
	int pending = 0;
	tcflag_t bufferable = 0;
};

/**
 * Nested input sources for one shell: base stdin, sourced files, eval
 * strings, and alias string pushes.
 */
struct InputStack {
	ParseFile base{};
	char basebuf[ibufsiz]{};
	ParseFile *top = &base;
	ParseFile *current = &base;
	StdinState stdin{};
	int stdin_istty = -1;

	InputStack()
	{
		base.nextc = base.buf = basebuf;
		base.linno = 1;
	}

	InputStack(const InputStack &) = delete;
	InputStack &operator=(const InputStack &) = delete;

	inline ParseFile &cur() { return *current; }
	inline const ParseFile &cur() const { return *current; }
	inline int &linno() { return current->linno; }
};

/**
 * Host-facing result of parsing process argv (`-c`, login shell, …).
 * The `-c` script string lives here, not on Context.
 */
struct ProcArgsResult {
	int login = 0;
	std::optional<std::string> minusc;
};

/**
 * One shell. Mutable shell state lives here so the program can be used as a
 * library: the caller owns a Context and runs commands on that object.
 *
 * Signal delivery is process-wide (POSIX handlers only receive the signo), so
 * at most one Context is the async signal target at a time — see
 * claim_signals() / release_signals().
 */
struct Context {
	using alias_map_type = std::unordered_map<
		std::string,
		std::shared_ptr<Alias>
	>;
	using variable_map_type = std::unordered_map<
		std::string,
		Variable
	>;
	using signal_map_type = std::unordered_map<int, std::string>;

	Output output{BUFSIZ, 1};
	Output errout{0, 2};
	Output preverrout;
	Output *out1 = &output;
	Output *out2 = &errout;

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
	std::vector<Frame> stack;
	variable_map_type variables;

	ExpandState expand;

	EvalSkipFlags evalskip;
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
	ShellParam shellparam = {};
	ShellOptions opts;
	NextOptState bopt;

	int rootpid = 0;
	int mypid = 0;
	int shlvl = 0;

	std::optional<Error> exception;
	int suppressint = 0;
	volatile sig_atomic_t intpending = 0;
	int errlinno = 0;

	const char *pathopt = nullptr;
	std::unordered_map<std::string, TableEntry> cmdtable;
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
	inline static Context *signaled = nullptr;

	/** Whether the constructor should become the process signal target. */
	enum class SignalOwnership {
		Claim,
		Leave,
	};

	/**
	 * Constructs a ready-to-use shell: variables, traps, signal names,
	 * and process ids are initialized. Call eval() directly afterward.
	 */
	explicit Context(SignalOwnership signals = SignalOwnership::Claim);

	~Context()
	{
		release_signals();
	}

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	/** Make this context the process-wide async signal target. */
	void claim_signals();

	/** Drop signal ownership if this context currently holds it. */
	void release_signals();

	bool owns_signals() const { return signaled == this; }

	/**
	 * Parse and execute a shell script from a string. One-shot entry
	 * point for embedding and for -c / eval / traps.
	 */
	Result<int> eval(const std::string &script, EvalFlags flags = {});

	/**
	 * Parse and execute a shell script from a file path.
	 */
	Result<int> eval_file(std::string_view path, EvalFlags flags = {});

	/**
	 * Process host argv: options (`-c`, `-i`, …), `$0`, and positional
	 * parameters. The optional `-c` script is returned in ProcArgsResult.
	 */
	Result<ProcArgsResult> parse_args(char **argv);

	/**
	 * Non-interactive read/eval loop until EOF (scripts, sourced files).
	 */
	Result<int> cmdloop();

	/**
	 * Interactive prompt/eval loop (ignoreeof, parse-error recovery).
	 */
	Result<int> run_interactive();

	/** Unwind eval/redir state after a caught shell error. */
	void exit_reset();

	/** Reset input/locals so an interactive shell can continue. */
	void reset();

	/**
	 * Run the EXIT trap (if any) and terminate the process. Does not return.
	 */
	[[noreturn]] void exit_shell();

	/**
	 * Deliver a deferred SIGINT (clear pending, restore default, re-raise).
	 * Always returns an error result whose code is Error::Interrupt.
	 */
	Result<void> on_interrupt();

	/**
	 * Parses and evaluates an arithmetic expression. Meant for `$((...))`
	 * style shell expressions.
	 */
	Result<std::intmax_t> arith(std::string_view expr);

	/**
	 * Returns the value of a variable, or `std::nullopt` if unset.
	 * The view is valid until the variable is modified or destroyed.
	 */
	std::optional<std::string_view> get_var(std::string_view name) const;

	/**
	 * Set the value of a variable. The flags argument is ORed with the flags
	 * of the variable. If value is `std::nullopt`, the variable is unset.
	 */
	Result<void> set_var(
		std::string_view name,
		const std::optional<std::string> &value = std::nullopt,
		const Variable::Flags &flags = {}
	);

	Result<std::intmax_t> get_int_var(std::string_view name) const;

	/**
	 * Set the given integer as the value of a variable. The flags argument is
	 * ORed with the flags of the variable.
	 */
	Result<std::intmax_t> set_int_var(
		std::string_view name,
		std::intmax_t value,
		const Variable::Flags &flags = {}
	);

	/** Set `$0` (shell/script name). */
	void set_arg0(std::string name);

	/** Replace positional parameters (`$1`, `$2`, …). */
	void set_args(std::span<const std::string_view> args);

	/**
	 * Enable or disable a `set -o` option by long name (`errexit`, …)
	 * or single-letter flag (`e`, `x`, …).
	 */
	Result<void> set_option(std::string_view name, bool enabled);
	Result<void> set_option(char letter, bool enabled);

	/** Current value of a `set -o` option, or nullopt if the name is unknown. */
	std::optional<bool> get_option(std::string_view name) const;

	/**
	 * Redirect shell stdout/stderr to a host callback instead of fds 1/2.
	 * Pass an empty function to restore fd-based output.
	 */
	void set_stdout(const Output::sink_type &sink);
	void set_stderr(const Output::sink_type &sink);

	/** Logical working directory tracked by the shell (`PWD` / `cd`). */
	std::string_view get_cwd() const;

	/**
	 * Update the shell's logical cwd and `PWD` without changing the
	 * process working directory (which is process-global).
	 */
	Result<void> set_cwd(std::string_view path);

	std::optional<std::string_view> find_signal_name(int signal) const;

	std::optional<int> find_signal_number(std::string_view name, int minsig) const;
};

}
