/*-
 * Shell lifecycle: construct-time init and reset paths formerly generated
 * by mkinit from scattered INIT/RESET blocks.
 */

#include "init.h"

#include "expand.h"
#include "eval.h"
#include "input.h"
#include "redir.h"
#include "shell_context.h"
#include "trap.h"
#include "var.h"

void
init(ShellContext &sh)
{
	/* InputStack member ctor already wires basebuf / linno. */
	init_signal_names(sh);
	init_traps(sh);
	init_shell_vars(sh);
}

void
exitreset(ShellContext &sh)
{
	eval_exitreset(sh);
	ifsfree(sh);
	unwindredir(sh, 0);
}

void
forkreset(ShellContext &sh, const Node *n)
{
	input_forkreset(sh);
	redir_forkreset(sh);
	(void)clear_traps(sh, n);
}

void
postexitreset(ShellContext &sh)
{
	flush_input(sh);
}

void
reset(ShellContext &sh)
{
	input_reset_interactive(sh);
	(void)unwindlocalvars(sh, 0);
}
