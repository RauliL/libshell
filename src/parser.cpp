/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "./config.hpp"

#include <climits>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <type_traits>

#include "alias.h"
#include "builtins.h"
#include "error.h"
#include "expand.h"	/* defines rmescapes() */
#include "expand_buf.h"
#include "input.h"
#include "mystring.h"
#include "nodes.h"
#include "output.h"
#include "parser.h"
#include "shell.h"
#include "syntax.h"
#include "system.h"
#include "var.h"
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace shell;

namespace shell {

ParseSession::ParseSession() = default;
ParseSession::~ParseSession() = default;
ParseSession::ParseSession(ParseSession &&) noexcept = default;
ParseSession &ParseSession::operator=(ParseSession &&) noexcept = default;

/*
 * Shell command parser.
 */

/* Used by expandstr to get here-doc like behaviour (address only). */
namespace {
const std::string fake_eofmark;
} // namespace

#define FAKEEOFMARK (&fake_eofmark)

struct synstack {
	const char *syntax;
	struct synstack *prev;
	struct synstack *next;
	int innerdq;
	int varpushed;
	int dblquote;
	int backq;		/* Inside back quotes (here-doc word only). */
	int varnest;		/* levels of variables expansion */
	int parenlevel;		/* levels of parens in arithmetic */
	int dqvarnest;		/* levels of variables expansion within double quotes */
};

namespace {
const std::unordered_map<std::string_view, Token> keyword_map = {
	{ "!", Token::Not },
	{ "case", Token::Case },
	{ "do", Token::Do },
	{ "done", Token::Done },
	{ "elif", Token::Elif },
	{ "else", Token::Else },
	{ "esac", Token::Esac },
	{ "fi", Token::Fi },
	{ "for", Token::For },
	{ "if", Token::If },
	{ "in", Token::In },
	{ "then", Token::Then },
	{ "until", Token::Until },
	{ "while", Token::While },
	{ "{", Token::Begin },
	{ "}", Token::End },
};

Result<Node::ptr> list(Context &sh, int nlflag, bool *eof_out = nullptr);
Result<Node::ptr> andor(Context &sh);
Result<Node::ptr> pipeline(Context &sh);
} // namespace

Result<Node::ptr> command(Context &sh);
namespace {
Result<Node::ptr> simplecmd(Context &sh);
Node::ptr makename(Context &sh);
Result<void> parsefname(Context &sh);
Result<void> parseheredoc(Context &sh);
Result<Token> readtoken(Context &sh);
Result<Token> xxreadtoken(Context &sh);
int pgetc_eatbnl(Context &sh);
Result<Token> readtoken1(Context &sh, int, char const *, const std::string *, bool);
tl::unexpected<Error> synexpect(Context &sh, std::optional<Token> = std::nullopt);
tl::unexpected<Error> synerror(Context &sh, const char *);
void setprompt(Context &sh, int);
} // namespace

namespace {
template <
	typename T,
	typename... Args,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
inline Node::ptr
make_node(Args &&...args)
{
	return std::make_unique<T>(std::forward<Args>(args)...);
}
} // namespace

bool
isassignment(std::string_view p)
{
	auto n = endofname(p);

	if (n == 0)
		return false;

	return n < p.size() && p[n] == '=';
}

bool
issimplecmd(const Node *n, std::string_view name)
{
	return n && n->type == Node::Type::Cmd &&
	       !as<CommandNode>(n)->args.empty() &&
	       as<ArgNode>(as<CommandNode>(n)->args[0])->text == name;
}

namespace {
inline bool
realeofmark(const std::string *eofmark)
{
	return eofmark && eofmark != FAKEEOFMARK;
}

void
grabstackblock(ExpandBuf &eb, std::size_t len)
{
	eb.commit(len);
}
} // namespace

/**
 * Returns textual description of token.
 */
namespace {
const char*
token_name(Token t)
{
	switch (t)
	{
		case Token::Eof:
			return "end of file";

		case Token::Blank:
			return "blank";

		case Token::Nl:
			return "newline";

		case Token::Semi:
			return "\";\"";

		case Token::Backgnd:
			return "\"&\"";

		case Token::And:
			return "\"&&\"";

		case Token::Or:
			return "\"||\"";

		case Token::Pipe:
			return "\"|\"";

		case Token::Lp:
			return "\"(\"";

		case Token::Rp:
			return "\")\"";

		case Token::Endcase:
			return "\";;\"";

		case Token::Endbquote:
			return "\"`\"";

		case Token::Redir:
			return "redirection";

		case Token::Word:
			return "word";

		case Token::Not:
			return "\"!\"";

		case Token::Case:
			return "\"case\"";

		case Token::Do:
			return "\"do\"";

		case Token::Done:
			return "\"done\"";

		case Token::Elif:
			return "\"elif\"";

		case Token::Else:
			return "\"else\"";

		case Token::Esac:
			return "\"esac\"";

		case Token::Fi:
			return "\"fi\"";

		case Token::For:
			return "\"for\"";

		case Token::If:
			return "\"if\"";

		case Token::In:
			return "\"in\"";

		case Token::Then:
			return "\"then\"";

		case Token::Until:
			return "\"until\"";

		case Token::While:
			return "\"while\"";

		case Token::Begin:
			return "\"{\"";

		case Token::End:
			return "\"}\"";
	}

	return "unknown";
}
} // namespace

/**
 * Tests whether given token is considered to mark end of a list.
 */
namespace {
bool is_end_of_list_token(Token t)
{
	switch (t)
	{
		case Token::Eof:
		case Token::Rp:
		case Token::Endcase:
		case Token::Endbquote:
		case Token::Do:
		case Token::Done:
		case Token::Elif:
		case Token::Else:
		case Token::Esac:
		case Token::Fi:
		case Token::Then:
		case Token::End:
			return true;

		default:
			return false;
	}
}
} // namespace

/*
 * Read and parse a command.  eof is set on end of file.  (A null tree is a
 * valid parse tree indicating a blank line.)
 */

Result<ParseCmdResult>
parsecmd(Context &sh, int interact)
{
	sh.parse.tokpushback = 0;
	sh.parse.checkkwd = 0;
	sh.parse.heredoclist.clear();
	sh.parse.doprompt = interact;
	if (sh.parse.doprompt)
		setprompt(sh, sh.parse.doprompt);
	sh.parse.needprompt = 0;
	bool eof = false;
	Node::ptr tree;
	TRY_SH_SET(tree, list(sh, 1, &eof));
	return ParseCmdResult{eof, std::move(tree)};
}

namespace {
Result<Node::ptr>
list(Context &sh, int nlflag, bool *eof_out)
{
	int chknl = nlflag & 1 ? 0 : CHKNL;
	Node::ptr n1, n2, n3;
	Token tok;

	for (;;) {
		sh.parse.checkkwd = chknl | CHKKWD | CHKALIAS;
		TRY_SH_SET(tok, readtoken(sh));
		switch (tok) {
		case Token::Nl:
			TRY_SHV(parseheredoc(sh));
			return n1;

		case Token::Eof:
			if (!n1 && !chknl && eof_out)
				*eof_out = true;
out_eof:
			TRY_SHV(parseheredoc(sh));
			sh.parse.tokpushback++;
			sh.parse.lasttoken = Token::Eof;
			return n1;
		default:
			break;
		}

		sh.parse.tokpushback++;
		if (nlflag == 2 && is_end_of_list_token(tok))
			return n1;
		nlflag |= 2;

		TRY_SH_SET(n2, andor(sh));
		TRY_SH_SET(tok, readtoken(sh));
		if (tok == Token::Backgnd) {
			if (n2->type == Node::Type::Pipe) {
				as<PipeNode>(n2)->backgnd = 1;
			} else {
				if (n2->type != Node::Type::Redir) {
					n3 = make_node<RedirNode>();
					as<RedirNode>(n3)->n = std::move(n2);
					n2 = std::move(n3);
				}
				n2->type = Node::Type::Backgnd;
			}
		}
		if (!n1) {
			n1 = std::move(n2);
		}
		else {
			n3 = make_node<BinaryNode>();
			n3->type = Node::Type::Semi;
			as<BinaryNode>(n3)->ch1 = std::move(n1);
			as<BinaryNode>(n3)->ch2 = std::move(n2);
			n1 = std::move(n3);
		}
		switch (tok) {
		case Token::Eof:
			goto out_eof;
		case Token::Nl:
			sh.parse.tokpushback++;
			/* fall through */
		case Token::Backgnd:
		case Token::Semi:
			break;
		default:
			if (!chknl)
				return synexpect(sh);
			sh.parse.tokpushback++;
			return n1;
		}
	}
}

Result<Node::ptr>
andor(Context &sh)
{
	Node::ptr n1, n2, n3;
	Token token;

	TRY_SH_SET(n1, pipeline(sh));
	for (;;) {
		TRY_SH_SET(token, readtoken(sh));
		Node::Type t;

		if (token == Token::And) {
			t = Node::Type::And;
		} else if (token == Token::Or) {
			t = Node::Type::Or;
		} else {
			sh.parse.tokpushback++;
			return n1;
		}
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		TRY_SH_SET(n2, pipeline(sh));
		n3 = make_node<BinaryNode>();
		n3->type = t;
		as<BinaryNode>(n3)->ch1 = std::move(n1);
		as<BinaryNode>(n3)->ch2 = std::move(n2);
		n1 = std::move(n3);
	}
}

Result<Node::ptr>
pipeline(Context &sh)
{
	Node::ptr n1, n2, pipenode;
	bool negate = false;
	Token tok;

	TRY_SH_SET(tok, readtoken(sh));
	if (tok == Token::Not) {
		negate = !negate;
		sh.parse.checkkwd = CHKKWD | CHKALIAS;
	} else
		sh.parse.tokpushback++;
	TRY_SH_SET(n1, command(sh));
	TRY_SH_SET(tok, readtoken(sh));
	if (tok == Token::Pipe) {
		pipenode = make_node<PipeNode>();
		pipenode->type = Node::Type::Pipe;
		as<PipeNode>(pipenode)->backgnd = 0;
		as<PipeNode>(pipenode)->cmdlist.push_back(std::move(n1));
		do {
			Node::ptr cmd;
			sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
			TRY_SH_SET(cmd, command(sh));
			as<PipeNode>(pipenode)->cmdlist.push_back(std::move(cmd));
			TRY_SH_SET(tok, readtoken(sh));
		} while (tok == Token::Pipe);
		n1 = std::move(pipenode);
	}
	sh.parse.tokpushback++;
	if (negate) {
		n2 = make_node<NotNode>();
		n2->type = Node::Type::Not;
		as<NotNode>(n2)->com = std::move(n1);
		return n2;
	} else
		return n1;
}
} // namespace

Result<Node::ptr>
command(Context &sh)
{
	Node::ptr n1, n2;
	std::vector<Node::ptr> redir;
	Token t;
	int savelinno;

	savelinno = plinno;

	TRY_SH_SET(t, readtoken(sh));
	switch (t) {
	default:
		return synexpect(sh);
	case Token::If: {
		Node::ptr part;
		n1 = make_node<IfNode>();
		n1->type = Node::Type::If;
		TRY_SH_SET(part, list(sh, 0));
		as<IfNode>(n1)->test = std::move(part);
		TRY_SH_SET(t, readtoken(sh));
		if (t != Token::Then)
			return synexpect(sh, Token::Then);
		TRY_SH_SET(part, list(sh, 0));
		as<IfNode>(n1)->ifpart = std::move(part);
		Node *nwalk = n1.get();
		TRY_SH_SET(t, readtoken(sh));
		while (t == Token::Elif) {
			as<IfNode>(nwalk)->elsepart = make_node<IfNode>();
			nwalk = as<IfNode>(nwalk)->elsepart.get();
			nwalk->type = Node::Type::If;
			TRY_SH_SET(part, list(sh, 0));
			as<IfNode>(nwalk)->test = std::move(part);
			TRY_SH_SET(t, readtoken(sh));
			if (t != Token::Then)
				return synexpect(sh, Token::Then);
			TRY_SH_SET(part, list(sh, 0));
			as<IfNode>(nwalk)->ifpart = std::move(part);
			TRY_SH_SET(t, readtoken(sh));
		}
		if (sh.parse.lasttoken == Token::Else) {
			TRY_SH_SET(part, list(sh, 0));
			as<IfNode>(nwalk)->elsepart = std::move(part);
		} else {
			sh.parse.tokpushback++;
		}
		t = Token::Fi;
		break;
	}
	case Token::While:
	case Token::Until: {
		Token got;
		Node::ptr part;
		n1 = make_node<BinaryNode>();
		n1->type = (sh.parse.lasttoken == Token::While)? Node::Type::While : Node::Type::Until;
		TRY_SH_SET(part, list(sh, 0));
		as<BinaryNode>(n1)->ch1 = std::move(part);
		TRY_SH_SET(got, readtoken(sh));
		if (got != Token::Do)
			return synexpect(sh, Token::Do);
		TRY_SH_SET(part, list(sh, 0));
		as<BinaryNode>(n1)->ch2 = std::move(part);
		t = Token::Done;
		break;
	}
	case Token::For: {
		Node::ptr part;
		TRY_SH_SET(t, readtoken(sh));
		if (t != Token::Word || sh.parse.quoteflag ||
		    endofname(sh.parse.wordtext) != sh.parse.wordtext.size())
			return synerror(sh, "Bad for loop variable");
		n1 = make_node<ForNode>();
		n1->type = Node::Type::For;
		as<ForNode>(n1)->linno = savelinno;
		as<ForNode>(n1)->var = sh.parse.wordtext;
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		TRY_SH_SET(t, readtoken(sh));
		if (t == Token::In) {
			TRY_SH_SET(t, readtoken(sh));
			while (t == Token::Word) {
				auto arg = make_node<ArgNode>();
				arg->type = Node::Type::Arg;
				as<ArgNode>(arg)->text = sh.parse.wordtext;
				as<ArgNode>(arg)->backquote = std::move(sh.parse.backquotelist);
				as<ForNode>(n1)->args.push_back(std::move(arg));
				TRY_SH_SET(t, readtoken(sh));
			}
			if (sh.parse.lasttoken != Token::Nl && sh.parse.lasttoken != Token::Semi)
				return synexpect(sh);
		} else {
			n2 = make_node<ArgNode>();
			n2->type = Node::Type::Arg;
			as<ArgNode>(n2)->text = dolatstr;
			as<ForNode>(n1)->args.push_back(std::move(n2));
			/*
			 * Newline or semicolon here is optional (but note
			 * that the original Bourne shell only allowed NL).
			 */
			if (sh.parse.lasttoken != Token::Semi)
				sh.parse.tokpushback++;
		}
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		TRY_SH_SET(t, readtoken(sh));
		if (t != Token::Do)
			return synexpect(sh, Token::Do);
		TRY_SH_SET(part, list(sh, 0));
		as<ForNode>(n1)->body = std::move(part);
		t = Token::Done;
		break;
	}
	case Token::Case: {
		Node::ptr part;
		n1 = make_node<CaseNode>();
		n1->type = Node::Type::Case;
		as<CaseNode>(n1)->linno = savelinno;
		TRY_SH_SET(t, readtoken(sh));
		if (t != Token::Word)
			return synexpect(sh, Token::Word);
		n2 = make_node<ArgNode>();
		n2->type = Node::Type::Arg;
		as<ArgNode>(n2)->text = sh.parse.wordtext;
		as<ArgNode>(n2)->backquote = std::move(sh.parse.backquotelist);
		as<CaseNode>(n1)->expr = std::move(n2);
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		TRY_SH_SET(t, readtoken(sh));
		if (t != Token::In)
			return synexpect(sh, Token::In);
next_case:
		sh.parse.checkkwd = CHKNL | CHKKWD;
		TRY_SH_SET(t, readtoken(sh));
		while (t != Token::Esac) {
			if (sh.parse.lasttoken == Token::Lp)
				TRY_SHV(readtoken(sh));
			auto cp = make_node<CaseListNode>();
			cp->type = Node::Type::CaseList;
			for (;;) {
				if (sh.parse.lasttoken < Token::Word)
					return synexpect(sh, Token::Word);
				auto arg = make_node<ArgNode>();
				arg->type = Node::Type::Arg;
				as<ArgNode>(arg)->text = sh.parse.wordtext;
				as<ArgNode>(arg)->backquote = std::move(sh.parse.backquotelist);
				as<CaseListNode>(cp)->pattern.push_back(std::move(arg));
				TRY_SH_SET(t, readtoken(sh));
				if (t != Token::Pipe)
					break;
				TRY_SHV(readtoken(sh));
			}
			if (sh.parse.lasttoken != Token::Rp)
				return synexpect(sh, Token::Rp);
			TRY_SH_SET(part, list(sh, 2));
			as<CaseListNode>(cp)->body = std::move(part);
			as<CaseNode>(n1)->cases.push_back(std::move(cp));

			sh.parse.checkkwd = CHKNL | CHKKWD;
			TRY_SH_SET(t, readtoken(sh));
			if (t != Token::Esac) {
				if (t != Token::Endcase)
					return synexpect(sh, Token::Endcase);
				else
					goto next_case;
			}
		}
		goto redir;
	}
	case Token::Lp: {
		Node::ptr part;
		n1 = make_node<RedirNode>();
		n1->type = Node::Type::Subshell;
		as<RedirNode>(n1)->linno = savelinno;
		TRY_SH_SET(part, list(sh, 0));
		as<RedirNode>(n1)->n = std::move(part);
		t = Token::Rp;
		break;
	}
	case Token::Begin:
		TRY_SH_SET(n1, list(sh, 0));
		t = Token::End;
		break;
	case Token::Word:
	case Token::Redir:
		sh.parse.tokpushback++;
		return simplecmd(sh);
	}

	{
		Token expected = t;
		Token got;
		TRY_SH_SET(got, readtoken(sh));
		if (got != expected)
			return synexpect(sh, expected);
	}

redir:
	/* Now check for redirection which may follow command */
	sh.parse.checkkwd = CHKKWD | CHKALIAS;
	TRY_SH_SET(t, readtoken(sh));
	while (t == Token::Redir) {
		TRY_SHV(parsefname(sh));
		redir.push_back(std::move(sh.parse.redirnode));
		TRY_SH_SET(t, readtoken(sh));
	}
	sh.parse.tokpushback++;
	if (!redir.empty()) {
		if (n1->type != Node::Type::Subshell) {
			n2 = make_node<RedirNode>();
			n2->type = Node::Type::Redir;
			as<RedirNode>(n2)->linno = savelinno;
			as<RedirNode>(n2)->n = std::move(n1);
			n1 = std::move(n2);
		}
		as<RedirNode>(n1)->redirect = std::move(redir);
	}

	return n1;
}

namespace {
Result<Node::ptr>
simplecmd(Context &sh)
{
	std::vector<Node::ptr> args;
	Node::ptr n;
	std::vector<Node::ptr> vars;
	std::vector<Node::ptr> redir;
	int savecheckkwd;
	int savelinno;
	Token tok;

	savecheckkwd = CHKALIAS;
	savelinno = plinno;
	for (;;) {
		sh.parse.checkkwd = savecheckkwd;
		TRY_SH_SET(tok, readtoken(sh));
		switch (tok) {
		case Token::Word:
			n = make_node<ArgNode>();
			n->type = Node::Type::Arg;
			as<ArgNode>(n)->text = sh.parse.wordtext;
			as<ArgNode>(n)->backquote = std::move(sh.parse.backquotelist);
			if (savecheckkwd && isassignment(sh.parse.wordtext)) {
				vars.push_back(std::move(n));
			} else {
				args.push_back(std::move(n));
				savecheckkwd = 0;
			}
			break;
		case Token::Redir:
			TRY_SHV(parsefname(sh));	/* read name of redirection file */
			redir.push_back(std::move(sh.parse.redirnode));
			break;
		case Token::Lp:
			if (args.size() == 1 && vars.empty() && redir.empty()) {
				const BuiltinCommand *bcmd;
				std::string fname = std::move(as<ArgNode>(args[0])->text);
				Node::ptr body;

				/* We have a function */
				TRY_SH_SET(tok, readtoken(sh));
				if (tok != Token::Rp)
					return synexpect(sh, Token::Rp);
				if (
					endofname(fname) != fname.size() || (
						(bcmd = find_builtin(fname)) &&
						bcmd->flags & BuiltinCommand::Flag::Special
					)
				)
					return synerror(sh, "Bad function name");
				args.clear();
				n = make_node<DefunNode>();
				n->type = Node::Type::Defun;
				sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
				as<DefunNode>(n)->text = std::move(fname);
				as<DefunNode>(n)->linno = plinno;
				TRY_SH_SET(body, command(sh));
				as<DefunNode>(n)->body = std::move(body);
				return n;
			}
			/* fall through */
		default:
			sh.parse.tokpushback++;
			goto out;
		}
	}
out:
	n = make_node<CommandNode>();
	n->type = Node::Type::Cmd;
	as<CommandNode>(n)->linno = savelinno;
	as<CommandNode>(n)->args = std::move(args);
	as<CommandNode>(n)->assign = std::move(vars);
	as<CommandNode>(n)->redirect = std::move(redir);
	return n;
}

Node::ptr
makename(Context &sh)
{
	Node::ptr n = make_node<ArgNode>();

	n->type = Node::Type::Arg;
	as<ArgNode>(n)->text = sh.parse.wordtext;
	as<ArgNode>(n)->backquote = std::move(sh.parse.backquotelist);

	return n;
}
} // namespace

Result<void>
fixredir(Context &sh, Node *n, const char *text, int err)
{
	if (!err)
		as<DupNode>(n)->vname.reset();

	if (is_digit(text[0]) && text[1] == '\0')
		as<DupNode>(n)->dupfd = digit_val(text[0]);
	else if (text[0] == '-' && text[1] == '\0')
		as<DupNode>(n)->dupfd = -1;
	else {
		if (err)
			return sh_error(sh, "Bad fd number: {}", text);

		as<DupNode>(n)->vname = makename(sh);
	}
	return {};
}

namespace {
Result<void>
parsefname(Context &sh)
{
	auto n = sh.parse.redirnode.get();
	Token tok;

	if (n->type == Node::Type::Here)
		sh.parse.checkkwd |= CHKEOFMARK;
	TRY_SH_SET(tok, readtoken(sh));
	if (tok != Token::Word)
		return synexpect(sh);
	sh.parse.checkkwd &= ~CHKEOFMARK;
	if (n->type == Node::Type::Here) {
		if (sh.parse.quoteflag == 0)
			n->type = Node::Type::XHere;
		sh.parse.wordtext = rmescapes(sh.parse.wordtext);
		sh.parse.pending_here->eofmark = sh.parse.wordtext;
		sh.parse.heredoclist.push_back(
			std::move(*sh.parse.pending_here));
		sh.parse.pending_here.reset();
	} else if (n->type == Node::Type::ToFd || n->type == Node::Type::FromFd) {
		TRY_SHV(fixredir(sh, n, sh.parse.wordtext.c_str(), 0));
	} else {
		as<FileNode>(n)->fname = makename(sh);
	}
	return {};
}
} // namespace

/*
 * Input any here documents.
 */

namespace {
Result<void>
parseheredoc(Context &sh)
{
	auto docs = std::move(sh.parse.heredoclist);

	for (auto &here : docs) {
		if (sh.parse.needprompt) {
			setprompt(sh, 2);
		}
		if (here.here->type == Node::Type::Here)
			TRY_SHV(readtoken1(sh, pgetc(sh), SQSYNTAX, &here.eofmark, here.striptabs));
		else
			TRY_SHV(readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, &here.eofmark, here.striptabs));
		Node::ptr n = make_node<ArgNode>();
		n->type = Node::Type::Arg;
		as<ArgNode>(n)->text = sh.parse.wordtext;
		as<ArgNode>(n)->backquote = std::move(sh.parse.backquotelist);
		as<HereNode>(here.here)->doc = std::move(n);
	}
	return {};
}

Result<Token>
readtoken(Context &sh)
{
	Token t;
	int kwd = sh.parse.checkkwd;

top:
	TRY_SH_SET(t, xxreadtoken(sh));

	/*
	 * eat newlines
	 */
	if (kwd & CHKNL) {
		while (t == Token::Nl) {
			TRY_SHV(parseheredoc(sh));
			sh.parse.checkkwd = 0;
			TRY_SH_SET(t, xxreadtoken(sh));
		}
	}

	kwd |= sh.parse.checkkwd;
	sh.parse.checkkwd = 0;

	if (t != Token::Word || sh.parse.quoteflag) {
		goto out;
	}

	/*
	 * check for keywords
	 */
	if (kwd & CHKKWD) {
		if (const auto pp = findkwd(sh.parse.wordtext)) {
			t = *pp;
			sh.parse.lasttoken = t;
			goto out;
		}
	}

	if (kwd & CHKALIAS) {
		if (const auto ap = lookupalias(sh, sh.parse.wordtext, true)) {
			if (!ap->value.empty())
				pushstring(sh, std::string_view(ap->value), ap);
			goto top;
		}
	}

out:
	return t;
}

void
nlprompt(Context &sh)
{
	plinno++;
	if (sh.parse.doprompt)
		setprompt(sh, 2);
}

void
nlnoprompt(Context &sh)
{
	plinno++;
	sh.parse.needprompt = sh.parse.doprompt;
}
} // namespace

/*
 * Read the next input token.
 * If the token is a word, we set sh.parse.backquotelist to the list of cmds in
 *	backquotes.  We set sh.parse.quoteflag to true if any part of the word was
 *	quoted.
 * If the token is Token::Redir, then we set sh.parse.redirnode to a structure containing
 *	the redirection.
 *
 * [Change comment:  here documents and internal procedures]
 * [Readtoken shouldn't have any arguments.  Perhaps we should make the
 *  word parsing code into a separate routine.  In this case, readtoken
 *  doesn't need to have any internal procedures, but parseword does.
 *  We could also make parseoperator in essence the main routine, and
 *  have parseword (readtoken1?) handle both words and redirection.]
 */

#define RETURN(token)	return sh.parse.lasttoken = token

namespace {
Result<Token>
xxreadtoken(Context &sh)
{
	int c;

	if (sh.parse.tokpushback) {
		sh.parse.tokpushback = 0;
		return sh.parse.lasttoken;
	}
	if (sh.parse.needprompt) {
		setprompt(sh, 2);
	}
	for (;;) {	/* until token or start of word found */
		Token tok;

		c = pgetc_eatbnl(sh);
		switch (c) {
		case ' ': case '\t':
			continue;
		case '#':
			while ((c = pgetc(sh)) != '\n' && c != PEOF);
			pungetc(sh);
			continue;
		case '\n':
			nlnoprompt(sh);
			RETURN(Token::Nl);
		case PEOF:
			RETURN(Token::Eof);
		case '&':
			if (pgetc_eatbnl(sh) == '&')
				RETURN(Token::And);
			pungetc(sh);
			RETURN(Token::Backgnd);
		case '|':
			if (pgetc_eatbnl(sh) == '|')
				RETURN(Token::Or);
			pungetc(sh);
			RETURN(Token::Pipe);
		case ';':
			if (pgetc_eatbnl(sh) == ';')
				RETURN(Token::Endcase);
			pungetc(sh);
			RETURN(Token::Semi);
		case '(':
			RETURN(Token::Lp);
		case ')':
			RETURN(Token::Rp);
		}
		TRY_SH_SET(tok, readtoken1(sh, c, BASESYNTAX, nullptr, 0));
		if (tok != Token::Blank)
			return tok;
	}
#undef RETURN
}

int
pgetc_eatbnl(Context &sh)
{
	int c;

	while ((c = pgetc(sh)) == '\\') {
		if (pgetc(sh) != '\n') {
			pungetc(sh);
			break;
		}

		nlprompt(sh);
	}

	return c;
}

int
pgetc_top(Context &sh, struct synstack *stack)
{
	return stack->syntax == SQSYNTAX ? pgetc(sh) : pgetc_eatbnl(sh);
}

void
synstack_push(Context &sh, struct synstack **stack, struct synstack *next,
			  const char *syntax)
{
	memset(next, 0, sizeof(*next));
	next->syntax = syntax;
	next->next = *stack;
	(*stack)->prev = next;
	*stack = next;
}

void
synstack_pop(Context &sh, struct synstack **stack)
{
	*stack = (*stack)->next;
}

/* Reuse a popped frame via prev, or allocate one that outlives readtoken1. */
synstack *
synstack_acquire(synstack *stack, std::deque<synstack> &storage)
{
	if (stack->prev)
		return stack->prev;
	storage.emplace_back();
	return &storage.back();
}
} // namespace

unsigned
getmbc(Context &sh, int c, char *out, int mode)
{
	char *const start = out;
	mbstate_t mbst = {};
	unsigned ml = 0;
	size_t ml2;
	wchar_t wc;
	char *mbc;

	if (likely(c >= 0 || c <= PEOF))
		return 0;

	mbc = (mode & 3) < 2 ? out + 2 + (mode == 1) : out;
	mbc[ml] = c;
	while ((ml2 = mbrtowc(&wc, mbc + ml++, 1, &mbst)) ==
	       static_cast<size_t>(-2)) {
		if (ml >= MB_LEN_MAX)
			break;
		c = pgetc_eoa(sh);
		if (c == PEOA || c == PEOF)
			break;
		mbc[ml] = c;
	}

	if (ml2 == 1 && ml > 1) {
		if (mode == 4 && iswblank(wc))
			return 1;

		if ((mode & 3) < 2) {
			(*(out)++ = (CTLMBCHAR));
			if (mode == 1)
				(*(out)++ = (CTLESC));
			(*(out)++ = (ml));
		}
		((out) += (ml));
		if ((mode & 3) < 2) {
			(*(out)++ = (ml));
			(*(out)++ = (CTLMBCHAR));
		}

		return out - start;
	}

	if (ml > 1)
		pungetn(sh, ml - 1);

	return 0;
}

namespace {
char *
dollarsq_escape(Context &sh, char *out)
{
	/* 10 = length of UXXXXXXXX + NUL */
	char str[10];
	unsigned len;
	char *p;

	for (len = 0; len < sizeof(str) - 1;) {
		int c = pgetc(sh);

		if (c <= PEOF)
			break;

		str[len++] = c;

		if (c == '\'')
			break;
	}
	str[len] = 0;

	p = str;
	if (*p != 'c') {
		unsigned ret;

		ret = conv_escape(sh, p, out, true);
		p += ret >> 4;
		out += ret & 15;
	} else if (*++p) {
		int conv_ch;
		int c;

		c = (unsigned char)*p++;

		p += !((c ^ *p) | (c ^ '\\'));

		conv_ch = (c & ~((c & 0x40) >> 1) & 0x7f) ^ 0x40;
		(*(out)++ = (conv_ch));
	}

	pungetn(sh, len - (p - str));
	return out;
}
} // namespace

/*
 * If eofmark is nullptr, read a word or a redirection symbol.  If eofmark
 * is not nullptr, read a here document.  In the latter case, eofmark is the
 * word which marks the end of the document and striptabs is true if
 * leading tabs should be stripped from the document.  The argument firstc
 * is the first character of the input token or document.
 *
 * Because C does not have internal subroutines, I have simulated them
 * using goto's to implement the subroutine linkage.  The following macros
 * will run code that appears at the end of readtoken1.
 */

#define CHECKEND()	{goto checkend; checkend_return:;}
#define PARSEREDIR()	{goto parseredir; parseredir_return:;}
#define PARSESUB()	{goto parsesub; parsesub_return:;}
#define PARSEBACKQOLD()	{oldstyle = 1; goto parsebackq; parsebackq_oldreturn:;}
#define PARSEBACKQNEW()	{oldstyle = 0; goto parsebackq; parsebackq_newreturn:;}
#define	PARSEARITH()	{goto parsearith; parsearith_return:;}

namespace {
Result<Token>
readtoken1(Context &sh, int firstc, char const *syntax, const std::string *eofmark, bool striptabs)
{
	ExpandBuf eb;
	struct synstack synbase = {
		.syntax = syntax,
		.dblquote = syntax == DQSYNTAX,
	};
	std::deque<synstack> syn_storage;
	int chkeofmark = sh.parse.checkkwd & CHKEOFMARK;
	struct synstack *synstack = &synbase;
	bool printesc = syntax == SQSYNTAX;
	const char *newsyn = nullptr;
	std::vector<Node::ptr> bqlist;
	int dollarsq = 0;
	int c = firstc;
	int quotef = 0;
	int oldstyle;
	size_t len;
	char *out;

	out = eb.base();
	loop: {	/* for each line, until end of word */
		CHECKEND();	/* set c to PEOF if at end of here document */
		/* Until end of line or end of word */
		for (;; c = pgetc_top(sh, synstack)) {
			int fieldsplitting;
			unsigned ml;

			/* Permit max(MB_LEN_MAX, 23) calls to USTPUTC. */
			out = eb.makespace((MB_LEN_MAX > 16 ? MB_LEN_MAX : 16) + 7, out);
			fieldsplitting = synstack->syntax == BASESYNTAX &&
					 !(synstack->varnest |
					   synstack->backq) ? 4 : 0;
			ml = getmbc(sh, c, out, fieldsplitting |
					    (printesc ? 2 : 0));
			if (ml == 1) {
				if (out == eb.base())
					return Token::Blank;
				c = pgetc(sh);
				break;
			}
			out += ml;
			if (ml)
				continue;
			switch(synstack->syntax[c]) {
			case CNL:	/* '\n' */
				if (fieldsplitting)
					goto endword;	/* exit outer loop */
				(*(out)++ = (c));
				nlprompt(sh);
				c = pgetc_top(sh, synstack);
				goto loop;		/* continue outer loop */
			case CWORD:
				(*(out)++ = (c));
				break;
			case CCTL:
				if (c == dollarsq) {
					out = dollarsq_escape(sh, out);
					break;
				}
				if ((!eofmark) | synstack->dblquote |
				    synstack->varnest)
					(*(out)++ = (CTLESC));
				(*(out)++ = (c));
				break;
			/* backslash */
			case CBACK:
				c = pgetc(sh);
				if (c == PEOF) {
					(*(out)++ = (CTLESC));
					(*(out)++ = ('\\'));
					pungetc(sh);
					break;
				}

				if (
					(synstack->dblquote |
					 synstack->backq) &&
					c != '\\' && c != '`' &&
					c != '$' && (
						c != '"' ||
						(eofmark && !synstack->varnest)
					) && (
						c != '}' ||
						!synstack->varnest
					)
				) {
					(*(out)++ = (CTLESC));
					(*(out)++ = ('\\'));
				}
				quotef++;

				ml = getmbc(sh, c, out, 1);
				out += ml;
				if (ml)
					break;

				(*(out)++ = (CTLESC));
				(*(out)++ = (c));
				break;
			case CSQUOTE:
csquote:
				synstack->syntax = SQSYNTAX;
quotemark:
				if (!eofmark) {
					(*(out)++ = (CTLQUOTEMARK));
				}
				break;
			case CDQUOTE:
				synstack->syntax = DQSYNTAX;
				synstack->dblquote = 1;
toggledq:
				if (synstack->varnest)
					synstack->innerdq ^= 1;
				goto quotemark;
			case CENDQUOTE:
				if (eofmark && !synstack->varnest) {
					(*(out)++ = (c));
					break;
				}

				if (synstack->dqvarnest == 0) {
					if (likely(dollarsq)) {
						char *p = eb.base();

						*out = 0;
						out = p + strlen(p);
						dollarsq = 0;
					}

					synstack->syntax = BASESYNTAX;
					synstack->dblquote = 0;
				}

				quotef++;

				if (c == '"')
					goto toggledq;

				goto quotemark;
			case CVAR:	/* '$' */
				PARSESUB();		/* parse substitution */
				break;
			case CENDVAR:	/* '}' */
				if (!synstack->innerdq &&
				    synstack->varnest > 0) {
					if (!--synstack->varnest &&
					    synstack->varpushed)
						synstack_pop(sh, &synstack);
					else if (synstack->dqvarnest > 0)
						synstack->dqvarnest--;
					if (!chkeofmark)
						c = CTLENDVAR;
				}
				(*(out)++ = (c));
				break;
			case CLP:	/* '(' in arithmetic */
				synstack->parenlevel++;
				(*(out)++ = (c));
				break;
			case CRP:	/* ')' in arithmetic */
				if (synstack->parenlevel > 0)
					--synstack->parenlevel;
				else if (pgetc_eatbnl(sh) == ')') {
					synstack_pop(sh, &synstack);
					if (chkeofmark)
						(*(out)++ = (c));
					else
						c = CTLENDARI;
				} else {
					/*
					 * unbalanced parens
					 *  (don't 2nd guess - no error)
					 */
					pungetc(sh);
				}
				(*(out)++ = (c));
				break;
			case CBQUOTE:	/* '`' */
				if (synstack->backq == 2)
					goto end_backq;
				(*(out)++ = ('`'));
				PARSEBACKQOLD();
				break;
			case CEOF:
				goto endword;		/* exit outer loop */
			default:
				if (c == ')' && synstack->backq == 1) {
end_backq:
					synstack_pop(sh, &synstack);
					printesc = 0;
				} else if (fieldsplitting)
					goto endword;	/* exit outer loop */
				(*(out)++ = (c));
			}
		}
	}
endword:
	if (synstack->syntax == ARISYNTAX)
		return synerror(sh, "Missing '))'");
	if ((synstack->syntax != BASESYNTAX && !eofmark) ||
	    synstack->backq)
		return synerror(sh, "Unterminated quoted string");
	if (synstack->varnest != 0) {
		/* { */
		return synerror(sh, "Missing '}'");
	}
	(*(out)++ = ('\0'));
	len = out - eb.base();
	out = eb.base();
	if (!eofmark) {
		if ((c == '>' || c == '<')
		 && quotef == 0
		 && len <= 2
		 && (*out == '\0' || is_digit(*out))) {
			PARSEREDIR();
			return sh.parse.lasttoken = Token::Redir;
		} else {
			pungetc(sh);
		}
	}
	sh.parse.quoteflag = quotef;
	sh.parse.backquotelist = std::move(bqlist);
	grabstackblock(eb, len);
	sh.parse.wordtext = out;
	return sh.parse.lasttoken = Token::Word;
/* end of readtoken routine */

/*
 * Check to see whether we are at the end of the here document.  When this
 * is called, c is set to the first character of the next input line.  If
 * we are at the end of the here document, this routine sets the c to PEOF.
 */

checkend: {
	if (realeofmark(eofmark)) {
		int markloc;
		const char *ep;
		char *p;

		if (striptabs) {
			while (c == '\t')
				c = pgetc(sh);
		}

		markloc = out - eb.base();
		for (ep = eofmark->c_str(); ((out) = eb.putc(out, c)), *ep; ep++) {
			if (c != (signed char)*ep)
				goto more_heredoc;

			c = pgetc(sh);
		}

		if (c == '\n' || c == PEOF) {
			c = PEOF;
			nlnoprompt(sh);
		} else {
			int len;

more_heredoc:
			p = eb.base() + markloc + 1;
			len = out - p;

			if (len) {
				len -= c <= PEOF;
				c = p[-1];

				if (len)
					pushstring(sh, std::string(p, static_cast<size_t>(len)));
			}
		}

		((out) += (eb.base() + markloc - out));
	}
	goto checkend_return;
}

/*
 * Parse a redirection operator.  The variable "out" points to a string
 * specifying the fd to be redirected.  The variable "c" contains the
 * first character of the redirection operator.
 */

parseredir: {
	char fd = *out;
	Node::ptr np;
	Node::Type redir_type;
	int default_fd;
	bool striptabs = false;

	if (c == '>') {
		default_fd = 1;
		c = pgetc_eatbnl(sh);
		if (c == '>')
			redir_type = Node::Type::Append;
		else if (c == '|')
			redir_type = Node::Type::Clobber;
		else if (c == '&')
			redir_type = Node::Type::ToFd;
		else {
			redir_type = Node::Type::To;
			pungetc(sh);
		}
	} else {	/* c == '<' */
		default_fd = 0;
		switch (c = pgetc_eatbnl(sh)) {
		case '<':
			redir_type = Node::Type::Here;
			if ((c = pgetc_eatbnl(sh)) == '-') {
				striptabs = true;
			} else {
				pungetc(sh);
			}
			break;

		case '&':
			redir_type = Node::Type::FromFd;
			break;

		case '>':
			redir_type = Node::Type::FromTo;
			break;

		default:
			redir_type = Node::Type::From;
			pungetc(sh);
			break;
		}
	}
	if (redir_type == Node::Type::Here)
		np = make_node<HereNode>();
	else if (redir_type == Node::Type::ToFd || redir_type == Node::Type::FromFd)
		np = make_node<DupNode>();
	else
		np = make_node<FileNode>();
	np->type = redir_type;
	as<RedirList>(np)->fd = fd != '\0' ? digit_val(fd) : default_fd;
	if (redir_type == Node::Type::Here) {
		HereDoc pending{};
		pending.here = np.get();
		pending.striptabs = striptabs;
		sh.parse.pending_here = std::move(pending);
	}
	sh.parse.redirnode = std::move(np);
	goto parseredir_return;
}

/*
 * Parse a substitution.  At this point, we have read the dollar sign
 * and nothing else.
 */

parsesub: {
	newsyn = synstack->syntax;
	static const char types[] = "}-+?=";
	int subtype;
	const char *p;

	(*(out)++ = ('$'));

	c = pgetc_eatbnl(sh);
	if (c == '(') {		/* $(command) or $((arith)) */
		(*(out)++ = (c));
		if (pgetc_eatbnl(sh) == '(') {
			PARSEARITH();
		} else {
			pungetc(sh);
			PARSEBACKQNEW();
		}
	} else if (c == '\'' && newsyn['&']) {
		((out) += (-1));
		dollarsq = '\\';
		goto csquote;
	} else if (c == '{' || is_name(c) || is_special(c)) {
		int typeloc = out - eb.base();

		((out) += (!chkeofmark));
		subtype = VSNORMAL;
		if (likely(c == '{')) {
			if (chkeofmark)
				(*(out)++ = ('{'));
			c = pgetc_eatbnl(sh);
			subtype = 0;
		}
varname:
		if (is_name(c)) {
			do {
				((out) = eb.putc(out, c));
				c = pgetc_eatbnl(sh);
			} while (is_in_name(c));
		} else if (is_digit(c)) {
			do {
				((out) = eb.putc(out, c));
				c = pgetc_eatbnl(sh);
			} while ((subtype <= 0 || subtype >= VSLENGTH) &&
				 is_digit(c));
		} else if (c != '}') {
			int cc = c;

			c = pgetc_eatbnl(sh);

			if (!subtype && cc == '#') {
				subtype = VSLENGTH;

				if (c == '_' || isalnum(c)) {
					if (chkeofmark)
						(*(out)++ = ('#'));
					goto varname;
				}

				cc = c;
				c = pgetc_eatbnl(sh);
				if (cc == '}' || c != '}') {
					pungetc(sh);
					subtype = 0;
					c = cc;
					cc = '#';
				} else if (chkeofmark)
					(*(out)++ = ('#'));
			}

			if (!is_special(cc)) {
				if (subtype == VSLENGTH)
					subtype = 0;
				goto badsub;
			}

			(*(out)++ = (cc));
		} else
			goto badsub;

		if (subtype == 0) {
			int cc = c;

			if (chkeofmark)
				((out) = eb.putc(out, c));

			switch (c) {
			case ':':
				subtype = VSNUL;
				c = pgetc_eatbnl(sh);
				if (chkeofmark)
					((out) = eb.putc(out, c));
				/*FALLTHROUGH*/
			default:
				if (!(p = std::strchr(types, c)))
					break;
				subtype |= p - types + VSNORMAL;
				break;
			case '%':
			case '#':
				subtype = c == '#' ? VSTRIMLEFT :
						     VSTRIMRIGHT;
				c = pgetc_eatbnl(sh);
				if (c == cc) {
					if (chkeofmark)
						((out) = eb.putc(out, c));
					subtype++;
				} else
					pungetc(sh);

				newsyn = BASESYNTAX;
				break;
			}
		} else {
			if (subtype == VSLENGTH && c != '}')
				subtype = 0;
badsub:
			pungetc(sh);
		}

		if (newsyn == ARISYNTAX)
			newsyn = DQSYNTAX;

		if ((newsyn != synstack->syntax || synstack->innerdq) &&
		    subtype != VSNORMAL) {
			synstack_push(sh, &synstack,
				      synstack_acquire(synstack, syn_storage),
				      newsyn);

			synstack->varpushed++;
			synstack->dblquote = newsyn != BASESYNTAX;
		}

		if (subtype != VSNORMAL) {
			synstack->varnest++;
			if (synstack->dblquote)
				synstack->dqvarnest++;
		}
		if (!chkeofmark) {
			char *p = eb.base();

			p[typeloc - 1] = CTLVAR;
			p[typeloc] = subtype | VSBIT;
			((out) = eb.putc(out, '='));
		}
	} else
		pungetc(sh);

	goto parsesub_return;
}

/*
 * Called to parse command substitutions.  Newstyle is set if the command
 * is enclosed inside $(...); nlpp is a pointer to the head of the linked
 * list of commands (passed by reference), and savelen is the number of
 * characters on the top of the stack which must be preserved.
 */

parsebackq: {
	int uninitialized_var(saveprompt);
	std::vector<HereDoc> saveheredoclist;
	size_t savelen;
	Node::ptr n;
	unsigned ml;
	char *pstr;
	char *str;

	if (chkeofmark) {
		synstack_push(sh, &synstack,
			      synstack_acquire(synstack, syn_storage),
			      BASESYNTAX);
		synstack->backq = oldstyle + 1;
		printesc = 1;
		goto parsebackq_out;
	}
	((out) += (oldstyle - 1));
	out[-1] = CTLBACKQ;
	str = eb.base();
	savelen = out - eb.base();
	grabstackblock(eb, savelen);
	out = eb.base();
        if (oldstyle) {
                /* We must read until the closing backquote, giving special
                   treatment to some slashes, and then push the string and
                   reread it as input, interpreting it normally.  */
		bool done = false;
		char *pout = out;
                int pc;

		while (!done) {
			pout = eb.makespace(MB_LEN_MAX + 1, pout);
			if (sh.parse.needprompt) {
				setprompt(sh, 2);
			}
			switch (pc = pgetc_eatbnl(sh)) {
			case '`':
				done = true;
				break;

			case '\\':
                                pc = pgetc(sh);
                                if (pc != '\\' && pc != '`' && pc != '$'
                                    && (!synstack->dblquote || pc != '"'))
                                        (*(pout)++ = ('\\'));
				ml = getmbc(sh, pc, pout, 2);
				pout += ml;
				if (ml)
					continue;
				break;

			case PEOF:
				return synerror(sh, "EOF in backquote substitution");

			case '\n':
				nlnoprompt(sh);
				break;

			default:
				break;
			}
			(*(pout)++ = (pc));
                }
		pout[-1] = 0;
		pstr = (eb.commit_to(pout));
		setinputstring(sh, std::string(pstr));
        }
	saveheredoclist = std::move(sh.parse.heredoclist);

	if (oldstyle) {
		saveprompt = sh.parse.doprompt;
		sh.parse.doprompt = 0;
	}

	TRY_SH_SET(n, list(sh, 2));

	if (oldstyle)
		sh.parse.doprompt = saveprompt;
	else {
		Token rp;
		TRY_SH_SET(rp, readtoken(sh));
		if (rp != Token::Rp)
			return synexpect(sh, Token::Rp);
		setinputstring(sh, std::string_view{});
	}

	TRY_SHV(parseheredoc(sh));
	sh.parse.heredoclist = std::move(saveheredoclist);

	bqlist.push_back(std::move(n));
	/* Start reading from old file again. */
	popfile(sh);

	out = eb.nputs(str, savelen, eb.base());

parsebackq_out:
	if (oldstyle) {
		/* Ignore any pushed back tokens left from the backquote
		 * parsing.
		 */
		sh.parse.tokpushback = 0;
		goto parsebackq_oldreturn;
	} else {
		goto parsebackq_newreturn;
	}
}

/*
 * Parse an arithmetic expansion (indicate start of one and set state)
 */
parsearith: {

	synstack_push(sh, &synstack,
		      synstack_acquire(synstack, syn_storage),
		      ARISYNTAX);
	synstack->dblquote = 1;
	if (chkeofmark)
		(*(out)++ = (c));
	else {
		((out) += (-1));
		out[-1] = CTLARI;
	}
	goto parsearith_return;
}

}
} // namespace
 /* end of readtoken */

/*
 * Return of a legal variable name (a letter or underscore followed by zero or
 * more letters, underscores, and digits).
 */

std::size_t
endofname(std::string_view name)
{
	if (name.empty() || !is_name(name[0]))
		return 0;
	std::size_t i = 1;
	while (i < name.size() && is_in_name(name[i]))
		++i;
	return i;
}

/*
 * Called when an unexpected token is read during the parse.  The argument
 * is the token that is expected, or -1 if more than one type of token can
 * occur at this point.
 */

namespace {
tl::unexpected<Error>
synexpect(Context &sh, std::optional<Token> token)
{
	std::string msg;

	if (token) {
		msg = std::format("{} unexpected (expecting {})",
			token_name(sh.parse.lasttoken), token_name(*token));
	} else {
		msg = std::format("{} unexpected", token_name(sh.parse.lasttoken));
	}
	return synerror(sh, msg.c_str());
}

tl::unexpected<Error>
synerror(Context &sh, const char *msg)
{
	sh.errlinno = plinno;
	sh.parse.errmsg = std::format("Syntax error: {}", msg);
	return tl::unexpected(Error::Parse);
}
} // namespace

void
report_parse_error(Context &sh)
{
	sh.exitstatus = 2;
	sh_warnx(sh, "{}", sh.parse.errmsg);
}

Result<void>
try_parse_string(Context &sh, std::string_view s)
{
	char save_nflag = sh.opts.noexec();

	setinputstring(sh, s);
	sh.opts.noexec() = 1;

	for (;;) {
		auto result = parsecmd(sh, 0);
		if (!result) {
			popfile(sh);
			sh.opts.noexec() = save_nflag;
			return shell_fail_of(result);
		}
		if (result->eof)
			break;
	}
	popfile(sh);
	sh.opts.noexec() = save_nflag;
	return {};
}

namespace {
void __attribute__((noinline))
setprompt(Context &sh, int which)
{
	sh.parse.needprompt = 0;
	sh.parse.whichprompt = which;

	if (!sh.input.cur().nleft())
		out2str(getprompt(sh, nullptr));
}
} // namespace

std::string
expandstr(Context &sh, const char *ps)
{
	std::size_t file_stop;
	std::vector<HereDoc> saveheredoclist;
	std::string result = ps;
	int saveprompt;
	struct ArgNode n{};
	int err = 0;

	file_stop = sh.input.index();

	setinputstring(sh, std::string_view(result));

	saveheredoclist = std::move(sh.parse.heredoclist);
	saveprompt = sh.parse.doprompt;
	sh.parse.doprompt = 0;
	sh.parse.needprompt = 0;
	if (auto tok = readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, FAKEEOFMARK, 0); !tok) {
		err = 1;
	} else {
		n.type = Node::Type::Arg;
		n.text = sh.parse.wordtext;
		n.backquote = std::move(sh.parse.backquotelist);

		if (auto r = expandarg_to_string(sh, reinterpret_cast<Node *>(&n), EXP_QUOTED); !r) {
			sh.exception = r.error();
			err = 1;
		} else {
			result = std::move(*r);
		}
	}

	restore_handler_expandarg(sh, err);

	sh.parse.doprompt = saveprompt;
	unwindfiles(sh, file_stop);
	sh.parse.heredoclist = std::move(saveheredoclist);

	return result;
}

/*
 * called by editline -- any expansions to the prompt
 *    should be added here.
 */
std::string
getprompt(Context &sh, void *unused)
{
	const char *prompt;

	switch (sh.parse.whichprompt) {
	default:
	case 0:
		return {};
	case 1:
		prompt = ps1val(sh);
		break;
	case 2:
		prompt = ps2val(sh);
		break;
	}

	return expandstr(sh, prompt);
}

std::optional<Token>
findkwd(std::string_view s)
{
	const auto it = keyword_map.find(s);

	if (it != std::end(keyword_map)) {
		return it->second;
	}

	return std::nullopt;
}

} // namespace shell
