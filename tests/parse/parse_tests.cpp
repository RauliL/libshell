#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>

#include "parse_helpers.hpp"


using namespace shell;
namespace fs = std::filesystem;

#ifndef PARSE_FIXTURE_DIR
#error "PARSE_FIXTURE_DIR must be defined"
#endif

static std::vector<std::string> list_scripts(const fs::path &dir)
{
	std::vector<std::string> files;
	if (!fs::exists(dir))
		return files;
	for (const auto &entry : fs::directory_iterator(dir)) {
		if (entry.is_regular_file() && entry.path().extension() == ".sh")
			files.push_back(entry.path().string());
	}
	std::sort(files.begin(), files.end());
	return files;
}

static std::string fixture_name(const std::string &path)
{
	return fs::path(path).filename().string();
}

TEST_CASE("valid POSIX shell scripts parse without executing", "[parse][valid]")
{
	const auto files = list_scripts(fs::path(PARSE_FIXTURE_DIR) / "valid");
	REQUIRE_FALSE(files.empty());

	const auto file = GENERATE_COPY(from_range(files));
	CAPTURE(fixture_name(file));
	INFO(file);
	REQUIRE(parse_test::parses_file_ok(file));
}

TEST_CASE("invalid shell scripts are rejected by the parser", "[parse][invalid]")
{
	const auto files = list_scripts(fs::path(PARSE_FIXTURE_DIR) / "invalid");
	REQUIRE_FALSE(files.empty());

	const auto file = GENERATE_COPY(from_range(files));
	CAPTURE(fixture_name(file));
	INFO(file);
	REQUIRE_FALSE(parse_test::parses_file_ok(file));
}

TEST_CASE("inline POSIX command snippets parse", "[parse][valid][inline]")
{
	const char *script = GENERATE(
		"echo hello",
		"FOO=bar echo ok",
		"echo a; echo b",
		"echo a | cat",
		"! false",
		"true && echo ok",
		"false || echo ok",
		"(echo a; echo b)",
		"{ echo a; echo b; }",
		"if true; then echo yes; else echo no; fi",
		"while false; do echo x; done",
		"until true; do echo x; done",
		"for i in a b c; do echo \"$i\"; done",
		"case x in y) echo y ;; *) echo other ;; esac",
		"f() { echo hi; }",
		"echo hi > /dev/null 2>&1",
		"cat <<END\nhello\nEND\n",
		"echo 'single' \"double\" escaped\\ word",
		"echo \"$HOME\" \"${HOME:-x}\" \"$(echo nested)\" $((1+2))",
		"# comment\necho ok\n"
	);

	CAPTURE(script);
	REQUIRE(parse_test::parses_ok(script));
}

TEST_CASE("inline invalid snippets are rejected", "[parse][invalid][inline]")
{
	const char *script = GENERATE(
		"if true; echo x; fi",
		"if true; then echo x",
		"while true; echo x; done",
		"for i in a; echo x; done",
		"case x in y) echo y ;;",
		"{ echo a; echo b;",
		"(echo a",
		"echo 'unterminated",
		"echo \"unterminated",
		"echo $(echo hi",
		"echo a |",
		"true &&",
		"echo hi >",
		"fi",
		"done",
		"esac",
		"&& echo x",
		"| echo x"
	);

	CAPTURE(script);
	REQUIRE_FALSE(parse_test::parses_ok(script));
}
