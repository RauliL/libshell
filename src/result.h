#ifndef RESULT_H
#define RESULT_H

#include <string>
#include <utility>

namespace dash {

template <typename T>
class Result {
public:
	static Result Ok(T value) { return Result(std::move(value)); }
	static Result Err(std::string message)
	{
		Result r;
		r.error_ = std::move(message);
		return r;
	}

	explicit operator bool() const { return ok_; }
	bool has_value() const { return ok_; }

	T &value() { return value_; }
	const T &value() const { return value_; }
	T &operator*() { return value_; }
	const T &operator*() const { return value_; }

	const std::string &error() const { return error_; }

private:
	Result() = default;
	explicit Result(T value) : value_(std::move(value)), ok_(true) {}

	T value_{};
	std::string error_;
	bool ok_ = false;
};

template <>
class Result<void> {
public:
	static Result Ok()
	{
		Result r;
		r.ok_ = true;
		return r;
	}
	static Result Err(std::string message)
	{
		Result r;
		r.error_ = std::move(message);
		return r;
	}

	explicit operator bool() const { return ok_; }
	bool has_value() const { return ok_; }
	const std::string &error() const { return error_; }

private:
	std::string error_;
	bool ok_ = false;
};

} /* namespace dash */

#endif
