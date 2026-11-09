/*
 * Parse-tree node cloning. Destructors are out-of-line so unique_ptr<Node>
 * sees a complete type.
 */

#include "nodes.h"
#include <memory>
#include <vector>

using namespace shell;

namespace shell {

namespace {
std::vector<Node::ptr> clonelist(const std::vector<Node::ptr> &src)
{
	std::vector<Node::ptr> out;

	out.reserve(src.size());
	for (const auto &child : src)
		out.push_back(child ? child->clone() : nullptr);

	return out;
}
} // namespace

CommandNode::~CommandNode() = default;

Node::ptr
CommandNode::clone() const
{
	auto c = std::make_unique<CommandNode>(type);

	c->linno = linno;
	c->assign = assign ? assign->clone() : nullptr;
	c->args = args ? args->clone() : nullptr;
	c->redirect = redirect ? redirect->clone() : nullptr;

	return c;
}

PipeNode::~PipeNode() = default;

Node::ptr
PipeNode::clone() const
{
	auto c = std::make_unique<PipeNode>(type);

	c->backgnd = backgnd;
	c->cmdlist = clonelist(cmdlist);

	return c;
}

RedirNode::~RedirNode() = default;

Node::ptr
RedirNode::clone() const
{
	auto c = std::make_unique<RedirNode>(type);

	c->linno = linno;
	c->n = n ? n->clone() : nullptr;
	c->redirect = redirect ? redirect->clone() : nullptr;

	return c;
}

BinaryNode::~BinaryNode() = default;

Node::ptr
BinaryNode::clone() const
{
	auto c = std::make_unique<BinaryNode>(type);

	c->ch1 = ch1 ? ch1->clone() : nullptr;
	c->ch2 = ch2 ? ch2->clone() : nullptr;

	return c;
}

IfNode::~IfNode() = default;

Node::ptr
IfNode::clone() const
{
	auto c = std::make_unique<IfNode>(type);

	c->test = test ? test->clone() : nullptr;
	c->ifpart = ifpart ? ifpart->clone() : nullptr;
	c->elsepart = elsepart ? elsepart->clone() : nullptr;

	return c;
}

ForNode::~ForNode() = default;

Node::ptr
ForNode::clone() const
{
	auto c = std::make_unique<ForNode>(type);

	c->linno = linno;
	c->args = args ? args->clone() : nullptr;
	c->body = body ? body->clone() : nullptr;
	c->var = var;

	return c;
}

CaseNode::~CaseNode() = default;

Node::ptr
CaseNode::clone() const
{
	auto c = std::make_unique<CaseNode>(type);

	c->linno = linno;
	c->expr = expr ? expr->clone() : nullptr;
	c->cases = cases ? cases->clone() : nullptr;

	return c;
}

CaseListNode::~CaseListNode() = default;

Node::ptr
CaseListNode::clone() const
{
	auto c = std::make_unique<CaseListNode>(type);

	c->next = next ? next->clone() : nullptr;
	c->pattern = pattern ? pattern->clone() : nullptr;
	c->body = body ? body->clone() : nullptr;

	return c;
}

DefunNode::~DefunNode() = default;

Node::ptr
DefunNode::clone() const
{
	auto c = std::make_unique<DefunNode>(type);

	c->linno = linno;
	c->text = text;
	c->body = body ? body->clone() : nullptr;

	return c;
}

ArgNode::~ArgNode() = default;

Node::ptr
ArgNode::clone() const
{
	auto c = std::make_unique<ArgNode>(type);

	c->next = next ? next->clone() : nullptr;
	c->text = text;
	c->backquote = clonelist(backquote);

	return c;
}

FileNode::~FileNode() = default;

Node::ptr
FileNode::clone() const
{
	auto c = std::make_unique<FileNode>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->fname = fname ? fname->clone() : nullptr;
	c->expfname = expfname;

	return c;
}

DupNode::~DupNode() = default;

Node::ptr
DupNode::clone() const
{
	auto c = std::make_unique<DupNode>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->dupfd = dupfd;
	c->vname = vname ? vname->clone() : nullptr;

	return c;
}

HereNode::~HereNode() = default;

Node::ptr
HereNode::clone() const
{
	auto c = std::make_unique<HereNode>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->doc = doc ? doc->clone() : nullptr;

	return c;
}

NotNode::~NotNode() = default;

Node::ptr
NotNode::clone() const
{
	auto c = std::make_unique<NotNode>(type);

	c->com = com ? com->clone() : nullptr;

	return c;
}

} // namespace shell
