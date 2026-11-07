/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Multi-Core Benchmark Server (app-httpreply-mc).
 *
 * Port of the Unikraft-ENA httpreply-mc sample to Google Cloud. Runs
 * the lwIP stack in per-core NO_SYS mode over the multi-queue
 * uk_netdev glue. Core i polls RX queue i and transmits on TX queue i.
 * GCP spreads flows over the queue pairs with RSS; the device steers
 * each new connection to exactly one queue.
 *
 * Each core runs an isolated run-to-completion loop. The core accepts
 * on its own SO_REUSEPORT listener, serves its connections, drives
 * its own stack timers, and allocates from its own heap partition.
 * There are no locks, no inter-core queues, and no shared stack state.
 *
 * GCP networking model (same rules as samples/httpreply, ticket
 * 73b23402f0): GCP does not answer ARP for guest IPs. Every peer that
 * talks to this server needs a static ARP entry with the virtual
 * gateway MAC 42:01:<gateway-ip-bytes>. The boot parameters carry
 * peer=<client-ip> for the benchmark client, and every accept adds
 * the client entry to the accepting core's own ARP table.
 *
 * Ticket 3dc1f3f6b6.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <uk/alloc.h>
#include <uk/allocbbuddy.h>
#include <uk/arch.h>
#include <uk/arch/time.h>
#include <uk/lcpu.h>
#include <uk/lcpu/pm.h>
#include <uk/netdev.h>
#include <uk/pcpuvar.h>
#include <uk/plat/time.h>
#include <uk/print.h>
#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/schedcoop.h>
#include <uk/thread.h>

#include <lwip/etharp.h>
#include <lwip/memp.h>
#include <lwip/netif.h>
#include <lwip/tcp.h>
#include <lwip/timeouts.h>

#include "netif/uknetdev.h"
#include "lwip_percore.h"
#include "idlebackoff.h"
#include "../common/http_close.h"

#define MC_MAX_WORKERS CONFIG_UKPLAT_CPU_MAXCOUNT
#define MC_RECVBUF_SIZE 4096

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define LISTEN_PORT 8000
#define BACKLOG 512
#define MAX_EVENTS 256
#define SOCK_BUF_SIZE 32768
#define MAX_TRACKED_FDS 2048

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-GCP-Benchmark-MC\r\n"
	"\r\n"
	"Hello, World!\n";

static const size_t http_resp_len = sizeof(http_response) - 1;

struct worker_ctx {
	int worker_id;
	int epoll_fd;
	int listener_fd;
	int conn_count;
	char *recv_buf;
	uint32_t *resp_pending;
	unsigned long req_count;
	unsigned long rx_count;
};

static struct worker_ctx mc_workers[MC_MAX_WORKERS];

/* Boot-time static ARP peers parsed from argv (peer=<ip>). */
static ip4_addr_t mc_peer_ips[8];
static unsigned int mc_peer_count;
/* Gateway address with the virtual gateway MAC already known. */
static ip4_addr_t mc_gw_ip;
static int mc_gw_valid;
/* Optional smoke-test client target from curl=<ip> (host smoke only). */
static ip4_addr_t mc_curl_ip;
static int mc_curl_valid;

/*
 * These live in lib/ukboot. They are declared here directly to avoid
 * including uk/boot.h in the app target, same as the Unikraft-ENA
 * original. [Ticket 3dc1f3f6b6]
 */
unsigned int uk_percore_heap_count(void);
int uk_percore_heap_get(unsigned int i, __uptr *base, __sz *len);

#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
/* Highest core index with a bound per-core allocator (cores 1..N). */
static int mc_percore_ready;
#endif

/*
 * One-shot alloc/free stress per core, run at worker start. Bounded
 * stress on this core's own allocator. Proves that each core only
 * touches its own region while all cores run at the same time.
 */
static int mc_selftest_done[MC_MAX_WORKERS];

static void mc_percore_alloc_selftest(unsigned int core_id)
{
	unsigned int i;
	unsigned int n = 4096;

	if (mc_selftest_done[core_id])
		return;
	mc_selftest_done[core_id] = 1;

	for (i = 0; i < n; i++) {
		size_t sz = 4096 + (i * 65536) % 262144;
		void *p = malloc(sz);

		if (!p) {
			printf("httpreply-mc: [ERR] core %u alloc self-test "
			       "failed at iteration %u (size %zu)\n",
			       (unsigned int)core_id, i, sz);
			return;
		}
		memset(p, 0x5A, sz);
		free(p);
	}

	printf("httpreply-mc: core %u alloc self-test passed (%u iterations)\n",
	       (unsigned int)core_id, n);
}

/*
 * Create a standalone bbuddy allocator on each per-core heap partition
 * and bind it to that core's per-CPU slot. Core i uses partition i - 1.
 * Core 0 keeps the default allocator.
 */
static void mc_percore_alloc_init(void)
{
#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
	unsigned int n = uk_percore_heap_count();
	unsigned int i;

	mc_percore_ready = 0;

	for (i = 1;
	     i <= n && i < MC_MAX_WORKERS && i < CONFIG_UKPLAT_CPU_MAXCOUNT;
	     i++) {
		__uptr base;
		__sz len;
		struct uk_alloc *a;

		if (uk_percore_heap_get(i - 1, &base, &len) < 0)
			break;

		a = uk_allocbbuddy_init((void *)base, len);
		if (!a)
			break;

		uk_pcpuvar_lval(i, uk_pcpuvar_percore_alloc) = a;
		mc_percore_ready = i;
		printf("httpreply-mc: core %u per-core allocator %p+%lu\n",
		       (unsigned int)i, (void *)base, (unsigned long)len);
	}

	if (mc_percore_ready == 0)
		printf("httpreply-mc: [WARN] no per-core heap partitions "
		       "available\n");
#endif
}

/*
 * A core may run only when its per-core allocator slot is bound.
 * Core 0 always runs on the default allocator.
 */
static int mc_core_has_percore_alloc(unsigned int core)
{
#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
	return (core == 0) || (core <= (unsigned int)mc_percore_ready);
#else
	(void)core;
	return 1;
#endif
}

/*
 * GCP does not resolve ARP for guest IPs, so map one IP to a static
 * entry with the gateway-derived MAC. Same rule as samples/httpreply.
 * etharp_add_static_entry() writes the ARP table of the calling core,
 * so each core owns its entries. [Ticket 73b23402f0]
 */
static void set_gcp_static_arp(const ip4_addr_t *target_ip)
{
	const ip4_addr_t *gw;
	struct eth_addr gw_mac;

	if (!netif_default || !target_ip)
		return;

	gw = netif_ip4_gw(netif_default);
	gw_mac.addr[0] = 0x42;
	gw_mac.addr[1] = 0x01;
	gw_mac.addr[2] = ip4_addr1(gw);
	gw_mac.addr[3] = ip4_addr2(gw);
	gw_mac.addr[4] = ip4_addr3(gw);
	gw_mac.addr[5] = ip4_addr4(gw);

	etharp_add_static_entry(target_ip, &gw_mac);
}

/* Give this core the boot-time static ARP entries: gateway and peers. */
static void mc_seed_static_arp(unsigned int core_id)
{
	unsigned int i;

	if (!netif_default)
		return;

	if (mc_gw_valid) {
		set_gcp_static_arp(&mc_gw_ip);
		printf("httpreply-mc: core %u seeded gateway ARP\n", core_id);
	}
	for (i = 0; i < mc_peer_count; i++)
		set_gcp_static_arp(&mc_peer_ips[i]);
	if (mc_peer_count)
		printf("httpreply-mc: core %u seeded %u peer ARP entries\n",
		       core_id, mc_peer_count);
}

/*
 * Scheduler of each CPU, indexed by CPU index. A secondary core
 * writes its own entry when it starts. uk_sched_head is in AP
 * registration order, so a walk by position can hand worker i the
 * scheduler of another CPU. Queue pair i then runs on two cores and
 * the lock-free per-core heap corrupts. [Ticket f47bdd0ed1]
 */
static struct uk_sched *mc_cpu_sched[MC_MAX_WORKERS];

static struct uk_sched *mc_get_sched(int idx)
{
	if (idx < 0 || idx >= MC_MAX_WORKERS)
		return NULL;

	return mc_cpu_sched[idx];
}

/*
 * True when the ACPI MADT registered CPU slot idx. A VM with fewer
 * vCPUs than CONFIG_UKPLAT_CPU_MAXCOUNT leaves the extra slots zero.
 */
static int mc_slot_present(int idx)
{
	if (idx <= 0 || idx >= MC_MAX_WORKERS)
		return 0;

	return uk_pcpuvar_lval((unsigned int)idx, uk_pcpuvar_cpu_idx) ==
	       (unsigned long)idx;
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

#if CONFIG_APPHTTPREPLYMC_CONSOLE_STATS
static unsigned int mc_count_memp_free(void *head)
{
	unsigned int count = 0;
	void **cur = (void **)head;

	while (cur && count < 65536) {
		count++;
		cur = (void **)*cur;
	}
	return count;
}
#endif

/*
 * Drive the local core network stack: receive packets from this
 * core's RX queue and process local stack timers. Returns the number
 * of packets taken from the queue.
 */
static unsigned int drive_core_stack(int core_id)
{
	static __nsec last_hb[MC_MAX_WORKERS];
	__nsec now = ukplat_monotonic_clock();
	unsigned int rx_pkts;

	rx_pkts = uknetdev_poll_rxqueue((uint16_t)core_id);
	mc_workers[core_id].rx_count += rx_pkts;
	sys_check_timeouts();

#if CONFIG_APPHTTPREPLYMC_CONSOLE_STATS
	if (now - last_hb[core_id] >= ukarch_time_sec_to_nsec(2)) {
		struct lwip_core_state *cs = lwip_get_core_state();
		struct worker_ctx *w = &mc_workers[core_id];
		unsigned int m_pbuf = 0;

		last_hb[core_id] = now;
		if (cs->memp_tabs[MEMP_PBUF])
			m_pbuf = mc_count_memp_free(cs->memp_tabs[MEMP_PBUF]);

		printf("httpreply-mc: core %d heartbeat (rx=%lu req=%lu conns=%d "
		       "free_pbuf=%u heapfree=%lld)\n",
		       core_id, w->rx_count, w->req_count, w->conn_count,
		       m_pbuf,
		       (long long)uk_alloc_availmem(uk_alloc_get_current()));
	}
#endif

	return rx_pkts;
}

static int send_pending_response(struct worker_ctx *w, int fd);

/*
 * Read one request from a connection and queue the response.
 * Returns -1 when the connection must be dropped.
 */
static int handle_readable(struct worker_ctx *w, int fd)
{
	ssize_t r = recv(fd, w->recv_buf, MC_RECVBUF_SIZE - 1, 0);

	if (r > 0) {
		w->recv_buf[r] = '\0';
		w->req_count++;

		if (fd >= MAX_TRACKED_FDS) {
			/*
			 * This fd has no slot in resp_pending, so the
			 * worker cannot finish a response that a send()
			 * left partial. A short reply would sit on a
			 * keep-alive connection and corrupt the next
			 * response. Close instead: the client sees an
			 * end of stream, not a broken byte stream. The
			 * old code called send() once and dropped the
			 * result. Ticket 5f45856dfe.
			 */
			return -1;
		}

		w->resp_pending[fd] = (uint32_t)http_resp_len;
		if (send_pending_response(w, fd) < 0)
			return -1;

		if (ukg_http_wants_close(w->recv_buf))
			return -1;
		return 0;
	}

	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
		return -1;
	return 0;
}

/*
 * Send as many bytes of pending response as the socket accepts.
 * Returns 1 when bytes remain, 0 when done, -1 on error.
 */
static int send_pending_response(struct worker_ctx *w, int fd)
{
	uint32_t pending;
	ssize_t n;

	if (fd < 0 || fd >= MAX_TRACKED_FDS)
		return -1;

	pending = w->resp_pending[fd];

	while (pending > 0) {
		n = send(fd, http_response + (http_resp_len - pending),
			 pending, 0);
		if (n > 0) {
			pending -= (uint32_t)n;
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == ENOBUFS || errno == EBUSY)) {
			drive_core_stack(w->worker_id);
			n = send(fd,
				 http_response + (http_resp_len - pending),
				 pending, 0);
			if (n > 0) {
				pending -= (uint32_t)n;
				continue;
			}
			break;
		}

		return -1;
	}

	w->resp_pending[fd] = pending;

	/*
	 * Do not toggle EPOLLOUT here. Under load the epoll poll-chain
	 * update re-enters itself and trips the pollq _tag assert on
	 * UK 0.21. The worker loop retries pending responses every
	 * iteration instead. [Ticket f47bdd0ed1]
	 */
	return pending > 0 ? 1 : 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from epoll,
 * and close the socket descriptor.
 *
 * The fd table (uk_fmap) is lock-free: index allocation uses an atomic
 * bitmap and slot access uses atomic exchange with retry. lwIP's socket
 * layer guards its own table with SYS_ARCH_PROTECT. So no app-level fd
 * lock is needed. An fd belongs to exactly one core between accept and
 * close. [Ticket a9c6945c21]
 */
static void drop_connection(struct worker_ctx *w, int fd)
{
	if (w->conn_count > 0)
		w->conn_count--;

	if (fd >= 0 && fd < MAX_TRACKED_FDS)
		w->resp_pending[fd] = 0;

	if (fd >= 0) {
		epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
	}
}

/*
 * Create an independent listener socket on LISTEN_PORT with
 * SO_REUSEPORT. Each core creates its own listener inside its local
 * lwIP state. GCP RSS steers each new flow to one queue, so the SYN
 * reaches exactly one core and that core's listener answers it.
 */
static int create_core_listener(int core_id)
{
	struct sockaddr_in addr;
	int opt = 1;
	int fd;

	printf("httpreply-mc: core %d create_core_listener starting "
	       "(lwip_core %u, pcpu_idx %lu)\n",
	       core_id, lwip_current_core_id(),
	       (unsigned long)uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx));

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		printf("httpreply-mc: [ERR] socket failed: errno %d\n",
		       errno);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEADDR failed: errno %d\n",
		       errno);
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEPORT failed: errno %d\n",
		       errno);
		close(fd);
		return -1;
	}

	configure_socket_options(fd);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(LISTEN_PORT);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("httpreply-mc: [ERR] bind failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	if (listen(fd, BACKLOG) < 0) {
		printf("httpreply-mc: [ERR] listen failed: errno %d\n",
		       errno);
		close(fd);
		return -1;
	}

	printf("httpreply-mc: core %d listener fd=%d (lwip_core %u)\n",
	       core_id, fd, lwip_current_core_id());
	return fd;
}

/*
 * Sleep this vCPU for at most ns nanoseconds.
 *
 * The KVM platform implements this with a real halt: it arms this
 * core's own LAPIC one-shot timer for the wake, then executes
 * sti; hlt. The vCPU leaves the run queue for the whole sleep, so an
 * idle core uses no host CPU. The datapath is pure polling: the wake
 * is the core's own timer interrupt, never a device interrupt.
 * [Ticket 597c1b9731, Ticket a5b91e7993]
 */
static inline void mc_idle_sleep(uint64_t ns)
{
	unsigned long flags;
	__nsec now;

	flags = uk_lcpu_save_irqf();
	uk_lcpu_disable_irq();
	now = ukplat_monotonic_clock();
	uk_lcpu_halt_irq_until(now + (__nsec)ns);
	uk_lcpu_irqs_handle_pending();
	uk_lcpu_restore_irqf(flags);
}

/*
 * Report whether this core's lwIP instance still owns an active TCP
 * connection. A tracked fd can close (drop_connection) while its pcb
 * stays in the retransmit queue waiting for an ACK. The fd count then
 * reads zero but the flow is still live, so the core must keep
 * polling its queue. [Ticket 1152cbcaca]
 */
static int mc_core_has_pcb(void)
{
	struct lwip_core_state *cs = lwip_get_core_state();

	if (cs == NULL)
		return 0;

	return cs->tcp_active_pcbs != NULL;
}

/*
 * Run-to-completion worker engine. Bound to a single dedicated core
 * and its assigned hardware queue pair.
 */
static __noreturn void run_to_completion_worker(int core_id)
{
	unsigned long cpu = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	struct worker_ctx *w = &mc_workers[core_id];
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	struct ib_state ib;
	int server_fd;
	int n, i;

	/*
	 * Queue pair core_id must stay on CPU core_id. Each per-core
	 * heap is lock-free, so a worker on another CPU writes a free
	 * list that its owner also writes. Report it at startup.
	 * [Ticket f47bdd0ed1]
	 */
	if (cpu != (unsigned long)core_id)
		printf("httpreply-mc: [ERR] worker %d runs on CPU %lu: "
		       "queue pair %d is not single-owner\n",
		       core_id, cpu, core_id);

	mc_percore_alloc_selftest((unsigned int)core_id);
	mc_seed_static_arp((unsigned int)core_id);

	server_fd = create_core_listener(core_id);
	if (server_fd < 0) {
		printf("httpreply-mc: [ERR] core %d failed to create "
		       "listener\n", core_id);
		for (;;)
			uk_sched_yield();
	}
	w->listener_fd = server_fd;

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("httpreply-mc: [ERR] core %d failed to add listener "
		       "to epoll: errno %d\n", core_id, errno);
		close(server_fd);
		for (;;)
			uk_sched_yield();
	}

	printf("httpreply-mc: [INFO] core %d listening on port %d (queue "
	       "pair %d)\n", core_id, LISTEN_PORT, core_id);

	/*
	 * Idle backoff state for this core. Work is detected from the
	 * packet count of this core's RX queue and from epoll events,
	 * so an iteration counts as work even when it produces no
	 * epoll event (for example a timer-driven retransmit).
	 */
	ib_init(&ib, ukplat_monotonic_clock());

	for (;;) {
		int work;
		unsigned int rx;
		uint64_t sleep_ns;

		rx = drive_core_stack(core_id);

		n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("httpreply-mc: [ERR] core %d epoll_wait "
			       "errno %d\n", core_id, errno);
			continue;
		}

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == server_fd) {
				for (;;) {
					struct sockaddr_in client_addr;
					socklen_t client_len =
						sizeof(client_addr);
					int cfd;

					cfd = accept4(server_fd,
						      (struct sockaddr *)
						      &client_addr,
						      &client_len,
						      SOCK_NONBLOCK);
					if (cfd < 0)
						break;

					/* The client needs a static ARP
					 * entry in this core's table
					 * before the first response.
					 */
					{
						ip4_addr_t client_ip;

						ip4_addr_set_u32(&client_ip,
							client_addr
								.sin_addr
								.s_addr);
						set_gcp_static_arp(&client_ip);
					}

					configure_socket_options(cfd);

					ev.events = EPOLLIN | EPOLLRDHUP;
					ev.data.fd = cfd;
					if (epoll_ctl(w->epoll_fd,
						      EPOLL_CTL_ADD, cfd,
						      &ev) < 0) {
						close(cfd);
						break;
					}
					w->conn_count++;
				}
				continue;
			}

			if (events[i].events &
			    (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				drop_connection(w, fd);
				continue;
			}

			if (fd < MAX_TRACKED_FDS && w->resp_pending[fd] > 0)
				continue;

			if (events[i].events & (EPOLLIN | EPOLLRDNORM)) {
				if (handle_readable(w, fd) < 0)
					drop_connection(w, fd);
			}
		}

		/*
		 * Idle backoff. This iteration found work if an epoll
		 * event fired or a packet moved on this core's queue.
		 * The policy (idlebackoff.h) decides whether to sleep
		 * and for how long. [Ticket 597c1b9731]
		 */
		work = (n > 0) || (rx > 0);

		/*
		 * Retry any response that a previous send() left
		 * partial. The EPOLLOUT path is avoided (see
		 * send_pending_response), so drive retries from the
		 * loop directly.
		 */
		{
			int fd2;

			for (fd2 = 0; fd2 < MAX_TRACKED_FDS; fd2++) {
				int rc;

				if (w->resp_pending[fd2] > 0) {
					rc = send_pending_response(w, fd2);
					if (rc < 0) {
						drop_connection(w, fd2);
					} else {
						work = 1;
						/*
						 * The response fully went
						 * out. The EPOLLIN guard
						 * above skips reads while
						 * a response is pending,
						 * and the event for data
						 * that arrived meanwhile
						 * may not repeat. Drain
						 * the socket now. [Ticket
						 * 1152cbcaca]
						 */
						if (rc == 0 &&
						    handle_readable(w,
								    fd2) < 0)
							drop_connection(w,
									fd2);
					}
				}
			}
		}

		/*
		 * Keep the active (busy) backoff regime while this core
		 * has open connections or a live pcb. A pcb can
		 * outlive its tracked fd while it retransmits, so check
		 * both. Idle cores with no flow still fall through to
		 * deep sleep.
		 */
		work |= (w->conn_count > 0) || mc_core_has_pcb();

		sleep_ns = ib_tick(&ib, ukplat_monotonic_clock(), work);

		if (sleep_ns)
			mc_idle_sleep(sleep_ns);
	}
}

static __noreturn void worker_thread(void *arg)
{
	struct worker_ctx *w = (struct worker_ctx *)arg;

	run_to_completion_worker(w->worker_id);
}

static __noreturn void mc_secondary_entry(void *arg)
{
	struct uk_lcpu *this_lcpu = (struct uk_lcpu *)arg;
	struct uk_alloc *a = uk_alloc_get_default();
	struct uk_sched *sec_s;
	unsigned int cpu;
	int r;

	r = uk_lcpu_init(this_lcpu);
	if (unlikely(r))
		uk_lcpu_halt();

	uk_lcpu_enable_irq();

	sec_s = uk_schedcoop_create(a, a, a, a);
	if (unlikely(!sec_s))
		uk_lcpu_halt();

	/*
	 * Publish this scheduler under the CPU that runs it. Worker i
	 * must run here, not on the core that registered first.
	 * [Ticket f47bdd0ed1]
	 */
	cpu = (unsigned int)uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	if (cpu < MC_MAX_WORKERS)
		mc_cpu_sched[cpu] = sec_s;
	else
		printf("httpreply-mc: [ERR] cpu %u is outside the worker "
		       "table (%d)\n", cpu, MC_MAX_WORKERS);

	uk_sched_register(sec_s);
	uk_sched_start(sec_s);

	while (1)
		uk_sched_yield();
}

static void mc_boot_secondary_cores(void)
{
#if CONFIG_HAVE_SMP && (CONFIG_UKPLAT_CPU_MAXCOUNT > 1)
	struct uk_alloc *a = uk_alloc_get_default();
	__u64 ap_idx[MC_MAX_WORKERS];
	__u64 ap_sp[MC_MAX_WORKERS];
	__u64 ap_entry[MC_MAX_WORKERS];
	unsigned int num_aps = 0;
	unsigned int i;
	int rc;

	for (i = 1; i < MC_MAX_WORKERS && i < CONFIG_UKPLAT_CPU_MAXCOUNT;
	     i++) {
		void *stk;

		/* Start only slots that the ACPI MADT registered. */
		if (!mc_slot_present((int)i))
			continue;

		if (!mc_core_has_percore_alloc(i)) {
			printf("httpreply-mc: [INFO] core %u not started: "
			       "no per-core allocator bound\n", i);
			continue;
		}

		stk = uk_malloc(a, 16384);
		if (!stk)
			continue;
		ap_idx[num_aps] = i;
		ap_sp[num_aps] = (__u64)((__uptr)stk + 16384);
		ap_entry[num_aps] = (__u64)(__uptr)mc_secondary_entry;
		num_aps++;
	}

	if (num_aps > 0) {
		unsigned int started = num_aps;
		unsigned int j;

		rc = uk_lcpu_start(ap_idx, &started, ap_sp, ap_entry, 0);
		if (rc != 0)
			printf("httpreply-mc: [ERR] uk_lcpu_start failed: "
			       "%d\n", rc);

		for (j = 0; j < started; j++) {
			unsigned long spins;
			int online = 0;

			/* Bounded wait for the AP to come online */
			for (spins = 0; spins < 200000000UL; spins++) {
				if (uk_lcpu_state_is_online(
					uk_pcpuvar_lval(ap_idx[j], uk_lcpus)
						.state)) {
					online = 1;
					break;
				}
				uk_arch_spinwait();
			}
			if (!online) {
				printf("httpreply-mc: [WARN] lcpu %u did "
				       "not come online\n",
				       (unsigned int)ap_idx[j]);
				continue;
			}

			/* Wait for the AP to publish its own scheduler */
			for (spins = 0; spins < 200000000UL; spins++) {
				if (mc_cpu_sched[ap_idx[j]] != NULL)
					break;
				uk_arch_spinwait();
			}
			if (mc_cpu_sched[ap_idx[j]] == NULL)
				printf("httpreply-mc: [WARN] lcpu %u came "
				       "online but published no "
				       "scheduler\n",
				       (unsigned int)ap_idx[j]);
		}
	}
#endif
}

/*
 * Bring the default interface up from the application parameters,
 * the same way samples/httpreply does. GCP passes ip=<ip>/<prefix>:<gw>
 * after the -- separator; the netdev.einfo library parameter created
 * the interface earlier. Peer entries seed the first handshake.
 */
static int bring_up_netif(int argc, char *argv[])
{
	const char *ip_param = NULL, *mask_param = NULL, *gw_param = NULL;
	const char *peer_param[8];
	int peer_n = 0;
	ip4_addr_t my_ip, my_mask, my_gw;
	int i;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "ip=", 3) == 0)
			ip_param = argv[i] + 3;
		else if (strncmp(argv[i], "mask=", 5) == 0)
			mask_param = argv[i] + 5;
		else if (strncmp(argv[i], "gw=", 3) == 0)
			gw_param = argv[i] + 3;
		else if (strncmp(argv[i], "peer=", 5) == 0 && peer_n < 8)
			peer_param[peer_n++] = argv[i] + 5;
		else if (strncmp(argv[i], "curl=", 5) == 0) {
			if (ip4addr_aton(argv[i] + 5, &mc_curl_ip) == 1)
				mc_curl_valid = 1;
			else
				printf("[WARN] Unparsable curl=%s\n",
				       argv[i] + 5);
		}
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
		mc_gw_ip = my_gw;
		mc_gw_valid = 1;

		/*
		 * Boot-time ARP for the benchmark client. lwIP sends the
		 * SYN-ACK inside the stack, before accept() can run, so
		 * the per-accept entry is too late for the handshake.
		 * The run scripts pass peer=<client ip> for exactly
		 * this. [Ticket 73b23402f0]
		 */
		for (i = 0; i < peer_n; i++) {
			if (ip4addr_aton(peer_param[i],
					 &mc_peer_ips[mc_peer_count]) == 1) {
				set_gcp_static_arp(
					&mc_peer_ips[mc_peer_count]);
				mc_peer_count++;
				printf("Static ARP added for peer %s\n",
				       peer_param[i]);
			} else {
				printf("[WARN] Unparsable peer %s\n",
				       peer_param[i]);
			}
		}
	}

	if (!(netif_is_up(netif_default) &&
	      netif_is_link_up(netif_default)) && !ip_param) {
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

/*
 * Host smoke-test client (curl=<server-ip> boot parameter). One
 * non-blocking GET on core 0, driven by this core's queue-0 poll
 * loop. Prints one machine-readable gate line and returns. Used for
 * the two-QEMU-VM traffic check; the cloud gate drives from a real
 * Linux client instead. [Ticket 3dc1f3f6b6]
 */
static void mc_client_probe(void)
{
	static const char get_req[] =
		"GET / HTTP/1.1\r\nHost: ukg\r\n\r\n";
	struct sockaddr_in sa;
	char rbuf[512];
	__nsec deadline;
	size_t sent = 0, got = 0;
	int fd, rc;

	set_gcp_static_arp(&mc_curl_ip);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		printf("GATE_FAIL: mc client socket errno %d\n", errno);
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(LISTEN_PORT);
	sa.sin_addr.s_addr = mc_curl_ip.addr;

	deadline = ukplat_monotonic_clock() + ukarch_time_sec_to_nsec(20);

	/* Connect: EINPROGRESS is expected; liveness proves the path. */
	rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0 && errno != EINPROGRESS && errno != EAGAIN) {
		printf("GATE_FAIL: mc client connect errno %d\n", errno);
		close(fd);
		return;
	}

	while (ukplat_monotonic_clock() < deadline) {
		uknetdev_poll_rxqueue(0);
		sys_check_timeouts();

		if (sent < sizeof(get_req) - 1) {
			rc = send(fd, get_req + sent,
				  sizeof(get_req) - 1 - sent, 0);
			if (rc > 0)
				sent += (size_t)rc;
		} else {
			ssize_t r;

			if (got >= sizeof(rbuf) - 1)
				break;
			r = recv(fd, rbuf + got,
				 sizeof(rbuf) - 1 - got, 0);

			if (r > 0) {
				got += (size_t)r;
				/* Whole reply received (headers +
				 * 14-byte body is < 200 bytes). */
				if (strstr(rbuf, "\r\n") != NULL &&
				    got >= 80)
					break;
			} else if (r == 0) {
				break;
			} else if (errno != EAGAIN &&
				   errno != EWOULDBLOCK) {
				printf("GATE_FAIL: mc client recv "
				       "errno %d (sent %zu got %zu)\n",
				       errno, sent, got);
				close(fd);
				return;
			}
		}
	}

	rbuf[got < sizeof(rbuf) ? got : sizeof(rbuf) - 1] = '\0';
	if (strstr(rbuf, "200 OK") != NULL)
		printf("GATE_MC_CURL_OK bytes=%zu resp=%.16s\n", got, rbuf);
	else
		printf("GATE_FAIL: mc client no response (got %zu bytes, "
		       "head=[%s])\n", got, rbuf);
	close(fd);
}

int main(int argc, char **argv)
{
	int i;

	printf("\n=============================================\n");
	printf(" Unikraft HTTP Benchmark Server (httpreply-mc)\n");
	printf(" Port: %d (TCP, SO_REUSEPORT per core)\n", LISTEN_PORT);
	printf(" Mode: Shared-nothing run-to-completion (NO_SYS)\n");
	printf(" Stack: lwIP per-core state, one queue pair per core\n");
	printf("=============================================\n\n");

	if (bring_up_netif(argc, argv) != 0)
		return 1;

	mc_percore_alloc_init();

	mc_boot_secondary_cores();

	if (mc_curl_valid)
		mc_client_probe();

	/*
	 * Count the CPUs that published a scheduler. Worker i runs on
	 * CPU i, so a CPU with no entry has no worker. [Ticket
	 * f47bdd0ed1]
	 */
	{
		int workers = 1;

		for (i = 1; i < MC_MAX_WORKERS &&
		     i < (int)CONFIG_UKPLAT_CPU_MAXCOUNT; i++)
			if (mc_get_sched(i) != NULL)
				workers++;
		printf("httpreply-mc: detected %d worker cores\n", workers);
	}

	for (i = 0; i < MC_MAX_WORKERS && i < (int)CONFIG_UKPLAT_CPU_MAXCOUNT;
	     i++) {
		mc_workers[i].worker_id = i;
		mc_workers[i].epoll_fd = epoll_create1(0);
		if (mc_workers[i].epoll_fd < 0) {
			printf("httpreply-mc: [ERR] epoll_create1 failed "
			       "for worker %d\n", i);
			return 1;
		}
		mc_workers[i].recv_buf = malloc(MC_RECVBUF_SIZE);
		mc_workers[i].resp_pending =
			calloc(MAX_TRACKED_FDS, sizeof(uint32_t));
		mc_workers[i].conn_count = 0;
		mc_workers[i].req_count = 0;
		mc_workers[i].rx_count = 0;
	}

	for (i = 1; i < MC_MAX_WORKERS && i < (int)CONFIG_UKPLAT_CPU_MAXCOUNT;
	     i++) {
		struct uk_sched *ws = mc_get_sched(i);
		struct uk_thread *th;

		if (ws == NULL) {
			/*
			 * Worker i polls queue pair i. On another CPU it
			 * shares a lock-free per-core heap with that
			 * CPU's worker. Say so now instead of faulting
			 * later. [Ticket f47bdd0ed1]
			 */
			if (mc_slot_present(i))
				printf("httpreply-mc: [WARN] core %d has no "
				       "scheduler: worker %d will not run on "
				       "its own CPU\n", i, i);
			ws = uk_sched_current();
		}

		th = uk_sched_thread_create(ws, worker_thread,
					    &mc_workers[i], "worker");
		if (th == NULL) {
			printf("httpreply-mc: [ERR] failed to start worker "
			       "%d\n", i);
			return 1;
		}
	}

	run_to_completion_worker(0);

	return 0;
}
