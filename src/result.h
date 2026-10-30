#ifndef RESULT_H
#define RESULT_H

/*
 * Error-or-value results use TartanLlama expected (tl::expected).
 * Success: return the value (or {} for void).
 * Failure: return tl::unexpected(std::string{...}).
 */

#include <string>
#include <tl/expected.hpp>

namespace dash {

template <typename T>
using Result = tl::expected<T, std::string>;

} /* namespace dash */

#endif
