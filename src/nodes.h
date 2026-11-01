#pragma once

/*
 * Parse-tree nodes. Tree edges use unique_ptr; function bodies in the
 * command table use shared_ptr.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

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

	struct Arg;
	struct Binary;
	struct Case;
	struct CaseList;
	struct Command;
	struct Defun;
	struct Dup;
	struct File;
	struct For;
	struct Here;
	struct If;
	struct Not;
	struct Pipe;
	struct Redir;
	struct RedirList;

	Type type;

	explicit Node(Type t) : type(t) {}
	Node(const Node &) = delete;
	Node &operator=(const Node &) = delete;
	virtual ~Node() = default;

	virtual ptr clone() const = 0;
};

template <
	typename T,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
constexpr T *as(Node *n) noexcept
{
	return static_cast<T *>(n);
}

template <
	typename T,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
constexpr const T *as(const Node *n) noexcept
{
	return static_cast<const T *>(n);
}

template <
	typename T,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
constexpr T *as(const Node::ptr &n) noexcept
{
	return static_cast<T *>(n.get());
}

template <
	typename T,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
constexpr T *as(const std::shared_ptr<Node> &n) noexcept
{
	return static_cast<T *>(n.get());
}

/* Common prefix for redirection list nodes (nfile / ndup / nhere). */
struct Node::RedirList : Node {
	ptr next;
	int fd{};

	explicit RedirList(Type t) : Node(t) {}
};

struct Node::Command : Node {
	int linno{};
	ptr assign;
	ptr args;
	ptr redirect;

	explicit Command(Type t = Type::Cmd) : Node(t) {}
	~Command() override;
	ptr clone() const override;
};

struct Node::Pipe : Node {
	int backgnd{};
	std::vector<ptr> cmdlist;

	explicit Pipe(Type t = Type::Pipe) : Node(t) {}
	~Pipe() override;
	ptr clone() const override;
};

struct Node::Redir : Node {
	int linno{};
	ptr n;
	ptr redirect;

	explicit Redir(Type t = Type::Redir) : Node(t) {}
	~Redir() override;
	ptr clone() const override;
};

struct Node::Binary : Node {
	ptr ch1;
	ptr ch2;

	explicit Binary(Type t = Type::Semi) : Node(t) {}
	~Binary() override;
	ptr clone() const override;
};

struct Node::If : Node {
	ptr test;
	ptr ifpart;
	ptr elsepart;

	explicit If(Type t = Type::If) : Node(t) {}
	~If() override;
	ptr clone() const override;
};

struct Node::For : Node {
	int linno{};
	ptr args;
	ptr body;
	std::string var;

	explicit For(Type t = Type::For) : Node(t) {}
	~For() override;
	ptr clone() const override;
};

struct Node::Case : Node {
	int linno{};
	ptr expr;
	ptr cases;

	explicit Case(Type t = Type::Case) : Node(t) {}
	~Case() override;
	ptr clone() const override;
};

struct Node::CaseList : Node {
	ptr next;
	ptr pattern;
	ptr body;

	explicit CaseList(Type t = Type::CaseList) : Node(t) {}
	~CaseList() override;
	ptr clone() const override;
};

struct Node::Defun : Node {
	int linno{};
	std::string text;
	ptr body;

	explicit Defun(Type t = Type::Defun) : Node(t) {}
	~Defun() override;
	ptr clone() const override;
};

struct Node::Arg : Node {
	ptr next;
	std::string text;
	std::vector<ptr> backquote;

	explicit Arg(Type t = Type::Arg) : Node(t) {}
	~Arg() override;
	ptr clone() const override;
};

struct Node::File : Node::RedirList {
	ptr fname;
	std::string expfname;

	explicit File(Type t = Type::To) : RedirList(t) {}
	~File() override;
	ptr clone() const override;
};

struct Node::Dup : Node::RedirList {
	int dupfd{};
	ptr vname;

	explicit Dup(Type t = Type::ToFd) : RedirList(t) {}
	~Dup() override;
	ptr clone() const override;
};

struct Node::Here : Node::RedirList {
	ptr doc;

	explicit Here(Type t = Type::Here) : RedirList(t) {}
	~Here() override;
	ptr clone() const override;
};

struct Node::Not : Node {
	ptr com;

	Not(Type t = Type::Not) : Node(t) {}
	~Not() override;
	ptr clone() const override;
};
