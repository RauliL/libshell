#pragma once

#include <array>
#include <string_view>
#include <unordered_map>

#include "token.h"

/*
 * Token metadata tables. Intended for a single translation unit
 * (parser.cpp); arrays are file-scope static.
 */

/* Array indicating which tokens mark the end of a list */
static const std::array<bool, 30> tokendlist = {
	1, /* Eof */
	0, /* Blank */
	0, /* Nl */
	0, /* Semi */
	0, /* Backgnd */
	0, /* And */
	0, /* Or */
	0, /* Pipe */
	0, /* Lp */
	1, /* Rp */
	1, /* Endcase */
	1, /* Endbquote */
	0, /* Redir */
	0, /* Word */
	0, /* Not */
	0, /* Case */
	1, /* Do */
	1, /* Done */
	1, /* Elif */
	1, /* Else */
	1, /* Esac */
	1, /* Fi */
	0, /* For */
	0, /* If */
	0, /* In */
	1, /* Then */
	0, /* Until */
	0, /* While */
	0, /* Begin */
	1, /* End */
};

static const std::array<const char *, 30> tokname = {
	"end of file",
	"blank",
	"newline",
	"\";\"",
	"\"&\"",
	"\"&&\"",
	"\"||\"",
	"\"|\"",
	"\"(\"",
	"\")\"",
	"\";;\"",
	"\"`\"",
	"redirection",
	"word",
	"\"!\"",
	"\"case\"",
	"\"do\"",
	"\"done\"",
	"\"elif\"",
	"\"else\"",
	"\"esac\"",
	"\"fi\"",
	"\"for\"",
	"\"if\"",
	"\"in\"",
	"\"then\"",
	"\"until\"",
	"\"while\"",
	"\"{\"",
	"\"}\"",
};

static const std::unordered_map<std::string_view, Token> parsekwd = {
	{ "!", Token::Not },
	{ "case", Token::Case },
	{ "do", Token::Do },
	{ "done", Token::Done },
	{ "elif", Token::Elif },
	{ "else", Token::Else },
	{ "esac", Token::Esac },
	{ "fi", Token::Fi },
	{ "for", Token::For },
	{ "if", Token::If },
	{ "in", Token::In },
	{ "then", Token::Then },
	{ "until", Token::Until },
	{ "while", Token::While },
	{ "{", Token::Begin },
	{ "}", Token::End },
};
