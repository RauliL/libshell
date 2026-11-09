#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "var.h"


using namespace shell;
namespace {

struct EvalFixture {
	Context sh;

	EvalFixture()
	{
		sh.arg0 = "arith_tests";
	}

	std::intmax_t eval(std::string_view expr)
	{
		auto r = sh.arith(expr);
		REQUIRE(r);
		return *r;
	}

	Result<std::intmax_t> eval_result(std::string_view expr)
	{
		return sh.arith(expr);
	}

	std::intmax_t var_int(std::string_view name)
	{
		auto r = sh.get_int_var(name);
		REQUIRE(r);
		return *r;
	}
};

} // namespace

TEST_CASE_METHOD(EvalFixture, "arith eval: numeric literal", "[arith][eval]")
{
	CHECK(eval("0") == 0);
	CHECK(eval("42") == 42);
	CHECK(eval("-7") == -7);
	CHECK(eval(std::to_string(INTMAX_MAX)) == INTMAX_MAX);
}

TEST_CASE_METHOD(EvalFixture, "arith eval: unary operators", "[arith][eval]")
{
	CHECK(eval("-5") == -5);
	CHECK(eval("-(-3)") == 3);
	CHECK(eval("!0") == 1);
	CHECK(eval("!1") == 0);
	CHECK(eval("!99") == 0);
	CHECK(eval("~0") == ~intmax_t{0});
	CHECK(eval("~1") == ~intmax_t{1});
}

TEST_CASE_METHOD(EvalFixture, "arith eval: binary arithmetic", "[arith][eval]")
{
	CHECK(eval("2+3") == 5);
	CHECK(eval("10-4") == 6);
	CHECK(eval("6*7") == 42);
	CHECK(eval("20/4") == 5);
	CHECK(eval("20%6") == 2);
	CHECK(eval("1<<4") == 16);
	CHECK(eval("32>>3") == 4);
}

TEST_CASE_METHOD(EvalFixture, "arith eval: comparisons", "[arith][eval]")
{
	CHECK(eval("1<2") == 1);
	CHECK(eval("2<1") == 0);
	CHECK(eval("2<=2") == 1);
	CHECK(eval("3>1") == 1);
	CHECK(eval("3>=3") == 1);
	CHECK(eval("5==5") == 1);
	CHECK(eval("5==6") == 0);
	CHECK(eval("5!=6") == 1);
}

TEST_CASE_METHOD(EvalFixture, "arith eval: bitwise operators", "[arith][eval]")
{
	CHECK(eval("12&10") == 8);   /* 0b1100 & 0b1010 */
	CHECK(eval("12|10") == 14);  /* 0b1100 | 0b1010 */
	CHECK(eval("12^10") == 6);   /* 0b1100 ^ 0b1010 */
}

TEST_CASE_METHOD(EvalFixture, "arith eval: division errors", "[arith][eval]")
{
	CHECK_FALSE(eval_result("1/0"));
	CHECK_FALSE(eval_result("1%0"));

	REQUIRE(sh.set_int_var("v", INTMAX_MIN));
	CHECK_FALSE(eval_result("v/-1"));
}

TEST_CASE_METHOD(EvalFixture, "arith eval: logical and/or with short-circuit",
		 "[arith][eval]")
{
	SECTION("&& short-circuits on false lhs")
	{
		REQUIRE(sh.set_int_var("x", 0));
		CHECK(eval("0&&(x=1)") == 0);
		CHECK(var_int("x") == 0);
	}

	SECTION("&& evaluates rhs when lhs is true")
	{
		REQUIRE(sh.set_int_var("x", 0));
		CHECK(eval("1&&(x=7)") == 1);
		CHECK(var_int("x") == 7);
	}

	SECTION("|| short-circuits on true lhs")
	{
		REQUIRE(sh.set_int_var("x", 0));
		CHECK(eval("1||(x=1)") == 1);
		CHECK(var_int("x") == 0);
	}

	SECTION("|| evaluates rhs when lhs is false")
	{
		REQUIRE(sh.set_int_var("x", 0));
		CHECK(eval("0||(x=9)") == 1);
		CHECK(var_int("x") == 9);
	}

	SECTION("logical values")
	{
		CHECK(eval("2&&3") == 1);
		CHECK(eval("2&&0") == 0);
		CHECK(eval("0||0") == 0);
		CHECK(eval("0||5") == 1);
	}
}

TEST_CASE_METHOD(EvalFixture, "arith eval: conditional with short-circuit",
		 "[arith][eval]")
{
	SECTION("true condition takes then-branch")
	{
		REQUIRE(sh.set_int_var("t", 0));
		REQUIRE(sh.set_int_var("e", 0));
		CHECK(eval("1?(t=3):(e=4)") == 3);
		CHECK(var_int("t") == 3);
		CHECK(var_int("e") == 0);
	}

	SECTION("false condition takes else-branch")
	{
		REQUIRE(sh.set_int_var("t", 0));
		REQUIRE(sh.set_int_var("e", 0));
		CHECK(eval("0?(t=3):(e=4)") == 4);
		CHECK(var_int("t") == 0);
		CHECK(var_int("e") == 4);
	}
}

TEST_CASE_METHOD(EvalFixture, "arith eval: variables", "[arith][eval]")
{
	SECTION("unset variable reads as zero")
	{
		CHECK(eval("unset_var_xyz") == 0);
	}

	SECTION("lookup of set variable")
	{
		REQUIRE(sh.set_int_var("n", 42));
		CHECK(eval("n") == 42);
	}
}

TEST_CASE_METHOD(EvalFixture, "arith eval: assignment", "[arith][eval]")
{
	SECTION("simple assign")
	{
		CHECK(eval("a=11") == 11);
		CHECK(var_int("a") == 11);
	}

	SECTION("compound assignments")
	{
		struct Case {
			const char *expr;
			intmax_t initial;
			intmax_t expected;
		};
		auto c = GENERATE(
			Case{"v+=3", 10, 13},
			Case{"v-=3", 10, 7},
			Case{"v*=3", 10, 30},
			Case{"v/=4", 20, 5},
			Case{"v%=6", 20, 2},
			Case{"v<<=3", 1, 8},
			Case{"v>>=2", 32, 8},
			Case{"v&=10", 15, 10},  /* 0b1111 &= 0b1010 */
			Case{"v|=3", 8, 11},   /* 0b1000 |= 0b0011 */
			Case{"v^=10", 12, 6}); /* 0b1100 ^= 0b1010 */

		REQUIRE(sh.set_int_var("v", c.initial));
		CHECK(eval(c.expr) == c.expected);
		CHECK(var_int("v") == c.expected);
	}

	SECTION("compound assign division by zero")
	{
		REQUIRE(sh.set_int_var("v", 5));
		CHECK_FALSE(eval_result("v/=0"));
		CHECK(var_int("v") == 5);
	}
}

TEST_CASE_METHOD(EvalFixture, "arith eval: nested expressions", "[arith][eval]")
{
	CHECK(eval("(2+3)*(10-4)") == 30);
	CHECK(eval("-((1+2)*3)") == -9);

	REQUIRE(sh.set_int_var("a", 2));
	CHECK(eval("a+=3*4") == 14);
	CHECK(var_int("a") == 14);
}

TEST_CASE_METHOD(EvalFixture, "arith eval: empty expression fails", "[arith][eval]")
{
	CHECK_FALSE(eval_result(""));
}
