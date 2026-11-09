/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)error.h	8.2 (Berkeley) 5/4/95
 */
#pragma once

#include <csignal>
#include <format>
#include <string>
#include <type_traits>
#include <utility>

#include <libshell.hpp>

/*
 * Types of operations (passed to the errmsg routine).
 */


namespace shell {

#define E_OPEN 01	/* opening a file */
#define E_CREAT 02	/* creating a file */
#define E_EXEC 04	/* executing a program */

/*
 * Interrupt deferral macros require Context &sh in scope.
 * INTON/FORCEINTON return from the enclosing ShellResult
 * function when a deferred SIGINT must unwind (via onint → exraise).
 */
tl::unexpected<Error> onint(Context &sh);

#define barrier() ({ __asm__ __volatile__ ("": : :"memory"); })
#define INTOFF(sh) \
	({ \
		(sh).suppressint++; \
		barrier(); \
		0; \
	})
#ifdef REALLY_SMALL
Result<void> __inton(Context &sh);
#define INTON(sh) \
	do { \
		if (const auto _r = __inton((sh)); !_r) \
			return _r.error(); \
	} while (0)
#else
#define INTON(sh) \
	do { \
		barrier(); \
		if (--(sh).suppressint == 0 && (sh).intpending) \
			return onint((sh)); \
	} while (0)
#endif
#define FORCEINTON(sh) \
	do { \
		barrier(); \
		(sh).suppressint = 0; \
		if ((sh).intpending) \
			return onint(sh); \
	} while (0)

#define int_pending(sh) (sh).intpending

/* For void helpers that cannot return ShellResult: drop suppressint and
 * leave intpending set so a later ShellResult INTON/FORCEINTON unwinds. */
#define INTON_DEFER(sh) \
	do { \
		barrier(); \
		--(sh).suppressint; \
	} while (0)

/**
 * RAII interrupt deferral. Destructor matches INTON_DEFER; call resume()
 * for full INTON semantics (may return onint).
 */
class InterruptSuppress {
	Context &sh_;
	bool active_ = true;

public:
	explicit InterruptSuppress(Context &sh);
	InterruptSuppress(const InterruptSuppress &) = delete;
	InterruptSuppress &operator=(const InterruptSuppress &) = delete;
	~InterruptSuppress();

	Result<void> resume();
};

/* Avoid `.error()` in macros: bltin.h `#define error(...)` would rewrite it. */
template <typename T>
inline tl::unexpected<Error>
shell_fail_of(const Result<T> &r)
{
	return tl::unexpected(r.error());
}

/* Propagate a nested ShellResult; for void results use TRY_SHV.
 * Returns tl::unexpected so the enclosing ShellResult<T> may differ in T. */
#define TRY_SH(expr) \
	({ \
		auto _try_sh_r = (expr); \
		if (!_try_sh_r) \
			return shell_fail_of(_try_sh_r); \
		*_try_sh_r; \
	})
#define TRY_SHV(expr) \
	do { \
		if (auto _try_sh_r = (expr); !_try_sh_r) \
			return shell_fail_of(_try_sh_r); \
	} while (0)

template <typename F>
auto with_interrupts_off(Context &sh, F &&f) -> decltype(f())
{
	InterruptSuppress guard(sh);
	if constexpr (std::is_void_v<decltype(f())>) {
		std::forward<F>(f)();
		/* Destructor unlocks as INTON_DEFER. */
	} else {
		auto result = std::forward<F>(f)();
		if (auto resumed = guard.resume(); !resumed)
			return shell_fail_of(resumed);
		return result;
	}
}

tl::unexpected<Error> exraise(Context &sh, Error);
tl::unexpected<Error> sh_error_msg(Context &sh, std::string msg);
tl::unexpected<Error> exerror_msg(Context &sh, Error cond,
					 std::string msg);

template <typename... Args>
tl::unexpected<Error>
sh_error(Context &sh, std::format_string<Args...> fmt, Args &&...args)
{
	return sh_error_msg(sh, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
tl::unexpected<Error>
exerror(Context &sh, Error cond, std::format_string<Args...> fmt,
	Args &&...args)
{
	return exerror_msg(sh, cond,
			   std::format(fmt, std::forward<Args>(args)...));
}

const char *errmsg(int, int);

void sh_warnx_msg(Context &sh, std::string msg);

template <typename... Args>
void
sh_warnx(Context &sh, std::format_string<Args...> fmt, Args &&...args)
{
	sh_warnx_msg(sh, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace shell
