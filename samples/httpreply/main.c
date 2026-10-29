/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Benchmark Server for Google Cloud (httpreply)
 *
 * Ported from the Unikraft-ENA httpreply sample. Micro-benchmark HTTP
 * echo server on the native gve (gVNIC) driver and the lwIP stack in
 * single-threaded (NO_SYS) mode.
 *
 * The lwIP stack runs without a dedicated stack thread. This one
 * thread does everything: it polls the network device
 * (uknetdev_poll_all()), drives the stack timers (sys_check_timeouts()),
 * and multiplexes sockets with level-triggered epoll. There are no
 * worker threads, no mailboxes, and no context switches between
 * device, stack, and application.
 *
 * GCP adaptations versus the ENA original (ticket 73b23402f0):
 *  - No DHCP. The run script passes netdev.ip=<ip>/<bits>:<gw> as a
 *    library parameter and ip=<same> as an application parameter. The
 *    bring-up block below mirrors samples/networkload.
 *  - GCP does not answer ARP for guest IPs. Every accepted client
 *    address gets a static ARP entry pointing at the virtual gateway
 *    MAC, the rule ticket 2569ff0157 proved on the B3 gate.
 *  - Gate markers: GATE_HTTP_READY once the listener is up on a
 *    configured interface, GATE_HTTP_FIRST_200 after the first full
 *    response, and GATE_FAIL on any fatal setup error.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* The ENA original avoids netinet/tcp.h: the lwIP shim header needs
 * BSD types that nolibc does not define. TCP_NODELAY is value-stable
 * on Linux, so a plain define keeps the same socket behavior. */
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#include <lwip/timeouts.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/etharp.h>
#include "netif/uknetdev.h"

#define LISTEN_PORT 80
#define BACKLOG 512
#define MAX_EVENTS 256
#define RECV_BUF_SIZE 4096
#define SOCK_BUF_SIZE 32768

static const char http_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 14\r\n"
    "Connection: keep-alive\r\n"
    "Server: Unikraft-GCP-Benchmark\r\n"
    "\r\n"
    "Hello, World!\n";

static const size_t http_resp_len = sizeof(http_response) - 1;

/*
 * A non-blocking send() may write fewer bytes than requested, or no
 * byte at all (EAGAIN), when the socket buffer is full. The number
 * of response bytes not yet sent is tracked per connection in this
 * table, indexed by fd number. The fd numbers come from the fixed
 * netconn pool, so the table covers every fd the socket stack can
 * hand out, and every close path clears the entry of the fd it
 * frees, so a reused fd number starts clean.
 */
#define MAX_TRACKED_FDS 2048
static uint32_t resp_pending[MAX_TRACKED_FDS];

/* One served-response counter for the first-200 gate marker only.
 * No periodic console prints: console I/O stalls the tail latency,
 * the finding the ENA run recorded as a P99 cause. */
static uint64_t served_total;

static void drive_stack(void);

/*
 * GCP routes every guest-to-guest frame through the virtual gateway.
 * The device does not resolve ARP for guest IPs, so map one IP to a
 * static entry with the gateway-derived MAC. Same rule as
 * samples/networkload (ticket 2569ff0157). Idempotent, so calling it
 * again for a known address is cheap.
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
 * Update the interest set of an fd that is already registered with
 * epoll.
 */
static int set_epoll_events(int epfd, int fd, uint32_t events)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.fd = fd;

	return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Send as many bytes of the pending response as the socket accepts.
 *
 * Returns 0 when the response is fully sent and EPOLLOUT is
 * disarmed, returns 1 when bytes are still pending and EPOLLOUT is
 * armed, and returns -1 when the connection must be dropped.
 */
static int send_pending_response(int epfd, int fd, uint32_t base_events)
{
	uint32_t pending;
	ssize_t n;

	if (fd < 0 || fd >= MAX_TRACKED_FDS)
		return -1;

	pending = resp_pending[fd];

	while (pending > 0) {
		n = send(fd, http_response + (http_resp_len - pending), pending,
			 0);
		if (n > 0) {
			pending -= (uint32_t)n;
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == ENOBUFS || errno == EBUSY)) {
			/* Drive network stack to poll completions and relieve
			 * netdev congestion */
			drive_stack();
			n = send(fd, http_response + (http_resp_len - pending),
				 pending, 0);
			if (n > 0) {
				pending -= (uint32_t)n;
				continue;
			}
			break;
		}

		return -1;
	}

	resp_pending[fd] = pending;

	if (pending > 0) {
		/* The socket buffer is full: wait for EPOLLOUT before
		 * sending the rest. */
		if (set_epoll_events(epfd, fd, base_events | EPOLLOUT) < 0)
			return -1;

		return 1;
	}

	/* The response is complete: disarm EPOLLOUT so a writable
	 * socket does not trigger a busy loop. */
	if (set_epoll_events(epfd, fd, base_events) < 0)
		return -1;

	/* First complete response: one-shot gate marker plus the
	 * client address for the run evidence. */
	if (++served_total == 1)
		printf("GATE_HTTP_FIRST_200\n");

	return 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from the
 * epoll interest list, and close the fd.
 */
static void drop_connection(int epfd, int fd)
{
	if (fd >= 0 && fd < MAX_TRACKED_FDS)
		resp_pending[fd] = 0;

	if (fd >= 0) {
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
	}
}

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCK_BUF_SIZE;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
	ioctl(fd, FIONBIO, &opt);
}

/*
 * Drive the lwIP stack: receive pending packets from the network
 * device and process pending stack timers (retransmits, polls).
 */
static void drive_stack(void)
{
	uknetdev_poll_all();
	sys_check_timeouts();
}

/*
 * Bring the default interface up from the application parameters,
 * the same way samples/networkload does. GCP passes
 * ip=<ip>/<prefix>:<gw> after the -- separator; the netdev.einfo
 * library parameter created the interface earlier.
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

		/* The gateway itself needs the static entry too. */
		set_gcp_static_arp(&my_gw);
	}

	if (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)) &&
	    !ip_param) {
		printf("GATE_FAIL: netif is not up and no ip= parameter\n");
		return -1;
	}

	{
		char ip_str[16] = {0};
		char gw_str[16] = {0};

		ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_str,
			       sizeof(ip_str));
		ip4addr_ntoa_r(netif_ip4_gw(netif_default), gw_str,
			       sizeof(gw_str));
		printf("Interface configured: IP=%s Gateway=%s\n", ip_str,
		       gw_str);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct sockaddr_in server_addr;
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	char buffer[RECV_BUF_SIZE];
	int epfd, server_fd;
	int opt = 1;
	int n, i;

	printf("\n========================================\n");
	printf(" Unikraft HTTP Benchmark Server (gve/gVNIC)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
	printf(" Mode: Single-threaded (NO_SYS, epoll)\n");
	printf(" Driver: Google gVNIC native netdev\n");
	printf(" Stack: lwIP (IPv4/TCP/Sockets)\n");
	printf("========================================\n\n");

	if (bring_up_netif(argc, argv) != 0)
		return 1;

	server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd < 0) {
		printf("GATE_FAIL: socket create errno %d\n", errno);
		return 1;
	}

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		printf("GATE_FAIL: SO_REUSEADDR errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	configure_socket_options(server_fd);

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(LISTEN_PORT);

	if (bind(server_fd, (struct sockaddr *)&server_addr,
		 sizeof(server_addr)) < 0) {
		printf("GATE_FAIL: bind errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	if (listen(server_fd, BACKLOG) < 0) {
		printf("GATE_FAIL: listen errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("GATE_FAIL: epoll_create1 errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("GATE_FAIL: listener epoll_ctl errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	printf("[INFO] HTTP server listening on port %d (backlog: %d)\n",
	       LISTEN_PORT, BACKLOG);
	printf("GATE_HTTP_READY\n");

	for (;;) {
		/* Receive packets and run stack timers first, so that
		 * events raised by the stack are already visible to
		 * epoll before we query it. */
		drive_stack();

		/* Non-blocking: timeout 0. */
		n = epoll_wait(epfd, events, MAX_EVENTS, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("GATE_FAIL: epoll_wait errno %d\n", errno);
			break;
		}

		for (i = 0; i < n; i++) {
			uint32_t base_events = EPOLLIN | EPOLLRDHUP;
			int fd = events[i].data.fd;

			if (fd == server_fd) {
				/* Accept all pending connections */
				for (;;) {
					struct sockaddr_in client_addr;
					socklen_t client_len =
					    sizeof(client_addr);
					ip4_addr_t cli_ip;
					int cfd;

					cfd = accept4(
					    server_fd,
					    (struct sockaddr *)&client_addr,
					    &client_len, SOCK_NONBLOCK);
					if (cfd < 0)
						break;

					/* Replies to this client need the
					 * GCP static route before lwIP can
					 * answer. Idempotent per address. */
					cli_ip.addr =
						client_addr.sin_addr.s_addr;
					set_gcp_static_arp(&cli_ip);

					configure_socket_options(cfd);

					ev.events = EPOLLIN | EPOLLRDHUP;
					ev.data.fd = cfd;
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd,
						      &ev) < 0) {
						close(cfd);
						break;
					}
				}
				continue;
			}

			if (events[i].events &
			    (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				/* Peer closed the connection or an error
				 * happened: drop the connection. */
				drop_connection(epfd, fd);
				continue;
			}

			if (fd < MAX_TRACKED_FDS && resp_pending[fd] > 0) {
				/* This connection has unsent response
				 * bytes. Finish the send before any new
				 * request is handled: when the socket is
				 * writable again (EPOLLOUT), send the
				 * rest, otherwise wait for the next
				 * EPOLLOUT. */
				if (events[i].events & EPOLLOUT) {
					if (send_pending_response(
						epfd, fd, base_events) < 0)
						drop_connection(epfd, fd);
				}

				continue;
			}

			if (events[i].events & (EPOLLIN | EPOLLRDNORM)) {
				ssize_t bytes_read =
				    recv(fd, buffer, sizeof(buffer) - 1, 0);
				if (bytes_read > 0) {
					buffer[bytes_read] = '\0';

					/* Track the response bytes that
					 * still have to go out and send
					 * what the socket accepts now. If
					 * the socket buffer fills up, the
					 * rest is re-sent when EPOLLOUT
					 * fires, so no byte of the
					 * response is ever dropped. */
					if (fd < MAX_TRACKED_FDS) {
						resp_pending[fd] =
						    (uint32_t)http_resp_len;
						if (send_pending_response(
							epfd, fd, base_events) <
						    0)
							drop_connection(epfd,
									fd);
					} else {
						send(fd, http_response,
						     http_resp_len, 0);
					}

					if (strstr(buffer,
						   "Connection: close") !=
						NULL ||
					    strstr(buffer,
						   "connection: close") !=
						NULL) {
						drop_connection(epfd, fd);
					}
				} else if (bytes_read == 0 ||
					   (bytes_read < 0 && errno != EAGAIN &&
					    errno != EWOULDBLOCK)) {
					drop_connection(epfd, fd);
				}
			}
		}

		if (n == 0) {
#if defined(__x86_64__)
			__asm__ __volatile__("pause");
#elif defined(__aarch64__)
			__asm__ __volatile__("yield");
#endif
		}
	}

	close(server_fd);
	close(epfd);

	return 0;
}
