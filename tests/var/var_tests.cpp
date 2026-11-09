#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "shell_context.h"

namespace {

struct VarFixture {
	Context sh{Context::SignalOwnership::Leave};

	VarFixture()
	{
		sh.arg0 = "var_tests";
	}
};

} // namespace

TEST_CASE_METHOD(VarFixture, "var: set and get string values", "[var]")
{
	REQUIRE_FALSE(sh.get_var("FOO").has_value());

	REQUIRE(sh.set_var("FOO", "bar"));
	auto v = sh.get_var("FOO");
	REQUIRE(v);
	CHECK(*v == "bar");

	REQUIRE(sh.set_var("FOO", "baz"));
	v = sh.get_var("FOO");
	REQUIRE(v);
	CHECK(*v == "baz");
}

TEST_CASE_METHOD(VarFixture, "var: empty string is set, not unset", "[var]")
{
	REQUIRE(sh.set_var("EMPTY", ""));
	auto v = sh.get_var("EMPTY");
	REQUIRE(v);
	CHECK(v->empty());
}

TEST_CASE_METHOD(VarFixture, "var: set_var with nullopt removes the variable", "[var]")
{
	REQUIRE(sh.set_var("TMP", "1"));
	REQUIRE(sh.get_var("TMP"));

	REQUIRE(sh.set_var("TMP", std::nullopt));
	CHECK_FALSE(sh.get_var("TMP").has_value());
	CHECK(sh.variables.find("TMP") == sh.variables.end());
}

TEST_CASE_METHOD(VarFixture, "var: integer set and get", "[var]")
{
	REQUIRE(sh.set_int_var("N", 42));
	auto v = sh.get_var("N");
	REQUIRE(v);
	CHECK(*v == "42");

	auto n = sh.get_int_var("N");
	REQUIRE(n);
	CHECK(*n == 42);

	REQUIRE(sh.set_int_var("N", -7));
	n = sh.get_int_var("N");
	REQUIRE(n);
	CHECK(*n == -7);
}

TEST_CASE_METHOD(VarFixture, "var: get_int_var on unset yields zero", "[var]")
{
	auto n = sh.get_int_var("MISSING");
	REQUIRE(n);
	CHECK(*n == 0);
}

TEST_CASE_METHOD(VarFixture, "var: bad names are rejected", "[var]")
{
	CHECK_FALSE(sh.set_var(""));
	CHECK_FALSE(sh.set_var("1bad"));
	CHECK_FALSE(sh.set_var("bad-name"));
	CHECK_FALSE(sh.get_var("1bad").has_value());
}

TEST_CASE_METHOD(VarFixture, "var: readonly cannot be modified or removed", "[var]")
{
	using Flag = shell::Variable::Flag;

	REQUIRE(sh.set_var("RO", "once", Flag::ReadOnly));
	CHECK(sh.get_var("RO") == "once");

	CHECK_FALSE(sh.set_var("RO", "twice"));
	CHECK(sh.get_var("RO") == "once");

	CHECK_FALSE(sh.set_var("RO", std::nullopt));
	CHECK(sh.get_var("RO") == "once");
}

TEST_CASE_METHOD(VarFixture, "var: export flag is retained on the map entry", "[var]")
{
	using Flag = shell::Variable::Flag;

	REQUIRE(sh.set_var("EXP", "yes", Flag::Exported));
	auto it = sh.variables.find("EXP");
	REQUIRE(it != sh.variables.end());
	CHECK(it->second.flags.test(Flag::Exported));
	CHECK(sh.get_var("EXP") == "yes");
}

TEST_CASE_METHOD(VarFixture, "var: strfixed builtins stay mapped when unset", "[var]")
{
	using Flag = shell::Variable::Flag;

	REQUIRE(sh.get_var("PATH"));
	REQUIRE(sh.set_var("PATH", std::nullopt));
	CHECK_FALSE(sh.get_var("PATH").has_value());

	auto it = sh.variables.find("PATH");
	REQUIRE(it != sh.variables.end());
	CHECK(it->second.flags.test(Flag::StrFixed));
	CHECK(it->second.flags.test(Flag::Unset));

	REQUIRE(sh.set_var("PATH", "/tmp"));
	CHECK(sh.get_var("PATH") == "/tmp");
	CHECK_FALSE(it->second.flags.test(Flag::Unset));
}

TEST_CASE_METHOD(VarFixture, "var: values are visible to eval expansions", "[var]")
{
	std::string out;
	sh.set_stdout([&](std::string_view s) { out.append(s); });

	REQUIRE(sh.set_var("MSG", "hello"));
	REQUIRE(sh.eval("printf '%s\\n' \"$MSG\""));
	CHECK(out == "hello\n");

	out.clear();
	REQUIRE(sh.set_var("MSG", std::nullopt));
	REQUIRE(sh.eval("printf '%s\\n' \"${MSG-gone}\""));
	CHECK(out == "gone\n");
}
