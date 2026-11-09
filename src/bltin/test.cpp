#include "./config.hpp"

/*
 * test(1); version 7-like  --  author Erik Baalbergen
 * modified by Eric Gisin to be used as built-in.
 * modified by Arnold Robbins to add SVR3 compatibility
 * (-x -c -b -p -u -g -k) plus Korn's -L -nt -ot -ef and new -S (socket).
 * modified by J.T. Conklin for NetBSD.
 *
 * This program is in the Public Domain.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include "../exec.h"
#include "bltin.h"
#include <cstdint>

/* test(1) accepts the following grammar:
	oexpr	::= aexpr | aexpr "-o" oexpr ;
	aexpr	::= nexpr | nexpr "-a" aexpr ;
	nexpr	::= primary | "!" primary
	primary	::= unary-operator operand
		| operand binary-operator operand
		| operand
		| "(" oexpr ")"
		;
	unary-operator ::= "-r"|"-w"|"-x"|"-f"|"-d"|"-c"|"-b"|"-p"|
		"-u"|"-g"|"-k"|"-s"|"-t"|"-z"|"-n"|"-o"|"-O"|"-G"|"-L"|"-S";

	binary-operator ::= "="|"!="|"-eq"|"-ne"|"-ge"|"-gt"|"-le"|"-lt"|
			"-nt"|"-ot"|"-ef";
	operand ::= <any legal UNIX file name>
*/

using namespace shell;

namespace shell {

enum token : int {
	EOI,
	FILRD,
	FILWR,
	FILEX,
	FILEXIST,
	FILREG,
	FILDIR,
	FILCDEV,
	FILBDEV,
	FILFIFO,
	FILSOCK,
	FILSYM,
	FILGZ,
	FILTT,
	FILSUID,
	FILSGID,
	FILSTCK,
	FILNT,
	FILOT,
	FILEQ,
	FILUID,
	FILGID,
	STREZ,
	STRNZ,
	STREQ,
	STRNE,
	STRLT,
	STRGT,
	INTEQ,
	INTNE,
	INTGE,
	INTGT,
	INTLE,
	INTLT,
	UNOT,
	BAND,
	BOR,
	LPAREN,
	RPAREN,
	OPERAND
};

enum token_types {
	UNOP,
	BINOP,
	BUNOP,
	BBINOP,
	PAREN
};

struct t_op {
	const char *op_text;
	short op_num, op_type;
};

/** Expression-walker state for one `test`/`[` invocation. */
struct TestState {
	char **wp = nullptr;
	const t_op *wp_op = nullptr;
};

namespace {
const struct t_op ops[] = {
	{"-r",	FILRD,	UNOP},
	{"-w",	FILWR,	UNOP},
	{"-x",	FILEX,	UNOP},
	{"-e",	FILEXIST,UNOP},
	{"-f",	FILREG,	UNOP},
	{"-d",	FILDIR,	UNOP},
	{"-c",	FILCDEV,UNOP},
	{"-b",	FILBDEV,UNOP},
	{"-p",	FILFIFO,UNOP},
	{"-u",	FILSUID,UNOP},
	{"-g",	FILSGID,UNOP},
	{"-k",	FILSTCK,UNOP},
	{"-s",	FILGZ,	UNOP},
	{"-t",	FILTT,	UNOP},
	{"-z",	STREZ,	UNOP},
	{"-n",	STRNZ,	UNOP},
	{"-h",	FILSYM,	UNOP},		/* for backwards compat */
	{"-O",	FILUID,	UNOP},
	{"-G",	FILGID,	UNOP},
	{"-L",	FILSYM,	UNOP},
	{"-S",	FILSOCK,UNOP},
	{"=",	STREQ,	BINOP},
	{"!=",	STRNE,	BINOP},
	{"<",	STRLT,	BINOP},
	{">",	STRGT,	BINOP},
	{"-eq",	INTEQ,	BINOP},
	{"-ne",	INTNE,	BINOP},
	{"-ge",	INTGE,	BINOP},
	{"-gt",	INTGT,	BINOP},
	{"-le",	INTLE,	BINOP},
	{"-lt",	INTLT,	BINOP},
	{"-nt",	FILNT,	BINOP},
	{"-ot",	FILOT,	BINOP},
	{"-ef",	FILEQ,	BINOP},
	{"!",	UNOT,	BUNOP},
	{"-a",	BAND,	BBINOP},
	{"-o",	BOR,	BBINOP},
	{"(",	LPAREN,	PAREN},
	{")",	RPAREN,	PAREN},
	{0,	0,	0}
};

tl::unexpected<Error> syntax(Context &sh, const char *,
					  const char *);
Result<int> oexpr(Context &sh, TestState &, enum token);
Result<int> aexpr(Context &sh, TestState &, enum token);
Result<int> nexpr(Context &sh, TestState &, enum token);
Result<int> primary(Context &sh, TestState &, enum token);
Result<int> binop(Context &sh, TestState &);
int filstat(char *, enum token);
enum token t_lex(TestState &, char **);
int isoperand(char **);
bool newerf(const char *, const char *);
bool olderf(const char *, const char *);
int equalf(const char *, const char *);
const t_op *getop(const char *s);
} // namespace

#ifdef HAVE_FACCESSAT
# ifdef HAVE_TRADITIONAL_FACCESSAT
namespace {
inline int
faccessat_confused_about_superuser(Context &sh)
{ return 1; }
} // namespace

# else
namespace {
inline int
faccessat_confused_about_superuser(Context &sh)
{ return 0; }
} // namespace

# endif
#endif

namespace {
inline Result<intmax_t>
getn(Context &sh, const char *s)
{
	return atomax10(sh, s);
}

const t_op *
getop(const char *s)
{
	for (const t_op *op = ops; op->op_text; op++) {
		if (strcmp(s, op->op_text) == 0)
			return op;
	}

	return nullptr;
}
} // namespace

Result<int>
testcmd(Context &sh, int argc, char **argv)
{
	const t_op *op;
	enum token n;
	int res = 1;
	TestState st;

	if (*argv[0] == '[') {
		if (*argv[--argc] != ']')
			return error("missing ]");
		argv[argc] = nullptr;
	}

recheck:
	argv++;
	argc--;

	if (argc < 1)
		return res;

	/*
	 * POSIX prescriptions: he who wrote this deserves the Nobel
	 * peace prize.
	 */
	switch (argc) {
	case 3:
		op = getop(argv[1]);
		if (op && op->op_type == BINOP) {
			n = OPERAND;
			goto eval;
		}
		/* fall through */

	case 4:
		if (!strcmp(argv[0], "(") && !strcmp(argv[argc - 1], ")")) {
			argv[--argc] = nullptr;
			argv++;
			argc--;
		} else if (!strcmp(argv[0], "!")) {
			res = 0;
			goto recheck;
		}
	}

	n = t_lex(st, argv);

eval:
	st.wp = argv;
	{
		int tmp;
		TRY_SH_SET(tmp, oexpr(sh, st, n));
		res ^= tmp;
	}
	argv = st.wp;

	if (argv[0] && argv[1])
		return syntax(sh, argv[0], "unexpected operator");

	return res;
}

namespace {
tl::unexpected<Error>
syntax(Context &sh, const char *op, const char *msg)
{
	if (op && *op)
		return error("{}: {}", op, msg);
	else
		return error("{}", msg);
}

Result<int>
oexpr(Context &sh, TestState &st, enum token n)
{
	int res = 0;

	for (;;) {
		int tmp;
		TRY_SH_SET(tmp, aexpr(sh, st, n));
		res |= tmp;
		if (!*st.wp)
			break;
		n = t_lex(st, st.wp + 1);
		if (n != BOR)
			break;
		n = t_lex(st, st.wp += 2);
	}
	return res;
}

Result<int>
aexpr(Context &sh, TestState &st, enum token n)
{
	int res = 1;

	for (;;) {
		int tmp;
		TRY_SH_SET(tmp, nexpr(sh, st, n));
		if (!tmp)
			res = 0;
		if (!*st.wp)
			break;
		n = t_lex(st, st.wp + 1);
		if (n != BAND)
			break;
		n = t_lex(st, st.wp += 2);
	}
	return res;
}

Result<int>
nexpr(Context &sh, TestState &st, enum token n)
{
	if (n != UNOT)
		return primary(sh, st, n);

	n = t_lex(st, st.wp + 1);
	if (n != EOI)
		st.wp++;
	{
		int tmp;
		TRY_SH_SET(tmp, nexpr(sh, st, n));
		return !tmp;
	}
}

Result<int>
primary(Context &sh, TestState &st, enum token n)
{
	enum token nn;
	int res;

	if (n == EOI)
		return 0;		/* missing expression */
	if (n == LPAREN) {
		if ((nn = t_lex(st, ++st.wp)) == RPAREN)
			return 0;	/* missing expression */
		TRY_SH_SET(res, oexpr(sh, st, nn));
		if (t_lex(st, ++st.wp) != RPAREN)
			return syntax(sh, nullptr, "closing paren expected");
		return res;
	}
	if (st.wp_op && st.wp_op->op_type == UNOP) {
		// unary expression */
		if (!*++st.wp)
			return syntax(sh, st.wp_op->op_text, "argument expected");
		switch (n) {
		case STREZ:
			return strlen(*st.wp) == 0;
		case STRNZ:
			return strlen(*st.wp) != 0;
		case FILTT: {
			intmax_t tmp;
			TRY_SH_SET(tmp, getn(sh, *st.wp));
			return isatty(tmp);
		}
#ifdef HAVE_FACCESSAT
		case FILRD:
			return test_file_access(sh, *st.wp, R_OK);
		case FILWR:
			return test_file_access(sh, *st.wp, W_OK);
		case FILEX:
			return test_file_access(sh, *st.wp, X_OK);
#endif
		default:
			return filstat(*st.wp, n);
		}
	}

	if (t_lex(st, st.wp + 1), st.wp_op && st.wp_op->op_type == BINOP) {
		return binop(sh, st);
	}

	return strlen(*st.wp) > 0;
}

Result<int>
binop(Context &sh, TestState &st)
{
	const char *opnd1, *opnd2;
	const t_op *op;

	opnd1 = *st.wp;
	t_lex(st, ++st.wp);
	op = st.wp_op;

	if (!(opnd2 = *++st.wp))
		return syntax(sh, op->op_text, "argument expected");

	switch (op->op_num) {
	default:
	case STREQ:
		return strcmp(opnd1, opnd2) == 0;
	case STRNE:
		return strcmp(opnd1, opnd2) != 0;
	case STRLT:
		return strcoll(opnd1, opnd2) < 0;
	case STRGT:
		return strcoll(opnd1, opnd2) > 0;
	case INTEQ: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a == b;
	}
	case INTNE: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a != b;
	}
	case INTGE: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a >= b;
	}
	case INTGT: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a > b;
	}
	case INTLE: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a <= b;
	}
	case INTLT: {
		intmax_t a, b;
		TRY_SH_SET(a, getn(sh, opnd1));
		TRY_SH_SET(b, getn(sh, opnd2));
		return a < b;
	}
	case FILNT:
		return newerf(opnd1, opnd2);
	case FILOT:
		return olderf(opnd1, opnd2);
	case FILEQ:
		return equalf(opnd1, opnd2);
	}
}

int
filstat(char *nm, enum token mode)
{
	struct stat64 s;

	if (mode == FILSYM ? lstat64(nm, &s) : stat64(nm, &s))
		return 0;

	switch (mode) {
#ifndef HAVE_FACCESSAT
	case FILRD:
		return test_access(&s, R_OK);
	case FILWR:
		return test_access(&s, W_OK);
	case FILEX:
		return test_access(&s, X_OK);
#endif
	case FILEXIST:
		return 1;
	case FILREG:
		return S_ISREG(s.st_mode);
	case FILDIR:
		return S_ISDIR(s.st_mode);
	case FILCDEV:
		return S_ISCHR(s.st_mode);
	case FILBDEV:
		return S_ISBLK(s.st_mode);
	case FILFIFO:
		return S_ISFIFO(s.st_mode);
	case FILSOCK:
		return S_ISSOCK(s.st_mode);
	case FILSYM:
		return S_ISLNK(s.st_mode);
	case FILSUID:
		return (s.st_mode & S_ISUID) != 0;
	case FILSGID:
		return (s.st_mode & S_ISGID) != 0;
#ifdef S_ISVTX
	case FILSTCK:
		return (s.st_mode & S_ISVTX) != 0;
#endif
	case FILGZ:
		return !!s.st_size;
	case FILUID:
		return s.st_uid == geteuid();
	case FILGID:
		return s.st_gid == getegid();
	default:
		return 1;
	}
}

enum token
t_lex(TestState &st, char **tp)
{
	char *s = *tp;

	if (s == 0) {
		st.wp_op = nullptr;
		return EOI;
	}

	const t_op *op = getop(s);
	if (op && !(op->op_type == UNOP && isoperand(tp)) &&
	    !(op->op_num == LPAREN && !tp[1])) {
		st.wp_op = op;
		return static_cast<token>(op->op_num);
	}

	st.wp_op = nullptr;
	return OPERAND;
}

int
isoperand(char **tp)
{
	char *s;

	if (!(s = tp[1]))
		return 1;
	if (!tp[2])
		return 0;

	const t_op *op = getop(s);
	return op && op->op_type == BINOP;
}

bool
newerf(const char *f1, const char *f2)
{
	struct stat64 b1, b2;

	if (stat64(f1, &b1) != 0)
		return false;
	if (stat64(f2, &b2) != 0)
		return true;

#ifdef HAVE_ST_MTIM
	return b1.st_mtim.tv_sec > b2.st_mtim.tv_sec ||
	       (b1.st_mtim.tv_sec == b2.st_mtim.tv_sec &&
		b1.st_mtim.tv_nsec > b2.st_mtim.tv_nsec);
#else
	return b1.st_mtime > b2.st_mtime;
#endif
}

bool
olderf(const char *f1, const char *f2)
{
	struct stat64 b1, b2;

	if (stat64(f2, &b2) != 0)
		return false;
	if (stat64(f1, &b1) != 0)
		return true;

#ifdef HAVE_ST_MTIM
	return b1.st_mtim.tv_sec < b2.st_mtim.tv_sec ||
	       (b1.st_mtim.tv_sec == b2.st_mtim.tv_sec &&
		b1.st_mtim.tv_nsec < b2.st_mtim.tv_nsec);
#else
	return b1.st_mtime < b2.st_mtime;
#endif
}

int
equalf(const char *f1, const char *f2)
{
	struct stat64 b1, b2;

	return (stat64(f1, &b1) == 0 &&
		stat64(f2, &b2) == 0 &&
		b1.st_dev == b2.st_dev &&
		b1.st_ino == b2.st_ino);
}
} // namespace

#ifdef HAVE_FACCESSAT
namespace {
int
has_exec_bit_set(Context &sh, const char *path)
{
	struct stat64 st;

	if (stat64(path, &st))
		return 0;
	return st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH);
}
} // namespace

int
test_file_access(Context &sh, const char *path, int mode)
{
	if (faccessat_confused_about_superuser(sh) &&
	    mode == X_OK && geteuid() == 0 && !has_exec_bit_set(sh, path))
		return 0;
	return !faccessat(AT_FDCWD, path, mode, AT_EACCESS);
}
#else	/* HAVE_FACCESSAT */
/*
 * The manual, and IEEE POSIX 1003.2, suggests this should check the mode bits,
 * not use access():
 *
 *	True shall indicate only that the write flag is on.  The file is not
 *	writable on a read-only file system even if this test indicates true.
 *
 * Unfortunately IEEE POSIX 1003.1-2001, as quoted in SuSv3, says only:
 *
 *	True shall indicate that permission to read from file will be granted,
 *	as defined in "File Read, Write, and Creation".
 *
 * and that section says:
 *
 *	When a file is to be read or written, the file shall be opened with an
 *	access mode corresponding to the operation to be performed.  If file
 *	access permissions deny access, the requested operation shall fail.
 *
 * and of course access permissions are described as one might expect:
 *
 *     * If a process has the appropriate privilege:
 *
 *        * If read, write, or directory search permission is requested,
 *          access shall be granted.
 *
 *        * If execute permission is requested, access shall be granted if
 *          execute permission is granted to at least one user by the file
 *          permission bits or by an alternate access control mechanism;
 *          otherwise, access shall be denied.
 *
 *   * Otherwise:
 *
 *        * The file permission bits of a file contain read, write, and
 *          execute/search permissions for the file owner class, file group
 *          class, and file other class.
 *
 *        * Access shall be granted if an alternate access control mechanism
 *          is not enabled and the requested access permission bit is set for
 *          the class (file owner class, file group class, or file other class)
 *          to which the process belongs, or if an alternate access control
 *          mechanism is enabled and it allows the requested access; otherwise,
 *          access shall be denied.
 *
 * and when I first read this I thought:  surely we can't go about using
 * open(O_WRONLY) to try this test!  However the POSIX 1003.1-2001 Rationale
 * section for test does in fact say:
 *
 *	On historical BSD systems, test -w directory always returned false
 *	because test tried to open the directory for writing, which always
 *	fails.
 *
 * and indeed this is in fact true for Seventh Edition UNIX, UNIX 32V, and UNIX
 * System III, and thus presumably also for BSD up to and including 4.3.
 *
 * Secondly I remembered why using open() and/or access() are bogus.  They
 * don't work right for detecting read and write permissions bits when called
 * by root.
 *
 * Interestingly the 'test' in 4.4BSD was closer to correct (as per
 * 1003.2-1992) and it was implemented efficiently with stat() instead of
 * open().
 *
 * This was apparently broken in NetBSD around about 1994/06/30 when the old
 * 4.4BSD implementation was replaced with a (arguably much better coded)
 * implementation derived from pdksh.
 *
 * Note that modern pdksh is yet different again, but still not correct, at
 * least not w.r.t. 1003.2-1992.
 *
 * As I think more about it and read more of the related IEEE docs I don't like
 * that wording about 'test -r' and 'test -w' in 1003.1-2001 at all.  I very
 * much prefer the original wording in 1003.2-1992.  It is much more useful,
 * and so that's what I've implemented.
 *
 * (Note that a strictly conforming implementation of 1003.1-2001 is in fact
 * totally useless for the case in question since its 'test -w' and 'test -r'
 * can never fail for root for any existing files, i.e. files for which 'test
 * -e' succeeds.)
 *
 * The rationale for 1003.1-2001 suggests that the wording was "clarified" in
 * 1003.1-2001 to align with the 1003.2b draft.  1003.2b Draft 12 (July 1999),
 * which is the latest copy I have, does carry the same suggested wording as is
 * in 1003.1-2001, with its rationale saying:
 *
 * 	This change is a clarification and is the result of interpretation
 * 	request PASC 1003.2-92 #23 submitted for IEEE Std 1003.2-1992.
 *
 * That interpretation can be found here:
 *
 *   http://www.pasc.org/interps/unofficial/db/p1003.2/pasc-1003.2-23.html
 *
 * Not terribly helpful, unfortunately.  I wonder who that fence sitter was.
 *
 * Worse, IMVNSHO, I think the authors of 1003.2b-D12 have mis-interpreted the
 * PASC interpretation and appear to be gone against at least one widely used
 * implementation (namely 4.4BSD).  The problem is that for file access by root
 * this means that if test '-r' and '-w' are to behave as if open() were called
 * then there's no way for a shell script running as root to check if a file
 * has certain access bits set other than by the grotty means of interpreting
 * the sh.output of 'ls -l'.  This was widely considered to be a bug in V7's
 * "test" and is, I believe, one of the reasons why direct use of access() was
 * avoided in some more recent implementations!
 *
 * I have always interpreted '-r' to match '-w' and '-x' as per the original
 * wording in 1003.2-1992, not the other way around.  I think 1003.2b goes much
 * too far the wrong way without any valid rationale and that it's best if we
 * stick with 1003.2-1992 and test the flags, and not mimic the behaviour of
 * open() since we already know very well how it will work -- existance of the
 * file is all that matters to open() for root.
 *
 * Unfortunately the SVID is no help at all (which is, I guess, partly why
 * we're in this mess in the first place :-).
 *
 * The SysV implementation (at least in the 'test' builtin in /bin/sh) does use
 * access(name, 2) even though it also goes to much greater lengths for '-x'
 * matching the 1003.2-1992 definition (which is no doubt where that definition
 * came from).
 *
 * The ksh93 implementation uses access() for '-r' and '-w' if
 * (euid==uid&&egid==gid), but uses st_mode for '-x' iff running as root.
 * i.e. it does strictly conform to 1003.1-2001 (and presumably 1003.2b).
 */
int test_access(const struct stat64 *sp, int stmode)
{
	int n;
	uid_t euid;
	int maxgroups;

	/*
	 * I suppose we could use access() if not running as root and if we are
	 * running with ((euid == uid) && (egid == gid)), but we've already
	 * done the stat() so we might as well just test the permissions
	 * directly instead of asking the kernel to do it....
	 */
	euid = geteuid();
	if (euid == 0) {
		if (stmode != X_OK)
			return 1;

		/* any bit is good enough */
		stmode = (stmode << 6) | (stmode << 3) | stmode;
	} else if (sp->st_uid == euid)
		stmode <<= 6;
	else if (sp->st_gid == getegid())
		stmode <<= 3;
	else {
		/* XXX stolen almost verbatim from ksh93.... */
		/* on some systems you can be in several groups */
		maxgroups = getgroups(0, nullptr);
		std::vector<gid_t> groupvec(static_cast<size_t>(maxgroups));
		n = getgroups(maxgroups, groupvec.data());
		while (--n >= 0) {
			if (groupvec[static_cast<size_t>(n)] == sp->st_gid) {
				stmode <<= 3;
				break;
			}
		}
	}

	return sp->st_mode & stmode;
}
#endif	/* HAVE_FACCESSAT */

} // namespace shell
