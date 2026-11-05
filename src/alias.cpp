/*-
 * Copyright (c) 1993
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
 */
#include <cstring>
#include <string>

#include "shell.h"
#include "shell_context.h"
#include "input.h"
#include "output.h"
#include "error.h"
#include "memalloc.h"
#include "mystring.h"
#include "alias.h"
#include "options.h"
#include "syntax.h"
#include "var.h"

#define ATABSIZE 39

static shell::Result<void> setalias(ShellContext &sh, const char *, const char *);
static struct alias * freealias(ShellContext &sh, struct alias *);
static struct alias ** __lookupalias(ShellContext &sh, std::string_view);

static shell::Result<void>
setalias(ShellContext &sh, const char *name, const char *val)
{
	struct alias *ap, **app;
	const char *p = name;
	size_t namelen;

	do {
		if (BASESYNTAX[(signed char)*p] != CWORD)
			return sh_error(sh, "Invalid alias name: {}", name);
	} while (*++p != '=');

	namelen = static_cast<size_t>(val - name - 1); /* bytes before '=' */

	app = __lookupalias(sh, name);
	ap = *app;
	INTOFF(sh);
	if (ap) {
		ap->flag &= ~ALIASDEAD;
	} else {
		ap = new alias;
		ap->flag = 0;
		ap->next = nullptr;
		*app = ap;
	}
	ap->name.assign(name, namelen);
	ap->val = val;
	INTON(sh);

	return {};
}

shell::Result<int>
unalias(ShellContext &sh, std::string_view name)
{
	struct alias **app;

	app = __lookupalias(sh, name);

	if (*app) {
		INTOFF(sh);
		*app = freealias(sh, *app);
		INTON(sh);
		return 0;
	}

	return 1;
}

static shell::Result<void>
rmaliases(ShellContext &sh)
{
	INTOFF(sh);
	for (int i = 0; i < ATABSIZE; i++) {
		alias **app = &sh.atab[i];

		for (alias *ap = *app; ap; ap = *app) {
			*app = freealias(sh, *app);
			if (ap == *app) {
				app = &ap->next;
			}
		}
	}
	INTON(sh);

	return {};
}

struct alias *
lookupalias(ShellContext &sh, std::string_view name, int check)
{
	struct alias *ap = *__lookupalias(sh, name);

	if (check && ap && (ap->flag & ALIASINUSE))
		return (NULL);
	return (ap);
}

shell::Result<int>
aliascmd(ShellContext &sh, int argc, char **argv)
{
	char *n, *v;
	int ret = 0;
	struct alias *ap;

	if (argc == 1) {
		int i;

		for (i = 0; i < ATABSIZE; i++)
			for (ap = sh.atab[i]; ap; ap = ap->next) {
				printalias(sh, ap);
			}
		return 0;
	}
	while ((n = *++argv) != NULL) {
		if (!*n || !(v = strchr(n + 1, '='))) {
			if ((ap = *__lookupalias(sh, n)) == NULL) {
                sh.out2->write(sh, std::format("{}: {} not found\n", "alias", n));
				ret = 1;
			} else
				printalias(sh, ap);
		} else {
			TRY_SHV(setalias(sh, n, v + 1));
		}
	}

	return ret;
}

shell::Result<int>
unaliascmd(ShellContext &sh, int argc, char **argv)
{
	int i;

	while ((i = TRY_SH(nextopt(sh, "a"))) != '\0') {
		if (i == 'a') {
			TRY_SHV(rmaliases(sh));
			return 0;
		}
	}
	for (i = 0; *sh.bopt.argptr; sh.bopt.argptr++) {
		if (TRY_SH(unalias(sh, *sh.bopt.argptr))) {
            sh.out2->write(sh, std::format("{}: {} not found\n", "unalias", *sh.bopt.argptr));
			i = 1;
		}
	}

	return i;
}

static struct alias *
freealias(ShellContext &sh, struct alias *ap)
{
	struct alias *next;

	if (ap->flag & ALIASINUSE) {
		ap->flag |= ALIASDEAD;
		return ap;
	}

	next = ap->next;
	delete ap;
	return next;
}

void __attribute__((noinline))
printalias(ShellContext &sh, const alias *ap)
{
	out1str(std::format("{}={}\n", ap->name, single_quote(ap->val)));
}

static struct alias **
__lookupalias(ShellContext &sh, std::string_view name)
{
	struct alias **app;

	app = &sh.atab[hashval(name) % ATABSIZE];

	for (; *app; app = &(*app)->next) {
		if (varequal(name, (*app)->name)) {
			break;
		}
	}

	return app;
}
