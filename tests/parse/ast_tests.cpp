#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "parse_helpers.hpp"


using namespace shell;
using parse_test::as_cmd;
using parse_test::ctl_arith;
using parse_test::ctl_backq;
using parse_test::ctl_var;
using parse_test::ctl_var_normal;
using parse_test::nth_arg;
using parse_test::parse_one;
using parse_test::parse_trees;
using parse_test::ctl_quoted;

TEST_CASE("AST: simple command", "[parse][ast]")
{
	auto tree = parse_one("echo hello\n");
	auto *cmd = as_cmd(tree.get());
	REQUIRE(cmd);
	REQUIRE(cmd->assign.empty());
	REQUIRE(cmd->redirect.empty());

	auto *a0 = nth_arg(cmd->args, 0);
	auto *a1 = nth_arg(cmd->args, 1);
	REQUIRE(a0);
	REQUIRE(a1);
	REQUIRE(a0->text == "echo");
	REQUIRE(a1->text == "hello");
	REQUIRE(cmd->args.size() == 2);
}

TEST_CASE("AST: assignment then command", "[parse][ast]")
{
	auto tree = parse_one("FOO=bar echo ok\n");
	auto *cmd = as_cmd(tree.get());
	REQUIRE(cmd);

	REQUIRE(cmd->assign.size() == 1);
	auto *asg = as<ArgNode>(cmd->assign[0]);
	REQUIRE(asg);
	REQUIRE(asg->text == "FOO=bar");

	REQUIRE(nth_arg(cmd->args, 0)->text == "echo");
	REQUIRE(nth_arg(cmd->args, 1)->text == "ok");
}

TEST_CASE("AST: semicolon list is Semi binary", "[parse][ast]")
{
	auto tree = parse_one("echo a; echo b\n");
	REQUIRE(tree->type == Node::Type::Semi);

	auto *bin = as<BinaryNode>(tree.get());
	REQUIRE(as_cmd(bin->ch1.get()));
	REQUIRE(as_cmd(bin->ch2.get()));
	REQUIRE(nth_arg(as_cmd(bin->ch1.get())->args, 1)->text == "a");
	REQUIRE(nth_arg(as_cmd(bin->ch2.get())->args, 1)->text == "b");
}

TEST_CASE("AST: newline yields separate trees", "[parse][ast]")
{
	auto trees = parse_trees("echo a\necho b\n");
	REQUIRE(trees.size() == 2);
	REQUIRE(as_cmd(trees[0].get()));
	REQUIRE(as_cmd(trees[1].get()));
	REQUIRE(nth_arg(as_cmd(trees[0].get())->args, 1)->text == "a");
	REQUIRE(nth_arg(as_cmd(trees[1].get())->args, 1)->text == "b");
}

TEST_CASE("AST: pipeline", "[parse][ast]")
{
	auto tree = parse_one("echo a | cat | cat\n");
	REQUIRE(tree->type == Node::Type::Pipe);

	auto *pipe = as<PipeNode>(tree.get());
	REQUIRE(pipe->backgnd == 0);
	REQUIRE(pipe->cmdlist.size() == 3);
	REQUIRE(as_cmd(pipe->cmdlist[0].get()));
	REQUIRE(as_cmd(pipe->cmdlist[1].get()));
	REQUIRE(as_cmd(pipe->cmdlist[2].get()));
	REQUIRE(nth_arg(as_cmd(pipe->cmdlist[0].get())->args, 0)->text == "echo");
	REQUIRE(nth_arg(as_cmd(pipe->cmdlist[1].get())->args, 0)->text == "cat");
}

TEST_CASE("AST: bang wraps pipeline or command in Not", "[parse][ast]")
{
	SECTION("bang command") {
		auto tree = parse_one("! false\n");
		REQUIRE(tree->type == Node::Type::Not);
		auto *cmd = as_cmd(as<NotNode>(tree.get())->com.get());
		REQUIRE(cmd);
		REQUIRE(nth_arg(cmd->args, 0)->text == "false");
	}
	SECTION("bang pipeline") {
		auto tree = parse_one("! true | false\n");
		REQUIRE(tree->type == Node::Type::Not);
		REQUIRE(as<NotNode>(tree.get())->com->type == Node::Type::Pipe);
	}
}

TEST_CASE("AST: and / or lists", "[parse][ast]")
{
	SECTION("&&") {
		auto tree = parse_one("true && echo ok\n");
		REQUIRE(tree->type == Node::Type::And);
		auto *bin = as<BinaryNode>(tree.get());
		REQUIRE(as_cmd(bin->ch1.get()));
		REQUIRE(as_cmd(bin->ch2.get()));
	}
	SECTION("||") {
		auto tree = parse_one("false || echo ok\n");
		REQUIRE(tree->type == Node::Type::Or);
		auto *bin = as<BinaryNode>(tree.get());
		REQUIRE(as_cmd(bin->ch1.get()));
		REQUIRE(as_cmd(bin->ch2.get()));
	}
	SECTION("&& and || associate left-to-right") {
		auto tree = parse_one("true && false || echo x\n");
		REQUIRE(tree->type == Node::Type::Or);
		auto *or_node = as<BinaryNode>(tree.get());
		REQUIRE(or_node->ch1->type == Node::Type::And);
		REQUIRE(as_cmd(or_node->ch2.get()));
	}
}

TEST_CASE("AST: subshell vs brace group", "[parse][ast]")
{
	SECTION("subshell") {
		auto tree = parse_one("(echo a; echo b)\n");
		REQUIRE(tree->type == Node::Type::Subshell);
		auto *redir = as<RedirNode>(tree.get());
		REQUIRE(redir->n->type == Node::Type::Semi);
	}
	SECTION("brace group is bare Semi (no Brace node)") {
		auto tree = parse_one("{ echo a; echo b; }\n");
		REQUIRE(tree->type == Node::Type::Semi);
		auto *bin = as<BinaryNode>(tree.get());
		REQUIRE(as_cmd(bin->ch1.get()));
		REQUIRE(as_cmd(bin->ch2.get()));
	}
}

TEST_CASE("AST: if / elif / else", "[parse][ast]")
{
	auto tree = parse_one(
		"if true; then echo yes; elif false; then echo maybe; else echo no; fi\n");
	REQUIRE(tree->type == Node::Type::If);

	auto *iff = as<IfNode>(tree.get());
	REQUIRE(as_cmd(iff->test.get()));
	REQUIRE(nth_arg(as_cmd(iff->test.get())->args, 0)->text == "true");
	REQUIRE(as_cmd(iff->ifpart.get()));
	REQUIRE(nth_arg(as_cmd(iff->ifpart.get())->args, 1)->text == "yes");

	REQUIRE(iff->elsepart);
	REQUIRE(iff->elsepart->type == Node::Type::If);
	auto *elif = as<IfNode>(iff->elsepart.get());
	REQUIRE(nth_arg(as_cmd(elif->test.get())->args, 0)->text == "false");
	REQUIRE(nth_arg(as_cmd(elif->ifpart.get())->args, 1)->text == "maybe");
	REQUIRE(as_cmd(elif->elsepart.get()));
	REQUIRE(nth_arg(as_cmd(elif->elsepart.get())->args, 1)->text == "no");
}

TEST_CASE("AST: while and until", "[parse][ast]")
{
	SECTION("while") {
		auto tree = parse_one("while false; do echo x; done\n");
		REQUIRE(tree->type == Node::Type::While);
		auto *bin = as<BinaryNode>(tree.get());
		REQUIRE(as_cmd(bin->ch1.get()));
		REQUIRE(as_cmd(bin->ch2.get()));
		REQUIRE(nth_arg(as_cmd(bin->ch1.get())->args, 0)->text == "false");
		REQUIRE(nth_arg(as_cmd(bin->ch2.get())->args, 1)->text == "x");
	}
	SECTION("until") {
		auto tree = parse_one("until true; do echo x; done\n");
		REQUIRE(tree->type == Node::Type::Until);
		auto *bin = as<BinaryNode>(tree.get());
		REQUIRE(as_cmd(bin->ch1.get()));
		REQUIRE(as_cmd(bin->ch2.get()));
	}
}

TEST_CASE("AST: for loop", "[parse][ast]")
{
	auto tree = parse_one("for i in a b c; do echo \"$i\"; done\n");
	REQUIRE(tree->type == Node::Type::For);

	auto *fr = as<ForNode>(tree.get());
	REQUIRE(fr->var == "i");

	REQUIRE(fr->args.size() == 3);
	REQUIRE(nth_arg(fr->args, 0)->text == "a");
	REQUIRE(nth_arg(fr->args, 1)->text == "b");
	REQUIRE(nth_arg(fr->args, 2)->text == "c");

	auto *body = as_cmd(fr->body.get());
	REQUIRE(body);
	REQUIRE(nth_arg(body->args, 0)->text == "echo");
	REQUIRE(nth_arg(body->args, 1)->text == ctl_quoted(ctl_var_normal("i")));
}

TEST_CASE("AST: case statement", "[parse][ast]")
{
	auto tree = parse_one("case x in y|z) echo y ;; *) echo other ;; esac\n");
	REQUIRE(tree->type == Node::Type::Case);

	auto *cs = as<CaseNode>(tree.get());
	REQUIRE(as<ArgNode>(cs->expr.get())->text == "x");
	REQUIRE(cs->cases.size() == 2);

	auto *c0 = as<CaseListNode>(cs->cases[0]);
	REQUIRE(c0);
	REQUIRE(c0->pattern.size() == 2);
	REQUIRE(nth_arg(c0->pattern, 0)->text == "y");
	REQUIRE(nth_arg(c0->pattern, 1)->text == "z");
	REQUIRE(nth_arg(as_cmd(c0->body.get())->args, 1)->text == "y");

	auto *c1 = as<CaseListNode>(cs->cases[1]);
	REQUIRE(c1);
	REQUIRE(c1->pattern.size() == 1);
	REQUIRE(nth_arg(c1->pattern, 0)->text == "*");
	REQUIRE(nth_arg(as_cmd(c1->body.get())->args, 1)->text == "other");
}

TEST_CASE("AST: function definition", "[parse][ast]")
{
	auto tree = parse_one("f() { echo hi; }\n");
	REQUIRE(tree->type == Node::Type::Defun);

	auto *fn = as<DefunNode>(tree.get());
	REQUIRE(fn->text == "f");
	auto *body = as_cmd(fn->body.get());
	REQUIRE(body);
	REQUIRE(nth_arg(body->args, 0)->text == "echo");
	REQUIRE(nth_arg(body->args, 1)->text == "hi");
}

TEST_CASE("AST: redirections", "[parse][ast]")
{
	SECTION("stdout redirect") {
		auto tree = parse_one("echo hi > /dev/null\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd);
		REQUIRE(cmd->redirect.size() == 1);
		REQUIRE(cmd->redirect[0]->type == Node::Type::To);

		auto *file = as<FileNode>(cmd->redirect[0]);
		REQUIRE(file->fd == 1);
		REQUIRE(as<ArgNode>(file->fname.get())->text == "/dev/null");
	}
	SECTION("fd dup and close") {
		auto tree = parse_one("echo hi > /dev/null 2>&1\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd);
		REQUIRE(cmd->redirect.size() == 2);

		auto *to = as<FileNode>(cmd->redirect[0]);
		REQUIRE(to->type == Node::Type::To);
		REQUIRE(cmd->redirect[1]->type == Node::Type::ToFd);

		auto *dup = as<DupNode>(cmd->redirect[1]);
		REQUIRE(dup->fd == 2);
		REQUIRE(dup->dupfd == 1);
	}
	SECTION("append") {
		auto tree = parse_one("echo hi >> log\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd->redirect.size() == 1);
		REQUIRE(cmd->redirect[0]->type == Node::Type::Append);
		REQUIRE(as<FileNode>(cmd->redirect[0])->fd == 1);
	}
}

TEST_CASE("AST: heredoc Here vs XHere", "[parse][ast]")
{
	SECTION("unquoted delimiter expands (XHere)") {
		auto tree = parse_one("cat <<END\nhello\nEND\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd);
		REQUIRE(cmd->redirect.size() == 1);
		REQUIRE(cmd->redirect[0]->type == Node::Type::XHere);

		auto *here = as<HereNode>(cmd->redirect[0]);
		REQUIRE(here->fd == 0);
		REQUIRE(as<ArgNode>(here->doc.get())->text == "hello\n");
	}
	SECTION("quoted delimiter is literal (Here)") {
		auto tree = parse_one("cat <<'END'\nhello\nEND\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd->redirect.size() == 1);
		REQUIRE(cmd->redirect[0]->type == Node::Type::Here);
		REQUIRE(as<ArgNode>(as<HereNode>(cmd->redirect[0])->doc.get())->text
			== "hello\n");
	}
}

TEST_CASE("AST: quotes and expansions in Arg::text", "[parse][ast]")
{
	SECTION("single and double quoted literals") {
		auto tree = parse_one("echo 'single' \"double\"\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 1)->text == ctl_quoted("single"));
		REQUIRE(nth_arg(cmd->args, 2)->text == ctl_quoted("double"));
	}
	SECTION("parameter expansion") {
		auto tree = parse_one("echo $HOME \"${HOME:-x}\"\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 1)->text == ctl_var_normal("HOME"));
		REQUIRE(nth_arg(cmd->args, 2)->text
			== ctl_quoted(ctl_var(VSMINUS | VSNUL, "HOME", "x")));
	}
	SECTION("command substitution stores tree in backquote") {
		auto tree = parse_one("echo $(echo nested)\n");
		auto *cmd = as_cmd(tree.get());
		auto *arg = nth_arg(cmd->args, 1);
		REQUIRE(arg->text == ctl_backq());
		REQUIRE(arg->backquote.size() == 1);
		auto *inner = as_cmd(arg->backquote[0].get());
		REQUIRE(inner);
		REQUIRE(nth_arg(inner->args, 0)->text == "echo");
		REQUIRE(nth_arg(inner->args, 1)->text == "nested");
	}
	SECTION("arithmetic expansion") {
		auto tree = parse_one("echo $((1+2))\n");
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 1)->text == ctl_arith("1+2"));
	}
}

TEST_CASE("AST: background job", "[parse][ast]")
{
	auto tree = parse_one("sleep 1 &\n");
	REQUIRE(tree->type == Node::Type::Backgnd);
	auto *bg = as<RedirNode>(tree.get());
	REQUIRE(as_cmd(bg->n.get()));
	REQUIRE(nth_arg(as_cmd(bg->n.get())->args, 0)->text == "sleep");
}

TEST_CASE("AST: alias expansion", "[parse][ast][alias]")
{
	SECTION("command word expands to multiple words") {
		auto tree = parse_one("ll\n", {{"ll", "ls -l"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(cmd);
		REQUIRE(nth_arg(cmd->args, 0)->text == "ls");
		REQUIRE(nth_arg(cmd->args, 1)->text == "-l");
		REQUIRE_FALSE(nth_arg(cmd->args, 2));
	}

	SECTION("arguments after the command are preserved") {
		auto tree = parse_one("ll /tmp\n", {{"ll", "ls -l"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == "ls");
		REQUIRE(nth_arg(cmd->args, 1)->text == "-l");
		REQUIRE(nth_arg(cmd->args, 2)->text == "/tmp");
	}

	SECTION("alias is not expanded in argument position") {
		auto tree = parse_one("echo ll\n", {{"ll", "ls -l"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == "echo");
		REQUIRE(nth_arg(cmd->args, 1)->text == "ll");
		REQUIRE_FALSE(nth_arg(cmd->args, 2));
	}

	SECTION("quoted command word is not expanded") {
		auto tree = parse_one("\"ll\"\n", {{"ll", "ls -l"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == ctl_quoted("ll"));
		REQUIRE_FALSE(nth_arg(cmd->args, 1));
	}

	SECTION("trailing blank re-enables alias on the next word") {
		auto tree = parse_one("foo bar\n",
				     {{"foo", "echo "}, {"bar", "hi"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == "echo");
		REQUIRE(nth_arg(cmd->args, 1)->text == "hi");
		REQUIRE_FALSE(nth_arg(cmd->args, 2));
	}

	SECTION("without trailing blank the next word is not aliased") {
		auto tree = parse_one("foo bar\n",
				     {{"foo", "echo"}, {"bar", "hi"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == "echo");
		REQUIRE(nth_arg(cmd->args, 1)->text == "bar");
		REQUIRE_FALSE(nth_arg(cmd->args, 2));
	}

	SECTION("alias expands after a pipe") {
		auto tree = parse_one("true | ll\n", {{"ll", "cat"}});
		REQUIRE(tree->type == Node::Type::Pipe);
		auto *pipe = as<PipeNode>(tree.get());
		REQUIRE(pipe->cmdlist.size() == 2);
		REQUIRE(as_cmd(pipe->cmdlist[1].get()));
		REQUIRE(nth_arg(as_cmd(pipe->cmdlist[1].get())->args, 0)->text
			== "cat");
	}

	SECTION("alias expands after assignments") {
		auto tree = parse_one("FOO=bar ll\n", {{"ll", "echo ok"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(as<ArgNode>(cmd->assign[0])->text == "FOO=bar");
		REQUIRE(nth_arg(cmd->args, 0)->text == "echo");
		REQUIRE(nth_arg(cmd->args, 1)->text == "ok");
	}

	SECTION("in-use alias is not expanded recursively") {
		auto tree = parse_one("x\n", {{"x", "x y"}});
		auto *cmd = as_cmd(tree.get());
		REQUIRE(nth_arg(cmd->args, 0)->text == "x");
		REQUIRE(nth_arg(cmd->args, 1)->text == "y");
		REQUIRE_FALSE(nth_arg(cmd->args, 2));
	}

	SECTION("alias may expand into a pipeline") {
		auto tree = parse_one("p\n", {{"p", "echo a | cat"}});
		REQUIRE(tree->type == Node::Type::Pipe);
		auto *pipe = as<PipeNode>(tree.get());
		REQUIRE(pipe->cmdlist.size() == 2);
		REQUIRE(nth_arg(as_cmd(pipe->cmdlist[0].get())->args, 0)->text
			== "echo");
		REQUIRE(nth_arg(as_cmd(pipe->cmdlist[0].get())->args, 1)->text
			== "a");
		REQUIRE(nth_arg(as_cmd(pipe->cmdlist[1].get())->args, 0)->text
			== "cat");
	}
}
