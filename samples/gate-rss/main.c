/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft gate-rss: multi-queue RSS steering cloud gate.
 *
 * The app drives the gve driver with N queue pairs and proves that
 * the device spreads client flows across every RX queue. It runs on
 * raw uk_netdev with no TCP/IP stack, so the app itself builds and
 * parses Ethernet, IPv4, and UDP headers. Ticket a9b1936640.
 *
 * Roles:
 *   client: sends flows of UDP packets. Each flow uses one distinct
 *     source port, so RSS sees a distinct 5-tuple per flow. The
 *     client counts echo replies, then requests the server report.
 *   server: receives on every configured RX queue, echoes each flow
 *     packet back, counts packets per RX queue, and answers the
 *     report request with those counts.
 *
 * Gate markers:
 *   server log: GATE_RSS_PASS queues=N perq=c0,c1,...  every RX
 *     queue carried traffic and at least 90 percent of the flow
 *     packets arrived.
 *   client log: GATE_RSS_TRAFFIC echoes=E sent=S  at least 99
 *     percent of the sent packets came back.
 *   GATE_FAIL line in either log marks the run failed.
 *
 * Addressing uses the GCP gateway MAC scheme from the networkload
 * sample. The fabric delivers a frame by destination IP when the
 * destination MAC is the gateway MAC: 0x42 0x01 followed by the
 * gateway IPv4 bytes. No ARP traffic is needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <uk/alloc.h>
#include <uk/netbuf.h>
#include <uk/netdev.h>
#include <uk/plat/time.h>

#define rss_htons(x)	((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define rss_ntohs(x)	rss_htons(x)
#define rss_htonl(x)	((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define rss_ntohl(x)	rss_htonl(x)

#define RSS_MAGIC	0x554b4733u	/* "UKG3" */

#define RSS_KIND_DATA		0u
#define RSS_KIND_ECHO		1u
#define RSS_KIND_REPORT_REQ	2u
#define RSS_KIND_REPORT		3u

#define RSS_PORT_DEFAULT	9000
#define RSS_FLOWS_DEFAULT	48
#define RSS_PKTS_DEFAULT	12
#define RSS_QUEUES_DEFAULT	2

#define RSS_MAX_QUEUES		8u	/* matches GVE_MAX_QUEUES */
#define RSS_MAX_FLOWS		128u
#define RSS_MAX_PKTS		64u
#define RSS_BUFLEN		2048u
#define RSS_FRAME_MAX		256u

struct rss_eth {
	uint8_t dst[6];
	uint8_t src[6];
	uint16_t type;
} __attribute__((packed));

struct rss_ipv4 {
	uint8_t vihl;
	uint8_t tos;
	uint16_t tot_len;
	uint16_t id;
	uint16_t frag;
	uint8_t ttl;
	uint8_t proto;
	uint16_t check;
	uint32_t src;
	uint32_t dst;
} __attribute__((packed));

struct rss_udp {
	uint16_t sport;
	uint16_t dport;
	uint16_t len;
	uint16_t check;
} __attribute__((packed));

struct rss_msg {
	uint32_t magic;
	uint8_t kind;
	uint8_t rsvd;
	uint16_t flow;
	uint32_t seq;
} __attribute__((packed));

/* Report body follows rss_msg: per-queue counts, total, and queue
 * count, all big-endian. */
struct rss_report {
	struct rss_msg hdr;
	uint32_t counts[RSS_MAX_QUEUES];
	uint32_t total;
	uint32_t queues;
} __attribute__((packed));

struct rss_ctx {
	struct uk_netdev *dev;
	struct uk_alloc *a;
	uint16_t queues;
	uint16_t flows;
	uint16_t pkts;
	uint16_t port;
	uint32_t own_ip;	/* host order */
	uint32_t peer_ip;	/* host order */
	uint8_t own_mac[6];
	uint8_t gw_mac[6];
	uint16_t align;	/* netbuf I/O alignment from info_get */
	uint16_t encap_tx;	/* tx headroom the device reserves */
	uint16_t encap_rx;	/* rx headroom the device reserves */
};

/*
 * IPv4 header checksum over the 20-byte header. The header is packed,
 * so read the 16-bit words as byte pairs instead of casting the
 * packed pointer. Summing big-endian words and complementing keeps
 * the result byte-order neutral, so it can be stored directly.
 */
static uint16_t rss_ip_csum(const struct rss_ipv4 *ih)
{
	const uint8_t *b = (const uint8_t *)ih;
	uint32_t sum = 0;
	int i;

	for (i = 0; i < 10; i++) {
		uint16_t w = (uint16_t)(((uint16_t)b[i * 2] << 8) |
					b[i * 2 + 1]);

		sum += w;
	}
	while (sum >> 16)
		sum = (sum & 0xffffU) + (sum >> 16);
	return (uint16_t)~sum;
}

/*
 * Build one Ethernet + IPv4 + UDP frame carrying an rss_msg. The
 * payload kind selects the size: a report carries the count block.
 * Returns the frame length, at least 60 by zero pad.
 */
static size_t rss_build_frame(struct rss_ctx *c, uint8_t *buf,
			      uint32_t dst_ip, uint16_t sport,
			      uint16_t dport, uint8_t kind,
			      uint16_t flow, uint32_t seq,
			      const struct rss_report *rep)
{
	struct rss_eth *eth = (struct rss_eth *)buf;
	struct rss_ipv4 *ip = (struct rss_ipv4 *)(buf + sizeof(*eth));
	struct rss_udp *udp = (struct rss_udp *)(ip + 1);
	struct rss_report *rp = (struct rss_report *)(udp + 1);
	size_t pay = sizeof(struct rss_msg);
	size_t hlen = sizeof(*eth) + sizeof(*ip) + sizeof(*udp);
	size_t len;

	memset(buf, 0, RSS_FRAME_MAX);
	memcpy(eth->dst, c->gw_mac, 6);
	memcpy(eth->src, c->own_mac, 6);
	eth->type = rss_htons(0x0800);

	ip->vihl = 0x45;
	ip->tot_len = rss_htons((uint16_t)(sizeof(*ip) + sizeof(*udp) + pay));
	ip->id = rss_htons(0x5253);
	ip->frag = rss_htons(0x4000);
	ip->ttl = 64;
	ip->proto = 17;
	ip->src = rss_htonl(c->own_ip);
	ip->dst = rss_htonl(dst_ip);
	ip->check = rss_ip_csum(ip);

	udp->sport = rss_htons(sport);
	udp->dport = rss_htons(dport);
	udp->check = 0;	/* UDP checksum is optional for IPv4 */

	rp->hdr.magic = rss_htonl(RSS_MAGIC);
	rp->hdr.kind = kind;
	rp->hdr.flow = rss_htons(flow);
	rp->hdr.seq = rss_htonl(seq);
	if (kind == RSS_KIND_REPORT && rep != NULL) {
		unsigned int q;

		pay = sizeof(struct rss_report);
		for (q = 0U; q < RSS_MAX_QUEUES; q++)
			rp->counts[q] = rss_htonl(rep->counts[q]);
		rp->total = rss_htonl(rep->total);
		rp->queues = rss_htonl(rep->queues);
	}
	udp->len = rss_htons((uint16_t)(sizeof(*udp) + pay));

	len = hlen + pay;
	return len < 60U ? 60U : len;
}

static int rss_tx(struct rss_ctx *c, uint16_t q, uint8_t *buf, size_t len)
{
	struct uk_netbuf *nb;
	int ret;

	nb = uk_netbuf_alloc_buf(c->a, RSS_BUFLEN, c->align,
				 c->encap_tx, 0, NULL);
	if (!nb)
		return -1;
	memcpy(nb->data, buf, len);
	nb->len = (uint32_t)len;
	ret = uk_netdev_tx_one(c->dev, q, nb);
	if (!uk_netdev_status_successful(ret)) {
		uk_netbuf_free(nb);
		return -1;
	}
	return 0;
}

static void rss_delay_ms(uint32_t ms)
{
	__nsec end = ukplat_monotonic_clock() + ukarch_time_msec_to_nsec(ms);

	while ((int64_t)(ukplat_monotonic_clock() - end) < 0)
		;
}

/* Send with pacing and bounded ring-full retries. The driver returns
 * -ENOSPC on a full ring and keeps the netbuf, so rss_tx frees it and
 * the retry rebuilds and submits again. */
static int rss_tx_retry(struct rss_ctx *c, uint16_t q, uint8_t *buf,
			size_t len, uint32_t pace_ms)
{
	int attempt;

	for (attempt = 0; attempt < 200; attempt++) {
		if (rss_tx(c, q, buf, len) == 0) {
			if (pace_ms != 0U)
				rss_delay_ms(pace_ms);
			return 0;
		}
		rss_delay_ms(1);
	}
	return -1;
}

static uint16_t rss_alloc_rxpkts(void *argp, struct uk_netbuf *nb[],
				 uint16_t count)
{
	struct rss_ctx *c = (struct rss_ctx *)argp;
	uint16_t i;

	for (i = 0U; i < count; i++) {
		nb[i] = uk_netbuf_alloc_buf(c->a, RSS_BUFLEN, c->align,
					    c->encap_rx, 0, NULL);
		if (!nb[i])
			break;
		/*
		 * The rx path consumes len as the writable space (the
		 * lwIP glue does the same). A fresh netbuf has len 0,
		 * which would post an empty receive buffer.
		 */
		nb[i]->len = nb[i]->buflen - c->encap_rx;
	}
	return i;
}

/*
 * Validate one received frame. On success it fills the message
 * fields and the IPv4 source address and source UDP port.
 */
static int rss_parse(struct rss_ctx *c, struct uk_netbuf *nb,
		     uint8_t *kind, uint16_t *flow, uint32_t *seq,
		     uint16_t *sport, uint32_t *src_ip,
		     struct rss_report *rep)
{
	struct rss_eth *eth;
	struct rss_ipv4 *ip;
	struct rss_udp *udp;
	struct rss_msg *msg;
	struct rss_report *rp;

	if (nb->len < sizeof(*eth) + 20U + sizeof(*udp) +
	    sizeof(struct rss_msg))
		return -1;
	eth = (struct rss_eth *)nb->data;
	if (rss_ntohs(eth->type) != 0x0800)
		return -1;
	ip = (struct rss_ipv4 *)(eth + 1);
	if ((ip->vihl & 0xfU) != 5U || ip->proto != 17)
		return -1;
	udp = (struct rss_udp *)(ip + 1);
	msg = (struct rss_msg *)(udp + 1);
	if (rss_ntohl(msg->magic) != RSS_MAGIC)
		return -1;
	if (rss_ntohl(ip->dst) != c->own_ip)
		return -1;
	*kind = msg->kind;
	*flow = rss_ntohs(msg->flow);
	*seq = rss_ntohl(msg->seq);
	*sport = rss_ntohs(udp->sport);
	*src_ip = rss_ntohl(ip->src);
	if (*kind == RSS_KIND_REPORT) {
		if (nb->len < sizeof(*eth) + 20U + sizeof(*udp) +
		    sizeof(struct rss_report))
			return -1;
		rp = (struct rss_report *)msg;
		if (rep != NULL) {
			unsigned int q;

			for (q = 0U; q < RSS_MAX_QUEUES; q++)
				rep->counts[q] = rss_ntohl(rp->counts[q]);
			rep->total = rss_ntohl(rp->total);
			rep->queues = rss_ntohl(rp->queues);
		}
	}
	return 0;
}

static int rss_bringup(struct rss_ctx *c)
{
	struct uk_netdev_info info;
	struct uk_netdev_conf dev_conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	const struct uk_hwaddr *hw;
	uint16_t q;
	int ret;

	ret = uk_netdev_probe(c->dev);
	if (ret != 0) {
		printf("GATE_FAIL: netdev probe failed rc=%d\n", ret);
		return -1;
	}
	uk_netdev_info_get(c->dev, &info);
	c->align = (info.ioalign != 0U) ? info.ioalign : 64U;
	c->encap_tx = info.nb_encap_tx;
	c->encap_rx = info.nb_encap_rx;
	if (c->queues > info.max_rx_queues || c->queues > info.max_tx_queues) {
		printf("GATE_FAIL: device supports %u rx / %u tx queues, gate wants %u\n",
		       info.max_rx_queues, info.max_tx_queues, c->queues);
		return -1;
	}

	memset(&dev_conf, 0, sizeof(dev_conf));
	dev_conf.nb_rx_queues = c->queues;
	dev_conf.nb_tx_queues = c->queues;
	ret = uk_netdev_configure(c->dev, &dev_conf);
	if (ret != 0) {
		printf("GATE_FAIL: netdev configure with %u pairs failed rc=%d\n",
		       c->queues, ret);
		return -1;
	}

	memset(&tx_conf, 0, sizeof(tx_conf));
	tx_conf.a = c->a;
	memset(&rx_conf, 0, sizeof(rx_conf));
	rx_conf.a = c->a;
	rx_conf.alloc_rxpkts = rss_alloc_rxpkts;
	rx_conf.alloc_rxpkts_argp = c;
	for (q = 0U; q < c->queues; q++) {
		ret = uk_netdev_txq_configure(c->dev, q, 0, &tx_conf);
		if (ret != 0) {
			printf("GATE_FAIL: tx queue %u configure failed rc=%d\n",
			       q, ret);
			return -1;
		}
		ret = uk_netdev_rxq_configure(c->dev, q, 0, &rx_conf);
		if (ret != 0) {
			printf("GATE_FAIL: rx queue %u configure failed rc=%d\n",
			       q, ret);
			return -1;
		}
	}

	ret = uk_netdev_start(c->dev);
	if (ret != 0) {
		printf("GATE_FAIL: netdev start failed rc=%d\n", ret);
		return -1;
	}
	hw = uk_netdev_hwaddr_get(c->dev);
	if (!hw) {
		printf("GATE_FAIL: no hardware address\n");
		return -1;
	}
	memcpy(c->own_mac, hw->addr_bytes, 6);
	return 0;
}

static void rss_print_perq(const uint32_t *perq, uint16_t queues,
			   char *out, size_t outsz)
{
	size_t pos = 0;
	uint16_t q;

	out[0] = '\0';
	for (q = 0U; q < queues; q++) {
		int n = snprintf(out + pos, outsz - pos, "%s%u",
				 (q == 0U) ? "" : ",", perq[q]);

		if (n < 0 || (size_t)n >= outsz - pos)
			break;
		pos += (size_t)n;
	}
}

static int run_server(struct rss_ctx *c)
{
	uint32_t perq[RSS_MAX_QUEUES] = { 0 };
	uint32_t expected = (uint32_t)c->flows * (uint32_t)c->pkts;
	uint32_t total = 0;
	__nsec start = ukplat_monotonic_clock();
	__nsec last_report = 0;
	__nsec last_stat = start;
	char perq_str[RSS_MAX_QUEUES * 12];
	int verdict = -1;

	printf("rss: server ready queues=%u expected=%u\n",
	       c->queues, expected);

	while (1) {
		__nsec now = ukplat_monotonic_clock();
		uint16_t q;

		for (q = 0U; q < c->queues; q++) {
			struct uk_netbuf *nb;
			int ret;
			unsigned int guard;

			for (guard = 0U; guard < 256U; guard++) {
				uint8_t kind;
				uint16_t flow, sport;
				uint32_t seq, src_ip;
				uint8_t buf[RSS_FRAME_MAX];
				size_t len;

				nb = NULL;
				ret = uk_netdev_rx_one(c->dev, q, &nb);
				if (!uk_netdev_status_successful(ret))
					break;
				if (!nb) {
					if (!uk_netdev_status_more(ret))
						break;
					continue;
				}
				if (rss_parse(c, nb, &kind, &flow, &seq,
					      &sport, &src_ip, NULL) == 0) {
					if (kind == RSS_KIND_DATA) {
						perq[q]++;
						total++;
						len = rss_build_frame(
							c, buf, src_ip,
							c->port, sport,
							RSS_KIND_ECHO,
							flow, seq, NULL);
						rss_tx_retry(c, q, buf,
							     len, 0);
					} else if (kind ==
						   RSS_KIND_REPORT_REQ) {
						struct rss_report rep;
						unsigned int i;

						memset(&rep, 0,
						       sizeof(rep));
						rep.hdr.kind =
							RSS_KIND_REPORT;
						for (i = 0U;
						     i < RSS_MAX_QUEUES;
						     i++)
							rep.counts[i] =
								perq[i];
						rep.total = total;
						rep.queues = c->queues;
						len = rss_build_frame(
							c, buf, src_ip,
							c->port, sport,
							RSS_KIND_REPORT,
							0, 0, &rep);
						rss_tx_retry(c, 0, buf,
							     len, 0);
						last_report = now;
					}
				}
				uk_netbuf_free(nb);
				if (!uk_netdev_status_more(ret))
					break;
			}
		}

		now = ukplat_monotonic_clock();
		if (now - last_stat >= ukarch_time_sec_to_nsec(5)) {
			rss_print_perq(perq, c->queues, perq_str,
				       sizeof(perq_str));
			printf("rss: total=%u/%u perq=%s\n", total,
			       expected, perq_str);
			last_stat = now;
		}

		/*
		 * Decide once the client asked for the report and the
		 * counts settled, or at the hard stop. The client asks
		 * after it saw its echoes, so the flow traffic is done
		 * at that point.
		 */
		if (verdict < 0 && last_report != 0 &&
		    now - last_report >= ukarch_time_sec_to_nsec(3))
			verdict = 0;
		if (verdict < 0 &&
		    now - start >= ukarch_time_sec_to_nsec(240)) {
			printf("rss: stop after 240 s idle cap\n");
			verdict = 0;
		}
		if (verdict == 0)
			break;
	}

	rss_print_perq(perq, c->queues, perq_str, sizeof(perq_str));
	{
		int all_used = 1;
		uint16_t q;

		for (q = 0U; q < c->queues; q++) {
			if (perq[q] == 0U)
				all_used = 0;
		}
		if (all_used && total >= (expected / 10U) * 9U) {
			printf("GATE_RSS_PASS queues=%u perq=%s total=%u\n",
			       c->queues, perq_str, total);
			return 0;
		}
		printf("GATE_FAIL: rss steering incomplete total=%u expected=%u perq=%s\n",
		       total, expected, perq_str);
	}
	return 1;
}

struct cli_state {
	uint32_t *cliq;
	uint8_t (*seen)[RSS_MAX_PKTS];
	uint32_t echoed;
	int got_report;
	struct rss_report rep;
};

/* Poll one RX queue once and account echo and report frames. */
static void cli_poll_queue(struct rss_ctx *c, uint16_t q,
			   struct cli_state *st)
{
	struct uk_netbuf *nb;
	unsigned int guard;

	for (guard = 0U; guard < 256U; guard++) {
		uint8_t kind;
		uint16_t flow, sport;
		uint32_t seq, src_ip;
		int ret;

		nb = NULL;
		ret = uk_netdev_rx_one(c->dev, q, &nb);
		if (!uk_netdev_status_successful(ret))
			break;
		if (!nb) {
			if (!uk_netdev_status_more(ret))
				break;
			continue;
		}
		if (rss_parse(c, nb, &kind, &flow, &seq, &sport, &src_ip,
			      &st->rep) == 0 && src_ip == c->peer_ip) {
			if (kind == RSS_KIND_ECHO &&
			    (seq / c->pkts) < RSS_MAX_FLOWS &&
			    (seq % c->pkts) < RSS_MAX_PKTS) {
				st->cliq[q]++;
				if (!st->seen[seq / c->pkts][seq %
							     c->pkts]) {
					st->seen[seq / c->pkts][seq %
								c->pkts] = 1;
					st->echoed++;
				}
			} else if (kind == RSS_KIND_REPORT) {
				st->got_report = 1;
			}
		}
		uk_netbuf_free(nb);
		if (!uk_netdev_status_more(ret))
			break;
	}
}

static void cli_poll_all(struct rss_ctx *c, struct cli_state *st)
{
	uint16_t q;

	for (q = 0U; q < c->queues; q++)
		cli_poll_queue(c, q, st);
}

static int run_client(struct rss_ctx *c)
{
	static uint8_t echo_seen[RSS_MAX_FLOWS][RSS_MAX_PKTS];
	uint32_t cliq[RSS_MAX_QUEUES] = { 0 };
	struct cli_state st;
	uint32_t sent = 0;
	uint16_t f, p;
	__nsec deadline;
	uint8_t buf[RSS_FRAME_MAX];
	char perq_str[RSS_MAX_QUEUES * 12];

	if (c->peer_ip == 0) {
		printf("GATE_FAIL: client role requires peer=<ip>\n");
		return 1;
	}
	printf("rss: client ready queues=%u flows=%u pkts=%u\n",
	       c->queues, c->flows, c->pkts);

	/* Let the server finish boot and start listening. */
	rss_delay_ms(2000);

	memset(&st, 0, sizeof(st));
	st.cliq = cliq;
	st.seen = echo_seen;
	memset(&st.rep, 0, sizeof(st.rep));

	for (f = 0U; f < c->flows; f++) {
		for (p = 0U; p < c->pkts; p++) {
			uint16_t sport = (uint16_t)(40000U + f);
			uint32_t seq = (uint32_t)f * c->pkts + p;
			size_t len;

			len = rss_build_frame(c, buf, c->peer_ip, sport,
					      c->port, RSS_KIND_DATA,
					      f, seq, NULL);
			if (rss_tx_retry(c, (uint16_t)(f % c->queues),
					 buf, len, 1) == 0)
				sent++;
			cli_poll_all(c, &st);
		}
	}
	printf("rss: sent=%u\n", sent);

	deadline = ukplat_monotonic_clock() + ukarch_time_sec_to_nsec(15);
	while (ukplat_monotonic_clock() < deadline && st.echoed < sent)
		cli_poll_all(c, &st);
	cli_poll_all(c, &st);
	rss_print_perq(cliq, c->queues, perq_str, sizeof(perq_str));
	printf("rss: echoed=%u sent=%u cli_perq=%s\n", st.echoed, sent,
	       perq_str);

	/* Ask the server for its per-queue report for the log. */
	{
		int try_no;

		for (try_no = 0; try_no < 3 && !st.got_report; try_no++) {
			size_t len;

			len = rss_build_frame(c, buf, c->peer_ip, 41000,
					      c->port, RSS_KIND_REPORT_REQ,
					      0, (uint32_t)try_no, NULL);
			rss_tx_retry(c, 0, buf, len, 0);
			deadline = ukplat_monotonic_clock() +
				   ukarch_time_sec_to_nsec(1);
			while (ukplat_monotonic_clock() < deadline &&
			       !st.got_report)
				cli_poll_all(c, &st);
		}
		if (st.got_report) {
			uint32_t rpt[RSS_MAX_QUEUES];
			uint16_t rq = (st.rep.queues <= RSS_MAX_QUEUES)
				      ? (uint16_t)st.rep.queues
				      : (uint16_t)RSS_MAX_QUEUES;
			char rpt_str[RSS_MAX_QUEUES * 12];
			unsigned int q;

			for (q = 0U; q < RSS_MAX_QUEUES; q++)
				rpt[q] = st.rep.counts[q];
			rss_print_perq(rpt, rq, rpt_str, sizeof(rpt_str));
			printf("rss: server_report total=%u perq=%s\n",
			       st.rep.total, rpt_str);
		} else {
			printf("rss: no server report received\n");
		}
	}

	if (sent > 0 && st.echoed >= (sent * 99U) / 100U) {
		printf("GATE_RSS_TRAFFIC sent=%u echoed=%u\n", sent,
		       st.echoed);
		return 0;
	}
	printf("GATE_FAIL: rss traffic low sent=%u echoed=%u\n", sent,
	       st.echoed);
	return 1;
}

static uint32_t parse_ip(const char *s)
{
	uint32_t v = 0;
	int parts = 0;

	while (*s != '\0' && parts < 4) {
		unsigned int octet = 0;
		int digits = 0;

		while (*s >= '0' && *s <= '9') {
			octet = octet * 10U + (unsigned int)(*s - '0');
			s++;
			digits++;
		}
		if (digits == 0 || octet > 255U)
			return 0;
		v = (v << 8) | octet;
		parts++;
		if (*s == '.')
			s++;
	}
	if (parts != 4 || *s != '\0')
		return 0;
	return v;	/* host order */
}

int main(int argc, char *argv[])
{
	struct rss_ctx ctx;
	const char *role = "server";
	const char *peer = NULL;
	const char *ip_param = NULL;
	const char *gw_param = NULL;
	uint32_t gw_ip;
	unsigned int ndevs;
	int i;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.queues = RSS_QUEUES_DEFAULT;
	ctx.flows = RSS_FLOWS_DEFAULT;
	ctx.pkts = RSS_PKTS_DEFAULT;
	ctx.port = RSS_PORT_DEFAULT;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "role=", 5) == 0)
			role = argv[i] + 5;
		else if (strncmp(argv[i], "peer=", 5) == 0)
			peer = argv[i] + 5;
		else if (strncmp(argv[i], "ip=", 3) == 0)
			ip_param = argv[i] + 3;
		else if (strncmp(argv[i], "netdev.ip=", 10) == 0)
			ip_param = argv[i] + 10;
		else if (strncmp(argv[i], "gw=", 3) == 0)
			gw_param = argv[i] + 3;
		else if (strncmp(argv[i], "queues=", 7) == 0)
			ctx.queues = (uint16_t)atoi(argv[i] + 7);
		else if (strncmp(argv[i], "flows=", 6) == 0)
			ctx.flows = (uint16_t)atoi(argv[i] + 6);
		else if (strncmp(argv[i], "pkts=", 5) == 0)
			ctx.pkts = (uint16_t)atoi(argv[i] + 5);
		else if (strncmp(argv[i], "port=", 5) == 0)
			ctx.port = (uint16_t)atoi(argv[i] + 5);
	}

	if (ctx.queues == 0U || ctx.queues > RSS_MAX_QUEUES ||
	    ctx.flows == 0U || ctx.flows > RSS_MAX_FLOWS ||
	    ctx.pkts == 0U || ctx.pkts > RSS_MAX_PKTS) {
		printf("GATE_FAIL: bad args queues=%u flows=%u pkts=%u\n",
		       ctx.queues, ctx.flows, ctx.pkts);
		return 1;
	}
	if (ip_param) {
		char ipbuf[32];
		const char *slash = strchr(ip_param, '/');
		size_t len = slash ? (size_t)(slash - ip_param)
				   : strlen(ip_param);

		if (len >= sizeof(ipbuf))
			len = sizeof(ipbuf) - 1;
		memcpy(ipbuf, ip_param, len);
		ipbuf[len] = '\0';
		ctx.own_ip = parse_ip(ipbuf);
	}
	if (gw_param)
		gw_ip = parse_ip(gw_param);
	else
		gw_ip = parse_ip("10.150.0.1");
	if (peer)
		ctx.peer_ip = parse_ip(peer);
	ctx.gw_mac[0] = 0x42;
	ctx.gw_mac[1] = 0x01;
	ctx.gw_mac[2] = (uint8_t)(gw_ip >> 24);
	ctx.gw_mac[3] = (uint8_t)(gw_ip >> 16);
	ctx.gw_mac[4] = (uint8_t)(gw_ip >> 8);
	ctx.gw_mac[5] = (uint8_t)gw_ip;

	if (ctx.own_ip == 0 || (gw_ip == 0)) {
		printf("GATE_FAIL: bad ip= or gw= argument\n");
		return 1;
	}

	printf("rss: role=%s queues=%u flows=%u pkts=%u port=%u\n",
	       role, ctx.queues, ctx.flows, ctx.pkts, ctx.port);

	ndevs = uk_netdev_count();
	if (ndevs == 0) {
		printf("GATE_FAIL: no netdev found\n");
		return 1;
	}
	ctx.dev = uk_netdev_get(0);
	ctx.a = uk_alloc_get_default();
	if (!ctx.dev || !ctx.a) {
		printf("GATE_FAIL: netdev or allocator init failed\n");
		return 1;
	}

	if (rss_bringup(&ctx) != 0)
		return 1;

	if (strcmp(role, "client") == 0)
		rc = run_client(&ctx);
	else
		rc = run_server(&ctx);

	printf("rss: %s done rc=%d\n", role, rc);
	return rc;
}
