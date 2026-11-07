/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Strict decimal integer parsing for sample command line parameters.
 * Tickets 3a53a6f5d4 and 5d4890b96e.
 *
 * atoi hides every error. A negative value turns into a huge size_t
 * after a cast, and a typo turns into 0. Both then size a buffer that
 * is too small for the traffic the run sends into it.
 *
 * This header holds the one parser the samples use. It takes the
 * allowed range from the caller and rejects anything else.
 *
 * Only stdint, stdlib and errno are needed. The Linux host build, the
 * Unikraft nolibc build, and a host test program all compile the same
 * code.
 */

#ifndef _UKG_SAMPLES_UKARG_H
#define _UKG_SAMPLES_UKARG_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Parse one decimal integer and store it in *out.
 *
 * The text must start with a digit and must end after the digits. A
 * sign, a leading space, a suffix such as 8K, an empty string, and an
 * overflow all fail. The value must also sit between min and max,
 * inclusive.
 *
 * Return 0 and set *out on success. Return -1 on any error and leave
 * *out unchanged.
 */
static inline int ukarg_u64(const char *text, uint64_t min, uint64_t max,
			    uint64_t *out)
{
	unsigned long long parsed;
	char *end;

	if (!text || !out)
		return -1;
	if (text[0] < '0' || text[0] > '9')
		return -1;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;
	if ((uint64_t)parsed < min || (uint64_t)parsed > max)
		return -1;

	*out = (uint64_t)parsed;
	return 0;
}

#endif /* _UKG_SAMPLES_UKARG_H */
