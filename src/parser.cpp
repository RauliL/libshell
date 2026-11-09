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

#if HAVE_ALLOCA_H
#include <alloca.h>
#endif

#include <format>
#include <climits>
#include <optional>
#include <type_traits>
#include <cstdlib>
#include <string>
#include <wchar.h>
#include <wctype.h>

#include "alias.h"
#include "builtins.h"
#include "error.h"
#include "exec.h"
#include "expand.h"	/* defines rmescapes() */
#include "expand_buf.h"
#include "input.h"
#include "jobs.h"
#include "memalloc.h"
#include "mystring.h"
#include "nodes.h"
#include "options.h"
#include "output.h"
#include "parse_error.h"
#include "parser.h"
#include "shell.h"
#include "shell_context.h"
#include "syntax.h"
#include "system.h"
#include "var.h"


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


Node::ptr list(Context &sh, int nlflag, bool *eof_out = nullptr);
Node::ptr andor(Context &sh);
Node::ptr pipeline(Context &sh);
} // namespace

Node::ptr command(Context &sh);
namespace {
Node::ptr simplecmd(Context &sh);
Node::ptr makename(Context &sh);
void parsefname(Context &sh);
void parseheredoc(Context &sh);
Token readtoken(Context &sh);
Token xxreadtoken(Context &sh);
int pgetc_eatbnl(Context &sh);
Token readtoken1(Context &sh, int, char const *, const std::string *, bool);
void synexpect(Context &sh, std::optional<Token> = std::nullopt) __attribute__((__noreturn__));
void synerror(Context &sh, const char *) __attribute__((__noreturn__));
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
	return n && n->type == Node::Type::Cmd && as<CommandNode>(n)->args &&
	       as<ArgNode>(as<CommandNode>(n)->args.get())->text == name;
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

ParseCmdResult
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
	Node::ptr tree = list(sh, 1, &eof);
	return {eof, std::move(tree)};
}


namespace {
Node::ptr
list(Context &sh, int nlflag, bool *eof_out)
{
	int chknl = nlflag & 1 ? 0 : CHKNL;
	Node::ptr n1, n2, n3;
	Token tok;

	for (;;) {
		sh.parse.checkkwd = chknl | CHKKWD | CHKALIAS;
		tok = readtoken(sh);
		switch (tok) {
		case Token::Nl:
			parseheredoc(sh);
			return n1;

		case Token::Eof:
			if (!n1 && !chknl && eof_out)
				*eof_out = true;
out_eof:
			parseheredoc(sh);
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

		n2 = andor(sh);
		tok = readtoken(sh);
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
				synexpect(sh);
			sh.parse.tokpushback++;
			return n1;
		}
	}
}



Node::ptr
andor(Context &sh)
{
	Node::ptr n1, n2, n3;

	n1 = pipeline(sh);
	for (;;) {
		const auto token = readtoken(sh);
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
		n2 = pipeline(sh);
		n3 = make_node<BinaryNode>();
		n3->type = t;
		as<BinaryNode>(n3)->ch1 = std::move(n1);
		as<BinaryNode>(n3)->ch2 = std::move(n2);
		n1 = std::move(n3);
	}
}



Node::ptr
pipeline(Context &sh)
{
	Node::ptr n1, n2, pipenode;
	bool negate = false;

	if (readtoken(sh) == Token::Not) {
		negate = !negate;
		sh.parse.checkkwd = CHKKWD | CHKALIAS;
	} else
		sh.parse.tokpushback++;
	n1 = command(sh);
	if (readtoken(sh) == Token::Pipe) {
		pipenode = make_node<PipeNode>();
		pipenode->type = Node::Type::Pipe;
		as<PipeNode>(pipenode)->backgnd = 0;
		as<PipeNode>(pipenode)->cmdlist.push_back(std::move(n1));
		do {
			sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
			as<PipeNode>(pipenode)->cmdlist.push_back(command(sh));
		} while (readtoken(sh) == Token::Pipe);
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




Node::ptr
command(Context &sh)
{
	Node::ptr n1, n2;
	Node::ptr ap;
	Node::ptr *app;
	Node::ptr *cpp;
	Node::ptr redir;
	Node::ptr *rpp;
	Node::ptr *rpp2;
	Token t;
	int savelinno;

	rpp2 = &redir;

	savelinno = plinno;

	switch (readtoken(sh)) {
	default:
		synexpect(sh);
		/* NOTREACHED */
	case Token::If: {
		n1 = make_node<IfNode>();
		n1->type = Node::Type::If;
		as<IfNode>(n1)->test = list(sh, 0);
		if (readtoken(sh) != Token::Then)
			synexpect(sh, Token::Then);
		as<IfNode>(n1)->ifpart = list(sh, 0);
		Node *nwalk = n1.get();
		while (readtoken(sh) == Token::Elif) {
			as<IfNode>(nwalk)->elsepart = make_node<IfNode>();
			nwalk = as<IfNode>(nwalk)->elsepart.get();
			nwalk->type = Node::Type::If;
			as<IfNode>(nwalk)->test = list(sh, 0);
			if (readtoken(sh) != Token::Then)
				synexpect(sh, Token::Then);
			as<IfNode>(nwalk)->ifpart = list(sh, 0);
		}
		if (sh.parse.lasttoken == Token::Else)
			as<IfNode>(nwalk)->elsepart = list(sh, 0);
		else {
			sh.parse.tokpushback++;
		}
		t = Token::Fi;
		break;
	}
	case Token::While:
	case Token::Until: {
		Token got;
		n1 = make_node<BinaryNode>();
		n1->type = (sh.parse.lasttoken == Token::While)? Node::Type::While : Node::Type::Until;
		as<BinaryNode>(n1)->ch1 = list(sh, 0);
		if ((got=readtoken(sh)) != Token::Do) {
			synexpect(sh, Token::Do);
		}
		as<BinaryNode>(n1)->ch2 = list(sh, 0);
		t = Token::Done;
		break;
	}
	case Token::For:
		if (readtoken(sh) != Token::Word || sh.parse.quoteflag ||
		    endofname(sh.parse.wordtext) != sh.parse.wordtext.size())
			synerror(sh, "Bad for loop variable");
		n1 = make_node<ForNode>();
		n1->type = Node::Type::For;
		as<ForNode>(n1)->linno = savelinno;
		as<ForNode>(n1)->var = sh.parse.wordtext;
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) == Token::In) {
			app = &ap;
			while (readtoken(sh) == Token::Word) {
				*app = make_node<ArgNode>();
				(*app)->type = Node::Type::Arg;
				as<ArgNode>(*app)->text = sh.parse.wordtext;
				as<ArgNode>(*app)->backquote = std::move(sh.parse.backquotelist);
				app = &as<ArgNode>(*app)->next;
			}
			as<ForNode>(n1)->args = std::move(ap);
			if (sh.parse.lasttoken != Token::Nl && sh.parse.lasttoken != Token::Semi)
				synexpect(sh);
		} else {
			n2 = make_node<ArgNode>();
			n2->type = Node::Type::Arg;
			as<ArgNode>(n2)->text = dolatstr;
			as<ForNode>(n1)->args = std::move(n2);
			/*
			 * Newline or semicolon here is optional (but note
			 * that the original Bourne shell only allowed NL).
			 */
			if (sh.parse.lasttoken != Token::Semi)
				sh.parse.tokpushback++;
		}
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) != Token::Do)
			synexpect(sh, Token::Do);
		as<ForNode>(n1)->body = list(sh, 0);
		t = Token::Done;
		break;
	case Token::Case:
		n1 = make_node<CaseNode>();
		n1->type = Node::Type::Case;
		as<CaseNode>(n1)->linno = savelinno;
		if (readtoken(sh) != Token::Word)
			synexpect(sh, Token::Word);
		n2 = make_node<ArgNode>();
		n2->type = Node::Type::Arg;
		as<ArgNode>(n2)->text = sh.parse.wordtext;
		as<ArgNode>(n2)->backquote = std::move(sh.parse.backquotelist);
		as<CaseNode>(n1)->expr = std::move(n2);
		sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) != Token::In)
			synexpect(sh, Token::In);
		cpp = &as<CaseNode>(n1)->cases;
next_case:
		sh.parse.checkkwd = CHKNL | CHKKWD;
		t = readtoken(sh);
		while(t != Token::Esac) {
			if (sh.parse.lasttoken == Token::Lp)
				readtoken(sh);
			*cpp = make_node<CaseListNode>();
			(*cpp)->type = Node::Type::CaseList;
			Node *cp = cpp->get();
			app = &as<CaseListNode>(cp)->pattern;
			for (;;) {
				if (sh.parse.lasttoken < Token::Word)
					synexpect(sh, Token::Word);
				*app = make_node<ArgNode>();
				(*app)->type = Node::Type::Arg;
				as<ArgNode>(*app)->text = sh.parse.wordtext;
				as<ArgNode>(*app)->backquote = std::move(sh.parse.backquotelist);
				if (readtoken(sh) != Token::Pipe)
					break;
				app = &as<ArgNode>(*app)->next;
				readtoken(sh);
			}
			if (sh.parse.lasttoken != Token::Rp)
				synexpect(sh, Token::Rp);
			as<CaseListNode>(cp)->body = list(sh, 2);

			cpp = &as<CaseListNode>(cp)->next;

			sh.parse.checkkwd = CHKNL | CHKKWD;
			if ((t = readtoken(sh)) != Token::Esac) {
				if (t != Token::Endcase)
					synexpect(sh, Token::Endcase);
				else
					goto next_case;
			}
		}
		goto redir;
	case Token::Lp:
		n1 = make_node<RedirNode>();
		n1->type = Node::Type::Subshell;
		as<RedirNode>(n1)->linno = savelinno;
		as<RedirNode>(n1)->n = list(sh, 0);
		t = Token::Rp;
		break;
	case Token::Begin:
		n1 = list(sh, 0);
		t = Token::End;
		break;
	case Token::Word:
	case Token::Redir:
		sh.parse.tokpushback++;
		return simplecmd(sh);
	}

	if (readtoken(sh) != t)
		synexpect(sh, t);

redir:
	/* Now check for redirection which may follow command */
	sh.parse.checkkwd = CHKKWD | CHKALIAS;
	rpp = rpp2;
	while (readtoken(sh) == Token::Redir) {
		parsefname(sh);
		*rpp = std::move(sh.parse.redirnode);
		rpp = &as<RedirList>(*rpp)->next;
	}
	sh.parse.tokpushback++;
	if (redir) {
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
Node::ptr
simplecmd(Context &sh)
{
	Node::ptr args;
	Node::ptr *app = &args;
	Node::ptr n;
	Node::ptr vars;
	Node::ptr *vpp = &vars;
	Node::ptr redir;
	Node::ptr *rpp = &redir;
	int savecheckkwd;
	int savelinno;

	savecheckkwd = CHKALIAS;
	savelinno = plinno;
	for (;;) {
		sh.parse.checkkwd = savecheckkwd;
		switch (readtoken(sh)) {
		case Token::Word:
			n = make_node<ArgNode>();
			n->type = Node::Type::Arg;
			as<ArgNode>(n)->text = sh.parse.wordtext;
			as<ArgNode>(n)->backquote = std::move(sh.parse.backquotelist);
			if (savecheckkwd && isassignment(sh.parse.wordtext)) {
				*vpp = std::move(n);
				vpp = &as<ArgNode>(*vpp)->next;
			} else {
				*app = std::move(n);
				app = &as<ArgNode>(*app)->next;
				savecheckkwd = 0;
			}
			break;
		case Token::Redir:
			parsefname(sh);	/* read name of redirection file */
			*rpp = std::move(sh.parse.redirnode);
			rpp = &as<RedirList>(*rpp)->next;
			break;
		case Token::Lp:
			if (
				args && app == &as<ArgNode>(args)->next &&
				!vars && !redir
			) {
				const BuiltinCommand *bcmd;
				std::string fname = std::move(as<ArgNode>(args)->text);

				/* We have a function */
				if (readtoken(sh) != Token::Rp)
					synexpect(sh, Token::Rp);
				if (
					endofname(fname) != fname.size() || (
						(bcmd = find_builtin(fname)) &&
						bcmd->flags & BuiltinCommand::Flag::Special
					)
				)
					synerror(sh, "Bad function name");
				args.reset();
				n = make_node<DefunNode>();
				n->type = Node::Type::Defun;
				sh.parse.checkkwd = CHKNL | CHKKWD | CHKALIAS;
				as<DefunNode>(n)->text = std::move(fname);
				as<DefunNode>(n)->linno = plinno;
				as<DefunNode>(n)->body = command(sh);
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


void
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
			throw ParseError(std::format("Bad fd number: {}", text), plinno);

		as<DupNode>(n)->vname = makename(sh);
	}
}


namespace {
void
parsefname(Context &sh)
{
	auto n = sh.parse.redirnode.get();

	if (n->type == Node::Type::Here)
		sh.parse.checkkwd |= CHKEOFMARK;
	if (readtoken(sh) != Token::Word)
		synexpect(sh);
	sh.parse.checkkwd &= ~CHKEOFMARK;
	if (n->type == Node::Type::Here) {
		if (sh.parse.quoteflag == 0)
			n->type = Node::Type::XHere;
		rmescapes(sh.parse.wordtext.data());
		sh.parse.pending_here->eofmark = sh.parse.wordtext;
		sh.parse.heredoclist.push_back(
			std::move(*sh.parse.pending_here));
		sh.parse.pending_here.reset();
	} else if (n->type == Node::Type::ToFd || n->type == Node::Type::FromFd) {
		fixredir(sh, n, sh.parse.wordtext.c_str(), 0);
	} else {
		as<FileNode>(n)->fname = makename(sh);
	}
}
} // namespace



/*
 * Input any here documents.
 */

namespace {
void
parseheredoc(Context &sh)
{
	auto docs = std::move(sh.parse.heredoclist);

	for (auto &here : docs) {
		if (sh.parse.needprompt) {
			setprompt(sh, 2);
		}
		if (here.here->type == Node::Type::Here)
			readtoken1(sh, pgetc(sh), SQSYNTAX, &here.eofmark, here.striptabs);
		else
			readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, &here.eofmark, here.striptabs);
		Node::ptr n = make_node<ArgNode>();
		n->type = Node::Type::Arg;
		as<ArgNode>(n)->text = sh.parse.wordtext;
		as<ArgNode>(n)->backquote = std::move(sh.parse.backquotelist);
		as<HereNode>(here.here)->doc = std::move(n);
	}
}

Token
readtoken(Context &sh)
{
	Token t;
	int kwd = sh.parse.checkkwd;

top:
	t = xxreadtoken(sh);

	/*
	 * eat newlines
	 */
	if (kwd & CHKNL) {
		while (t == Token::Nl) {
			parseheredoc(sh);
			sh.parse.checkkwd = 0;
			t = xxreadtoken(sh);
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
			if (!ap->value.empty()) {
				char *text = ap->value.data();

				pushstring(sh, text, std::move(ap));
			}
			goto top;
		}
	}

out:
	return (t);
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
Token
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
		tok = readtoken1(sh, c, BASESYNTAX, nullptr, 0);
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
	while ((ml2 = mbrtowc(&wc, mbc + ml++, 1, &mbst)) == -2) {
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
Token
readtoken1(Context &sh, int firstc, char const *syntax, const std::string *eofmark, bool striptabs)
{
	ExpandBuf eb;
	struct synstack synbase = {
		.syntax = syntax,
		.dblquote = syntax == DQSYNTAX,
	};
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
			({ char *_q = (out); size_t _l = ((MB_LEN_MAX > 16 ? MB_LEN_MAX : 16) + 7); size_t _m = static_cast<size_t>(eb.cap_ptr() - _q); if (_l > _m) (out) = eb.makespace(_l, _q); 0; });
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
		synerror(sh, "Missing '))'");
	if ((synstack->syntax != BASESYNTAX && !eofmark) ||
	    synstack->backq)
		synerror(sh, "Unterminated quoted string");
	if (synstack->varnest != 0) {
		/* { */
		synerror(sh, "Missing '}'");
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

				if (len) {
					char *str;

					str = static_cast<char *>(alloca(len + 1));
					*static_cast<char *>(mempcpy(str, p, len)) = 0;

					pushstring(sh, str, nullptr);
				}
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
				      synstack->prev ?:
				      static_cast<struct synstack *>(alloca(sizeof(*synstack))),
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
			      synstack->prev ?: static_cast<struct synstack *>(alloca(sizeof(*synstack))),
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
			({ char *_q = (pout); size_t _l = (MB_LEN_MAX + 1); size_t _m = static_cast<size_t>(eb.cap_ptr() - _q); if (_l > _m) (pout) = eb.makespace(_l, _q); 0; });
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
				synerror(sh, "EOF in backquote substitution");

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
		setinputstring(sh, pstr);
        }
	saveheredoclist = std::move(sh.parse.heredoclist);

	if (oldstyle) {
		saveprompt = sh.parse.doprompt;
		sh.parse.doprompt = 0;
	}

	n = list(sh, 2);

	if (oldstyle)
		sh.parse.doprompt = saveprompt;
	else {
		if (readtoken(sh) != Token::Rp)
			synexpect(sh, Token::Rp);
		setinputstring(sh, nullstr);
	}

	parseheredoc(sh);
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
		      synstack->prev ?: static_cast<struct synstack *>(alloca(sizeof(*synstack))),
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
void
synexpect(Context &sh, std::optional<Token> token)
{
	std::string msg;

	if (token) {
		msg = std::format("{} unexpected (expecting {})",
			token_name(sh.parse.lasttoken), token_name(*token));
	} else {
		msg = std::format("{} unexpected", token_name(sh.parse.lasttoken));
	}
	synerror(sh, msg.c_str());
	/* NOTREACHED */
}


void
synerror(Context &sh, const char *msg)
{
	sh.errlinno = plinno;

	throw ParseError(std::format("Syntax error: {}", msg), plinno);
}
} // namespace


void
report_parse_error(Context &sh, const ParseError &e)
{
	sh.errlinno = e.lineno();
	sh.exitstatus = 2;
	sh_warnx(sh, "{}", e.what());
}

tl::expected<void, std::string>
try_parse_string(Context &sh, std::string_view s)
{
	char save_nflag = sh.opts.noexec();
	std::string copy(s);

	setinputstring(sh, copy.data());
	sh.opts.noexec() = 1;

	try {
		for (;;) {
			ParseCmdResult result = parsecmd(sh, 0);
			if (result.eof)
				break;
		}
		popfile(sh);
		sh.opts.noexec() = save_nflag;
		return {};
	} catch (const ParseError &e) {
		sh.errlinno = e.lineno();
		popfile(sh);
		sh.opts.noexec() = save_nflag;

		return tl::unexpected(std::string(e.what()));
	}
}

namespace {
void __attribute__((noinline))
setprompt(Context &sh, int which)
{
	sh.parse.needprompt = 0;
	sh.parse.whichprompt = which;

	if (!sh.input.current->nleft)
		out2str(getprompt(sh, nullptr));
}
} // namespace


std::string
expandstr(Context &sh, const char *ps)
{
	ParseFile *file_stop;
	std::vector<HereDoc> saveheredoclist;
	std::string result = ps;
	int saveprompt;
	struct ArgNode n{};
	int err = 0;

	file_stop = sh.input.current;

	setinputstring(sh, result.data());

	saveheredoclist = std::move(sh.parse.heredoclist);
	saveprompt = sh.parse.doprompt;
	sh.parse.doprompt = 0;
	sh.parse.needprompt = 0;
	try {
		readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, FAKEEOFMARK, 0);

		n.type = Node::Type::Arg;
		n.next = nullptr;
		n.text = sh.parse.wordtext;
		n.backquote = std::move(sh.parse.backquotelist);

		if (auto r = expandarg_to_string(sh, reinterpret_cast<Node *>(&n), EXP_QUOTED); !r) {
			sh.exception = r.error();
			err = 1;
		} else {
			result = std::move(*r);
		}
	} catch (const ParseError &) {
		err = 1;
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
