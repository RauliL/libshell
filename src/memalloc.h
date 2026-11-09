#ifndef MEMALLOC_H
#define MEMALLOC_H
/*-
 * Heap wrappers (ckmalloc / ckfree).
 *
 * These no longer take a Context; allocation policy is not part of
 * shell execution state. Prefer stack / std::vector / new where practical.
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

rawmem ckmalloc(std::size_t);

#define ckfree(p)	std::free(const_cast<void *>(static_cast<const void *>(p)))
#endif
