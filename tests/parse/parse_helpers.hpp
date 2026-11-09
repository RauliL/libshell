#pragma once

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "input.h"
#include "nodes.h"
#include "parse_error.h"
#include "parser.h"
#include "shell.h"
#include "shell_context.h"

namespace parse_test {

inline void init_shell(ShellContext &sh)
{
	sh.arg0 = "parse_tests";
}

inline bool parses_ok(std::string_view script)
{
	ShellContext sh;
	init_shell(sh);

	std::string copy(script);
	return static_cast<bool>(try_parse_string(sh, copy.data()));
}

inline bool parses_file_ok(const std::string &path)
{
	FILE *fp = fopen(path.c_str(), "r");
	if (!fp)
		return false;

	std::string contents;
	char buf[4096];
	while (size_t n = fread(buf, 1, sizeof(buf), fp))
		contents.append(buf, n);
	fclose(fp);

	return parses_ok(contents);
}

/* Parse a script into one tree per top-level command (blank lines are nullptr). */
inline std::vector<Node::ptr> parse_trees(std::string_view script)
{
	ShellContext sh;
	init_shell(sh);

	std::string copy(script);
	setinputstring(sh, copy.data());

	std::vector<Node::ptr> trees;
	try {
		for (;;) {
			ParseCmdResult result = parsecmd(sh, 0);
			if (result.eof)
				break;
			trees.push_back(std::move(result.tree));
		}
		popfile(sh);
	} catch (const shell::ParseError &) {
		popfile(sh);
		throw;
	}
	return trees;
}

/* Parse a script that should yield exactly one non-null tree. */
inline Node::ptr parse_one(std::string_view script)
{
	auto trees = parse_trees(script);
	if (trees.size() != 1 || !trees[0])
		throw std::runtime_error("expected exactly one non-null parse tree");
	return std::move(trees[0]);
}

/* --- CTL-encoded argument text helpers --- */

inline std::string ctl_quoted(std::string_view inner)
{
	std::string s;
	s.push_back(static_cast<char>(CTLQUOTEMARK));
	s.append(inner);
	s.push_back(static_cast<char>(CTLQUOTEMARK));
	return s;
}

inline std::string ctl_var(int subtype, std::string_view name,
			   std::optional<std::string_view> alt = std::nullopt)
{
	std::string s;
	s.push_back(static_cast<char>(CTLVAR));
	s.push_back(static_cast<char>(subtype | VSBIT));
	s.append(name);
	s.push_back('=');
	if (alt) {
		s.append(*alt);
		s.push_back(static_cast<char>(CTLENDVAR));
	}
	return s;
}

inline std::string ctl_var_normal(std::string_view name)
{
	return ctl_var(VSNORMAL, name);
}

inline std::string ctl_backq()
{
	return std::string(1, static_cast<char>(CTLBACKQ));
}

inline std::string ctl_arith(std::string_view expr)
{
	std::string s;
	s.push_back(static_cast<char>(CTLARI));
	s.append(expr);
	s.push_back(static_cast<char>(CTLENDARI));
	return s;
}

/* --- AST accessors --- */

inline Node::Arg *nth_arg(Node *args, int n)
{
	Node *cur = args;
	for (int i = 0; cur && i < n; ++i)
		cur = as<Node::Arg>(cur)->next.get();
	return cur ? as<Node::Arg>(cur) : nullptr;
}

inline Node::Command *as_cmd(Node *n)
{
	return n && n->type == Node::Type::Cmd ? as<Node::Command>(n) : nullptr;
}

} /* namespace parse_test */
