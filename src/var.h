#ifndef VAR_H
#define VAR_H

#include "error.h"
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
 *	@(#)var.h	8.2 (Berkeley) 5/4/95
 */

#include <inttypes.h>
#include <string>
#include <vector>

struct ShellContext;

/*
 * Shell variables.
 */

/* flags */
#define VEXPORT		0x01	/* variable is exported */
#define VREADONLY	0x02	/* variable cannot be modified */
#define VSTRFIXED	0x04	/* variable struct is statically allocated */
#define VTEXTFIXED	0x08	/* legacy: ignored (text always owned) */
#define VSTACK		0x10	/* legacy: unused */
#define VUNSET		0x20	/* the variable is not set */
#define VNOFUNC		0x40	/* don't call the callback function */
#define VFULL		0x80	/* pass value suitable for putenv */
#define VNOSAVE		0x100	/* caller heap buffer; setvareq copies then frees */


struct var {
	struct var *next;		/* next entry in hash list */
	int flags;			/* flags are defined above */
	std::string text;		/* name=value (owned; empty value when VUNSET) */
	void (*func)(ShellContext &, const char *);
					/* function to be called when  */
					/* the variable gets set/unset */
};


struct localvar {
	struct localvar *next;		/* next local variable in list */
	struct var *vp;			/* the variable that was made local */
	int flags;			/* saved flags */
	std::string text;		/* saved text or $- optlist bytes */
};

struct localvar_list;


/* varinit / defifsvar / linenovar live on ShellContext; macros require &sh. */

#define vifs (sh.varinit[0])
#define vpath (&vifs)[1]
#define vps1 (&vpath)[1]
#define vps2 (&vps1)[1]
#define vps4 (&vps2)[1]
#define voptind (&vps4)[1]
#ifdef WITH_LINENO
#define vlineno (&voptind)[1]
#endif

extern const char defpathvar[];
#define defifs (sh.defifsvar + 4)
#define defpath (defpathvar + 36)

/*
 * The following macros access the values of the above variables.
 * They have to skip over the name.  They return the null string
 * for unset variables.
 */

#define ifsval()	(vifs.text.c_str() + 4)
#define ifsset()	((vifs.flags & VUNSET) == 0)
#define pathval()	(vpath.text.c_str() + 5)
#define ps1val()	(vps1.text.c_str() + 4)
#define ps2val()	(vps2.text.c_str() + 4)
#define ps4val()	(vps4.text.c_str() + 4)
#define optindval()	(voptind.text.c_str() + 7)
#define linenoval()	(vlineno.text.c_str() + 7)

struct ShellContext;
void initvar(ShellContext &sh);
dash::ShellResult<struct var *> setvar(ShellContext &sh, const char *name, const char *val, int flags);
dash::ShellResult<intmax_t> setvarint(ShellContext &sh, const char *, intmax_t, int);
dash::ShellResult<struct var *> setvareq(ShellContext &sh, char *s, int flags);
char *lookupvar(ShellContext &sh, const char *);
dash::ShellResult<intmax_t> lookupvarint(ShellContext &sh, const char *);
std::vector<char *> listvars(ShellContext &sh, int, int);
inline std::vector<char *> environment(ShellContext &sh)
{
	return listvars(sh, VEXPORT, VUNSET);
}
dash::ShellResult<int> showvars(ShellContext &sh, const char *, int, int);
dash::ShellResult<int> exportcmd(ShellContext &sh, int, char **);
dash::ShellResult<int> localcmd(ShellContext &sh, int, char **);
dash::ShellResult<void> mklocal(ShellContext &sh, char *name, int flags);
dash::ShellResult<struct localvar_list *> pushlocalvars(ShellContext &sh, int push);
dash::ShellResult<void> unwindlocalvars(ShellContext &sh, struct localvar_list *stop);
dash::ShellResult<int> unsetcmd(ShellContext &sh, int, char **);
dash::ShellResult<void> unsetvar(ShellContext &sh, const char *);
int varcmp(const char *, const char *);

static inline unsigned int hashval(const char *p)
{
	unsigned int hashval;

	hashval = ((unsigned char) *p) << 4;
	while (*p) {
		hashval += (unsigned char) *p++;
		if (*p == '=')
			break;
	}

	return hashval;
}


static inline int varequal(const char *a, const char *b) {
	return !varcmp(a, b);
}

/*
 * Search the environment of a builtin command.
 */



#endif
