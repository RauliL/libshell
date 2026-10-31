/*-
 * Heap allocation helpers and StackStr-backed string building.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "shell.h"
#include "shell_context.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "machdep.h"
#include "mystring.h"
#include "system.h"

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

char *
savestr(ShellContext &sh, const char *s)
{
	return checknull(sh, ::strdup(s));
}

void
clear_stack_temps(ShellContext &sh)
{
	sh.stackstr.reset();
}

rawmem
stackstr_commit(ShellContext &sh, std::size_t nbytes)
{
	return {sh.stackstr.commit(nbytes)};
}

void
stunalloc(ShellContext &sh, void *p)
{
	if (!p)
		return;
	/* Only supports un-commit of the trailing string region. */
	char *base = sh.stackstr.buf.data();
	char *cp = static_cast<char *>(p);
	if (cp >= base && cp <= base + sh.stackstr.used)
		sh.stackstr.rewind(static_cast<std::size_t>(cp - base));
}

static char *
growstackblock(ShellContext &sh, size_t min)
{
	/* Preserve relative cursor if someone held a pointer into the buffer —
	 * callers re-base via makestrspace return value. */
	sh.stackstr.ensure_avail(min < 128 ? 128 : min);
	/* Growing may reallocate; return new base of in-progress string. */
	return sh.stackstr.base();
}

rawmem
growstackstr(ShellContext &sh)
{
	std::size_t len = stackblocksize();
	growstackblock(sh, len + 128);
	return {sh.stackstr.base() + len};
}

__attribute__((__noinline__)) char *
growstackto(ShellContext &sh, size_t len)
{
	if (stackblocksize() < len)
		growstackblock(sh, len);
	return stackblock();
}

__attribute__((__noinline__)) char *
makestrspace(ShellContext &sh, size_t newlen, char *p)
{
	size_t len = static_cast<size_t>(p - sh.stackstr.base());
	growstackto(sh, len + newlen);
	return sh.stackstr.base() + len;
}

__attribute__((__noinline__)) char *
stnputs(ShellContext &sh, const char *s, size_t n, char *p)
{
	p = makestrspace(sh, n, p);
	p = static_cast<char *>(mempcpy(p, s, n));
	return p;
}

char *
stputs(ShellContext &sh, const char *s, char *p)
{
	return stnputs(sh, s, strlen(s), p);
}

char *
_STPUTC(ShellContext &sh, int c, char *p)
{
	if (p >= sh.stackstr.cap()) {
		size_t off = static_cast<size_t>(p - sh.stackstr.base());
		growstackblock(sh, off + 2);
		p = sh.stackstr.base() + off;
	}
	*p++ = static_cast<char>(c);
	return p;
}
