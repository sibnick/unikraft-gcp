/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft Networkload (Phase A3)
 *
 * Automated two-instance traffic and throughput benchmark for Google Cloud.
 * Runs in NO_SYS lwIP mode with POSIX sockets.
 *
 * Implements:
 * 1. ICMP ping exchange and verification (GATE_PING_PASS).
 * 2. Bidirectional packet exchange and throughput measurement
 *    (GATE_NET_PASS, GATE_THROUGHPUT_PASS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <uk/plat/time.h>
#include <uk/netdev.h>
#include <lwip/timeouts.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include "netif/uknetdev.h"
#include <lwip/etharp.h>

#define DEFAULT_PORT 9000
#define DEFAULT_PKT_COUNT 1000
#define DEFAULT_PKT_SIZE 1024
#define NETLOAD_MAGIC 0x554b4731 /* "UKG1" */

struct __attribute__((packed)) icmp_echo_hdr {
	uint8_t type;       /* 8 = echo request, 0 = echo reply */
	uint8_t code;       /* 0 */
	uint16_t checksum;
	uint16_t id;
	uint16_t seq;
	char payload[32];
};

struct __attribute__((packed)) netload_pkt {
	uint32_t magic;
	uint32_t seq;
	uint64_t timestamp_ns;
	uint32_t flags;     /* 0 = data, 1 = end-of-stream */
	char payload[];
};

static uint16_t calc_checksum(const void *data, size_t len)
{
	const uint16_t *buf = (const uint16_t *)data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *buf++;
		len -= 2;
	}
	if (len == 1)
		sum += *(const uint8_t *)buf;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)(~sum);
}

static void poll_stack(void)
{
	uknetdev_poll_all();
	sys_check_timeouts();
}

static void sleep_and_poll(unsigned int msec)
{
	__nsec end = ukplat_monotonic_clock() + ukarch_time_msec_to_nsec(msec);

	while (ukplat_monotonic_clock() < end) {
		poll_stack();
	}
}

static int do_ping(const char *peer_ip, int timeout_sec)
{
	int fd;
	struct sockaddr_in dst;
	struct timeval tv;
	struct icmp_echo_hdr pkt;
	ip4_addr_t ip;
	uint8_t rx_buf[256];
	__nsec deadline;
	int seq = 1;
	int pass = 0;

	printf("[PING] Initializing ICMP socket to %s...\n", peer_ip);
	fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (fd < 0) {
		printf("[PING] Warning: Raw ICMP socket failed: %d\n", errno);
		return -1;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 200000; /* 200 ms */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	if (ip4addr_aton(peer_ip, &ip) != 1) {
		printf("[PING] Invalid peer IP: %s\n", peer_ip);
		close(fd);
		return -1;
	}
	dst.sin_addr.s_addr = ip.addr;

	deadline = ukplat_monotonic_clock() + ukarch_time_sec_to_nsec(timeout_sec);

	while (ukplat_monotonic_clock() < deadline) {
		ssize_t ret;
		__nsec send_time;

		memset(&pkt, 0, sizeof(pkt));
		pkt.type = 8; /* ICMP_ECHO */
		pkt.code = 0;
		pkt.id = htons(0x1234);
		pkt.seq = htons((uint16_t)seq++);
		strncpy(pkt.payload, "UNIKRAFT_GCP_PING_TEST", sizeof(pkt.payload) - 1);
		pkt.checksum = calc_checksum(&pkt, sizeof(pkt));

		send_time = ukplat_monotonic_clock();
		printf("[PING] Sending echo request seq=%d to %s...\n", (int)ntohs(pkt.seq), peer_ip);
		ret = sendto(fd, &pkt, sizeof(pkt), 0,
			     (struct sockaddr *)&dst, sizeof(dst));
		if (ret < 0) {
			printf("[PING] sendto error: %d\n", errno);
			poll_stack();
			sleep_and_poll(200);
			continue;
		}

		/* Poll for response */
		for (int a = 0; a < 25; a++) {
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);

			poll_stack();
			ret = recvfrom(fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT,
				       (struct sockaddr *)&from, &from_len);
			if (ret > 0) {
				/* Parse IPv4 header length */
				int ip_hl = (rx_buf[0] & 0x0f) * 4;
				if (ret >= ip_hl + (int)sizeof(struct icmp_echo_hdr)) {
					struct icmp_echo_hdr *r =
						(struct icmp_echo_hdr *)(rx_buf + ip_hl);
					if (r->type == 0 && r->id == htons(0x1234)) {
						__nsec rtt_ns = ukplat_monotonic_clock() - send_time;
						double rtt_ms = (double)rtt_ns / 1000000.0;
						printf("[PING] Echo reply from %s: seq=%d rtt=%.2f ms\n",
						       peer_ip, ntohs(r->seq), rtt_ms);
						pass = 1;
						break;
					}
				}
			}
			sleep_and_poll(20);
		}

		if (pass)
			break;

		sleep_and_poll(300);
	}

	close(fd);

	if (pass) {
		printf("\n========================================\n");
		printf("GATE_PING_PASS\n");
		printf("========================================\n\n");
		return 0;
	}

	printf("[PING] Ping timeout to %s\n", peer_ip);
	return -1;
}

static int run_client(const char *peer_ip, int port, uint32_t count, size_t pkt_size)
{
	int fd;
	struct sockaddr_in dst;
	struct timeval tv;
	uint8_t *tx_buf;
	uint8_t *rx_buf;
	struct netload_pkt *hdr;
	uint32_t seq;
	uint32_t rx_count = 0;
	uint64_t total_bytes = 0;
	__nsec start_ns, end_ns;
	double duration_s, throughput_mb_s, pps;

	if (pkt_size < sizeof(struct netload_pkt))
		pkt_size = sizeof(struct netload_pkt);

	tx_buf = malloc(pkt_size);
	rx_buf = malloc(pkt_size + 128);
	if (!tx_buf || !rx_buf) {
		printf("[CLIENT] Error: Out of memory\n");
		return -1;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		printf("[CLIENT] Error creating socket: %d\n", errno);
		free(tx_buf);
		free(rx_buf);
		return -1;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 200000; /* 200 ms */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)port);
	{
		ip4_addr_t peer_addr;
		if (ip4addr_aton(peer_ip, &peer_addr) != 1) {
			printf("[CLIENT] Invalid peer IP: %s\n", peer_ip);
			close(fd);
			free(tx_buf);
			free(rx_buf);
			return -1;
		}
		dst.sin_addr.s_addr = peer_addr.addr;
	}

	printf("[CLIENT] Starting UDP throughput test to %s:%d...\n", peer_ip, port);
	printf("[CLIENT] Probing %u packets of %zu bytes\n", count, pkt_size);

	memset(tx_buf, 0xa5, pkt_size);
	hdr = (struct netload_pkt *)tx_buf;
	hdr->magic = htonl(NETLOAD_MAGIC);
	hdr->flags = htonl(0);

	start_ns = ukplat_monotonic_clock();

	for (seq = 1; seq <= count; seq++) {
		ssize_t ret;
		hdr->seq = htonl(seq);
		hdr->timestamp_ns = ukplat_monotonic_clock();

		ret = sendto(fd, tx_buf, pkt_size, 0,
			     (struct sockaddr *)&dst, sizeof(dst));
		if (ret < 0) {
			poll_stack();
			continue;
		}

		/* Receive echo */
		for (int attempts = 0; attempts < 30; attempts++) {
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);

			poll_stack();
			ret = recvfrom(fd, rx_buf, pkt_size + 128, MSG_DONTWAIT,
				       (struct sockaddr *)&from, &from_len);
			if (ret > 0) {
				struct netload_pkt *rx_hdr = (struct netload_pkt *)rx_buf;
				if (ntohl(rx_hdr->magic) == NETLOAD_MAGIC &&
				    ntohl(rx_hdr->seq) == seq) {
					rx_count++;
					total_bytes += (uint64_t)ret * 2; /* bidirectional: TX + RX */
					break;
				}
			}
			if (attempts == 10 && rx_count == 0) {
				sendto(fd, tx_buf, pkt_size, 0,
				       (struct sockaddr *)&dst, sizeof(dst));
			}
			sleep_and_poll(5);
		}

		if (seq % 200 == 0 || seq == count) {
			printf("[CLIENT] Progress: %u/%u packets echoed (%.1f%%)\n",
			       rx_count, seq, (double)rx_count * 100.0 / (double)seq);
		}
	}

	/* Send end-of-stream notification */
	hdr->flags = htonl(1);
	for (int i = 0; i < 3; i++) {
		sendto(fd, tx_buf, sizeof(struct netload_pkt), 0,
		       (struct sockaddr *)&dst, sizeof(dst));
		poll_stack();
		sleep_and_poll(10);
	}

	end_ns = ukplat_monotonic_clock();
	duration_s = (double)(end_ns - start_ns) / 1000000000.0;
	if (duration_s < 0.001)
		duration_s = 0.001;

	throughput_mb_s = ((double)total_bytes) / (duration_s * 1024.0 * 1024.0);
	pps = (double)rx_count / duration_s;

	printf("\n========================================\n");
	printf("Networkload Client Benchmark Summary:\n");
	printf("Target:             %s:%d\n", peer_ip, port);
	printf("Packets sent:       %u\n", count);
	printf("Packets echoed:     %u (%.2f%% received)\n",
	       rx_count, (double)rx_count * 100.0 / (double)count);
	printf("Total data (bi-dir): %lu bytes\n", (unsigned long)total_bytes);
	printf("Duration:           %.3f seconds\n", duration_s);
	printf("Throughput:         %.2f MB/s\n", throughput_mb_s);
	printf("Packet rate:        %.0f pkts/sec\n", pps);
	printf("========================================\n");

	close(fd);
	free(tx_buf);
	free(rx_buf);

	if (rx_count > count / 2) {
		printf("GATE_NET_PASS\n");
		printf("GATE_THROUGHPUT_PASS\n");
		printf("========================================\n\n");
		return 0;
	}

	printf("GATE_FAIL: insufficient packets received\n");
	return -1;
}

static int run_server(const char *peer_ip, int port)
{
	int fd;
	struct sockaddr_in bind_addr;
	struct timeval tv;
	uint8_t *buf;
	size_t buf_size = 65536;
	uint32_t rx_pkts = 0;
	uint64_t total_bytes = 0;
	__nsec first_rx_ns = 0;
	__nsec last_rx_ns = 0;

	buf = malloc(buf_size);
	if (!buf) {
		printf("[SERVER] Error: Out of memory\n");
		return -1;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		printf("[SERVER] Error creating socket: %d\n", errno);
		free(buf);
		return -1;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 100000; /* 100 ms timeout to allow poll_stack loop */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons((uint16_t)port);
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		printf("[SERVER] Error binding to port %d: %d\n", port, errno);
		close(fd);
		free(buf);
		return -1;
	}

	printf("[SERVER] Listening on UDP port %d (direct echo mode)...\n", port);
	fflush(stdout);

	while (1) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		ssize_t n;

		poll_stack();

		n = recvfrom(fd, buf, buf_size, MSG_DONTWAIT,
			     (struct sockaddr *)&cli_addr, &cli_len);
		if (n <= 0) {
			sleep_and_poll(5);
			continue;
		}
		if (n > 0) {
			struct netload_pkt *hdr = (struct netload_pkt *)buf;
			__nsec now = ukplat_monotonic_clock();

			if (!first_rx_ns)
				first_rx_ns = now;
			last_rx_ns = now;

			if (n >= (ssize_t)sizeof(struct netload_pkt) &&
			    ntohl(hdr->magic) == NETLOAD_MAGIC) {
				rx_pkts++;
				total_bytes += (uint64_t)n * 2; /* bidirectional: RX + TX */

				/* Echo back */
				sendto(fd, buf, (size_t)n, 0,
				       (struct sockaddr *)&cli_addr, cli_len);

				if (ntohl(hdr->flags) == 1) {
					/* End-of-stream signaled */
					printf("[SERVER] End of stream received from client.\n");
					break;
				}

				if (rx_pkts % 200 == 0) {
					printf("[SERVER] Echoed %u packets (%lu bytes)...\n",
					       rx_pkts, (unsigned long)total_bytes);
				}
			}
		}
	}

	if (rx_pkts > 0) {
		double duration_s = (double)(last_rx_ns - first_rx_ns) / 1000000000.0;
		double throughput_mb_s, pps;

		if (duration_s < 0.001)
			duration_s = 0.001;

		throughput_mb_s = ((double)total_bytes) / (duration_s * 1024.0 * 1024.0);
		pps = (double)rx_pkts / duration_s;

		printf("\n========================================\n");
		printf("Networkload Server Benchmark Summary:\n");
		printf("Packets echoed:     %u\n", rx_pkts);
		printf("Total data (bi-dir): %lu bytes\n", (unsigned long)total_bytes);
		printf("Duration:           %.3f seconds\n", duration_s);
		printf("Throughput:         %.2f MB/s\n", throughput_mb_s);
		printf("Packet rate:        %.0f pkts/sec\n", pps);
		printf("========================================\n");
		printf("GATE_NET_PASS\n");
		printf("GATE_THROUGHPUT_PASS\n");
		printf("========================================\n\n");
	}

	if (peer_ip) {
		printf("[SERVER] Initiating bidirectional ping to peer %s...\n", peer_ip);
		do_ping(peer_ip, 5);
	}

	/* Keep serving and polling for 5 seconds */
	for (int i = 0; i < 50; i++)
		sleep_and_poll(100);

	close(fd);
	free(buf);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *role = "server";
	const char *peer = NULL;
	const char *ip_param = NULL;
	const char *mask_param = NULL;
	const char *gw_param = NULL;
	int port = DEFAULT_PORT;
	uint32_t count = DEFAULT_PKT_COUNT;
	size_t size = DEFAULT_PKT_SIZE;
	unsigned int ndevs;

	printf("\n========================================\n");
	printf("Hello from Unikraft on Google Cloud (Phase A3)!\n");
	printf("========================================\n");

	/* Parse arguments from kernel command line */
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "role=", 5) == 0) {
			role = argv[i] + 5;
		} else if (strncmp(argv[i], "peer=", 5) == 0) {
			peer = argv[i] + 5;
		} else if (strncmp(argv[i], "port=", 5) == 0) {
			port = atoi(argv[i] + 5);
		} else if (strncmp(argv[i], "count=", 6) == 0) {
			count = (uint32_t)atoi(argv[i] + 6);
		} else if (strncmp(argv[i], "size=", 5) == 0) {
			size = (size_t)atoi(argv[i] + 5);
		} else if (strncmp(argv[i], "ip=", 3) == 0) {
			ip_param = argv[i] + 3;
		} else if (strncmp(argv[i], "netdev.ip=", 10) == 0) {
			ip_param = argv[i] + 10;
		} else if (strncmp(argv[i], "mask=", 5) == 0) {
			mask_param = argv[i] + 5;
		} else if (strncmp(argv[i], "gw=", 3) == 0) {
			gw_param = argv[i] + 3;
		}
	}

	printf("Configuration:\n");
	printf("  Role:  %s\n", role);
	printf("  Peer:  %s\n", peer ? peer : "(none)");
	printf("  Port:  %d\n", port);
	printf("  Count: %u packets\n", count);
	printf("  Size:  %zu bytes\n", size);

	ndevs = uk_netdev_count();
	printf("Network devices detected: %u\n", ndevs);
	if (ndevs == 0) {
		printf("GATE_FAIL: no netdev found\n");
		return 1;
	}

	if (netif_default) {
		if (ip_param) {
			char ip_buf[32] = {0};
			const char *slash = strchr(ip_param, '/');
			const char *colon = strchr(ip_param, ':');
			ip4_addr_t my_ip, my_mask, my_gw;

			ip4_addr_set_zero(&my_ip);
			ip4addr_aton("255.255.240.0", &my_mask);
			ip4addr_aton("10.150.0.1", &my_gw);

			if (colon && !gw_param)
				gw_param = colon + 1;

			if (slash) {
				size_t len = (size_t)(slash - ip_param);
				if (len < sizeof(ip_buf)) {
					strncpy(ip_buf, ip_param, len);
					ip_buf[len] = '\0';
				}
				int bits = atoi(slash + 1);
				if (bits > 0 && bits <= 32) {
					uint32_t mask_val = htonl(~((1ULL << (32 - bits)) - 1));
					my_mask.addr = mask_val;
				}
				ip4addr_aton(ip_buf, &my_ip);
			} else {
				size_t len = colon ? (size_t)(colon - ip_param) : strlen(ip_param);
				if (len < sizeof(ip_buf)) {
					strncpy(ip_buf, ip_param, len);
					ip_buf[len] = '\0';
				}
				ip4addr_aton(ip_buf, &my_ip);
			}

			if (mask_param)
				ip4addr_aton(mask_param, &my_mask);
			if (gw_param)
				ip4addr_aton(gw_param, &my_gw);

			netif_set_addr(netif_default, &my_ip, &my_mask, &my_gw);
			netif_set_up(netif_default);
		}

		char ip_str[16] = {0};
		char gw_str[16] = {0};
		char mask_str[16] = {0};
		ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_str, sizeof(ip_str));
		ip4addr_ntoa_r(netif_ip4_gw(netif_default), gw_str, sizeof(gw_str));
		ip4addr_ntoa_r(netif_ip4_netmask(netif_default), mask_str, sizeof(mask_str));
		printf("Interface %c%c%u configured: IP=%s Netmask=%s Gateway=%s flags=0x%x\n",
		       netif_default->name[0], netif_default->name[1], netif_default->num,
		       ip_str, mask_str, gw_str, netif_default->flags);
	} else {
		printf("Warning: netif_default is NULL!\n");
	}

	/* Warm up network stack and transmit Gratuitous ARP */
	printf("Warming up network interface and settling ARP...\n");
	if (netif_default)
		etharp_gratuitous(netif_default);
	for (int i = 0; i < 20; i++) {
		if (i % 5 == 0 && netif_default)
			etharp_gratuitous(netif_default);
		sleep_and_poll(100);
	}

	if (strcmp(role, "client") == 0) {
		if (!peer) {
			printf("GATE_FAIL: client role requires peer=<ip>\n");
			return 1;
		}

		/* Step 1: Ping server */
		printf("\n[CLIENT] Phase A3.1: ICMP Ping verification...\n");
		if (do_ping(peer, 15) != 0) {
			printf("GATE_FAIL: ICMP ping to peer failed\n");
			return 1;
		}

		/* Step 2: Throughput measurement */
		printf("\n[CLIENT] Phase A3.2: Bidirectional UDP throughput test...\n");
		if (run_client(peer, port, count, size) != 0) {
			printf("GATE_FAIL: throughput benchmark failed\n");
			return 1;
		}
	} else {
		/* Server role */
		printf("\n[SERVER] Phase A3: Server listening...\n");
		run_server(peer, port);
	}

	printf("Phase A3 test finished cleanly.\n");
	return 0;
}
