#include "shell.h"
#include "shell_context.h"

int main(int argc, char **argv)
{
	ShellContext shell;
	return run(shell, argc, argv);
}
