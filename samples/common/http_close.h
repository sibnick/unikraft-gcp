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

#include <stddef.h>

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
 * option. Match "Connection:" only at request start or line start.
 * Parse comma-delimited and space-delimited tokens in the header value.
 * Match "close" as a whole token without case sensitivity.
 * A request without this option returns 0. Tickets 5f45856dfe, d20c2280d4.
 */
static inline int ukg_http_wants_close(const char *req)
{
	static const char name[] = "connection:";
	const char *p = req;

	if (!req)
		return 0;

	while (*p != '\0') {
		/* Match only at start of request or immediately after newline */
		if (p == req || *(p - 1) == '\n') {
			if (ukg_starts_ci(p, name)) {
				const char *v = p + (sizeof(name) - 1);

				while (*v != '\0' && *v != '\r' && *v != '\n') {
					const char *tok_start;
					size_t tok_len;

					/* Skip whitespace and commas */
					while (*v == ' ' || *v == '\t' || *v == ',')
						v++;
					if (*v == '\0' || *v == '\r' || *v == '\n')
						break;

					tok_start = v;
					while (*v != '\0' && *v != '\r' && *v != '\n' &&
					       *v != ' ' && *v != '\t' && *v != ',')
						v++;
					tok_len = (size_t)(v - tok_start);

					if (tok_len == 5 &&
					    ukg_starts_ci(tok_start, "close"))
						return 1;
				}
			}
		}
		p++;
	}
	return 0;
}

#endif /* _UKG_SAMPLES_HTTP_CLOSE_H */
