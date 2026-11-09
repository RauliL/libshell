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

shell::Result<int> echocmd(Context &sh, int, char **);
shell::Result<int> printfcmd(Context &sh, int, char **);
shell::Result<int> testcmd(Context &sh, int argc, char **argv);
shell::Result<int> timescmd(Context &sh, int argc, char *argv[]);
shell::Result<int> breakcmd(Context &sh, int argc, char **argv);
shell::Result<int> returncmd(Context &sh, int argc, char **argv);
shell::Result<int> falsecmd(Context &sh, int argc, char **argv);
shell::Result<int> truecmd(Context &sh, int argc, char **argv);
shell::Result<int> execcmd(Context &sh, int argc, char **argv);
shell::Result<int> hashcmd(Context &sh, int argc, char **argv);
shell::Result<int> typecmd(Context &sh, int argc, char **argv);
shell::Result<int> commandcmd(Context &sh, int argc, char **argv);
shell::Result<int> readcmd(Context &sh, int argc, char **argv);
shell::Result<int> umaskcmd(Context &sh, int argc, char **argv);
#if HAVE_GETRLIMIT
shell::Result<int> ulimitcmd(Context &sh, int argc, char **argv);
#endif

namespace {

constexpr unsigned S = shell::BuiltinCommand::Flag::Special;
constexpr unsigned R = shell::BuiltinCommand::Flag::Regular;
constexpr unsigned A = shell::BuiltinCommand::Flag::Assign;

const shell::BuiltinCommand builtin_dot{".", dotcmd, S | R};
const shell::BuiltinCommand builtin_colon{":", truecmd, S | R};
const shell::BuiltinCommand builtin_test_brack{"[", testcmd, 0};
const shell::BuiltinCommand builtin_alias{"alias", aliascmd, A | R};
const shell::BuiltinCommand builtin_break{"break", breakcmd, S | R};
const shell::BuiltinCommand builtin_cd{"cd", cdcmd, R};
const shell::BuiltinCommand builtin_chdir{"chdir", cdcmd, 0};
const shell::BuiltinCommand builtin_continue{"continue", breakcmd, S | R};
const shell::BuiltinCommand builtin_echo{"echo", echocmd, 0};
const shell::BuiltinCommand builtin_exit{"exit", exitcmd, S | R};
const shell::BuiltinCommand builtin_export{"export", exportcmd, S | R | A};
const shell::BuiltinCommand builtin_false{"false", falsecmd, R};
const shell::BuiltinCommand builtin_getopts{"getopts", getoptscmd, R};
const shell::BuiltinCommand builtin_hash{"hash", hashcmd, R};
const shell::BuiltinCommand builtin_kill{"kill", killcmd, R};
const shell::BuiltinCommand builtin_local{"local", localcmd, S | R | A};
const shell::BuiltinCommand builtin_printf{"printf", printfcmd, 0};
const shell::BuiltinCommand builtin_pwd{"pwd", pwdcmd, R};
const shell::BuiltinCommand builtin_read{"read", readcmd, R};
const shell::BuiltinCommand builtin_readonly{"readonly", exportcmd, S | R | A};
const shell::BuiltinCommand builtin_return{"return", returncmd, S | R};
const shell::BuiltinCommand builtin_set{"set", setcmd, S | R};
const shell::BuiltinCommand builtin_shift{"shift", shiftcmd, S | R};
const shell::BuiltinCommand builtin_test{"test", testcmd, 0};
const shell::BuiltinCommand builtin_times{"times", timescmd, S | R};
const shell::BuiltinCommand builtin_true{"true", truecmd, R};
const shell::BuiltinCommand builtin_type{"type", typecmd, R};
#if HAVE_GETRLIMIT
const shell::BuiltinCommand builtin_ulimit{"ulimit", ulimitcmd, R};
#endif
const shell::BuiltinCommand builtin_umask{"umask", umaskcmd, R};
const shell::BuiltinCommand builtin_unalias{"unalias", unaliascmd, R};
const shell::BuiltinCommand builtin_unset{"unset", unsetcmd, S | R};
const shell::BuiltinCommand builtin_wait{"wait", waitcmd, R};

static const std::unordered_map<std::string_view, const shell::BuiltinCommand *> map = {
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

const shell::BuiltinCommand builtin_command{"command", commandcmd, R};
const shell::BuiltinCommand builtin_eval{"eval", nullptr, S | R};
const shell::BuiltinCommand builtin_exec{"exec", execcmd, S | R};
const shell::BuiltinCommand builtin_trap{"trap", trapcmd, S | R};

const shell::BuiltinCommand *
find_builtin(std::string_view name)
{
	const auto it = map.find(name);

	return it != std::end(map) ? it->second : nullptr;
}
