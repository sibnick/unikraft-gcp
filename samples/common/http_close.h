/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * HTTP Connection header scan for the httpreply samples.
 * Ticket 5f45856dfe.
 *
 * The server must close a connection when the client asks for it with
 * "Connection: close". HTTP gives the header name and its value no
 * case, and it allows spaces after the colon. nolibc has no strcasestr,
 * so this header carries the compare loop.
 *
 * The functions use plain chars only. A host test program runs the
 * same code the unikernel runs.
 */

#ifndef _UKG_SAMPLES_HTTP_CLOSE_H
#define _UKG_SAMPLES_HTTP_CLOSE_H

/* Return the lower-case form of one ASCII character. */
static inline char ukg_ci_lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		return (char)(c - 'A' + 'a');
	return c;
}

/* Return 1 when text starts with name, ignoring case. */
static inline int ukg_starts_ci(const char *text, const char *name)
{
	while (*name != '\0') {
		if (ukg_ci_lower(*text) != ukg_ci_lower(*name))
			return 0;
		text++;
		name++;
	}
	return 1;
}

/*
 * Return 1 when the request carries a Connection header with the close
 * option. "Connection: close", "CONNECTION:CLOSE", and
 * "Connection:  Close" all count. A header with another value, and a
 * request with no such header, return 0.
 */
static inline int ukg_http_wants_close(const char *req)
{
	static const char name[] = "connection:";
	static const char opt[] = "close";
	const char *p = req;

	while (*p != '\0') {
		const char *v;

		if (!ukg_starts_ci(p, name)) {
			p++;
			continue;
		}
		v = p + (sizeof(name) - 1);
		while (*v == ' ' || *v == '\t')
			v++;
		if (ukg_starts_ci(v, opt))
			return 1;
		p = v;
	}
	return 0;
}

#endif /* _UKG_SAMPLES_HTTP_CLOSE_H */
