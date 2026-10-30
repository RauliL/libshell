/*-
 * Heap allocation helpers and StackStr-backed string building.
 */

#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "shell.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "machdep.h"
#include "mystring.h"
#include "system.h"

__attribute__((__always_inline__)) inline void ShellContext::outofspace(void)
{
	sh_error("Out of space");
}

rawmem ShellContext::checknull(void *p)
{
	if (!p)
		outofspace();
	return {p};
}

__attribute__((__noinline__)) rawmem ShellContext::ckmalloc(std::size_t nbytes)
{
	return checknull(std::malloc(nbytes));
}

__attribute__((__noinline__)) rawmem ShellContext::ckrealloc(void *p, std::size_t nbytes)
{
	return checknull(std::realloc(p, nbytes));
}

char *
ShellContext::savestr(const char *s)
{
	return checknull(::strdup(s));
}

/*
 * Allocate nbytes on the heap. Tracked for mark-based / command-scope cleanup.
 * String commits go through stackstr_commit() (see grabstackstr).
 * AST nodes should use astalloc() so freenode() owns them.
 */
rawmem ShellContext::stalloc(std::size_t nbytes)
{
	std::size_t aligned = SHELL_ALIGN(nbytes);
	void *p = ckmalloc(aligned);
	stackheaps.push_back(p);
	return {p};
}

rawmem ShellContext::astalloc(std::size_t nbytes)
{
	std::size_t n = SHELL_ALIGN(nbytes);
	void *p = ckmalloc(n);
	std::memset(p, 0, n);
	return {p};
}

void ShellContext::clear_stack_temps()
{
	for (void *p : stackheaps)
		ckfree(p);
	stackheaps.clear();
	stackstr.reset();
}

rawmem ShellContext::stackstr_commit(std::size_t nbytes)
{
	return {stackstr.commit(nbytes)};
}

void ShellContext::stunalloc(void *p)
{
	if (!p)
		return;
	/* Only supports un-commit of the trailing string region. */
	char *base = stackstr.buf.data();
	char *cp = static_cast<char *>(p);
	if (cp >= base && cp <= base + stackstr.used)
		stackstr.rewind(static_cast<std::size_t>(cp - base));
}

char *ShellContext::growstackblock(size_t min)
{
	std::size_t off = 0;
	/* Preserve relative cursor if someone held a pointer into the buffer —
	 * callers re-base via makestrspace return value. */
	(void)off;
	stackstr.ensure_avail(min < 128 ? 128 : min);
	/* Growing may reallocate; return new base of in-progress string. */
	return stackstr.base();
}

rawmem ShellContext::growstackstr()
{
	std::size_t len = stackblocksize();
	growstackblock(len + 128);
	return {stackstr.base() + len};
}

__attribute__((__noinline__)) char *ShellContext::growstackto(size_t len)
{
	if (stackblocksize() < len)
		growstackblock(len);
	return stackblock();
}

__attribute__((__noinline__)) char *ShellContext::makestrspace(size_t newlen, char *p)
{
	size_t len = static_cast<size_t>(p - stackstr.base());
	growstackto(len + newlen);
	return stackstr.base() + len;
}

__attribute__((__noinline__)) char *ShellContext::stnputs(const char *s, size_t n, char *p)
{
	p = makestrspace(n, p);
	p = static_cast<char *>(mempcpy(p, s, n));
	return p;
}

char *
ShellContext::stputs(const char *s, char *p)
{
	return stnputs(s, strlen(s), p);
}

char *ShellContext::_STPUTC(int c, char *p)
{
	if (p >= stackstr.cap()) {
		size_t off = static_cast<size_t>(p - stackstr.base());
		growstackblock(off + 2);
		p = stackstr.base() + off;
	}
	*p++ = static_cast<char>(c);
	return p;
}

void ShellContext::grabstackblock(size_t len)
{
	stackstr_commit(len);
}
