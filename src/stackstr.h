#ifndef STACKSTR_H
#define STACKSTR_H

#include <cstddef>
#include <cstring>
#include <string>

/*
 * Growable contiguous buffer for shell string building, backed by std::string.
 * Replaces the ash linked-list stack arena. During a write, base stays fixed
 * and the cursor advances; commit advances the used offset.
 */
struct StackStr {
	std::string buf = std::string(256, '\0');
	std::size_t used = 0; /* committed bytes; also base of in-progress write */
	std::size_t cap_ = 256;

	char *data() { return buf.data(); }
	const char *data() const { return buf.data(); }

	char *base()
	{
		ensure_avail(1);
		return buf.data() + used;
	}
	char *cap()
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

	/* Commit nbytes starting at the current base; return that base. */
	char *commit(std::size_t nbytes)
	{
		ensure_avail(nbytes);
		char *p = buf.data() + used;
		used += nbytes;
		return p;
	}

	void rewind(std::size_t u)
	{
		if (u > used)
			u = used;
		used = u;
	}

	void reset() { used = 0; }
};

#endif
