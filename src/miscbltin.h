#ifndef MISCBLTIN_H
#define MISCBLTIN_H

#include <libshell.hpp>

namespace shell {

Result<int> readcmd(Context &sh, BuiltinArgs);
Result<int> umaskcmd(Context &sh, BuiltinArgs);
#if HAVE_GETRLIMIT
Result<int> ulimitcmd(Context &sh, BuiltinArgs);
#endif

} // namespace shell

#endif
