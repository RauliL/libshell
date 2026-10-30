#include "shell.h"

int main(int argc, char **argv)
{
	ShellContext shell;
	return shell.run(argc, argv);
}
