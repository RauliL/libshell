#ifndef PARSE_HELPERS_HPP
#define PARSE_HELPERS_HPP

#include <cstdio>
#include <string>
#include <string_view>

#include "shell.h"

namespace parse_test {

/*
 * Parse a shell script without executing it (equivalent to dash -n).
 * Returns true if the script parses successfully.
 * Syntax errors are reported via Result — no setjmp.
 */
inline bool parses_ok(std::string_view script)
{
	ShellContext sh;

	sh.mypid = sh.rootpid = getpid();
	sh.arg0 = const_cast<char *>("parse_tests");
	sh.init();

	std::string copy(script);
	return static_cast<bool>(sh.try_parse_string(copy.data()));
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

} /* namespace parse_test */

#endif
