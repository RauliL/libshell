/*-
 * Copyright (c) 1993
 *	The Regents of the University of California. All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>. All rights reserved.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "error.h"
#include "mystring.h"
#include "alias.h"
#include "options.h"
#include "syntax.h"
#include <cstdlib>
#include <format>
#include <utility>

using namespace shell;

namespace shell {

namespace {
Result<void>
setalias(Context &sh, std::string_view name, std::string_view val)
{
	if (name.empty())
		return sh_error(sh, "Invalid alias name: {}", name);

	for (unsigned char c : name) {
		if (BASESYNTAX[static_cast<signed char>(c)] != CWORD)
			return sh_error(sh, "Invalid alias name: {}", name);
	}

	return with_interrupts_off(sh, [&]() -> Result<void> {
		std::string key(name);
		auto &slot = sh.aliases[key];

		if (!slot)
			slot = std::make_shared<Alias>();
		slot->name = std::move(key);
		slot->value = std::string(val);

		return {};
	});
}
} // namespace

Result<int>
unalias(Context &sh, std::string_view name)
{
	return with_interrupts_off(sh, [&]() -> Result<int> {
		const bool found = sh.aliases.erase(std::string(name)) != 0;

		return found ? EXIT_SUCCESS : EXIT_FAILURE;
	});
}

namespace {
Result<void>
rmaliases(Context &sh)
{
	return with_interrupts_off(sh, [&]() -> Result<void> {
		sh.aliases.clear();

		return {};
	});
}
} // namespace

std::shared_ptr<Alias>
lookupalias(Context &sh, std::string_view name, bool check)
{
	auto it = sh.aliases.find(std::string(name));

	if (it == sh.aliases.end() || (check && it->second->in_use))
		return nullptr;

	return it->second;
}

Result<int>
aliascmd(Context &sh, BuiltinArgs argv)
{
	int ret = EXIT_SUCCESS;

	if (argv.size() == 1) {
		for (const auto &entry : sh.aliases) {
			printalias(sh, *entry.second);
		}

		return EXIT_SUCCESS;
	}
	for (std::size_t i = 1; i < argv.size(); ++i) {
		auto n = argv[i];
		auto eq = n.find('=', 1);

		if (n.empty() || eq == std::string_view::npos) {
			const auto ap = lookupalias(sh, n, false);

			if (!ap) {
				sh.out2->write(sh, std::format("alias: {} not found\n", n));
				ret = EXIT_FAILURE;
			} else {
				printalias(sh, *ap);
			}
		} else {
			TRY_SHV(setalias(sh, n.substr(0, eq), n.substr(eq + 1)));
		}
	}

	return ret;
}

Result<int>
unaliascmd(Context &sh, BuiltinArgs)
{
	int i;
	int status = 0;

	for (;;) {
		TRY_SH_SET(i, nextopt(sh, "a"));
		if (i == '\0')
			break;
		if (i == 'a') {
			TRY_SHV(rmaliases(sh));

			return EXIT_SUCCESS;
		}
	}
	for (auto name : sh.bopt.remaining()) {
		int found;
		TRY_SH_SET(found, unalias(sh, name));
		if (found) {
			sh.out2->write(sh, std::format("unalias: {} not found\n", name));
			status = EXIT_FAILURE;
		}
	}

	return status;
}

void
printalias(Context &sh, const Alias &ap)
{
	sh.out1->write(
		sh,
		std::format("{}={}\n", ap.name, single_quote(ap.value))
	);
}

} // namespace shell
