#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "config.hpp"
#include <libshell.hpp>

using namespace shell;

namespace {

struct BuiltinFixture {
	Context sh{Context::SignalOwnership::Leave};
	std::string out;
	std::string err;

	BuiltinFixture()
	{
		sh.arg0 = "builtin_tests";
		sh.set_stdout([&](std::string_view s) { out.append(s); });
		sh.set_stderr([&](std::string_view s) { err.append(s); });
	}

	void clear_io()
	{
		out.clear();
		err.clear();
	}

	int run(const std::string &script)
	{
		auto r = sh.eval(script);
		REQUIRE(r);
		return *r;
	}

	Result<int> run_result(const std::string &script)
	{
		return sh.eval(script);
	}
};

} // namespace

TEST_CASE_METHOD(BuiltinFixture, "builtin: true / false / colon", "[builtin]")
{
	CHECK(run("true") == 0);
	CHECK(run("false") == 1);
	CHECK(run(":") == 0);
	CHECK(run(": ignored args") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: echo", "[builtin]")
{
	CHECK(run("echo hello") == 0);
	CHECK(out == "hello\n");

	clear_io();
	CHECK(run("echo -n no-nl") == 0);
	CHECK(out == "no-nl");

	clear_io();
	CHECK(run("echo a b c") == 0);
	CHECK(out == "a b c\n");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: printf", "[builtin]")
{
	CHECK(run("printf '%s\\n' hi") == 0);
	CHECK(out == "hi\n");

	clear_io();
	CHECK(run("printf '%d' 42") == 0);
	CHECK(out == "42");

	clear_io();
	CHECK(run("printf '%s %s\\n' foo bar") == 0);
	CHECK(out == "foo bar\n");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: test / [ string and integer", "[builtin]")
{
	CHECK(run("test a = a") == 0);
	CHECK(run("test a != b") == 0);
	CHECK(run("test a = b") == 1);
	CHECK(run("test -z ''") == 0);
	CHECK(run("test -n x") == 0);
	CHECK(run("test -z x") == 1);
	CHECK(run("test 3 -eq 3") == 0);
	CHECK(run("test 3 -lt 4") == 0);
	CHECK(run("test 4 -gt 3") == 0);
	CHECK(run("test 3 -ne 4") == 0);
	CHECK(run("[ 1 -le 1 ]") == 0);
	CHECK(run("[ 2 -ge 2 ]") == 0);
	CHECK(run("! test a = a") == 1);
	CHECK(run("! test a = b") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: pwd", "[builtin]")
{
	REQUIRE(sh.set_cwd("/tmp"));
	CHECK(run("pwd") == 0);
	CHECK(out == "/tmp\n");
	CHECK(sh.get_cwd() == "/tmp");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: export / readonly / unset", "[builtin]")
{
	CHECK(run("export FOO=bar") == 0);
	CHECK(sh.get_var("FOO") == "bar");
	auto it = sh.variables.find("FOO");
	REQUIRE(it != sh.variables.end());
	CHECK(it->second.flags.test(Variable::Flag::Exported));

	CHECK(run("readonly RO=once") == 0);
	CHECK(sh.get_var("RO") == "once");
	it = sh.variables.find("RO");
	REQUIRE(it != sh.variables.end());
	CHECK(it->second.flags.test(Variable::Flag::ReadOnly));

	CHECK(run("unset FOO") == 0);
	CHECK_FALSE(sh.get_var("FOO").has_value());

	clear_io();
	CHECK(run("readonly RO=twice") != 0);
	CHECK(sh.get_var("RO") == "once");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: set and shift", "[builtin]")
{
	CHECK(run("set -- one two three") == 0);
	REQUIRE(sh.shellparam.p.size() == 3);
	CHECK(sh.shellparam.p[0] == "one");
	CHECK(sh.shellparam.p[1] == "two");
	CHECK(sh.shellparam.p[2] == "three");

	clear_io();
	CHECK(run("printf '%s\\n' \"$1\" \"$2\" \"$3\"") == 0);
	CHECK(out == "one\ntwo\nthree\n");

	CHECK(run("shift") == 0);
	REQUIRE(sh.shellparam.p.size() == 2);
	CHECK(sh.shellparam.p[0] == "two");

	CHECK(run("shift 2") == 0);
	CHECK(sh.shellparam.p.empty());

	CHECK(run("set -e") == 0);
	CHECK(sh.get_option("errexit") == true);
	CHECK(run("set +e") == 0);
	CHECK(sh.get_option("errexit") == false);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: getopts", "[builtin]")
{
	CHECK(run("set -- -a -b val rest; getopts ab: opt; printf '%s\\n' \"$opt\"") == 0);
	CHECK(out == "a\n");
	CHECK(sh.get_var("opt") == "a");

	clear_io();
	CHECK(run("getopts ab: opt; printf '%s:%s\\n' \"$opt\" \"$OPTARG\"") == 0);
	CHECK(out == "b:val\n");
	CHECK(sh.get_var("opt") == "b");
	CHECK(sh.get_var("OPTARG") == "val");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: alias / unalias", "[builtin]")
{
	CHECK(run("alias greeter='echo hi'") == 0);
	REQUIRE(sh.aliases.count("greeter") == 1);
	CHECK(sh.aliases["greeter"]->value == "echo hi");

	clear_io();
	CHECK(run("alias greeter") == 0);
	CHECK(out.find("greeter=") != std::string::npos);

	clear_io();
	CHECK(run("greeter") == 0);
	CHECK(out == "hi\n");

	CHECK(run("unalias greeter") == 0);
	CHECK(sh.aliases.count("greeter") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: type and command -V", "[builtin]")
{
	CHECK(run("type true") == 0);
	CHECK(out.find("true") != std::string::npos);
	CHECK(out.find("builtin") != std::string::npos);

	clear_io();
	CHECK(run("command -V false") == 0);
	CHECK(out.find("false") != std::string::npos);
	CHECK(out.find("builtin") != std::string::npos);

	clear_io();
	CHECK(run("command true") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: hash", "[builtin]")
{
	CHECK(run("hash") == 0);
	CHECK(run("hash -r") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: times", "[builtin]")
{
	CHECK(run("times") == 0);
	// two lines: shell times, then children times
	REQUIRE(out.find('\n') != std::string::npos);
	CHECK(out.back() == '\n');
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: eval", "[builtin]")
{
	CHECK(run("eval \"printf '%s\\n' ok\"") == 0);
	CHECK(out == "ok\n");

	clear_io();
	CHECK(run("eval false") == 1);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: break / continue", "[builtin]")
{
	CHECK(run("i=0; while test $i -lt 5; do i=$((i + 1)); break; done; printf '%s\\n' \"$i\"") == 0);
	CHECK(out == "1\n");

	clear_io();
	CHECK(run(
		"i=0; n=0; while test $i -lt 5; do "
		"i=$((i + 1)); "
		"if test $i -eq 2; then continue; fi; "
		"n=$((n + 1)); "
		"done; printf '%s\\n' \"$n\""
	) == 0);
	CHECK(out == "4\n");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: return and local", "[builtin]")
{
	CHECK(run(
		"f() { local X=inner; printf '%s\\n' \"$X\"; return 7; }; "
		"X=outer; f; printf '%s:%s\\n' \"$?\" \"$X\""
	) == 0);
	CHECK(out == "inner\n7:outer\n");
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: trap list and assign", "[builtin]")
{
	CHECK(run("trap 'printf trapped\\n' USR1") == 0);
	clear_io();
	CHECK(run("trap") == 0);
	CHECK(out.find("USR1") != std::string::npos);

	CHECK(run("trap - USR1") == 0);
	clear_io();
	CHECK(run("trap") == 0);
	CHECK(out.find("USR1") == std::string::npos);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: wait with no jobs", "[builtin]")
{
	CHECK(run("wait") == 0);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: umask display", "[builtin]")
{
	CHECK(run("umask") == 0);
	REQUIRE_FALSE(out.empty());
	CHECK(out.back() == '\n');
	// octal mask: digits only before newline
	for (char c : out.substr(0, out.size() - 1))
		CHECK((c >= '0' && c <= '7'));
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: kill -l", "[builtin]")
{
	CHECK(run("kill -l") == 0);
	CHECK(out.find("TERM") != std::string::npos);
	CHECK(out.find("HUP") != std::string::npos);
}

TEST_CASE_METHOD(BuiltinFixture, "builtin: exit raises Error::Exit", "[builtin]")
{
	auto r = run_result("exit 42");
	REQUIRE_FALSE(r);
	CHECK(r.error() == Error::Exit);
	CHECK(sh.savestatus == 42);
}

#if HAVE_GETRLIMIT
TEST_CASE_METHOD(BuiltinFixture, "builtin: ulimit display", "[builtin]")
{
	CHECK(run("ulimit") == 0);
	REQUIRE_FALSE(out.empty());
}
#endif
