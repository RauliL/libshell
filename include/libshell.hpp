#pragma once

#include <cstdint>

#include <tl/expected.hpp>

namespace shell {

/**
 * Enumeration of shell error codes.
 */
enum class Error : std::uint8_t {
	/** SIGINT received. */
	Interrupt = 0,
	/** A generic error. */
	Generic = 1,
	/** Exit the shell. */
	End = 3,
	/** Exit the shell via exit command. */
	Exit = 4,
};

template<typename T>
using Result = tl::expected<T, Error>;

}
