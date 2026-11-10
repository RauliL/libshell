#ifndef EXPAND_BUF_H
#define EXPAND_BUF_H

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

/*
 * Growable contiguous string builder for shell word expansion and similar
 * assembly. Always stack-local / passed by reference — never stored on
 * Context.
 *
 * Write position is an absolute index into buf (pos), not a raw char*.
 * Relocations from growth never invalidate the cursor; char* helpers rebase
 * from indices when callers still need a temporary pointer.
 */
namespace shell {

struct ExpandBuf {
	std::string buf = std::string(256, '\0');
	std::size_t used = 0; /* committed bytes; also base of in-progress write */
	std::size_t pos = 0;  /* absolute write cursor within buf */
	std::size_t cap_ = 256;

	char *data() { return buf.data(); }
	const char *data() const { return buf.data(); }

	char *base()
	{
		ensure_avail(1);
		return buf.data() + used;
	}
	const char *base() const { return buf.data() + used; }

	/* Absolute write cursor as a pointer (valid until the next growth). */
	char *ptr()
	{
		ensure_avail(1);
		return buf.data() + pos;
	}
	const char *ptr() const { return buf.data() + pos; }

	/* Offset of the write cursor from the in-progress base. */
	std::size_t rel() const { return pos >= used ? pos - used : 0; }

	void set_rel(std::size_t r) { pos = used + r; }

	void set_ptr(const char *p)
	{
		pos = static_cast<std::size_t>(p - buf.data());
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
		buf.resize(newcap);
		cap_ = newcap;
	}

	void start() { pos = used; }

	/* Advance the committed frontier by nbytes. Does not move pos. */
	char *commit(std::size_t nbytes)
	{
		ensure_avail(nbytes);
		char *p = buf.data() + used;
		used += nbytes;
		return p;
	}

	/* Commit the in-progress region and reset the cursor to its end. */
	char *commit_rel()
	{
		char *p = commit(rel());
		pos = used;
		return p;
	}

	char *commit_to(char *p)
	{
		char *r = commit(static_cast<std::size_t>(p - base()));
		pos = used;
		return r;
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
		pos = 0;
	}

	/* In-progress region as a view (no trailing NUL required). */
	std::string_view view() const
	{
		return {buf.data() + used, rel()};
	}

	std::string_view view(std::size_t beg, std::size_t end) const
	{
		return {buf.data() + used + beg, end - beg};
	}

	/* Grow so there are at least newlen free bytes after p; return rebased p. */
	char *makespace(std::size_t newlen, char *p)
	{
		std::size_t len = static_cast<std::size_t>(p - base());
		if (avail() < len + newlen)
			ensure_avail(len + newlen);
		return base() + len;
	}

	void makespace(std::size_t newlen) { ensure_avail(rel() + newlen); }

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

	void putc(int c)
	{
		ensure_avail(rel() + 2);
		buf[pos++] = static_cast<char>(c);
	}

	char *nputs(const char *s, std::size_t n, char *p)
	{
		p = makespace(n, p);
		std::memcpy(p, s, n);
		return p + n;
	}

	void nputs(const char *s, std::size_t n)
	{
		ensure_avail(rel() + n);
		std::memcpy(buf.data() + pos, s, n);
		pos += n;
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

	void putc_nul()
	{
		ensure_avail(rel() + 1);
		buf[pos] = '\0';
	}
};

} // namespace shell

#endif
