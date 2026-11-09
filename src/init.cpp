/*-
 * Shell lifecycle: construct-time init and reset paths formerly generated
 * by mkinit from scattered INIT/RESET blocks.
 */
#include "eval.h"
#include "expand.h"
#include "init.h"
#include "input.h"
#include "redir.h"
#include "shell.h"
#include "trap.h"
#include "var.h"


using namespace shell;


namespace shell {

void
init(Context &sh)
{
	/* InputStack member ctor already wires basebuf / linno. */
	init_signal_names(sh);
	init_traps(sh);
	init_shell_vars(sh);
}

void
exitreset(Context &sh)
{
	eval_exitreset(sh);
	ifsfree(sh);
	unwindredir(sh, 0);
}

void
forkreset(Context &sh, const Node *n)
{
	input_forkreset(sh);
	redir_forkreset(sh);
	(void)clear_traps(sh, n);
}

void
postexitreset(Context &sh)
{
	flush_input(sh);
}

void
reset(Context &sh)
{
	input_reset_interactive(sh);
	(void)unwindlocalvars(sh, 0);
}

} // namespace shell
