#ifndef MEMALLOC_H
#define MEMALLOC_H
/*-
 * Heap wrappers (ckmalloc / ckfree).
 */

#include <cstddef>
#include <cstdlib>

#include "machdep.h"

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

#define ckfree(p)	std::free(const_cast<void *>(static_cast<const void *>(p)))
#endif
