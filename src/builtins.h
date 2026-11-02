#pragma once

/*
 * Shell builtin command registry. Entries have static storage so pointer
 * identity can be used for a few special cases (command, eval, exec, …).
 */

#include <string_view>

#include "error.h"

struct ShellContext;

constexpr unsigned BUILTIN_SPECIAL = 0x1;
constexpr unsigned BUILTIN_REGULAR = 0x2;
constexpr unsigned BUILTIN_ASSIGN = 0x4;

struct builtincmd {
	const char *name;
	shell::Result<int> (*builtin)(ShellContext &, int, char **);
	unsigned flags;
};

const builtincmd *find_builtin(std::string_view name);

/* Stable identities used by eval / jobs / trap. */
extern const builtincmd builtin_command;
extern const builtincmd builtin_eval;
extern const builtincmd builtin_exec;
extern const builtincmd builtin_jobs;
extern const builtincmd builtin_trap;
