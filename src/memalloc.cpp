/*-
 * Heap allocation helpers.
 */

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "shell.h"
#include "shell_context.h"
#include "memalloc.h"
#include "error.h"

static rawmem
checknull(ShellContext &sh, void *p)
{
	if (!p) {
		// OOM is fatal; avoid ShellResult blast radius through ckmalloc.
		std::fprintf(stderr, "Out of space\n");
		_exit(2);
	}

	return {p};
}

__attribute__((__noinline__)) rawmem
ckmalloc(ShellContext &sh, std::size_t nbytes)
{
	return checknull(sh, std::malloc(nbytes));
}

__attribute__((__noinline__)) rawmem
ckrealloc(ShellContext &sh, void *p, std::size_t nbytes)
{
	return checknull(sh, std::realloc(p, nbytes));
}
