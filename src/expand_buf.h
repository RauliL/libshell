#ifndef EXPAND_BUF_H
#define EXPAND_BUF_H

#include <cstddef>
#include <cstring>
#include <string>

/*
 * Growable contiguous string builder for shell word expansion and similar
 * assembly. Always stack-local / passed by reference — never stored on
 * Context.
 */
struct ExpandBuf {
	std::string buf = std::string(256, '\0');
	std::size_t used = 0; /* committed bytes; also base of in-progress write */
	std::size_t cap_ = 256;
	char *dest = nullptr; /* write cursor within the in-progress region */

	char *data() { return buf.data(); }
	const char *data() const { return buf.data(); }

	char *base()
	{
		ensure_avail(1);
		return buf.data() + used;
	}
	char *cap_ptr()
	{
		ensure_avail(1);
		return buf.data() + cap_;
	}
	std::size_t avail() const
	{
		return cap_ >= used ? cap_ - used : 0;
	}

	void ensure_avail(std::size_t n)
	{
		if (avail() >= n)
			return;
		std::size_t need = used + n;
		std::size_t newcap = cap_ ? cap_ : 256;
		while (newcap < need)
			newcap *= 2;
		std::size_t dest_off = dest ? static_cast<std::size_t>(dest - buf.data()) : used;
		buf.resize(newcap);
		cap_ = newcap;
		if (dest)
			dest = buf.data() + dest_off;
	}

	void start() { dest = base(); }

	char *commit(std::size_t nbytes)
	{
		ensure_avail(nbytes);
		char *p = buf.data() + used;
		used += nbytes;
		return p;
	}

	char *commit_to(char *p)
	{
		return commit(static_cast<std::size_t>(p - base()));
	}

	void rewind(std::size_t u)
	{
		if (u > used)
			u = used;
		used = u;
	}

	void reset()
	{
		used = 0;
		dest = nullptr;
	}

	/* Grow so there are at least newlen free bytes after p; return rebased p. */
	char *makespace(std::size_t newlen, char *p)
	{
		std::size_t len = static_cast<std::size_t>(p - base());
		if (avail() < len + newlen)
			ensure_avail(len + newlen);
		return base() + len;
	}

	char *putc(char *p, int c)
	{
		if (p >= cap_ptr()) {
			std::size_t off = static_cast<std::size_t>(p - base());
			ensure_avail(off + 2);
			p = base() + off;
		}
		*p++ = static_cast<char>(c);
		return p;
	}

	char *nputs(const char *s, std::size_t n, char *p)
	{
		p = makespace(n, p);
		return static_cast<char *>(mempcpy(p, s, n));
	}

	char *puts(const char *s, char *p)
	{
		return nputs(s, std::strlen(s), p);
	}

	void putc_nul(char *&p)
	{
		if (p == cap_ptr()) {
			std::size_t off = static_cast<std::size_t>(p - base());
			ensure_avail(off + 2);
			p = base() + off;
		}
		*p = '\0';
	}
};

#endif
