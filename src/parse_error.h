#pragma once

#include <stdexcept>
#include <string>

namespace shell {

/* Thrown by the command parser instead of longjmp via sh_error. */
class ParseError : public std::runtime_error {
public:
	ParseError(std::string message, int lineno)
		: std::runtime_error(std::move(message))
		, lineno_(lineno) {}

	inline int lineno() const
	{
		return lineno_;
	}

private:
	int lineno_;
};

} /* namespace shell */
