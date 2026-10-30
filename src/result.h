#ifndef RESULT_H
#define RESULT_H

/*
 * Error-or-value results use TartanLlama expected (tl::expected).
 *
 * dash::Result<T>     — string error (arith / parse helpers)
 * dash::ShellResult<T> — shell unwind codes (replaces exraise / ShellError)
 */

#include <string>
#include <tl/expected.hpp>

namespace dash {

struct ShellErr {
	int code; /* EXINT | EXERROR | EXEND | EXEXIT */
};

template <typename T>
using Result = tl::expected<T, std::string>;

template <typename T>
using ShellResult = tl::expected<T, ShellErr>;

inline tl::unexpected<ShellErr> fail(int code)
{
	return tl::unexpected(ShellErr{code});
}

} /* namespace dash */

#endif
