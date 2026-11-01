/*
 * Parse-tree node cloning. Destructors are out-of-line so unique_ptr<Node>
 * sees a complete type.
 */

#include "nodes.h"

static std::vector<Node::ptr> clonelist(const std::vector<Node::ptr> &src)
{
	std::vector<Node::ptr> out;

	out.reserve(src.size());
	for (const auto &child : src)
		out.push_back(child ? child->clone() : nullptr);

	return out;
}

Node::Command::~Command() = default;

Node::ptr Node::Command::clone() const
{
	auto c = std::make_unique<Command>(type);

	c->linno = linno;
	c->assign = assign ? assign->clone() : nullptr;
	c->args = args ? args->clone() : nullptr;
	c->redirect = redirect ? redirect->clone() : nullptr;

	return c;
}

Node::Pipe::~Pipe() = default;

Node::ptr Node::Pipe::clone() const
{
	auto c = std::make_unique<Pipe>(type);

	c->backgnd = backgnd;
	c->cmdlist = clonelist(cmdlist);

	return c;
}

Node::Redir::~Redir() = default;

Node::ptr Node::Redir::clone() const
{
	auto c = std::make_unique<Redir>(type);

	c->linno = linno;
	c->n = n ? n->clone() : nullptr;
	c->redirect = redirect ? redirect->clone() : nullptr;

	return c;
}

Node::Binary::~Binary() = default;

Node::ptr Node::Binary::clone() const
{
	auto c = std::make_unique<Binary>(type);

	c->ch1 = ch1 ? ch1->clone() : nullptr;
	c->ch2 = ch2 ? ch2->clone() : nullptr;

	return c;
}

Node::If::~If() = default;

Node::ptr Node::If::clone() const
{
	auto c = std::make_unique<If>(type);

	c->test = test ? test->clone() : nullptr;
	c->ifpart = ifpart ? ifpart->clone() : nullptr;
	c->elsepart = elsepart ? elsepart->clone() : nullptr;

	return c;
}

Node::For::~For() = default;

Node::ptr Node::For::clone() const
{
	auto c = std::make_unique<For>(type);

	c->linno = linno;
	c->args = args ? args->clone() : nullptr;
	c->body = body ? body->clone() : nullptr;
	c->var = var;

	return c;
}

Node::Case::~Case() = default;

Node::ptr Node::Case::clone() const
{
	auto c = std::make_unique<Case>(type);

	c->linno = linno;
	c->expr = expr ? expr->clone() : nullptr;
	c->cases = cases ? cases->clone() : nullptr;

	return c;
}

Node::CaseList::~CaseList() = default;

Node::ptr Node::CaseList::clone() const
{
	auto c = std::make_unique<CaseList>(type);

	c->next = next ? next->clone() : nullptr;
	c->pattern = pattern ? pattern->clone() : nullptr;
	c->body = body ? body->clone() : nullptr;

	return c;
}

Node::Defun::~Defun() = default;

Node::ptr Node::Defun::clone() const
{
	auto c = std::make_unique<Defun>(type);

	c->linno = linno;
	c->text = text;
	c->body = body ? body->clone() : nullptr;

	return c;
}

Node::Arg::~Arg() = default;

Node::ptr Node::Arg::clone() const
{
	auto c = std::make_unique<Arg>(type);

	c->next = next ? next->clone() : nullptr;
	c->text = text;
	c->backquote = clonelist(backquote);

	return c;
}

Node::File::~File() = default;

Node::ptr Node::File::clone() const
{
	auto c = std::make_unique<File>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->fname = fname ? fname->clone() : nullptr;
	c->expfname = expfname;

	return c;
}

Node::Dup::~Dup() = default;

Node::ptr Node::Dup::clone() const
{
	auto c = std::make_unique<Dup>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->dupfd = dupfd;
	c->vname = vname ? vname->clone() : nullptr;

	return c;
}

Node::Here::~Here() = default;

Node::ptr Node::Here::clone() const
{
	auto c = std::make_unique<Here>(type);

	c->next = next ? next->clone() : nullptr;
	c->fd = fd;
	c->doc = doc ? doc->clone() : nullptr;

	return c;
}

Node::Not::~Not() = default;

Node::ptr Node::Not::clone() const
{
	auto c = std::make_unique<Not>(type);

	c->com = com ? com->clone() : nullptr;

	return c;
}
