/*
 * Builtin command table. Lookups use an unordered_map of name → entry.
 */

#include "builtins.h"

#include <unordered_map>

#include "alias.h"
#include "cd.h"
#include "eval.h"
#include "jobs.h"
#include "main.h"
#include "options.h"
#include "shell.h"
#include "trap.h"
#include "var.h"

struct ShellContext;

shell::Result<int> echocmd(ShellContext &sh, int, char **);
shell::Result<int> printfcmd(ShellContext &sh, int, char **);
shell::Result<int> testcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> timescmd(ShellContext &sh, int argc, char *argv[]);
shell::Result<int> breakcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> returncmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> falsecmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> truecmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> execcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> hashcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> typecmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> commandcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> readcmd(ShellContext &sh, int argc, char **argv);
shell::Result<int> umaskcmd(ShellContext &sh, int argc, char **argv);
#if HAVE_GETRLIMIT
shell::Result<int> ulimitcmd(ShellContext &sh, int argc, char **argv);
#endif

namespace {

constexpr unsigned S = BUILTIN_SPECIAL;
constexpr unsigned R = BUILTIN_REGULAR;
constexpr unsigned A = BUILTIN_ASSIGN;

const builtincmd builtin_dot{".", dotcmd, S | R};
const builtincmd builtin_colon{":", truecmd, S | R};
const builtincmd builtin_test_brack{"[", testcmd, 0};
const builtincmd builtin_alias{"alias", aliascmd, A | R};
const builtincmd builtin_break{"break", breakcmd, S | R};
const builtincmd builtin_cd{"cd", cdcmd, R};
const builtincmd builtin_chdir{"chdir", cdcmd, 0};
const builtincmd builtin_continue{"continue", breakcmd, S | R};
const builtincmd builtin_echo{"echo", echocmd, 0};
const builtincmd builtin_exit{"exit", exitcmd, S | R};
const builtincmd builtin_export{"export", exportcmd, S | R | A};
const builtincmd builtin_false{"false", falsecmd, R};
const builtincmd builtin_getopts{"getopts", getoptscmd, R};
const builtincmd builtin_hash{"hash", hashcmd, R};
const builtincmd builtin_kill{"kill", killcmd, R};
const builtincmd builtin_local{"local", localcmd, S | R | A};
const builtincmd builtin_printf{"printf", printfcmd, 0};
const builtincmd builtin_pwd{"pwd", pwdcmd, R};
const builtincmd builtin_read{"read", readcmd, R};
const builtincmd builtin_readonly{"readonly", exportcmd, S | R | A};
const builtincmd builtin_return{"return", returncmd, S | R};
const builtincmd builtin_set{"set", setcmd, S | R};
const builtincmd builtin_shift{"shift", shiftcmd, S | R};
const builtincmd builtin_test{"test", testcmd, 0};
const builtincmd builtin_times{"times", timescmd, S | R};
const builtincmd builtin_true{"true", truecmd, R};
const builtincmd builtin_type{"type", typecmd, R};
#if HAVE_GETRLIMIT
const builtincmd builtin_ulimit{"ulimit", ulimitcmd, R};
#endif
const builtincmd builtin_umask{"umask", umaskcmd, R};
const builtincmd builtin_unalias{"unalias", unaliascmd, R};
const builtincmd builtin_unset{"unset", unsetcmd, S | R};
const builtincmd builtin_wait{"wait", waitcmd, R};

static const std::unordered_map<std::string_view, const builtincmd *> map = {
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

const builtincmd builtin_command{"command", commandcmd, R};
const builtincmd builtin_eval{"eval", nullptr, S | R};
const builtincmd builtin_exec{"exec", execcmd, S | R};
const builtincmd builtin_trap{"trap", trapcmd, S | R};

const builtincmd *
find_builtin(std::string_view name)
{
	const auto it = map.find(name);

	return it != std::end(map) ? it->second : nullptr;
}
