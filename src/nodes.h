#pragma once

/*
 * Parse-tree nodes. Tree edges use unique_ptr; function bodies in the
 * command table use shared_ptr. Homogeneous sibling lists use vector<ptr>.
 */

#include <libshell.hpp>

#include "eval.h"

namespace shell {

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

/* Common prefix for redirection nodes (nfile / ndup / nhere). */
struct RedirList : Node {
	int fd{};

	explicit RedirList(Type t) : Node(t) {}
};

struct CommandNode : Node {
	int linno{};
	std::vector<ptr> assign;
	std::vector<ptr> args;
	std::vector<ptr> redirect;

	explicit CommandNode(Type t = Type::Cmd) : Node(t) {}
	~CommandNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct PipeNode : Node {
	int backgnd{};
	std::vector<ptr> cmdlist;

	explicit PipeNode(Type t = Type::Pipe) : Node(t) {}
	~PipeNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct RedirNode : Node {
	int linno{};
	ptr n;
	std::vector<ptr> redirect;

	explicit RedirNode(Type t = Type::Redir) : Node(t) {}
	~RedirNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct BinaryNode : Node {
	ptr ch1;
	ptr ch2;

	explicit BinaryNode(Type t = Type::Semi) : Node(t) {}
	~BinaryNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct IfNode : Node {
	ptr test;
	ptr ifpart;
	ptr elsepart;

	explicit IfNode(Type t = Type::If) : Node(t) {}
	~IfNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct ForNode : Node {
	int linno{};
	std::vector<ptr> args;
	ptr body;
	std::string var;

	explicit ForNode(Type t = Type::For) : Node(t) {}
	~ForNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct CaseNode : Node {
	int linno{};
	ptr expr;
	std::vector<ptr> cases;

	explicit CaseNode(Type t = Type::Case) : Node(t) {}
	~CaseNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct CaseListNode : Node {
	std::vector<ptr> pattern;
	ptr body;

	explicit CaseListNode(Type t = Type::CaseList) : Node(t) {}
	~CaseListNode() override;
	ptr clone() const override;
};

struct DefunNode : Node {
	int linno{};
	std::string text;
	ptr body;

	explicit DefunNode(Type t = Type::Defun) : Node(t) {}
	~DefunNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

struct ArgNode : Node {
	std::string text;
	std::vector<ptr> backquote;

	explicit ArgNode(Type t = Type::Arg) : Node(t) {}
	~ArgNode() override;
	ptr clone() const override;
};

struct FileNode : RedirList {
	ptr fname;
	std::string expfname;

	explicit FileNode(Type t = Type::To) : RedirList(t) {}
	~FileNode() override;
	ptr clone() const override;
};

struct DupNode : RedirList {
	int dupfd{};
	ptr vname;

	explicit DupNode(Type t = Type::ToFd) : RedirList(t) {}
	~DupNode() override;
	ptr clone() const override;
};

struct HereNode : RedirList {
	ptr doc;

	explicit HereNode(Type t = Type::Here) : RedirList(t) {}
	~HereNode() override;
	ptr clone() const override;
};

struct NotNode : Node {
	ptr com;

	NotNode(Type t = Type::Not) : Node(t) {}
	~NotNode() override;
	ptr clone() const override;
	Result<int> eval(
		Context &sh,
		EvalFlags flags,
		bool &checkexit
	) const override;
};

} // namespace shell
