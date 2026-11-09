/*
 * Signal name tables. Built once from #ifdef SIG* so number ↔ name mapping
 * matches the host platform without a build-time generator.
 */
#include <csignal>
#include <string>

#include "mystring.h"
#include "shell_context.h"


using namespace shell;


namespace shell {

void
init_signal_names(Context &sh)
{
	static const Context::signal_map_type global_signals = {
		// EXIT is trap "signal" 0.
		{ 0, "EXIT" },

		// AIX
#if defined(SIGLOST)
		{ SIGLOST, "LOST" },
#endif
#if defined(SIGMSG)
		{ SIGMSG, "MSG" },
#endif
#if defined(SIGDANGER)
		{ SIGDANGER, "DANGER" },
#endif
#if defined(SIGMIGRATE)
		{ SIGMIGRATE, "MIGRATE" },
#endif
#if defined(SIGPRE)
		{ SIGPRE, "PRE" },
#endif
#if defined(SIGVIRT)
		{ SIGVIRT, "VIRT" },
#endif
#if defined(SIGALRM1)
		{ SIGALRM1, "ALRM1" },
#endif
#if defined(SIGWAITING)
		{ SIGWAITING, "WAITING" },
#endif
#if defined(SIGGRANT)
		{ SIGGRANT, "GRANT" },
#endif
#if defined(SIGKAP)
		{ SIGKAP, "KAP" },
#endif
#if defined(SIGRETRACT)
		{ SIGRETRACT, "RETRACT" },
#endif
#if defined(SIGSOUND)
		{ SIGSOUND, "SOUND" },
#endif
#if defined(SIGSAK)
		{ SIGSAK, "SAK" },
#endif

		// SunOS5
#if defined(SIGLWP)
		{ SIGLWP, "LWP" },
#endif
#if defined(SIGFREEZE)
		{ SIGFREEZE, "FREEZE" },
#endif
#if defined(SIGTHAW)
		{ SIGTHAW, "THAW" },
#endif
#if defined(SIGCANCEL)
		{ SIGCANCEL, "CANCEL" },
#endif

		// HP-UX
#if defined(SIGDIL)
		{ SIGDIL, "DIL" },
#endif

		// System V
#if defined(SIGCLD)
		{ SIGCLD, "CLD" },
#endif
#if defined(SIGPWR)
		{ SIGPWR, "PWR" },
#endif
#if defined(SIGPOLL)
		{ SIGPOLL, "POLL" },
#endif

#if defined(SIGWINDOW)
		{ SIGWINDOW, "WINDOW" },
#endif

		// Common
#if defined(SIGHUP)
		{ SIGHUP, "HUP" },
#endif
#if defined(SIGINT)
		{ SIGINT, "INT" },
#endif
#if defined(SIGQUIT)
		{ SIGQUIT, "QUIT" },
#endif
#if defined(SIGILL)
		{ SIGILL, "ILL" },
#endif
#if defined(SIGTRAP)
		{ SIGTRAP, "TRAP" },
#endif
#if defined(SIGIOT)
		{ SIGIOT, "IOT" },
#endif
#if defined(SIGABRT)
		{ SIGABRT, "ABRT" },
#endif
#if defined(SIGEMT)
		{ SIGEMT, "EMT" },
#endif
#if defined(SIGFPE)
		{ SIGFPE, "FPE" },
#endif
#if defined(SIGKILL)
		{ SIGKILL, "KILL" },
#endif
#if defined(SIGBUS)
		{ SIGBUS, "BUS" },
#endif
#if defined(SIGSEGV)
		{ SIGSEGV, "SEGV" },
#endif
#if defined(SIGSYS)
		{ SIGSYS, "SYS" },
#endif
#if defined(SIGPIPE)
		{ SIGPIPE, "PIPE" },
#endif
#if defined(SIGALRM)
		{ SIGALRM, "ALRM" },
#endif
#if defined(SIGTERM)
		{ SIGTERM, "TERM" },
#endif
#if defined(SIGURG)
		{ SIGURG, "URG" },
#endif
#if defined(SIGSTOP)
		{ SIGSTOP, "STOP" },
#endif
#if defined(SIGTSTP)
		{ SIGTSTP, "TSTP" },
#endif
#if defined(SIGCONT)
		{ SIGCONT, "CONT" },
#endif
#if defined(SIGCHLD)
		{ SIGCHLD, "CHLD" },
#endif
#if defined(SIGTTIN)
		{ SIGTTIN, "TTIN" },
#endif
#if defined(SIGTTOU)
		{ SIGTTOU, "TTOU" },
#endif
#if defined(SIGIO)
		{ SIGIO, "IO" },
#endif
#if defined(SIGXCPU)
		{ SIGXCPU, "XCPU" },
#endif
#if defined(SIGXFSZ)
		{ SIGXFSZ, "XFSZ" },
#endif
#if defined(SIGVTALRM)
		{ SIGVTALRM, "VTALRM" },
#endif
#if defined(SIGPROF)
		{ SIGPROF, "PROF" },
#endif
#if defined(SIGWINCH)
		{ SIGWINCH, "WINCH" },
#endif
#if defined(SIGINFO) && !defined(_SEQUENT_)
		{ SIGINFO, "INFO" },
#endif
#if defined(SIGUSR1)
		{ SIGUSR1, "USR1" },
#endif
#if defined(SIGUSR2)
		{ SIGUSR2, "USR2" },
#endif
#if defined(SIGKILLTHR)
		{ SIGKILLTHR, "KILLTHR" },
#endif
	};

	sh.signal_map = global_signals;

#if defined(SIGRTMIN)
	const int rtmin = SIGRTMIN;

	sh.signal_map[rtmin] = "RTMIN";
#if defined(SIGRTMAX)
	const int rt_limit = 256;
	const int rtmax = SIGRTMAX;

	sh.signal_map[rtmax] = "RTMAX";
	if (rtmax > rtmin) {
		int rtcnt = (rtmax - rtmin - 1) / 2;

		if (rtcnt >= rt_limit / 2)
			rtcnt = rt_limit / 2 - 1;

		for (int i = 1; i <= rtcnt; ++i) {
			sh.signal_map[rtmin + i] = "RTMIN+" + std::to_string(i);
			sh.signal_map[rtmax - i] = "RTMAX-" + std::to_string(i);
		}

		if (rtcnt < rt_limit / 2 - 1 && rtcnt != (rtmax - rtmin) / 2)
			sh.signal_map[rtmin + rtcnt + 1] = "RTMIN+" + std::to_string(rtcnt + 1);
	}
#endif
#endif
}

std::optional<std::string_view>
Context::find_signal_name(int signo) const
{
	const auto it = signal_map.find(signo);

	return it != std::end(signal_map) ? it->second : "";
}

std::optional<int>
Context::find_signal_number(std::string_view name, int minsig) const
{
	const auto upper = ascii_upper(name);

	for (const auto &entry : signal_map)
	{
		if (entry.second == upper && entry.first >= minsig)
		{
			return entry.first;
		}
	}

	return std::nullopt;
}

} // namespace shell
