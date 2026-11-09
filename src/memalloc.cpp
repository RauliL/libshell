/*-
 * Heap allocation helpers.
 */

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "memalloc.h"


using namespace shell;


namespace shell {

namespace {
rawmem
checknull(void *p)
{
	if (!p) {
		// OOM is fatal; avoid ShellResult blast radius through ckmalloc.
		std::fprintf(stderr, "Out of space\n");
		_exit(2);
	}

	return {p};
}
} // namespace


__attribute__((__noinline__)) rawmem
ckmalloc(std::size_t nbytes)
{
	return checknull(std::malloc(nbytes));
}

} // namespace shell
