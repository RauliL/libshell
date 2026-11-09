#pragma once

/*
 * Shell builtin command registry. Entries have static storage so pointer
 * identity can be used for a few special cases (command, eval, exec, …).
 */

#include <string_view>

#include "error.h"

const shell::BuiltinCommand *find_builtin(std::string_view name);

/* Stable identities used by eval / trap. */
extern const shell::BuiltinCommand builtin_command;
extern const shell::BuiltinCommand builtin_eval;
extern const shell::BuiltinCommand builtin_exec;
extern const shell::BuiltinCommand builtin_trap;
