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

rawmem ckmalloc(std::size_t);
rawmem ckrealloc(void *, std::size_t);
char *savestr(const char *);
rawmem stalloc(std::size_t);
rawmem astalloc(std::size_t);
void clear_stack_temps();
void stunalloc(void *);
rawmem growstackstr();
char *growstackto(std::size_t len);
char *makestrspace(size_t, char *);
char *stnputs(const char *, size_t, char *);
char *stputs(const char *, char *);

/* These names resolve to ShellContext::stackstr / helpers inside methods. */
#define stackblock() (stackstr.base())
#define stackblocksize() (stackstr.avail())
#define stacknxt (stackstr.base())
#define stacknleft (stackstr.avail())
#define sstrend (stackstr.cap())

#define STARTSTACKSTR(p) ((p) = static_cast<std::remove_reference_t<decltype(p)>>( \
	static_cast<void *>(stackstr.base())))
#define STPUTC(c, p) ((p) = _STPUTC((c), (p)))
#define CHECKSTRSPACE(n, p) \
	({ \
		char *_q = (p); \
		size_t _l = (n); \
		size_t _m = static_cast<size_t>(sstrend - _q); \
		if (_l > _m) \
			(p) = makestrspace(_l, _q); \
		0; \
	})
#define USTPUTC(c, p)	(*p++ = (c))
#define STACKSTRNUL(p)	((p) == sstrend? (p = growstackstr(), *p = '\0') : (*p = '\0'))
#define STUNPUTC(p)	(--p)
#define STTOPC(p)	p[-1]
#define STADJUST(amount, p)	(p += (amount))

#define grabstackstr(p)	(stackstr_commit(static_cast<size_t>( \
	reinterpret_cast<char *>(p) - (char *)stackblock())))
#define ungrabstackstr(s, p) stunalloc((s))
#define stackstrend() (static_cast<void *>(sstrend))

#define ckfree(p)	std::free(const_cast<void *>(static_cast<const void *>(p)))
#endif
