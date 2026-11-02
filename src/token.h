#pragma once

#include <cstdint>
#include <type_traits>

/*
 * Parser token codes. Numeric values and order must stay stable:
 * keyword tokens start at Token::Not and align with parsekwd[].
 */
enum class Token : std::uint8_t {
	Eof = 0,
	Blank = 1,
	Nl = 2,
	Semi = 3,
	Backgnd = 4,
	And = 5,
	Or = 6,
	Pipe = 7,
	Lp = 8,
	Rp = 9,
	Endcase = 10,
	Endbquote = 11,
	Redir = 12,
	Word = 13,
	Not = 14,
	Case = 15,
	Do = 16,
	Done = 17,
	Elif = 18,
	Else = 19,
	Esac = 20,
	Fi = 21,
	For = 22,
	If = 23,
	In = 24,
	Then = 25,
	Until = 26,
	While = 27,
	Begin = 28,
	End = 29,
};
