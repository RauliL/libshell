#pragma once

#include <array>
#include <bitset>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <sys/types.h>
#include <termios.h>

#include <tl/expected.hpp>

struct ShellContext; // TODO: Remove

namespace shell {

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
 * outlive that push. ShellContext::aliases owns shared_ptrs; the input stack
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
	using callback_type = void(*)(ShellContext &, std::string_view);

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

	class Flags {
	private:
		static constexpr std::size_t nbits =
			static_cast<std::size_t>(Flag::Full) + 1;
		std::bitset<nbits> bits{};

		static constexpr std::size_t index(Flag flag)
		{
			return static_cast<std::size_t>(flag);
		}

		explicit Flags(const std::bitset<nbits> &b)
			: bits(b) {}

	public:
		Flags() = default;

		Flags(Flag flag)
		{
			bits.set(index(flag));
		}

		inline bool test(Flag flag) const
		{
			return bits.test(index(flag));
		}

		inline bool any() const
		{
			return bits.any();
		}

		inline bool none() const
		{
			return bits.none();
		}

		Flags &set(Flag flag)
		{
			bits.set(index(flag));

			return *this;
		}

		Flags &reset(Flag flag)
		{
			bits.reset(index(flag));

			return *this;
		}

		Flags &operator|=(Flag flag)
		{
			return set(flag);
		}

		Flags &operator|=(const Flags &other)
		{
			bits |= other.bits;

			return *this;
		}

		Flags &operator&=(const Flags &other)
		{
			bits &= other.bits;

			return *this;
		}

		Flags operator|(Flag flag) const
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

		Flags operator&(Flag flag) const
		{
			return *this & Flags(flag);
		}

		Flags operator&(const Flags &other) const
		{
			Flags out = *this;

			out &= other;

			return out;
		}

		Flags operator~() const {
			return Flags(~bits);
		}

		friend bool operator==(const Flags &a, const Flags &b)
		{
			return a.bits == b.bits;
		}

		friend bool operator!=(const Flags &a, const Flags &b)
		{
			return !(a == b);
		}

		friend bool operator==(Flags a, Flag b)
		{
			return a == Flags(b);
		}

		friend bool operator!=(Flags a, Flag b)
		{
			return !(a == b);
		}
	};

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
 * Buffered writer for a file descriptor. Owns its buffer via std::vector;
 * allocation does not go through ShellContext / ckmalloc.
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

	void flush(ShellContext &);

	void write(ShellContext &, const char *, std::size_t);

	inline void write(ShellContext &context, std::string_view s)
	{
		write(context, s.data(), s.size());
	}

private:
	void ensure_buf();
	int emit(ShellContext &, const char *, std::size_t);
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
	 * procs; job numbers (%N) are slot + 1 in ShellContext::jobs.
	 */
	struct Process {
		pid_t pid = -1;
		/** Last wait() status; -1 = still running. */
		int status = -1;
		std::string command;
	};

	/** Index of ShellContext::jobs. */
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

	using callback_type = Result<int>(*)(ShellContext &, int, char **);

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

class EvalFlags {
	static constexpr std::size_t nbits =
		static_cast<std::size_t>(EvalFlag::Tested) + 1;
	std::bitset<nbits> bits{};

	static constexpr std::size_t index(EvalFlag flag)
	{
		return static_cast<std::size_t>(flag);
	}

public:
	EvalFlags() = default;
	EvalFlags(EvalFlag flag) { bits.set(index(flag)); }

	bool test(EvalFlag flag) const { return bits.test(index(flag)); }

	EvalFlags &set(EvalFlag flag)
	{
		bits.set(index(flag));
		return *this;
	}

	EvalFlags &reset(EvalFlag flag)
	{
		bits.reset(index(flag));
		return *this;
	}

	/* Keep only this flag, and only when it was already set. */
	EvalFlags &operator&=(EvalFlag flag)
	{
		bool on = test(flag);
		bits.reset();
		if (on)
			bits.set(index(flag));
		return *this;
	}

	EvalFlags operator&(EvalFlag flag) const
	{
		EvalFlags out = *this;
		out &= flag;
		return out;
	}

	EvalFlags &operator|=(EvalFlag flag) { return set(flag); }

	EvalFlags operator|(EvalFlag flag) const
	{
		EvalFlags out = *this;
		out.set(flag);
		return out;
	}

	unsigned long to_ulong() const { return bits.to_ulong(); }
};

inline EvalFlags operator|(EvalFlag a, EvalFlag b)
{
	EvalFlags flags(a);
	flags.set(b);
	return flags;
}

static_assert(std::is_trivially_copyable_v<EvalFlags>);
static_assert(std::is_trivially_destructible_v<EvalFlags>);

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
		ShellContext &sh,
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

}
