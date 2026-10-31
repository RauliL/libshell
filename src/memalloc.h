#ifndef MEMALLOC_H
#define MEMALLOC_H
/*-
 * Heap wrappers (ckmalloc) and string-scratch helpers. The old ash linked
 * stack arena is gone; string building uses StackStr on ShellContext.
 */

#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "machdep.h"
#include "stackstr.h"

struct rawmem {
	void *ptr;
	template<class T>
	operator T *() const
	{
		return static_cast<T *>(ptr);
	}
};

struct ShellContext;

rawmem ckmalloc(ShellContext &sh, std::size_t);
rawmem ckrealloc(ShellContext &sh, void *, std::size_t);
char *savestr(ShellContext &sh, const char *);
void clear_stack_temps(ShellContext &sh);
void stunalloc(ShellContext &sh, void *);
rawmem growstackstr(ShellContext &sh);
char *growstackto(ShellContext &sh, std::size_t len);
char *makestrspace(ShellContext &sh, size_t, char *);
char *stnputs(ShellContext &sh, const char *, size_t, char *);
char *stputs(ShellContext &sh, const char *, char *);
rawmem stackstr_commit(ShellContext &sh, std::size_t);
char *_STPUTC(ShellContext &sh, int c, char *p);

/* These names resolve to sh.stackstr via shell_access.h (ShellContext &sh in scope). */
#define stackblock() (sh.stackstr.base())
#define stackblocksize() (sh.stackstr.avail())
#define stacknxt (sh.stackstr.base())
#define stacknleft (sh.stackstr.avail())
#define sstrend (sh.stackstr.cap())

#define STARTSTACKSTR(p) ((p) = static_cast<std::remove_reference_t<decltype(p)>>( \
	static_cast<void *>(sh.stackstr.base())))
#define STPUTC(c, p) ((p) = _STPUTC(sh, (c), (p)))
#define CHECKSTRSPACE(n, p) \
	({ \
		char *_q = (p); \
		size_t _l = (n); \
		size_t _m = static_cast<size_t>(sstrend - _q); \
		if (_l > _m) \
			(p) = makestrspace(sh, _l, _q); \
		0; \
	})
#define USTPUTC(c, p)	(*p++ = (c))
#define STACKSTRNUL(p)	((p) == sstrend? (p = growstackstr(sh), *p = '\0') : (*p = '\0'))
#define STUNPUTC(p)	(--p)
#define STTOPC(p)	p[-1]
#define STADJUST(amount, p)	(p += (amount))

#define grabstackstr(p)	(stackstr_commit(sh, static_cast<size_t>( \
	reinterpret_cast<char *>(p) - (char *)stackblock())))
#define ungrabstackstr(s, p) stunalloc(sh, (s))
#define stackstrend() (static_cast<void *>(sstrend))

#define ckfree(p)	std::free(const_cast<void *>(static_cast<const void *>(p)))
#endif
