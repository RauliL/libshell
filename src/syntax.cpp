/*-
 * Lexer / expander syntax tables. Built at compile time; replaces mksyntax.
 */
#include "parser.h"
#include "syntax.h"
#include <array>

using namespace shell;

namespace shell {

namespace {

/* Character-class bitflags (only used while building is_type). */
constexpr char ISDIGIT = 01;
constexpr char ISUPPER = 02;
constexpr char ISLOWER = 04;
constexpr char ISUNDER = 010;
constexpr char ISSPECL = 020;

constexpr void
set_chars(std::array<char, syn_table_size> &table, const char *chars, char type)
{
	for (; *chars; ++chars)
		table[static_cast<signed char>(*chars) + SYNBASE] = type;
}

constexpr void
or_chars(std::array<char, syn_table_size> &table, const char *chars, char type)
{
	for (; *chars; ++chars)
		table[static_cast<signed char>(*chars) + SYNBASE] |= type;
}

constexpr std::array<char, syn_table_size>
make_word_table(const char *specials, char special_type)
{
	std::array<char, syn_table_size> table{};
	table.fill(CWORD);
	table[0] = CEOF;
	for (int ctl = CTL_FIRST; ctl <= CTL_LAST; ++ctl)
		table[SYNBASE + ctl] = CCTL;
	set_chars(table, specials, special_type);
	return table;
}

constexpr std::array<char, syn_table_size>
make_basesyntax()
{
	auto table = make_word_table("\n", CNL);
	set_chars(table, "\\", CBACK);
	set_chars(table, "'", CSQUOTE);
	set_chars(table, "\"", CDQUOTE);
	set_chars(table, "`", CBQUOTE);
	set_chars(table, "$", CVAR);
	set_chars(table, "}", CENDVAR);
	set_chars(table, "<>();&| \t", CSPCL);
	return table;
}

constexpr std::array<char, syn_table_size>
make_dqsyntax()
{
	auto table = make_word_table("\n", CNL);
	set_chars(table, "\\", CBACK);
	set_chars(table, "\"", CENDQUOTE);
	set_chars(table, "`", CBQUOTE);
	set_chars(table, "$", CVAR);
	set_chars(table, "}", CENDVAR);
	/* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
	set_chars(table, "^!*?[=~:/-]", CCTL);
	return table;
}

constexpr std::array<char, syn_table_size>
make_sqsyntax()
{
	auto table = make_word_table("\n", CNL);
	set_chars(table, "'", CENDQUOTE);
	/* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
	set_chars(table, "^!*?[=~:/-]\\", CCTL);
	return table;
}

constexpr std::array<char, syn_table_size>
make_arisyntax()
{
	auto table = make_word_table("\n", CNL);
	set_chars(table, "\\", CBACK);
	set_chars(table, "`", CBQUOTE);
	set_chars(table, "$", CVAR);
	set_chars(table, "}", CENDVAR);
	set_chars(table, "(", CLP);
	set_chars(table, ")", CRP);
	return table;
}

constexpr std::array<char, syn_table_size>
make_is_type()
{
	std::array<char, syn_table_size> table{};
	or_chars(table, "0123456789", ISDIGIT);
	or_chars(table, "abcdefghijklmnopqrstuvwxyz", ISLOWER);
	or_chars(table, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", ISUPPER);
	or_chars(table, "_", ISUNDER);
	or_chars(table, "#?$!-*@", ISSPECL);
	return table;
}

} // namespace

const std::array<char, syn_table_size> basesyntax = make_basesyntax();
const std::array<char, syn_table_size> dqsyntax = make_dqsyntax();
const std::array<char, syn_table_size> sqsyntax = make_sqsyntax();
const std::array<char, syn_table_size> arisyntax = make_arisyntax();
const std::array<char, syn_table_size> is_type = make_is_type();

} // namespace shell
