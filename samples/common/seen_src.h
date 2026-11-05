/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bounded list of peer addresses that already have a static ARP entry.
 * Ticket 47ca2a4774.
 *
 * The GCP fabric never answers ARP for a guest address. A sample that
 * replies to an inbound packet must map that source to the encoded
 * gateway MAC itself. Without a bound, a sender that rotates its
 * source address adds one entry per packet and fills the lwIP ARP
 * table.
 *
 * This list holds the addresses the app already handled. It uses plain
 * uint32_t values, so a host test program runs the same code the
 * unikernel runs. Addresses stay in network byte order, the order both
 * the socket API and lwIP use.
 */

#ifndef _UKG_SAMPLES_SEEN_SRC_H
#define _UKG_SAMPLES_SEEN_SRC_H

#include <stdint.h>

/* A benchmark run uses one client and one server. Eight is far above
 * that, and it caps the ARP pressure one sender can cause. */
#define UKG_SEEN_SRC_MAX 8

struct ukg_seen_src {
	uint32_t addr[UKG_SEEN_SRC_MAX];
	unsigned int count;
};

/* Return 1 when addr is already in the list. Return 0 otherwise. */
static inline int ukg_seen_src_known(const struct ukg_seen_src *list,
				     uint32_t addr)
{
	unsigned int i;

	for (i = 0; i < list->count; i++) {
		if (list->addr[i] == addr)
			return 1;
	}
	return 0;
}

/*
 * Record one source address.
 *
 * Return 1 when the address is new and now stored. Return 0 when the
 * list already held it. Return -1 when the list is full, which tells
 * the caller to skip this source.
 */
static inline int ukg_seen_src_add(struct ukg_seen_src *list, uint32_t addr)
{
	if (ukg_seen_src_known(list, addr))
		return 0;
	if (list->count >= UKG_SEEN_SRC_MAX)
		return -1;

	list->addr[list->count++] = addr;
	return 1;
}

/*
 * Decide whether src sits in the local subnet. Pass the local address
 * and the netmask as the netif reports them. All three stay in network
 * byte order, so one AND per side is enough.
 *
 * A netmask of 0 matches every address. That is the right fallback for
 * a netif with no mask: the caller keeps its previous behavior.
 */
static inline int ukg_src_in_subnet(uint32_t src, uint32_t local,
				    uint32_t mask)
{
	return (src & mask) == (local & mask);
}

#endif /* _UKG_SAMPLES_SEEN_SRC_H */
