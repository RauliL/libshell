/*-
 * Copyright (c) 1993
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

#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include <libshell.hpp>

/*
 * Evaluate a command.
 */

#include "init.h"
#include "shell.h"
#include "nodes.h"
#include "expand.h"
#include "parser.h"
#include "jobs.h"
#include "eval.h"
#include "builtins.h"
#include "options.h"
#include "exec.h"
#include "redir.h"
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "error.h"
#include "mystring.h"
#include "parse_error.h"
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

using namespace shell;

namespace shell {

namespace {
void evaltreenr(Context &, Node *, EvalFlags)
	__attribute__((__noreturn__));
Result<int> evalloop(Context &sh, const BinaryNode *, EvalFlags);
Result<int> evalfor(Context &sh, const ForNode *, EvalFlags);
Result<int> evalcase(Context &sh, const CaseNode *, EvalFlags);
Result<int> evalsubshell(Context &sh, const Node *, EvalFlags);
Result<void> expredir(Context &sh, Node *);
Result<int> evalpipe(Context &sh, const PipeNode *, EvalFlags);
Result<int> evalcommand(Context &sh, const CommandNode *, EvalFlags);
Result<int> evalbltin(Context &sh, const BuiltinCommand *, int, char **, EvalFlags);
Result<int> evalfun(Context &sh, const std::shared_ptr<Node> &, int, char **, EvalFlags);
Result<void> prehash(Context &sh, Node *);
int eprintlist(Context &sh, Output *, const ArgList &, int);
Result<int> bltincmd(Context &sh, int, char **);

const struct BuiltinCommand bltin = {
	.name = "",
	.builtin = bltincmd,
	.flags = BuiltinCommand::Flag::Regular,
};
} // namespace

/*
 * Called to reset things after an sh.exception.
 */

void
eval_exitreset(Context &sh)
{
	if (sh.savestatus >= 0) {
		if (sh.exception == Error::Exit || sh.evalskip == EvalSkip::FuncDef)
			sh.exitstatus = sh.savestatus;
		sh.savestatus = -1;
	}
	sh.evalskip.clear();
	sh.loopnest = 0;
	sh.inps4 = 0;

	if (sh.tpip[0] >= 0) {
		close(sh.tpip[0]);
		close(sh.tpip[1]);
	}
}

/*
 * The eval commmand.
 */

Result<int>
evalcmd(Context &sh, int argc, char **argv, EvalFlags flags)
{
        if (argc > 1) {
                if (argc == 2)
                        return sh.eval(argv[1], flags & EvalFlag::Tested);
                std::string concat = argv[1];
                for (char **ap = argv + 2; *ap; ap++) {
                        concat.push_back(' ');
                        concat.append(*ap);
                }
                return sh.eval(concat, flags & EvalFlag::Tested);
        }
        return 0;
}

namespace {
inline bool
parser_eof(const Context &sh)
{
	return sh.parse.tokpushback && sh.parse.lasttoken == Token::Eof;
}
} // namespace

/*
 * Execute a command or commands contained in a string.
 */

namespace {
Result<int>
eval_input(Context &sh, EvalFlags flags)
{
	int status = 0;

	try {
		for (;;) {
			ParseCmdResult cmd = parsecmd(sh, 0);
			if (cmd.eof)
				break;
			Node *n = cmd.tree.get();

			EvalFlags child = flags;
			if (!parser_eof(sh))
				child.reset(EvalFlag::Exit);
			auto r = evaltree(sh, n, child);
			if (!r) {
				sh.exception = r.error();
				popfile(sh);
				flushall(sh);
				if (r.error() == Error::Generic)
					return sh.exitstatus;
				return r;
			}
			if (n)
				status = *r;

			if (sh.evalskip)
				break;
		}
		popfile(sh);
		flushall(sh);
		return status;
	} catch (const ParseError &e) {
		report_parse_error(sh, e);
		popfile(sh);
		flushall(sh);
		return sh.exitstatus;
	}
}
} // namespace

Result<int>
Context::eval(const std::string &script, EvalFlags flags)
{
	std::string copy = script;

	setinputstring(*this, copy.data());
	return eval_input(*this, flags);
}

Result<int>
Context::eval_file(std::string_view path, EvalFlags flags)
{
	TRY_SHV(setinputfile(*this, path, INPUT_PUSH_FILE));
	return eval_input(*this, flags);
}

/*
 * Evaluate a parse tree.  The value is left in sh.exitstatus.
 * Type-specific work is dispatched via Node::eval().
 */

Result<int>
evaltree(Context &sh, Node *n, EvalFlags flags)
{
	bool checkexit = false;
	int status = 0;

	if (sh.opts.noexec())
		goto out;

	if (!n) {
		goto out;
	}

	TRY_SHV(dotrap(sh));

	status = TRY_SH(n->eval(sh, flags, checkexit));
	sh.exitstatus = status;

out:
	TRY_SHV(dotrap(sh));

	if (sh.opts.errexit() && checkexit && !flags.test(EvalFlag::Tested) && status)
		goto exexit;

	if (flags.test(EvalFlag::Exit)) {
exexit:
		return exraise(sh, Error::End);
	}

	return sh.exitstatus;
}

Result<int>
Node::eval(Context &, EvalFlags, bool &) const
{
	return EXIT_SUCCESS;
}

Result<int>
NotNode::eval(Context &sh, EvalFlags, bool &) const
{
	auto status = TRY_SH(evaltree(sh, com.get(), EvalFlag::Tested));

	if (!sh.evalskip)
		status = !status;

	return status;
}

Result<int>
RedirNode::eval(Context &sh, EvalFlags flags, bool &checkexit) const
{
	if (type == Type::Subshell || type == Type::Backgnd) {
		checkexit = true;

		return evalsubshell(sh, this, flags);
	}

	sh.errlinno = sh.lineno = linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;
	TRY_SHV(expredir(sh, redirect.get()));
	pushredir(sh, redirect.get());
	int status = TRY_SH(redirectsafe(sh, redirect.get(), REDIR_PUSH));
	if (status)
		checkexit = true;
	else
		status = TRY_SH(evaltree(sh, n.get(), flags & EvalFlag::Tested));
	if (redirect)
		popredir(sh, 0);
	return status;
}

Result<int>
CommandNode::eval(Context &sh, EvalFlags flags, bool &checkexit) const
{
	checkexit = true;

	return evalcommand(sh, this, flags);
}

Result<int>
ForNode::eval(Context &sh, EvalFlags flags, bool &) const
{
	return evalfor(sh, this, flags);
}

Result<int>
BinaryNode::eval(Context &sh, EvalFlags flags, bool &) const
{
	if (type == Type::While || type == Type::Until)
		return evalloop(sh, this, flags);

	static_assert(static_cast<int>(Type::And) + 1 == static_cast<int>(Type::Or));
	static_assert(static_cast<int>(Type::Or) + 1 == static_cast<int>(Type::Semi));

	unsigned isor = static_cast<int>(type) - static_cast<int>(Type::And);
	EvalFlags left;
	if (type != Type::Semi || flags.test(EvalFlag::Tested))
		left.set(EvalFlag::Tested);
	int status = TRY_SH(evaltree(sh, ch1.get(), left));
	if ((!status) == isor || sh.evalskip)
		return status;
	return evaltree(sh, ch2.get(), flags);
}

Result<int>
PipeNode::eval(Context &sh, EvalFlags flags, bool &checkexit) const
{
	checkexit = true;

	return evalpipe(sh, this, flags);
}

Result<int>
CaseNode::eval(Context &sh, EvalFlags flags, bool &) const
{
	return evalcase(sh, this, flags);
}

Result<int>
IfNode::eval(Context &sh, EvalFlags flags, bool &) const
{
	const auto status = TRY_SH(evaltree(sh, test.get(), EvalFlag::Tested));

	if (sh.evalskip)
		return status;
	if (!status)
		return evaltree(sh, ifpart.get(), flags);
	if (elsepart)
		return evaltree(sh, elsepart.get(), flags);

	return EXIT_SUCCESS;
}

Result<int>
DefunNode::eval(Context &sh, EvalFlags, bool &) const
{
	defun(sh, this);

	return EXIT_SUCCESS;
}

namespace {
void
evaltreenr(Context &sh, Node *n, EvalFlags flags)
{
	evaltree(sh, n, flags);
	exitshell(sh);
}

EvalSkipFlags
skiploop(Context &sh)
{
	EvalSkipFlags skip = sh.evalskip;

	if (skip == EvalSkip::Break || skip == EvalSkip::Continue) {
		if (likely(--sh.skipcount <= 0)) {
			sh.evalskip.clear();
		} else {
			skip = EvalSkip::Break;
		}
	}

	return skip;
}

Result<int>
evalloop(Context &sh, const BinaryNode *n, EvalFlags flags)
{
	EvalSkipFlags skip;
	int status;

	sh.loopnest++;
	status = 0;
	flags &= EvalFlag::Tested;
	do {
		int i;

		i = TRY_SH(evaltree(sh, n->ch1.get(), EvalFlag::Tested));
		skip = skiploop(sh);
		if (skip == EvalSkip::Func)
			status = i;
		if (skip)
			continue;
		if (n->type != Node::Type::While)
			i = !i;
		if (i != 0)
			break;
		status = TRY_SH(evaltree(sh, n->ch2.get(), flags));
		skip = skiploop(sh);
	} while (!(skip & ~EvalSkip::Continue));
	sh.loopnest--;

	return status;
}

Result<int>
evalfor(Context &sh, const ForNode *n, EvalFlags flags)
{
	ArgList args;
	Node *argp;
	int status;

	sh.errlinno = sh.lineno = n->linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	for (argp = n->args.get() ; argp ; argp = as<ArgNode>(argp)->next.get()) {
		TRY_SHV(expandarg(sh, argp, &args, EXP_FULL | EXP_TILDE));
	}

	status = 0;
	sh.loopnest++;
	flags &= EvalFlag::Tested;
	for (const std::string &word : args) {
		TRY_SHV(setvar(sh, n->var, word));
		status = TRY_SH(evaltree(sh, n->body.get(), flags));
		if (skiploop(sh) & ~EvalSkip::Continue)
			break;
	}
	sh.loopnest--;

	return status;
}

Result<int>
evalcase(Context &sh, const CaseNode *n, EvalFlags flags)
{
	Node *cp;
	Node *patp;
	ArgList args;
	int status = 0;

	sh.errlinno = sh.lineno = n->linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	expandarg(sh, n->expr.get(), &args, FNMATCH_IS_ENABLED ? EXP_TILDE :
					   EXP_TILDE | EXP_MBCHAR);
	for (cp = n->cases.get() ; cp && !sh.evalskip ; cp = as<CaseListNode>(cp)->next.get()) {
		for (patp = as<CaseListNode>(cp)->pattern.get() ; patp ; patp = as<ArgNode>(patp)->next.get()) {
			if (casematch(sh, patp, args[0].data())) {
				/* Ensure body is non-empty as otherwise
				 * EvalFlag::Exit may prevent us from setting the
				 * exit status.
				 */
				if (!sh.evalskip && as<CaseListNode>(cp)->body) {
					status = TRY_SH(evaltree(sh,
							as<CaseListNode>(cp)->body.get(),
							flags));
				}
				goto out;
			}
		}
	}
out:
	return status;
}

inline bool
have_traps(const Context& sh)
{
	return sh.trapcnt != 0;
}
} // namespace

/*
 * Kick off a subshell to evaluate a tree.
 */

namespace {
Result<int>
evalsubshell(Context &sh, const Node *n, EvalFlags flags)
{
	Job *jp;
	bool backgnd = n->type == Node::Type::Backgnd;
	int status;

	sh.errlinno = sh.lineno = as<RedirNode>(n)->linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	TRY_SHV(expredir(sh, as<RedirNode>(n)->redirect.get()));
	INTOFF(sh);
	if (!backgnd && flags.test(EvalFlag::Exit) && !have_traps(sh)) {
		forkreset(sh, nullptr);
		goto nofork;
	}
	jp = makejob(sh, 1);
	if (TRY_SH(forkshell(sh, jp, as<RedirNode>(n)->n.get(), backgnd)) == 0) {
		flags.set(EvalFlag::Exit);
		if (backgnd)
			flags.reset(EvalFlag::Tested);
nofork:
		INTON(sh);
		TRY_SHV(redirect(sh, as<RedirNode>(n)->redirect.get(), 0));
		evaltreenr(sh, as<RedirNode>(n)->n.get(), flags);
		/* never returns */
	}
	status = 0;
	if (! backgnd)
		status = TRY_SH(waitforjob(sh, jp));
	INTON(sh);

	return status;
}
} // namespace

/*
 * Compute the names of the files in a redirection list.
 */

namespace {
Result<void>
expredir(Context &sh, Node *n)
{
	Node *redir;

	for (redir = n ; redir ; redir = as<RedirList>(redir)->next.get()) {
		ArgList fn;
		switch (redir->type) {
		case Node::Type::FromTo:
		case Node::Type::From:
		case Node::Type::To:
		case Node::Type::Clobber:
		case Node::Type::Append:
			TRY_SHV(expandarg(sh, as<FileNode>(redir)->fname.get(), &fn, EXP_TILDE | EXP_REDIR));
			as<FileNode>(redir)->expfname = fn[0];
			break;
		case Node::Type::FromFd:
		case Node::Type::ToFd:
			if (as<DupNode>(redir)->vname) {
				TRY_SHV(expandarg(sh, as<DupNode>(redir)->vname.get(), &fn, EXP_TILDE | EXP_REDIR));
				fixredir(sh, redir, fn[0].c_str(), 1);
			}
			break;
		default:
			break;
		}
	}
	return {};
}
} // namespace

/*
 * Evaluate a pipeline.  All the processes in the pipeline are children
 * of the process creating the pipeline.  (This differs from some versions
 * of the shell, which make the last process in a pipeline the parent
 * of all the rest.)
 */

namespace {
Result<int>
evalpipe(Context &sh, const PipeNode *n, EvalFlags flags)
{
	Job *jp;
	int pipelen;
	int prevfd;
	int pip[2];
	int status = 0;

	pipelen = static_cast<int>(n->cmdlist.size());
	flags.set(EvalFlag::Exit);
	INTOFF(sh);
	jp = makejob(sh, pipelen);
	prevfd = -1;
	for (std::size_t i = 0; i < n->cmdlist.size(); i++) {
		auto cmd = n->cmdlist[i].get();

		TRY_SHV(prehash(sh, cmd));
		pip[1] = -1;
		if (i + 1 < n->cmdlist.size()) {
			if (pipe(pip) < 0) {
				close(prevfd);
				return sh_error(sh, "Pipe call failed");
			}
		}
		if (TRY_SH(forkshell(sh, jp, cmd, n->backgnd)) == 0) {
			INTON(sh);
			if (pip[1] >= 0) {
				close(pip[0]);
			}
			if (prevfd > 0) {
				reset_input(sh);
				dup2(prevfd, 0);
				close(prevfd);
			}
			if (pip[1] > 1) {
				dup2(pip[1], 1);
				close(pip[1]);
			}
			evaltreenr(sh, cmd, flags);
			/* never returns */
		}
		if (prevfd >= 0)
			close(prevfd);
		prevfd = pip[0];
		close(pip[1]);
	}
	if (n->backgnd == 0) {
		status = TRY_SH(waitforjob(sh, jp));
	}
	INTON(sh);

	return status;
}
} // namespace

/*
 * Execute a command inside back quotes.  If it's a builtin command, we
 * want to save its sh.output in a block obtained from malloc.  Otherwise
 * we fork off a subprocess and get the sh.output of the command via a pipe.
 * Should be called with interrupts off.
 */

Result<void>
evalbackcmd(Context &sh, Node *n, struct backcmd *result)
{
	Job *jp;
	int pip[2];
	int pid;

	*result = backcmd{};
	if (!n) {
        return {};
	}

	TRY_SHV(sh_pipe(sh, pip, 0));
	sh.tpip[0] = pip[0];
	sh.tpip[1] = pip[1];
	jp = makejob(sh, 1);
	pid = TRY_SH(forkshell(sh, jp, n, FORK_NOJOB));
	sh.tpip[0] = -1;
	if (pid == 0) {
		FORCEINTON(sh);
		close(pip[0]);
		if (pip[1] != 1) {
			dup2(pip[1], 1);
			close(pip[1]);
		}
		ifsfree(sh);
		evaltreenr(sh, n, EvalFlag::Exit);
		/* NOTREACHED */
	}
	close(pip[1]);
	result->fd = pip[0];
	result->jp = jp;

	return {};
}

/* Expand at least one more word into ArgList; return false if none. */
namespace {
Result<bool>
fill_arglist(Context &sh, ArgList *alist, Node **argpp)
{
	size_t before = alist->size();
	Node *argp;

	while ((argp = *argpp)) {
		TRY_SHV(expandarg(sh, argp, alist, EXP_FULL | EXP_TILDE));
		*argpp = as<ArgNode>(argp)->next.get();
		if (alist->size() > before)
			return true;
	}

	return false;
}
} // namespace

/* Skip `command [-p] [--]` words; set *arg0 to the real command index. */
namespace {
Result<int>
parse_command_args(Context &sh, ArgList *alist, Node **argpp,
			      const char **path, size_t *arg0)
{
	size_t start = *arg0;

	for (;;) {
		if (start + 1 >= alist->size() &&
		    !TRY_SH(fill_arglist(sh, alist, argpp)))
			return 0;
		start++;
		char *cp = (*alist)[start].data();
		if (*cp++ != '-')
			break;
		char c;
		if (!(c = *cp++))
			break;
		if (c == '-' && !*cp) {
			if (start + 1 >= alist->size() &&
			    !TRY_SH(fill_arglist(sh, alist, argpp)))
				return 0;
			start++;
			break;
		}
		do {
			switch (c) {
			case 'p':
				*path = defpath;
				break;
			default:
				/* run 'typecmd' for other options */
				return 0;
			}
		} while ((c = *cp++));
	}

	*arg0 = start;
	return DO_NOFUNC;
}
} // namespace

/*
 * Execute a simple command.
 */

namespace {
Result<int>
evalcommand(Context &sh, const CommandNode *cmd, EvalFlags flags)
{
	std::size_t localvar_stop;
	ParseFile *file_stop;
	std::size_t redir_stop;
	Node *argp;
	ArgList args;
	ArgList vars;
	char **argv;
	int argc;
	size_t arg0 = 0;
	size_t xtrace_end = 0;
	struct cmdentry cmdentry;
	Job *jp;
	char *lastarg;
	const char *path;
	int spclbltin;
	int cmd_flag;
	int execcmd;
	int status;
	std::vector<char *> argv_store;
	Variable::Flags vflags;
	int vlocal;

	sh.errlinno = sh.lineno = cmd->linno;
	if (sh.funcline)
		sh.lineno -= sh.funcline - 1;

	/* First expand the arguments. */
	file_stop = sh.input.current;
	sh.back_exitstatus = 0;

	cmdentry.type = TableEntry::Type::Builtin;
	cmdentry.u.cmd = &bltin;

	cmd_flag = 0;
	execcmd = 0;
	spclbltin = -1;
	vflags = {};
	vlocal = 0;
	path = nullptr;

	argc = 0;
	argp = cmd->args.get();
	if (TRY_SH(fill_arglist(sh, &args, &argp))) {
		int pseudovarflag = 0;

		arg0 = 0;
		xtrace_end = args.size();
		for (;;) {
			TRY_SHV(find_command(sh, args[arg0].data(), &cmdentry,
				     cmd_flag | DO_REGBLTIN, pathval(sh)));

			vlocal++;

			/* implement bltin and command here */
			if (cmdentry.type != TableEntry::Type::Builtin)
				break;

			pseudovarflag = cmdentry.u.cmd->flags & BuiltinCommand::Flag::Assign;
			if (likely(spclbltin < 0)) {
				spclbltin = cmdentry.u.cmd->flags & BuiltinCommand::Flag::Special;
				vlocal = spclbltin ^ BuiltinCommand::Flag::Special;
			}
			execcmd = cmdentry.u.cmd == &builtin_exec;
			if (likely(cmdentry.u.cmd != &builtin_command))
				break;

			cmd_flag = TRY_SH(parse_command_args(sh, &args, &argp, &path,
						     &arg0));
			if (!cmd_flag)
				break;
			xtrace_end = args.size();
		}

		for (; argp; argp = as<ArgNode>(argp)->next.get())
			TRY_SHV(expandarg(sh, argp, &args,
				  pseudovarflag &&
				  isassignment(as<ArgNode>(argp)->text) ?
				  EXP_VARTILDE : EXP_FULL | EXP_TILDE));

		argc = static_cast<int>(args.size() - arg0);

		if (execcmd && argc > 1)
			vflags = Variable::Flag::Exported;
	}

	localvar_stop = TRY_SH(pushlocalvars(sh, vlocal));

	/* Reserve one extra spot at the front for shellexec. */
	argv_store.reserve(static_cast<size_t>(argc) + 2);
	argv_store.push_back(nullptr);
	for (std::size_t i = arg0; i < args.size(); i++) {
		argv_store.push_back(args[i].data());
	}
	argv_store.push_back(nullptr);
	argv = argv_store.data() + 1;

	lastarg = nullptr;
	if (sh.opts.interactive() && sh.funcline == 0 && argc > 0)
		lastarg = argv[argc - 1];

	sh.preverrout.fd = 2;
	TRY_SHV(expredir(sh, cmd->redirect.get()));
	redir_stop = pushredir(sh, cmd->redirect.get());
	status = TRY_SH(redirectsafe(sh, cmd->redirect.get(), REDIR_PUSH|REDIR_SAVEFD2));

	if (unlikely(status)) {
bail:
		sh.exitstatus = status;

		/* We have a redirection error. */
		if (spclbltin > 0)
			return exraise(sh, Error::Generic);

		goto out;
	}

	for (argp = cmd->assign.get(); argp; argp = as<ArgNode>(argp)->next.get()) {
		std::size_t before = vars.size();

		TRY_SHV(expandarg(sh, argp, &vars, EXP_VARTILDE));
		for (std::size_t i = before; i < vars.size(); i++) {
			if (vlocal)
				TRY_SHV(mklocal(sh, vars[i], Variable::Flag::Exported));
			else
				TRY_SHV(setvareq(sh, vars[i], vflags));
		}
	}

	/* Print the command if sh.opts.xtrace() is set. */
	if (sh.opts.xtrace() && !sh.inps4) {
        Output *out;
		int sep;

		out = &sh.preverrout;
		sh.inps4 = 1;
		out->write(sh, expandstr(sh, ps4val(sh)));
		sh.inps4 = 0;
		sep = 0;
		sep = eprintlist(sh, out, vars, sep);
		{
			ArgList xargs(args.begin(),
				      args.begin() + static_cast<std::ptrdiff_t>(xtrace_end));
			eprintlist(sh, out, xargs, sep);
		}
		outcslow(sh, '\n', out);
#ifdef FLUSHERR
		out->flush(sh);
#endif
	}

	/* Now locate the command. */
	if (cmdentry.type != TableEntry::Type::Builtin ||
	    !(cmdentry.u.cmd->flags & BuiltinCommand::Flag::Regular)) {
		path = unlikely(path) ? path : pathval(sh);
		TRY_SHV(find_command(sh, argv[0], &cmdentry, cmd_flag | DO_ERR, path));
	}

	jp = nullptr;

	/* Execute the command. */
	switch (cmdentry.type) {
	case TableEntry::Type::Unknown:
		status = 127;
#ifdef FLUSHERR
		sh.errout.flush(sh);
#endif
		goto bail;

	default:
		flush_input(sh);

		/* Fork off a child process if necessary. */
		if (!flags.test(EvalFlag::Exit) || have_traps(sh)) {
			INTOFF(sh);
			jp = TRY_SH(vforkexec(sh, cmd, argv, path, cmdentry.u.index));
			break;
		}
		shellexec(sh, argv, path, cmdentry.u.index);
		/* NOTREACHED */

	case TableEntry::Type::Builtin: {
		auto br = evalbltin(sh, cmdentry.u.cmd, argc, argv, flags);
		if (!br)
			return br;
		if (*br && !(sh.exception == Error::Generic && spclbltin <= 0))
			return exraise(sh, sh.exception.value_or(Error::Interrupt));
		break;
	}

	case TableEntry::Type::Function:
		TRY_SHV(evalfun(sh, cmdentry.u.func, argc, argv, flags));
		break;
	}

	status = TRY_SH(waitforjob(sh, jp));
	FORCEINTON(sh);

out:
	if (cmd->redirect)
		popredir(sh, execcmd);
	unwindredir(sh, redir_stop);
	unwindfiles(sh, file_stop);
	TRY_SHV(unwindlocalvars(sh, localvar_stop));
	if (lastarg)
		TRY_SHV(setvar(sh, "_", lastarg));

	return status;
}

void
freestdout(Context &sh)
{
	sh.output.discard();
}

Result<int>
evalbltin(Context &sh, const BuiltinCommand *cmd, int argc, char **argv, EvalFlags flags)
{
	std::string savecmdname = std::move(sh.commandname);
	int i = 0;

	sh.commandname = argv[0] ? argv[0] : "";
	sh.bopt.argptr = argv + 1;
	sh.bopt.optptr = nullptr;			/* initialize nextopt */

	Result<int> r;
	if (cmd == &builtin_eval)
		r = evalcmd(sh, argc, argv, flags);
	else
		r = cmd->builtin(sh, argc, argv);

	if (!r) {
		sh.exception = r.error();
		i = 1;
		if (r.error() != Error::Generic) {
			freestdout(sh);
			sh.commandname = std::move(savecmdname);
			return r;
		}
	} else {
		int status = *r;
		flushall(sh);
		if (outerr(sh.out1))
			sh_warnx(sh, "{}: I/O error", sh.commandname);
		status |= outerr(sh.out1);
		sh.exitstatus = status;
	}
	freestdout(sh);
	sh.commandname = std::move(savecmdname);

	return i;
}

Result<int>
evalfun(Context &sh, const std::shared_ptr<Node> &func, int argc, char **argv, EvalFlags flags)
{
	ShellParam saveparam = std::move(sh.shellparam);
	int savefuncline = sh.funcline;
	int saveloopnest = sh.loopnest;
	bool failed = false;
	Error fail_err;

	INTOFF(sh);
	sh.funcline = as<DefunNode>(func)->linno;
	sh.loopnest = 0;
	barrier();
	if (--sh.suppressint == 0 && sh.intpending) {
		auto u = onint(sh);
		fail_err = u.value();
		failed = true;
	} else {
		sh.shellparam.p.clear();
		for (int i = 1; i < argc; i++)
			sh.shellparam.p.emplace_back(argv[i]);
		sh.shellparam.optind = 1;
		sh.shellparam.optoff = -1;
		auto r = evaltree(sh, as<DefunNode>(func)->body.get(),
				  flags & EvalFlag::Tested);
		if (!r) {
			fail_err = r.error();
			sh.exception = fail_err;
			failed = true;
		}
	}
	INTOFF(sh);
	sh.loopnest = saveloopnest;
	sh.funcline = savefuncline;
	sh.shellparam = std::move(saveparam);
	INTON(sh);
	sh.evalskip.reset(EvalSkip::Func).reset(EvalSkip::FuncDef);
	if (failed)
		return tl::unexpected(fail_err);

	return 0;
}
} // namespace

/*
 * Search for a command.  This is called before we fork so that the
 * location of the command will be available in the parent as well as
 * the child.  The check for "goodname" is an overly conservative
 * check that the name will not be subject to expansion.
 */

namespace {
Result<void>
prehash(Context &sh, Node *n)
{
	struct cmdentry entry;

	if (n->type == Node::Type::Cmd && as<CommandNode>(n)->args)
		if (endofname(as<ArgNode>(as<CommandNode>(n)->args)->text) ==
		    as<ArgNode>(as<CommandNode>(n)->args)->text.size())
			TRY_SHV(find_command(sh, as<ArgNode>(as<CommandNode>(n)->args)->text.data(), &entry, 0,
				     pathval(sh)));
	return {};
}

/*
 * Builtin commands.  Builtin commands whose functions are closely
 * tied to evaluation are implemented here.
 */

} // namespace

/*
 * No command given.
 */

namespace {
Result<int>
bltincmd(Context &sh, int argc, char **argv)
{
	/*
	 * Preserve sh.exitstatus of a previous possible redirection
	 * as POSIX mandates
	 */
	return sh.back_exitstatus;
}
} // namespace

/*
 * Handle break and continue commands.  Break, continue, and return are
 * all handled by setting the sh.evalskip flag.  The evaluation routines
 * above all check this flag, and if it is set they start skipping
 * commands rather than executing them.  The variable sh.skipcount is
 * the number of loops to break/continue, or the number of function
 * levels to return.  (The latter is always 1.)  It should probably
 * be an error to break out of more loops than exist, but it isn't
 * in the standard shell so we don't make it one here.
 */

Result<int>
breakcmd(Context &sh, int argc, char **argv)
{
	int n = argc > 1 ? TRY_SH(number(sh, argv[1])) : 1;

	if (n <= 0)
		return badnum(sh, argv[1]);
	if (n > sh.loopnest)
		n = sh.loopnest;
	if (n > 0) {
		sh.evalskip = (**argv == 'c') ? EvalSkip::Continue : EvalSkip::Break;
		sh.skipcount = n;
	}
	return 0;
}

/*
 * The return command.
 */

Result<int>
returncmd(Context &sh, int argc, char **argv)
{
	EvalSkip skip;
	int status;

	/*
	 * If called outside a function, do what ksh does;
	 * skip the rest of the file.
	 */
	if (argv[1]) {
		skip = EvalSkip::Func;
		status = TRY_SH(number(sh, argv[1]));
	} else {
		skip = EvalSkip::FuncDef;
		status = sh.exitstatus;
	}
	sh.evalskip = skip;

	return status;
}

Result<int>
falsecmd(Context &sh, int argc, char **argv)
{
	return EXIT_FAILURE;
}

Result<int>
truecmd(Context &sh, int argc, char **argv)
{
	return EXIT_SUCCESS;
}

Result<int>
execcmd(Context &sh, int argc, char **argv)
{
	if (argc > 1) {
		sh.opts.interactive() = 0;		/* exit on error */
		sh.opts.monitor() = 0;
		optschanged(sh);
		flush_input(sh);
		shellexec(sh, argv + 1, pathval(sh), 0);
	}

	return EXIT_SUCCESS;
}

namespace {
int
eprintlist(Context &sh, Output *out, const ArgList &list, int sep)
{
	for (const std::string &s : list) {
		const char *p;

		p = " %s";
		p += (1 - sep);
		sep |= 1;
		outfmt(sh, out, p, s.c_str());
	}

	return sep;
}
} // namespace

} // namespace shell
