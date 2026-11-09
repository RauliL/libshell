/*
 * Builtin command table. Lookups use an unordered_map of name → entry.
 */
#include <unordered_map>

#include <libshell.hpp>

#include "alias.h"
#include "builtins.h"
#include "cd.h"
#include "eval.h"
#include "jobs.h"
#include "main.h"
#include "options.h"
#include "shell.h"
#include "trap.h"
#include "var.h"


using namespace shell;


namespace shell {

Result<int> echocmd(Context &sh, int, char **);
Result<int> printfcmd(Context &sh, int, char **);
Result<int> testcmd(Context &sh, int argc, char **argv);
Result<int> timescmd(Context &sh, int argc, char *argv[]);
Result<int> breakcmd(Context &sh, int argc, char **argv);
Result<int> returncmd(Context &sh, int argc, char **argv);
Result<int> falsecmd(Context &sh, int argc, char **argv);
Result<int> truecmd(Context &sh, int argc, char **argv);
Result<int> execcmd(Context &sh, int argc, char **argv);
Result<int> hashcmd(Context &sh, int argc, char **argv);
Result<int> typecmd(Context &sh, int argc, char **argv);
Result<int> commandcmd(Context &sh, int argc, char **argv);
Result<int> readcmd(Context &sh, int argc, char **argv);
Result<int> umaskcmd(Context &sh, int argc, char **argv);
#if HAVE_GETRLIMIT
Result<int> ulimitcmd(Context &sh, int argc, char **argv);
#endif

namespace {

constexpr unsigned S = BuiltinCommand::Flag::Special;
constexpr unsigned R = BuiltinCommand::Flag::Regular;
constexpr unsigned A = BuiltinCommand::Flag::Assign;

const BuiltinCommand builtin_dot{".", dotcmd, S | R};
const BuiltinCommand builtin_colon{":", truecmd, S | R};
const BuiltinCommand builtin_test_brack{"[", testcmd, 0};
const BuiltinCommand builtin_alias{"alias", aliascmd, A | R};
const BuiltinCommand builtin_break{"break", breakcmd, S | R};
const BuiltinCommand builtin_cd{"cd", cdcmd, R};
const BuiltinCommand builtin_chdir{"chdir", cdcmd, 0};
const BuiltinCommand builtin_continue{"continue", breakcmd, S | R};
const BuiltinCommand builtin_echo{"echo", echocmd, 0};
const BuiltinCommand builtin_exit{"exit", exitcmd, S | R};
const BuiltinCommand builtin_export{"export", exportcmd, S | R | A};
const BuiltinCommand builtin_false{"false", falsecmd, R};
const BuiltinCommand builtin_getopts{"getopts", getoptscmd, R};
const BuiltinCommand builtin_hash{"hash", hashcmd, R};
const BuiltinCommand builtin_kill{"kill", killcmd, R};
const BuiltinCommand builtin_local{"local", localcmd, S | R | A};
const BuiltinCommand builtin_printf{"printf", printfcmd, 0};
const BuiltinCommand builtin_pwd{"pwd", pwdcmd, R};
const BuiltinCommand builtin_read{"read", readcmd, R};
const BuiltinCommand builtin_readonly{"readonly", exportcmd, S | R | A};
const BuiltinCommand builtin_return{"return", returncmd, S | R};
const BuiltinCommand builtin_set{"set", setcmd, S | R};
const BuiltinCommand builtin_shift{"shift", shiftcmd, S | R};
const BuiltinCommand builtin_test{"test", testcmd, 0};
const BuiltinCommand builtin_times{"times", timescmd, S | R};
const BuiltinCommand builtin_true{"true", truecmd, R};
const BuiltinCommand builtin_type{"type", typecmd, R};
#if HAVE_GETRLIMIT
const BuiltinCommand builtin_ulimit{"ulimit", ulimitcmd, R};
#endif
const BuiltinCommand builtin_umask{"umask", umaskcmd, R};
const BuiltinCommand builtin_unalias{"unalias", unaliascmd, R};
const BuiltinCommand builtin_unset{"unset", unsetcmd, S | R};
const BuiltinCommand builtin_wait{"wait", waitcmd, R};

static const std::unordered_map<std::string_view, const BuiltinCommand *> map = {
	{builtin_dot.name, &builtin_dot},
	{builtin_colon.name, &builtin_colon},
	{builtin_test_brack.name, &builtin_test_brack},
	{builtin_alias.name, &builtin_alias},
	{builtin_break.name, &builtin_break},
	{builtin_cd.name, &builtin_cd},
	{builtin_chdir.name, &builtin_chdir},
	{builtin_command.name, &builtin_command},
	{builtin_continue.name, &builtin_continue},
	{builtin_echo.name, &builtin_echo},
	{builtin_eval.name, &builtin_eval},
	{builtin_exec.name, &builtin_exec},
	{builtin_exit.name, &builtin_exit},
	{builtin_export.name, &builtin_export},
	{builtin_false.name, &builtin_false},
	{builtin_getopts.name, &builtin_getopts},
	{builtin_hash.name, &builtin_hash},
	{builtin_kill.name, &builtin_kill},
	{builtin_local.name, &builtin_local},
	{builtin_printf.name, &builtin_printf},
	{builtin_pwd.name, &builtin_pwd},
	{builtin_read.name, &builtin_read},
	{builtin_readonly.name, &builtin_readonly},
	{builtin_return.name, &builtin_return},
	{builtin_set.name, &builtin_set},
	{builtin_shift.name, &builtin_shift},
	{builtin_test.name, &builtin_test},
	{builtin_times.name, &builtin_times},
	{builtin_trap.name, &builtin_trap},
	{builtin_true.name, &builtin_true},
	{builtin_type.name, &builtin_type},
#if HAVE_GETRLIMIT
	{builtin_ulimit.name, &builtin_ulimit},
#endif
	{builtin_umask.name, &builtin_umask},
	{builtin_unalias.name, &builtin_unalias},
	{builtin_unset.name, &builtin_unset},
	{builtin_wait.name, &builtin_wait},
};

} // namespace

const BuiltinCommand builtin_command{"command", commandcmd, R};
const BuiltinCommand builtin_eval{"eval", nullptr, S | R};
const BuiltinCommand builtin_exec{"exec", execcmd, S | R};
const BuiltinCommand builtin_trap{"trap", trapcmd, S | R};

const BuiltinCommand *
find_builtin(std::string_view name)
{
	const auto it = map.find(name);

	return it != std::end(map) ? it->second : nullptr;
}

} // namespace shell
