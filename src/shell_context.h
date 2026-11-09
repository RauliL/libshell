#pragma once

#include <libshell.hpp>

/*
 * Compatibility helpers that still live outside libshell.hpp.
 * Context itself is defined in shell:: (see include/libshell.hpp).
 */

#include <sys/resource.h>

#include "alias.h"
#include "cd.h"
#include "error.h"
#include "exec.h"
#include "eval.h"
#include "expand.h"
#include "input.h"
#include "jobs.h"
#include "memalloc.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "redir.h"
#include "system.h"
#include "var.h"


/** Fill signal_map from host SIG* names (called during Context init). */

namespace shell {

void init_signal_names(Context &sh);

inline const char *bltinlookup(Context &sh, std::string_view name)
{
	if (auto value = sh.get_var(name))
		return value->data();
	return nullptr;
}

} // namespace shell
