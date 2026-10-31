/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * Interactive / process driver for the dash executable.  Non-interactive
 * command looping, `.`, and `exit` live in main.cpp (the library).
 */

#include <stdio.h>
#include <unistd.h>

#include "shell.h"
#include "main.h"
#include "shell_context.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#include "var.h"
#include "show.h"
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#include "parse_error.h"
#include "nodes_free.h"

#define PROFILE 0

#if PROFILE
short profile_buf[16384];
extern int etext();
#endif

dash::ShellResult<int> run_interactive(ShellContext &sh);

static dash::ShellResult<void> read_profile(ShellContext &sh, const char *name);

static dash::ShellResult<void>
read_profile(ShellContext &sh, const char *name)
{
	name = expandstr(sh, name);
	if (auto _r = setinputfile(sh, name, INPUT_PUSH_FILE | INPUT_NOFILE_OK); !_r || *_r < 0)
		return {};

	auto r = cmdloop(sh);
	popfile(sh);
	if (!r)
		return tl::unexpected(r.error());
	return {};
}

/*
 * Executable entry.  ShellResult replaces setjmp for interactive recovery:
 * abort the current stage and continue (profiles → ENV → main loop), or
 * exit when non-interactive / EXEXIT / EXEND.
 */
static int
run(ShellContext &sh, int argc, char **argv)
{
	char *shinit;
	int state = 0;
	int login = 0;

#ifdef __GLIBC__
	dash_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif

#ifdef DEBUG
	opentrace(sh);
	trputs(sh, "Shell args:  ");  trargs(sh, argv);
#endif
	sh.mypid = sh.rootpid = getpid();
	init(sh);

resume:
	dash::ShellErr err{};
	bool failed = false;

	if (state == 0) {
		auto pr = procargs(sh, argv);
		if (!pr) {
			err = pr.error();
			failed = true;
		} else {
			login = *pr;
		}
	}

	auto take = [&](auto &&r) {
		if (!failed && !r) {
			err = r.error();
			failed = true;
		}
	};

	if (!failed && state <= 1 && login) {
		if (state < 1) {
			state = 1;
			take(read_profile(sh, "/etc/profile"));
		}
		if (!failed) {
			state = 2;
			take(read_profile(sh, "$HOME/.profile"));
		}
	}
	if (!failed && state < 3) {
		state = 3;
		if (
#ifndef linux
			getuid() == geteuid() && getgid() == getegid() &&
#endif
			iflag
		) {
			if ((shinit = lookupvar(sh, "ENV")) != NULL &&
			    *shinit != '\0') {
				take(read_profile(sh, shinit));
			}
		}
		if (!failed)
			clear_stack_temps(sh);
	}
	if (!failed && state < 4) {
		state = 4;
		if (sh.minusc)
			take(sh.eval(*sh.minusc,
				     sflag ? EvalFlags{} : EvalFlag::exit));
	}
	if (!failed) {
		if (iflag)
			take(run_interactive(sh));
		else if (sflag || !sh.minusc)
			take(cmdloop(sh));
	}

	if (failed) {
		int ecode = err.code;

handle_err:
		exitreset(sh);
		sh.exception = ecode;
		if (ecode == EXEND || ecode == EXEXIT || state == 0 ||
		    iflag == 0 || sh.shlvl)
			goto exit;

		reset(sh);

		if (ecode == EXINT) {
			out2c('\n');
#ifdef FLUSHERR
			flushout(sh, sh.out2);
#endif
		}
		clear_stack_temps(sh);
		barrier();
		sh.suppressint = 0;
		if (sh.intpending) {
			auto u = onint(sh);
			ecode = u.value().code; /* tl::unexpected<ShellErr>::value */
			goto handle_err;
		}
		goto resume;
	}
exit:
#if PROFILE
	monitor(0);
#endif
#if GPROF
	{
		extern void _mcleanup();
		_mcleanup();
	}
#endif
	exitshell(sh);
	/* NOTREACHED */
}

int
main(int argc, char **argv)
{
	ShellContext shell;
	return run(shell, argc, argv);
}
