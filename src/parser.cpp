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

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string>
#include <wchar.h>
#include <wctype.h>

#include "shell.h"
#include "shell_context.h"
#include "parser.h"
#include "nodes.h"
#include "expand.h"	/* defines rmescapes() */
#include "exec.h"	/* defines find_builtin() */
#include "syntax.h"
#include "options.h"
#include "input.h"
#include "jobs.h"
#include "output.h"
#include "var.h"
#include "error.h"
#include "memalloc.h"
#include "mystring.h"
#include "alias.h"
#include "show.h"
#include "builtins.h"
#include "system.h"
#include "parse_error.h"
#include "result.h"

/*
 * Shell command parser.
 */

/* values returned by readtoken */
#include "token_vars.h"



/* Used by expandstr to get here-doc like behaviour (address only). */
static const std::string fake_eofmark;
#define FAKEEOFMARK (&fake_eofmark)



struct heredoc {
	struct heredoc *next = nullptr;	/* next here document in list */
	Node *here = nullptr;	/* redirection Node */
	std::string eofmark;		/* string indicating end of input */
	int striptabs = 0;		/* if set, strip leading tabs */
};

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



	/* list of here documents to read */
			/* if set, prompt the user */
			/* true if interactive and at start of line */
			/* last token read */
		/* last token pushed back */
			/* text of last word returned by readtoken */




			/* set if (part of) last token was quoted */


static Node::ptr list(ShellContext &sh, int nlflag, bool *eof_out = nullptr);
static Node::ptr andor(ShellContext &sh);
static Node::ptr pipeline(ShellContext &sh);
Node::ptr command(ShellContext &sh);
static Node::ptr simplecmd(ShellContext &sh);
static Node::ptr makename(ShellContext &sh);
static void parsefname(ShellContext &sh);
static void parseheredoc(ShellContext &sh);
static int readtoken(ShellContext &sh);
static int xxreadtoken(ShellContext &sh);
static int pgetc_eatbnl(ShellContext &sh);
static int readtoken1(ShellContext &sh, int, char const *, const std::string *, int);
static void synexpect(ShellContext &sh, int) __attribute__((__noreturn__));
static void synerror(ShellContext &sh, const char *) __attribute__((__noreturn__));
static void setprompt(ShellContext &sh, int);


template <
	typename T,
	typename... Args,
	typename = typename std::enable_if<std::is_base_of<Node, T>::value>::type
>
static inline Node::ptr
make_node(Args &&...args)
{
	return std::make_unique<T>(std::forward<Args>(args)...);
}


bool
isassignment(std::string_view p)
{
	auto n = endofname(p);

	if (n == 0)
		return false;

	return n < p.size() && p[n] == '=';
}

bool
issimplecmd(Node *n, std::string_view name)
{
	return n && n->type == Node::Type::Cmd && as<Node::Command>(n)->args &&
	       as<Node::Arg>(as<Node::Command>(n)->args.get())->text == name;
}

static inline bool
realeofmark(const std::string *eofmark)
{
	return eofmark && eofmark != FAKEEOFMARK;
}

static void
grabstackblock(ShellContext &sh, std::size_t len)
{
	stackstr_commit(sh, len);
}


/*
 * Read and parse a command.  eof is set on end of file.  (A null tree is a
 * valid parse tree indicating a blank line.)
 */

ParseCmdResult
parsecmd(ShellContext &sh, int interact)
{
	sh.tokpushback = 0;
	sh.checkkwd = 0;
	sh.heredoclist = 0;
	sh.doprompt = interact;
	if (sh.doprompt)
		setprompt(sh, sh.doprompt);
	sh.needprompt = 0;
	bool eof = false;
	Node::ptr tree = list(sh, 1, &eof);
	return {eof, std::move(tree)};
}


static Node::ptr
list(ShellContext &sh, int nlflag, bool *eof_out)
{
	int chknl = nlflag & 1 ? 0 : CHKNL;
	Node::ptr n1, n2, n3;
	int tok;

	for (;;) {
		sh.checkkwd = chknl | CHKKWD | CHKALIAS;
		tok = readtoken(sh);
		switch (tok) {
		case TNL:
			parseheredoc(sh);
			return n1;

		case TEOF:
			if (!n1 && !chknl && eof_out)
				*eof_out = true;
out_eof:
			parseheredoc(sh);
			sh.tokpushback++;
			sh.lasttoken = TEOF;
			return n1;
		}

		sh.tokpushback++;
		if (nlflag == 2 && tokendlist[tok])
			return n1;
		nlflag |= 2;

		n2 = andor(sh);
		tok = readtoken(sh);
		if (tok == TBACKGND) {
			if (n2->type == Node::Type::Pipe) {
				as<Node::Pipe>(n2)->backgnd = 1;
			} else {
				if (n2->type != Node::Type::Redir) {
					n3 = make_node<Node::Redir>();
					as<Node::Redir>(n3)->n = std::move(n2);
					n2 = std::move(n3);
				}
				n2->type = Node::Type::Backgnd;
			}
		}
		if (!n1) {
			n1 = std::move(n2);
		}
		else {
			n3 = make_node<Node::Binary>();
			n3->type = Node::Type::Semi;
			as<Node::Binary>(n3)->ch1 = std::move(n1);
			as<Node::Binary>(n3)->ch2 = std::move(n2);
			n1 = std::move(n3);
		}
		switch (tok) {
		case TEOF:
			goto out_eof;
		case TNL:
			sh.tokpushback++;
			/* fall through */
		case TBACKGND:
		case TSEMI:
			break;
		default:
			if (!chknl)
				synexpect(sh, -1);
			sh.tokpushback++;
			return n1;
		}
	}
}



static Node::ptr
andor(ShellContext &sh)
{
	Node::ptr n1, n2, n3;

	n1 = pipeline(sh);
	for (;;) {
		const auto token = readtoken(sh);
		Node::Type t;

		if (token == TAND) {
			t = Node::Type::And;
		} else if (token == TOR) {
			t = Node::Type::Or;
		} else {
			sh.tokpushback++;
			return n1;
		}
		sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		n2 = pipeline(sh);
		n3 = make_node<Node::Binary>();
		n3->type = t;
		as<Node::Binary>(n3)->ch1 = std::move(n1);
		as<Node::Binary>(n3)->ch2 = std::move(n2);
		n1 = std::move(n3);
	}
}



static Node::ptr
pipeline(ShellContext &sh)
{
	Node::ptr n1, n2, pipenode;
	int negate;

	negate = 0;
	TRACE(("pipeline: entered\n"));
	if (readtoken(sh) == TNOT) {
		negate = !negate;
		sh.checkkwd = CHKKWD | CHKALIAS;
	} else
		sh.tokpushback++;
	n1 = command(sh);
	if (readtoken(sh) == TPIPE) {
		pipenode = make_node<Node::Pipe>();
		pipenode->type = Node::Type::Pipe;
		as<Node::Pipe>(pipenode)->backgnd = 0;
		as<Node::Pipe>(pipenode)->cmdlist.push_back(std::move(n1));
		do {
			sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
			as<Node::Pipe>(pipenode)->cmdlist.push_back(command(sh));
		} while (readtoken(sh) == TPIPE);
		n1 = std::move(pipenode);
	}
	sh.tokpushback++;
	if (negate) {
		n2 = make_node<Node::Not>();
		n2->type = Node::Type::Not;
		as<Node::Not>(n2)->com = std::move(n1);
		return n2;
	} else
		return n1;
}



Node::ptr
command(ShellContext &sh)
{
	Node::ptr n1, n2;
	Node::ptr ap;
	Node::ptr *app;
	Node::ptr *cpp;
	Node::ptr redir;
	Node::ptr *rpp;
	Node::ptr *rpp2;
	int t;
	int savelinno;

	rpp2 = &redir;

	savelinno = plinno;

	switch (readtoken(sh)) {
	default:
		synexpect(sh, -1);
		/* NOTREACHED */
	case TIF: {
		n1 = make_node<Node::If>();
		n1->type = Node::Type::If;
		as<Node::If>(n1)->test = list(sh, 0);
		if (readtoken(sh) != TTHEN)
			synexpect(sh, TTHEN);
		as<Node::If>(n1)->ifpart = list(sh, 0);
		Node *nwalk = n1.get();
		while (readtoken(sh) == TELIF) {
			as<Node::If>(nwalk)->elsepart = make_node<Node::If>();
			nwalk = as<Node::If>(nwalk)->elsepart.get();
			nwalk->type = Node::Type::If;
			as<Node::If>(nwalk)->test = list(sh, 0);
			if (readtoken(sh) != TTHEN)
				synexpect(sh, TTHEN);
			as<Node::If>(nwalk)->ifpart = list(sh, 0);
		}
		if (sh.lasttoken == TELSE)
			as<Node::If>(nwalk)->elsepart = list(sh, 0);
		else {
			sh.tokpushback++;
		}
		t = TFI;
		break;
	}
	case TWHILE:
	case TUNTIL: {
		int got;
		n1 = make_node<Node::Binary>();
		n1->type = (sh.lasttoken == TWHILE)? Node::Type::While : Node::Type::Until;
		as<Node::Binary>(n1)->ch1 = list(sh, 0);
		if ((got=readtoken(sh)) != TDO) {
TRACE(("expecting DO got %s %s\n", tokname[got], got == TWORD ? sh.wordtext.c_str() : ""));
			synexpect(sh, TDO);
		}
		as<Node::Binary>(n1)->ch2 = list(sh, 0);
		t = TDONE;
		break;
	}
	case TFOR:
		if (readtoken(sh) != TWORD || sh.quoteflag ||
		    endofname(sh.wordtext) != sh.wordtext.size())
			synerror(sh, "Bad for loop variable");
		n1 = make_node<Node::For>();
		n1->type = Node::Type::For;
		as<Node::For>(n1)->linno = savelinno;
		as<Node::For>(n1)->var = sh.wordtext;
		sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) == TIN) {
			app = &ap;
			while (readtoken(sh) == TWORD) {
				*app = make_node<Node::Arg>();
				(*app)->type = Node::Type::Arg;
				as<Node::Arg>(*app)->text = sh.wordtext;
				as<Node::Arg>(*app)->backquote = std::move(sh.backquotelist);
				app = &as<Node::Arg>(*app)->next;
			}
			as<Node::For>(n1)->args = std::move(ap);
			if (sh.lasttoken != TNL && sh.lasttoken != TSEMI)
				synexpect(sh, -1);
		} else {
			n2 = make_node<Node::Arg>();
			n2->type = Node::Type::Arg;
			as<Node::Arg>(n2)->text = dolatstr;
			as<Node::For>(n1)->args = std::move(n2);
			/*
			 * Newline or semicolon here is optional (but note
			 * that the original Bourne shell only allowed NL).
			 */
			if (sh.lasttoken != TSEMI)
				sh.tokpushback++;
		}
		sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) != TDO)
			synexpect(sh, TDO);
		as<Node::For>(n1)->body = list(sh, 0);
		t = TDONE;
		break;
	case TCASE:
		n1 = make_node<Node::Case>();
		n1->type = Node::Type::Case;
		as<Node::Case>(n1)->linno = savelinno;
		if (readtoken(sh) != TWORD)
			synexpect(sh, TWORD);
		n2 = make_node<Node::Arg>();
		n2->type = Node::Type::Arg;
		as<Node::Arg>(n2)->text = sh.wordtext;
		as<Node::Arg>(n2)->backquote = std::move(sh.backquotelist);
		as<Node::Case>(n1)->expr = std::move(n2);
		sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
		if (readtoken(sh) != TIN)
			synexpect(sh, TIN);
		cpp = &as<Node::Case>(n1)->cases;
next_case:
		sh.checkkwd = CHKNL | CHKKWD;
		t = readtoken(sh);
		while(t != TESAC) {
			if (sh.lasttoken == TLP)
				readtoken(sh);
			*cpp = make_node<Node::CaseList>();
			(*cpp)->type = Node::Type::CaseList;
			Node *cp = cpp->get();
			app = &as<Node::CaseList>(cp)->pattern;
			for (;;) {
				if (sh.lasttoken < TWORD)
					synexpect(sh, TWORD);
				*app = make_node<Node::Arg>();
				(*app)->type = Node::Type::Arg;
				as<Node::Arg>(*app)->text = sh.wordtext;
				as<Node::Arg>(*app)->backquote = std::move(sh.backquotelist);
				if (readtoken(sh) != TPIPE)
					break;
				app = &as<Node::Arg>(*app)->next;
				readtoken(sh);
			}
			if (sh.lasttoken != TRP)
				synexpect(sh, TRP);
			as<Node::CaseList>(cp)->body = list(sh, 2);

			cpp = &as<Node::CaseList>(cp)->next;

			sh.checkkwd = CHKNL | CHKKWD;
			if ((t = readtoken(sh)) != TESAC) {
				if (t != TENDCASE)
					synexpect(sh, TENDCASE);
				else
					goto next_case;
			}
		}
		goto redir;
	case TLP:
		n1 = make_node<Node::Redir>();
		n1->type = Node::Type::Subshell;
		as<Node::Redir>(n1)->linno = savelinno;
		as<Node::Redir>(n1)->n = list(sh, 0);
		t = TRP;
		break;
	case TBEGIN:
		n1 = list(sh, 0);
		t = TEND;
		break;
	case TWORD:
	case TREDIR:
		sh.tokpushback++;
		return simplecmd(sh);
	}

	if (readtoken(sh) != t)
		synexpect(sh, t);

redir:
	/* Now check for redirection which may follow command */
	sh.checkkwd = CHKKWD | CHKALIAS;
	rpp = rpp2;
	while (readtoken(sh) == TREDIR) {
		parsefname(sh);
		*rpp = std::move(sh.redirnode);
		rpp = &as<Node::RedirList>(*rpp)->next;
	}
	sh.tokpushback++;
	if (redir) {
		if (n1->type != Node::Type::Subshell) {
			n2 = make_node<Node::Redir>();
			n2->type = Node::Type::Redir;
			as<Node::Redir>(n2)->linno = savelinno;
			as<Node::Redir>(n2)->n = std::move(n1);
			n1 = std::move(n2);
		}
		as<Node::Redir>(n1)->redirect = std::move(redir);
	}

	return n1;
}


static Node::ptr
simplecmd(ShellContext &sh)
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
		sh.checkkwd = savecheckkwd;
		switch (readtoken(sh)) {
		case TWORD:
			n = make_node<Node::Arg>();
			n->type = Node::Type::Arg;
			as<Node::Arg>(n)->text = sh.wordtext;
			as<Node::Arg>(n)->backquote = std::move(sh.backquotelist);
			if (savecheckkwd && isassignment(sh.wordtext)) {
				*vpp = std::move(n);
				vpp = &as<Node::Arg>(*vpp)->next;
			} else {
				*app = std::move(n);
				app = &as<Node::Arg>(*app)->next;
				savecheckkwd = 0;
			}
			break;
		case TREDIR:
			parsefname(sh);	/* read name of redirection file */
			*rpp = std::move(sh.redirnode);
			rpp = &as<Node::RedirList>(*rpp)->next;
			break;
		case TLP:
			if (
				args && app == &as<Node::Arg>(args)->next &&
				!vars && !redir
			) {
				struct builtincmd *bcmd;
				std::string fname = std::move(as<Node::Arg>(args)->text);

				/* We have a function */
				if (readtoken(sh) != TRP)
					synexpect(sh, TRP);
				if (
					endofname(fname) != fname.size() || (
						(bcmd = find_builtin(sh, fname.c_str())) &&
						bcmd->flags & BUILTIN_SPECIAL
					)
				)
					synerror(sh, "Bad function name");
				args.reset();
				n = make_node<Node::Defun>();
				n->type = Node::Type::Defun;
				sh.checkkwd = CHKNL | CHKKWD | CHKALIAS;
				as<Node::Defun>(n)->text = std::move(fname);
				as<Node::Defun>(n)->linno = plinno;
				as<Node::Defun>(n)->body = command(sh);
				return n;
			}
			/* fall through */
		default:
			sh.tokpushback++;
			goto out;
		}
	}
out:
	n = make_node<Node::Command>();
	n->type = Node::Type::Cmd;
	as<Node::Command>(n)->linno = savelinno;
	as<Node::Command>(n)->args = std::move(args);
	as<Node::Command>(n)->assign = std::move(vars);
	as<Node::Command>(n)->redirect = std::move(redir);
	return n;
}

static Node::ptr
makename(ShellContext &sh)
{
	Node::ptr n = make_node<Node::Arg>();

	n->type = Node::Type::Arg;
	as<Node::Arg>(n)->text = sh.wordtext;
	as<Node::Arg>(n)->backquote = std::move(sh.backquotelist);

	return n;
}

void
fixredir(ShellContext &sh, Node *n, const char *text, int err)
{
	TRACE(("Fix redir %s %d\n", text, err));
	if (!err)
		as<Node::Dup>(n)->vname.reset();

	if (is_digit(text[0]) && text[1] == '\0')
		as<Node::Dup>(n)->dupfd = digit_val(text[0]);
	else if (text[0] == '-' && text[1] == '\0')
		as<Node::Dup>(n)->dupfd = -1;
	else {

		if (err)
			throw dash::ParseError(
				std::string("Bad fd number: ") + text, plinno);
		else
			as<Node::Dup>(n)->vname = makename(sh);
	}
}


static void
parsefname(ShellContext &sh)
{
	auto n = sh.redirnode.get();

	if (n->type == Node::Type::Here)
		sh.checkkwd |= CHKEOFMARK;
	if (readtoken(sh) != TWORD)
		synexpect(sh, -1);
	sh.checkkwd &= ~CHKEOFMARK;
	if (n->type == Node::Type::Here) {
		struct heredoc *here = sh.heredoc;
		struct heredoc *p;

		if (sh.quoteflag == 0)
			n->type = Node::Type::XHere;
		TRACE(("Here document %d\n", n->type));
		rmescapes(sh.wordtext.data());
		here->eofmark = sh.wordtext;
		here->next = NULL;
		if (sh.heredoclist == NULL)
			sh.heredoclist = here;
		else {
			for (p = sh.heredoclist ; p->next ; p = p->next);
			p->next = here;
		}
	} else if (n->type == Node::Type::ToFd || n->type == Node::Type::FromFd) {
		fixredir(sh, n, sh.wordtext.c_str(), 0);
	} else {
		as<Node::File>(n)->fname = makename(sh);
	}
}


/*
 * Input any here documents.
 */

static void
parseheredoc(ShellContext &sh)
{
	struct heredoc *here;

	here = sh.heredoclist;
	sh.heredoclist = 0;

	while (here) {
		struct heredoc *next = here->next;
		if (sh.needprompt) {
			setprompt(sh, 2);
		}
		if (here->here->type == Node::Type::Here)
			readtoken1(sh, pgetc(sh), SQSYNTAX, &here->eofmark, here->striptabs);
		else
			readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, &here->eofmark, here->striptabs);
		Node::ptr n = make_node<Node::Arg>();
		n->type = Node::Type::Arg;
		as<Node::Arg>(n)->text = sh.wordtext;
		as<Node::Arg>(n)->backquote = std::move(sh.backquotelist);
		as<Node::Here>(here->here)->doc = std::move(n);
		delete here;
		here = next;
	}
}

static int
readtoken(ShellContext &sh)
{
	int t;
	int kwd = sh.checkkwd;
#ifdef DEBUG
	int alreadyseen = sh.tokpushback;
#endif

top:
	t = xxreadtoken(sh);

	/*
	 * eat newlines
	 */
	if (kwd & CHKNL) {
		while (t == TNL) {
			parseheredoc(sh);
			sh.checkkwd = 0;
			t = xxreadtoken(sh);
		}
	}

	kwd |= sh.checkkwd;
	sh.checkkwd = 0;

	if (t != TWORD || sh.quoteflag) {
		goto out;
	}

	/*
	 * check for keywords
	 */
	if (kwd & CHKKWD) {
		const char *const *pp;

		if ((pp = findkwd(sh, sh.wordtext.c_str()))) {
			sh.lasttoken = t = pp - parsekwd + KWDOFFSET;
			TRACE(("keyword %s recognized\n", tokname[t]));
			goto out;
		}
	}

	if (kwd & CHKALIAS) {
		struct alias *ap;
		if ((ap = lookupalias(sh, sh.wordtext, 1)) != NULL) {
			if (!ap->val.empty()) {
				pushstring(sh, ap->val.data(), ap);
			}
			goto top;
		}
	}
out:
#ifdef DEBUG
	if (!alreadyseen)
	    TRACE(("token %s %s\n", tokname[t], t == TWORD ? sh.wordtext.c_str() : ""));
	else
	    TRACE(("reread token %s %s\n", tokname[t], t == TWORD ? sh.wordtext.c_str() : ""));
#endif
	return (t);
}

static void
nlprompt(ShellContext &sh)
{
	plinno++;
	if (sh.doprompt)
		setprompt(sh, 2);
}

static void
nlnoprompt(ShellContext &sh)
{
	plinno++;
	sh.needprompt = sh.doprompt;
}


/*
 * Read the next input token.
 * If the token is a word, we set sh.backquotelist to the list of cmds in
 *	backquotes.  We set sh.quoteflag to true if any part of the word was
 *	quoted.
 * If the token is TREDIR, then we set sh.redirnode to a structure containing
 *	the redirection.
 *
 * [Change comment:  here documents and internal procedures]
 * [Readtoken shouldn't have any arguments.  Perhaps we should make the
 *  word parsing code into a separate routine.  In this case, readtoken
 *  doesn't need to have any internal procedures, but parseword does.
 *  We could also make parseoperator in essence the main routine, and
 *  have parseword (readtoken1?) handle both words and redirection.]
 */

#define RETURN(token)	return sh.lasttoken = token

static int
xxreadtoken(ShellContext &sh)
{
	int c;

	if (sh.tokpushback) {
		sh.tokpushback = 0;
		return sh.lasttoken;
	}
	if (sh.needprompt) {
		setprompt(sh, 2);
	}
	for (;;) {	/* until token or start of word found */
		int tok;

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
			RETURN(TNL);
		case PEOF:
			RETURN(TEOF);
		case '&':
			if (pgetc_eatbnl(sh) == '&')
				RETURN(TAND);
			pungetc(sh);
			RETURN(TBACKGND);
		case '|':
			if (pgetc_eatbnl(sh) == '|')
				RETURN(TOR);
			pungetc(sh);
			RETURN(TPIPE);
		case ';':
			if (pgetc_eatbnl(sh) == ';')
				RETURN(TENDCASE);
			pungetc(sh);
			RETURN(TSEMI);
		case '(':
			RETURN(TLP);
		case ')':
			RETURN(TRP);
		}
		tok = readtoken1(sh, c, BASESYNTAX, nullptr, 0);
		if (tok != TBLANK)
			return tok;
	}
#undef RETURN
}

static int
pgetc_eatbnl(ShellContext &sh)
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

static int
pgetc_top(ShellContext &sh, struct synstack *stack)
{
	return stack->syntax == SQSYNTAX ? pgetc(sh) : pgetc_eatbnl(sh);
}

static void
synstack_push(ShellContext &sh, struct synstack **stack, struct synstack *next,
			  const char *syntax)
{
	memset(next, 0, sizeof(*next));
	next->syntax = syntax;
	next->next = *stack;
	(*stack)->prev = next;
	*stack = next;
}

static void
synstack_pop(ShellContext &sh, struct synstack **stack)
{
	*stack = (*stack)->next;
}

unsigned
getmbc(ShellContext &sh, int c, char *out, int mode)
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
			USTPUTC(CTLMBCHAR, out);
			if (mode == 1)
				USTPUTC(CTLESC, out);
			USTPUTC(ml, out);
		}
		STADJUST(ml, out);
		if ((mode & 3) < 2) {
			USTPUTC(ml, out);
			USTPUTC(CTLMBCHAR, out);
		}

		return out - start;
	}

	if (ml > 1)
		pungetn(sh, ml - 1);

	return 0;
}

static char *
dollarsq_escape(ShellContext &sh, char *out)
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
		USTPUTC(conv_ch, out);
	}

	pungetn(sh, len - (p - str));
	return out;
}

/*
 * If eofmark is NULL, read a word or a redirection symbol.  If eofmark
 * is not NULL, read a here document.  In the latter case, eofmark is the
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

static int
readtoken1(ShellContext &sh, int firstc, char const *syntax, const std::string *eofmark, int striptabs)
{
	struct synstack synbase = {
		.syntax = syntax,
		.dblquote = syntax == DQSYNTAX,
	};
	int chkeofmark = sh.checkkwd & CHKEOFMARK;
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

	STARTSTACKSTR(out);
	loop: {	/* for each line, until end of word */
		CHECKEND();	/* set c to PEOF if at end of here document */
		/* Until end of line or end of word */
		for (;; c = pgetc_top(sh, synstack)) {
			int fieldsplitting;
			unsigned ml;

			/* Permit max(MB_LEN_MAX, 23) calls to USTPUTC. */
			CHECKSTRSPACE((MB_LEN_MAX > 16 ? MB_LEN_MAX : 16) + 7,
				      out);
			fieldsplitting = synstack->syntax == BASESYNTAX &&
					 !(synstack->varnest |
					   synstack->backq) ? 4 : 0;
			ml = getmbc(sh, c, out, fieldsplitting |
					    (printesc ? 2 : 0));
			if (ml == 1) {
				if (out == stackblock())
					return TBLANK;
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
				USTPUTC(c, out);
				nlprompt(sh);
				c = pgetc_top(sh, synstack);
				goto loop;		/* continue outer loop */
			case CWORD:
				USTPUTC(c, out);
				break;
			case CCTL:
				if (c == dollarsq) {
					out = dollarsq_escape(sh, out);
					break;
				}
				if ((!eofmark) | synstack->dblquote |
				    synstack->varnest)
					USTPUTC(CTLESC, out);
				USTPUTC(c, out);
				break;
			/* backslash */
			case CBACK:
				c = pgetc(sh);
				if (c == PEOF) {
					USTPUTC(CTLESC, out);
					USTPUTC('\\', out);
					pungetc(sh);
					break;
				}

				if (
					(synstack->dblquote |
					 synstack->backq) &&
					c != '\\' && c != '`' &&
					c != '$' && (
						c != '"' ||
						(eofmark != NULL &&
						 !synstack->varnest)
					) && (
						c != '}' ||
						!synstack->varnest
					)
				) {
					USTPUTC(CTLESC, out);
					USTPUTC('\\', out);
				}
				quotef++;

				ml = getmbc(sh, c, out, 1);
				out += ml;
				if (ml)
					break;

				USTPUTC(CTLESC, out);
				USTPUTC(c, out);
				break;
			case CSQUOTE:
csquote:
				synstack->syntax = SQSYNTAX;
quotemark:
				if (eofmark == NULL) {
					USTPUTC(CTLQUOTEMARK, out);
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
					USTPUTC(c, out);
					break;
				}

				if (synstack->dqvarnest == 0) {
					if (likely(dollarsq)) {
						char *p = stackblock();

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
				USTPUTC(c, out);
				break;
			case CLP:	/* '(' in arithmetic */
				synstack->parenlevel++;
				USTPUTC(c, out);
				break;
			case CRP:	/* ')' in arithmetic */
				if (synstack->parenlevel > 0)
					--synstack->parenlevel;
				else if (pgetc_eatbnl(sh) == ')') {
					synstack_pop(sh, &synstack);
					if (chkeofmark)
						USTPUTC(c, out);
					else
						c = CTLENDARI;
				} else {
					/*
					 * unbalanced parens
					 *  (don't 2nd guess - no error)
					 */
					pungetc(sh);
				}
				USTPUTC(c, out);
				break;
			case CBQUOTE:	/* '`' */
				if (synstack->backq == 2)
					goto end_backq;
				USTPUTC('`', out);
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
				USTPUTC(c, out);
			}
		}
	}
endword:
	if (synstack->syntax == ARISYNTAX)
		synerror(sh, "Missing '))'");
	if ((synstack->syntax != BASESYNTAX && eofmark == NULL) ||
	    synstack->backq)
		synerror(sh, "Unterminated quoted string");
	if (synstack->varnest != 0) {
		/* { */
		synerror(sh, "Missing '}'");
	}
	USTPUTC('\0', out);
	len = out - (char *)stackblock();
	out = stackblock();
	if (eofmark == NULL) {
		if ((c == '>' || c == '<')
		 && quotef == 0
		 && len <= 2
		 && (*out == '\0' || is_digit(*out))) {
			PARSEREDIR();
			return sh.lasttoken = TREDIR;
		} else {
			pungetc(sh);
		}
	}
	sh.quoteflag = quotef;
	sh.backquotelist = std::move(bqlist);
	grabstackblock(sh, len);
	sh.wordtext = out;
	return sh.lasttoken = TWORD;
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

		markloc = out - (char *)stackblock();
		for (ep = eofmark->c_str(); STPUTC(c, out), *ep; ep++) {
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
			p = (char *)stackblock() + markloc + 1;
			len = out - p;

			if (len) {
				len -= c <= PEOF;
				c = p[-1];

				if (len) {
					char *str;

					str = static_cast<char *>(alloca(len + 1));
					*(char *)mempcpy(str, p, len) = 0;

					pushstring(sh, str, NULL);
				}
			}
		}

		STADJUST((char *)stackblock() + markloc - out, out);
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
	int striptabs = 0;

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
				striptabs = 1;
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
		np = make_node<Node::Here>();
	else if (redir_type == Node::Type::ToFd || redir_type == Node::Type::FromFd)
		np = make_node<Node::Dup>();
	else
		np = make_node<Node::File>();
	np->type = redir_type;
	as<Node::RedirList>(np)->fd = fd != '\0' ? digit_val(fd) : default_fd;
	if (redir_type == Node::Type::Here) {
		sh.heredoc = new heredoc;
		sh.heredoc->here = np.get();
		sh.heredoc->striptabs = striptabs;
	}
	sh.redirnode = std::move(np);
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

	USTPUTC('$', out);

	c = pgetc_eatbnl(sh);
	if (c == '(') {		/* $(command) or $((arith)) */
		USTPUTC(c, out);
		if (pgetc_eatbnl(sh) == '(') {
			PARSEARITH();
		} else {
			pungetc(sh);
			PARSEBACKQNEW();
		}
	} else if (c == '\'' && newsyn['&']) {
		STADJUST(-1, out);
		dollarsq = '\\';
		goto csquote;
	} else if (c == '{' || is_name(c) || is_special(c)) {
		int typeloc = out - (char *)stackblock();

		STADJUST(!chkeofmark, out);
		subtype = VSNORMAL;
		if (likely(c == '{')) {
			if (chkeofmark)
				USTPUTC('{', out);
			c = pgetc_eatbnl(sh);
			subtype = 0;
		}
varname:
		if (is_name(c)) {
			do {
				STPUTC(c, out);
				c = pgetc_eatbnl(sh);
			} while (is_in_name(c));
		} else if (is_digit(c)) {
			do {
				STPUTC(c, out);
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
						USTPUTC('#', out);
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
					USTPUTC('#', out);
			}

			if (!is_special(cc)) {
				if (subtype == VSLENGTH)
					subtype = 0;
				goto badsub;
			}

			USTPUTC(cc, out);
		} else
			goto badsub;

		if (subtype == 0) {
			int cc = c;

			if (chkeofmark)
				STPUTC(c, out);

			switch (c) {
			case ':':
				subtype = VSNUL;
				c = pgetc_eatbnl(sh);
				if (chkeofmark)
					STPUTC(c, out);
				/*FALLTHROUGH*/
			default:
				p = std::strchr(types, c);
				if (p == NULL)
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
						STPUTC(c, out);
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
			char *p = stackblock();

			p[typeloc - 1] = CTLVAR;
			p[typeloc] = subtype | VSBIT;
			STPUTC('=', out);
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
	struct heredoc *saveheredoclist;
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
	STADJUST(oldstyle - 1, out);
	out[-1] = CTLBACKQ;
	str = stackblock();
	savelen = out - (char *)stackblock();
	grabstackblock(sh, savelen);
	STARTSTACKSTR(out);
        if (oldstyle) {
                /* We must read until the closing backquote, giving special
                   treatment to some slashes, and then push the string and
                   reread it as input, interpreting it normally.  */
		bool done = false;
		char *pout = out;
                int pc;

		while (!done) {
			CHECKSTRSPACE(MB_LEN_MAX + 1, pout);
			if (sh.needprompt) {
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
                                        USTPUTC('\\', pout);
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
			USTPUTC(pc, pout);
                }
		pout[-1] = 0;
		pstr = grabstackstr(pout);
		setinputstring(sh, pstr);
        }
	saveheredoclist = sh.heredoclist;
	sh.heredoclist = NULL;

	if (oldstyle) {
		saveprompt = sh.doprompt;
		sh.doprompt = 0;
	}

	n = list(sh, 2);

	if (oldstyle)
		sh.doprompt = saveprompt;
	else {
		if (readtoken(sh) != TRP)
			synexpect(sh, TRP);
		setinputstring(sh, nullstr);
	}

	parseheredoc(sh);
	sh.heredoclist = saveheredoclist;

	bqlist.push_back(std::move(n));
	/* Start reading from old file again. */
	popfile(sh);

	out = stnputs(sh, str, savelen, stackblock());

parsebackq_out:
	if (oldstyle) {
		/* Ignore any pushed back tokens left from the backquote
		 * parsing.
		 */
		sh.tokpushback = 0;
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
		USTPUTC(c, out);
	else {
		STADJUST(-1, out);
		out[-1] = CTLARI;
	}
	goto parsearith_return;
}

} /* end of readtoken */



#ifdef mkinit
INCLUDE "parser.h"
#endif


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

static void
synexpect(ShellContext &sh, int token)
{
	char msg[64];

	if (token >= 0) {
		fmtstr(sh, msg, 64, "%s unexpected (expecting %s)",
			tokname[sh.lasttoken], tokname[token]);
	} else {
		fmtstr(sh, msg, 64, "%s unexpected", tokname[sh.lasttoken]);
	}
	synerror(sh, msg);
	/* NOTREACHED */
}


static void
synerror(ShellContext &sh, const char *msg)
{
	sh.errlinno = plinno;
	throw dash::ParseError(std::string("Syntax error: ") + msg, plinno);
}

void
report_parse_error(ShellContext &sh, const dash::ParseError &e)
{
	sh.errlinno = e.lineno();
	sh.exitstatus = 2;
	sh_warnx(sh, "%s", e.what());
}

dash::Result<void>
try_parse_string(ShellContext &sh, std::string_view s)
{
	char save_nflag = nflag;
	std::string copy(s);

	setinputstring(sh, copy.data());
	nflag = 1;

	try {
		for (;;) {
			ParseCmdResult result = parsecmd(sh, 0);
			if (result.eof)
				break;
			clear_stack_temps(sh);
		}
		clear_stack_temps(sh);
		popfile(sh);
		nflag = save_nflag;
		return {};
	} catch (const dash::ParseError &e) {
		sh.errlinno = e.lineno();
		clear_stack_temps(sh);
		popfile(sh);
		nflag = save_nflag;
		return tl::unexpected(std::string(e.what()));
	}
}

static void __attribute__((noinline))
setprompt(ShellContext &sh, int which)
{
	sh.needprompt = 0;
	sh.whichprompt = which;

	if (!sh.input.current->nleft) {
		std::size_t su = sh.stackstr.used;
		sh.stackstr.commit(stackblocksize());
		out2str(getprompt(sh, NULL));
		sh.stackstr.rewind(su);
	}
}

const char *
expandstr(ShellContext &sh, const char *ps)
{
	struct parsefile *file_stop;
	struct heredoc *saveheredoclist;
	const char *result;
	int saveprompt;
	struct Node::Arg n{};
	int err = 0;

	file_stop = sh.input.current;

	/* XXX Fix (char *) cast. */
	setinputstring(sh, (char *)ps);

	saveheredoclist = sh.heredoclist;
	sh.heredoclist = NULL;
	saveprompt = sh.doprompt;
	sh.doprompt = 0;
	sh.needprompt = 0;
	result = ps;
	try {
		readtoken1(sh, pgetc_eatbnl(sh), DQSYNTAX, FAKEEOFMARK, 0);

		n.type = Node::Type::Arg;
		n.next = nullptr;
		n.text = sh.wordtext;
		n.backquote = std::move(sh.backquotelist);

		if (auto r = expandarg(sh, reinterpret_cast<Node *>(&n), NULL, EXP_QUOTED); !r) {
			sh.exception = r.error().code;
			err = 1;
		} else {
			result = stackblock();
		}
	} catch (const dash::ParseError &) {
		err = 1;
	}

	restore_handler_expandarg(sh, err);

	sh.doprompt = saveprompt;
	unwindfiles(sh, file_stop);
	sh.heredoclist = saveheredoclist;

	return result;
}

/*
 * called by editline -- any expansions to the prompt
 *    should be added here.
 */
const char *
getprompt(ShellContext &sh, void *unused)
{
	const char *prompt;

	switch (sh.whichprompt) {
	default:
#ifdef DEBUG
		return "<internal prompt error>";
#endif
	case 0:
		return nullstr;
	case 1:
		prompt = ps1val();
		break;
	case 2:
		prompt = ps2val();
		break;
	}

	return expandstr(sh, prompt);
}

const char *const *
findkwd(ShellContext &sh, const char *s)
{
	return findstring(
		s, parsekwd, sizeof(parsekwd) / sizeof(const char *)
	);
}
