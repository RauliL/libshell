#ifndef MEMALLOC_H
#define MEMALLOC_H
/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)memalloc.h	8.2 (Berkeley) 5/4/95
 */

#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include "machdep.h"

#define MINSIZE SHELL_ALIGN(504)

struct stack_block {
	struct stack_block *prev;
	char space[MINSIZE];
};

struct stackmark {
	struct stack_block *stackp;
	char *stacknxt;
	size_t stacknleft;
};


extern char *stacknxt;
extern size_t stacknleft;
extern char *sstrend;

/* Untyped heap/stack block. Call sites keep the C shape
 * (`char *p = ckmalloc(n)`) and the conversion operator supplies the
 * pointer type C++ will not implicitly convert from void *. */
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
void stunalloc(void *);
void pushstackmark(struct stackmark *mark, std::size_t len);
void setstackmark(struct stackmark *);
void popstackmark(struct stackmark *);
rawmem growstackstr();
char *growstackto(std::size_t len);
char *makestrspace(size_t, char *);
char *stnputs(const char *, size_t, char *);
char *stputs(const char *, char *);


#define stackblock() (stacknxt)
#define stackblocksize() stacknleft
#define STARTSTACKSTR(p) ((p) = static_cast<std::remove_reference_t<decltype(p)>>( \
	static_cast<void *>(stacknxt)))
#define STPUTC(c, p) ((p) = _STPUTC((c), (p)))
#define CHECKSTRSPACE(n, p) \
	({ \
		char *_q = (p); \
		size_t _l = (n); \
		size_t _m = sstrend - _q; \
		if (_l > _m) \
			(p) = makestrspace(_l, _q); \
		0; \
	})
#define USTPUTC(c, p)	(*p++ = (c))
#define STACKSTRNUL(p)	((p) == sstrend? (p = growstackstr(), *p = '\0') : (*p = '\0'))
#define STUNPUTC(p)	(--p)
#define STTOPC(p)	p[-1]
#define STADJUST(amount, p)	(p += (amount))

#define grabstackstr(p)	stalloc((char *)(p) - (char *)stackblock())
#define ungrabstackstr(s, p) stunalloc((s))
#define stackstrend() (static_cast<void *>(sstrend))

#define ckfree(p)	std::free(const_cast<void *>(static_cast<const void *>(p)))
#endif
