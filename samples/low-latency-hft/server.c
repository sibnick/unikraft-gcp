/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Low-Latency UDP Echo Server (C lang)
 *
 * Designed for low-latency packet processing and round-trip time (RTT)
 * measurement. Supports both standard Linux and Unikraft unikernels.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#ifndef __Unikraft__
#include <signal.h>
#endif
#include <time.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__linux__) && !defined(__Unikraft__)
#include <sched.h>
#include <pthread.h>
#endif

#if defined(__Unikraft__)
#include <lwip/timeouts.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/etharp.h>
#include "netif/uknetdev.h"

/* nolibc has no signal.h; the unikernel is stopped by poweroff from
 * the run script, so the loop always runs. */
static volatile int g_running = 1;

/* nolibc has no exit-code macros; the unikernel binary never returns
 * to a shell, the values only keep the source dual-target. */
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

/* The lwIP port header declares inet_pton and inet_ntop but builds
 * no definition, and nolibc has no perror. Thin dual-target fill-ins
 * so the Linux side keeps the standard names untouched. */
int inet_pton(int af, const char *src, void *dst)
{
	ip4_addr_t a;

	if (af != AF_INET)
		return -1;
	if (ip4addr_aton(src, &a) != 1)
		return 0;
	memcpy(dst, &a, sizeof(a));
	return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
	if (af != AF_INET)
		return NULL;
	if (!ip4addr_ntoa_r((const ip4_addr_t *)src, dst, (int)size))
		return NULL;
	return dst;
}

#define perror(s) printf("ERR: %s: errno %d\n", (s), errno)
#endif

#define DEFAULT_PORT 9000
#define DEFAULT_BIND_IP "0.0.0.0"
#define BUFFER_SIZE 65536
#define SOCKET_BUFFER_SIZE (4 * 1024 * 1024)

#if !defined(__Unikraft__)
static volatile sig_atomic_t g_running = 1;
#endif

#ifndef __Unikraft__
static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}
#endif

#if defined(__linux__) && !defined(__Unikraft__)
static int pin_to_core(int core_id)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
				      &cpuset);
}
#endif

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCKET_BUFFER_SIZE;

	/* Allow quick address and port reuse */
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
		perror("setsockopt(SO_REUSEADDR)");

#ifdef SO_REUSEPORT
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
		perror("setsockopt(SO_REUSEPORT)");
#endif

	/* Increase OS receive and send buffers to avoid drops */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) <
	    0) {
		perror("setsockopt(SO_RCVBUF)");
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) <
	    0) {
		perror("setsockopt(SO_SNDBUF)");
	}

	/* Set low-delay IP TOS (DSCP CS4 / low delay) */
#ifdef IP_TOS
	int tos = 0x10; /* IPTOS_LOWDELAY */
	if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
		/* Not fatal */
	}
#endif

	/* Enable busy polling if supported by Linux kernel */
#ifdef SO_BUSY_POLL
	int busy_poll_us = 50;
	if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us,
		       sizeof(busy_poll_us)) < 0) {
		/* Not fatal: ignore if unsupported */
	}
#endif
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -p, --port <port>        UDP port to bind (default: %d)\n",
	       DEFAULT_PORT);
	printf("  -b, --bind <ip>          IP address to bind (default: %s)\n",
	       DEFAULT_BIND_IP);
	printf(
	    "  -c, --core <id>          Pin server process to CPU core ID\n");
	printf("  -v, --verbose            Print packet activity\n");
	printf("  -h, --help               Show this help message\n");
}

#if defined(__Unikraft__)
/*
 * GCP routes every guest-to-guest frame through the virtual gateway
 * and never answers ARP for guest IPs. Map one IP to a static entry
 * with the gateway-derived MAC, the rule ticket 2569ff0157 proved.
 * Idempotent per address.
 */
static void set_gcp_static_arp(const ip4_addr_t *target_ip)
{
	if (!netif_default || !target_ip)
		return;

	const ip4_addr_t *gw = netif_ip4_gw(netif_default);
	struct eth_addr gw_mac;

	gw_mac.addr[0] = 0x42;
	gw_mac.addr[1] = 0x01;
	gw_mac.addr[2] = ip4_addr1(gw);
	gw_mac.addr[3] = ip4_addr2(gw);
	gw_mac.addr[4] = ip4_addr3(gw);
	gw_mac.addr[5] = ip4_addr4(gw);

	etharp_add_static_entry(target_ip, &gw_mac);
}

/*
 * Bring the default interface up from ip=<ip>/<bits>:<gw>, the same
 * parameter style as samples/networkload. Returns 0 on success.
 */
static int bring_up_netif(int argc, char *argv[])
{
	const char *ip_param = NULL, *mask_param = NULL, *gw_param = NULL;
	ip4_addr_t my_ip, my_mask, my_gw;
	int i;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "ip=", 3) == 0)
			ip_param = argv[i] + 3;
		else if (strncmp(argv[i], "mask=", 5) == 0)
			mask_param = argv[i] + 5;
		else if (strncmp(argv[i], "gw=", 3) == 0)
			gw_param = argv[i] + 3;
	}

	if (!netif_default) {
		printf("GATE_FAIL: no default netif (is netdev.ip set?)\n");
		return -1;
	}
	if (uk_netdev_count() == 0) {
		printf("GATE_FAIL: no netdev found\n");
		return -1;
	}

	if (ip_param) {
		char ip_buf[32] = {0};
		const char *slash = strchr(ip_param, '/');
		const char *colon = strchr(ip_param, ':');

		ip4_addr_set_zero(&my_ip);
		ip4addr_aton("255.255.240.0", &my_mask);
		ip4addr_aton("10.150.0.1", &my_gw);

		if (colon && !gw_param)
			gw_param = colon + 1;

		if (slash) {
			size_t len = (size_t)(slash - ip_param);
			int bits;

			if (len < sizeof(ip_buf)) {
				strncpy(ip_buf, ip_param, len);
				ip_buf[len] = '\0';
			}
			bits = atoi(slash + 1);
			if (bits > 0 && bits <= 32) {
				uint32_t mask_val =
					htonl(~((1ULL << (32 - bits)) - 1));
				my_mask.addr = mask_val;
			}
			ip4addr_aton(ip_buf, &my_ip);
		} else {
			size_t len = colon ? (size_t)(colon - ip_param) :
					     strlen(ip_param);
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
		set_gcp_static_arp(&my_gw);
	}

	if (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)) &&
	    !ip_param) {
		printf("GATE_FAIL: netif is not up and no ip= parameter\n");
		return -1;
	}

	{
		char ip_str[16] = {0};

		ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_str,
			       sizeof(ip_str));
		printf("Interface configured: IP=%s\n", ip_str);
	}
	return 0;
}
#endif /* __Unikraft__ */

int main(int argc, char *argv[])
{
	int port = DEFAULT_PORT;
	const char *bind_ip = DEFAULT_BIND_IP;
	int pin_core = -1;
	bool verbose = false;

	static struct option long_options[] = {
	    {"port", required_argument, 0, 'p'},
	    {"bind", required_argument, 0, 'b'},
	    {"core", required_argument, 0, 'c'},
	    {"verbose", no_argument, 0, 'v'},
	    {"help", no_argument, 0, 'h'},
	    {0, 0, 0, 0}};

	int opt;
	while ((opt = getopt_long(argc, argv, "p:b:c:vh", long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			if (port <= 0 || port > 65535) {
				fprintf(stderr, "Error: Invalid port %s\n",
					optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'b':
			bind_ip = optarg;
			break;
		case 'c':
			pin_core = atoi(optarg);
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

#if defined(__linux__) && !defined(__Unikraft__)
	if (pin_core >= 0) {
		if (pin_to_core(pin_core) == 0) {
			printf("Pinned server process to CPU core %d\n",
			       pin_core);
		} else {
			fprintf(stderr,
				"Warning: Failed to pin to CPU core %d\n",
				pin_core);
		}
	}
#endif

#if defined(__Unikraft__)
	if (bring_up_netif(argc, argv) != 0)
		return EXIT_FAILURE;
#else
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sa.sa_flags =
	    0; /* Do NOT set SA_RESTART so blocking recvfrom is interrupted */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
#endif

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

#if defined(__Unikraft__)
	/* The stack is driven by the poll in the loop below, so the
	 * socket is non-blocking and needs no receive timeout. */
	int nb = 1;
	ioctl(fd, FIONBIO, &nb);
#else
	/* Periodic receive timeout to ensure loop checks g_running */
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 200000; /* 200ms */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	configure_socket_options(fd);

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons((uint16_t)port);

	if (inet_pton(AF_INET, bind_ip, &server_addr.sin_addr) <= 0) {
		fprintf(stderr, "Error: Invalid bind address '%s'\n", bind_ip);
		close(fd);
		return EXIT_FAILURE;
	}

	if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
	    0) {
		perror("bind");
		close(fd);
		return EXIT_FAILURE;
	}

	printf("========================================\n");
	printf(" Low-Latency UDP Echo Server\n");
	printf(" Listening on: %s:%d\n", bind_ip, port);
	printf(" Mode: Zero-copy direct echo\n");
	printf("========================================\n");
	fflush(stdout);
#if defined(__Unikraft__)
	printf("GATE_HFT_READY\n");
#endif

	/* The 64 KiB echo buffer must not sit on the unikernel boot
	 * stack; static storage works on both targets. */
	static uint8_t buffer[BUFFER_SIZE];
	struct sockaddr_in client_addr;
	socklen_t client_len;
	uint64_t rx_packets = 0;
	uint64_t tx_packets = 0;
	uint64_t rx_bytes = 0;
#if defined(__Unikraft__)
	uint32_t last_src = 0; /* gateway ARP for each new source */
#endif

	while (g_running) {
#if defined(__Unikraft__)
		uknetdev_poll_all();
		sys_check_timeouts();
#endif
		client_len = sizeof(client_addr);
		ssize_t n =
		    recvfrom(fd, buffer, sizeof(buffer), 0,
			     (struct sockaddr *)&client_addr, &client_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			perror("recvfrom");
			break;
		}

#if defined(__Unikraft__)
		/* Echo needs the GCP static route for this source. The
		 * benchmark keeps one client, so comparing against the
		 * last source is enough; a new client re-adds the entry,
		 * which is idempotent. */
		if (client_addr.sin_addr.s_addr != last_src) {
			ip4_addr_t src_ip;

			src_ip.addr = client_addr.sin_addr.s_addr;
			set_gcp_static_arp(&src_ip);
			last_src = client_addr.sin_addr.s_addr;
		}
#endif

		rx_packets++;
		rx_bytes += (uint64_t)n;

		/* In-place echo back to sender */
		ssize_t sent =
		    sendto(fd, buffer, (size_t)n, 0,
			   (struct sockaddr *)&client_addr, client_len);
		if (sent > 0) {
			tx_packets++;
		} else if (sent < 0 && errno != EINTR) {
			if (verbose)
				perror("sendto");
		}

		if (verbose && (rx_packets % 10000 == 0 || rx_packets <= 10)) {
			char client_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &client_addr.sin_addr, client_str,
				  sizeof(client_str));
			printf("Echoed %zd bytes to %s:%d (total rx: %lu)\n", n,
			       client_str, ntohs(client_addr.sin_port),
			       (unsigned long)rx_packets);
			fflush(stdout);
		}
	}

	printf("\nServer shutdown complete.\n");
	printf("Total packets received: %lu\n", (unsigned long)rx_packets);
	printf("Total packets echoed:   %lu\n", (unsigned long)tx_packets);
	printf("Total bytes processed:  %lu\n", (unsigned long)rx_bytes);

	close(fd);
	return EXIT_SUCCESS;
}
