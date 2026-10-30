/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Google Virtual Ethernet (gVNIC) PCI and uk_netdev driver for Unikraft.
 * Probes PCI 1ae0:0042, maps 64-bit BARs, initializes Admin Queue,
 * configures device resources, creates TX/RX queues, and registers uk_netdev.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <uk/config.h>
#include <uk/alloc.h>
#include <uk/print.h>
#include <uk/essentials.h>
#include <uk/bus/pci.h>
#include <uk/netbuf.h>
#include <uk/netdev.h>
#include <uk/netdev_core.h>
#include <uk/netdev_driver.h>
#include <uk/plat/time.h>
#include <gve.h>

#define DRIVER_NAME		"gve"
#define GVE_PAGE_SIZE		4096UL
#define GVE_PKT_BUF_ALIGN	2048UL
/*
 * Admin Queue completion bound. The Linux gve driver waits up to
 * 10 s for one command. The guest is single-threaded here, so the
 * wait is a busy loop with pause instead of a sleep.
 */
#define GVE_ADMINQ_TIMEOUT_MSEC	10000UL
/* Bound for the link-up poll in netdev_start. */
#define GVE_LINK_TIMEOUT_MSEC	2000UL

/*
 * Upper bound on queue pairs the driver can hold. The device budget
 * (min of max queues and half the event counters) is always smaller,
 * but the static queue arrays need a compile-time bound. Eight pairs
 * covers every GCP machine size. Ticket a9b1936640.
 */
#define GVE_MAX_QUEUES		8U

static struct uk_alloc *drv_alloc;

/* 2 MiB page directories for mapping BAR0 (slot 0) and BAR2 (slot 1) */
static uint64_t gve_bar_pd[2][512] __attribute__((aligned(4096)));

struct gve_device;

struct uk_netdev_tx_queue {
	struct gve_device *gdev;
	struct uk_alloc *alloc;	/* allocator that owns these buffers */
	uint16_t qid;
	uint16_t cnt;
	uint16_t mask;
	uint16_t comp_cnt;
	uint16_t comp_mask;
	uint32_t ntfy_id;
	uint32_t db_idx;	/* device BAR2 doorbell index, checked at configure */
	struct gve_queue_resources *q_resources;
	uint8_t *bounce_bufs;

	/* DQO_RDA rings and state */
	union gve_tx_desc_dqo *dqo_tx_ring;
	struct gve_tx_compl_desc *dqo_compl_ring;
	uint32_t dqo_tail;	/* free-running producer entry counter */
	uint32_t dqo_head;	/* free-running reclaimed entry counter */
	uint32_t dqo_complq_head;
	uint8_t dqo_cur_gen_bit;

	/* GQI_QPL / GQI_RDA ring and state */
	union gve_tx_desc *gqi_tx_ring;
	uint32_t gqi_req;
	uint32_t gqi_done;
	uint32_t qpl_id;
	uint32_t num_qpl_pages;
	uint8_t qpl_registered;	/* device holds this QPL: unregister on free */
	uint8_t *qpl_pages;
	uint64_t *qpl_page_bus;
};

struct uk_netdev_rx_queue {
	struct gve_device *gdev;
	struct uk_alloc *alloc;	/* allocator that owns these buffers */
	uint16_t qid;
	uint16_t cnt;
	uint16_t mask;
	uint16_t buf_cnt;
	uint16_t buf_mask;
	uint32_t ntfy_id;
	uint32_t db_idx;	/* device BAR2 doorbell index, checked at configure */
	struct gve_queue_resources *q_resources;
	uk_netdev_alloc_rxpkts alloc_rxpkts;
	void *alloc_rxpkts_argp;

	/* DQO_RDA rings and state */
	struct gve_rx_desc_dqo *dqo_bufq_ring;
	struct gve_rx_compl_desc_dqo *dqo_complq_ring;
	uint8_t *dqo_rx_bufs;
	uint32_t dqo_bufq_tail;
	uint32_t dqo_refill_pending;	/* reposts since last doorbell */
	uint32_t dqo_complq_head;
	uint8_t dqo_cur_gen_bit;

	/* GQI_QPL / GQI_RDA rings and state */
	struct gve_rx_desc *gqi_desc_ring;
	union gve_rx_data_slot *gqi_data_ring;
	uint32_t gqi_cnt;
	uint32_t gqi_fill_cnt;
	uint8_t gqi_seqno;
	uint32_t qpl_id;
	uint32_t num_qpl_pages;
	uint8_t qpl_registered;	/* device holds this QPL: unregister on free */
	uint8_t *qpl_pages;
	uint64_t *qpl_page_bus;
};

struct gve_device {
	struct uk_netdev netdev;
	struct pci_device *pdev;
	volatile struct gve_registers *reg_bar0;
	volatile uint32_t *db_bar2;
	uint64_t bar0_size;	/* BAR0 byte size from the config-space probe */
	uint64_t bar2_size;	/* BAR2 byte size from the config-space probe */
	union gve_adminq_command *adminq;
	uint32_t adminq_prod_cnt;

	enum gve_queue_format queue_format;
	int has_dqo_rda;
	int has_dqo_qpl;
	int has_gqi_rda;
	int has_gqi_qpl;

	struct uk_hwaddr hw_addr;
	uint16_t mtu;
	uint16_t max_mtu;
	uint16_t tx_desc_cnt;
	uint16_t rx_desc_cnt;
	uint16_t tx_comp_ring_entries;
	uint16_t rx_buff_ring_entries;
	uint16_t tx_pages_per_qpl;
	uint16_t rx_pages_per_qpl;
	uint16_t num_event_counters;
	uint32_t max_tx_queues;
	uint32_t max_rx_queues;
	uint64_t max_registered_pages;
	uint64_t registered_pages;	/* pages in QPLs the device holds now */

	uint32_t *counter_array;
	struct gve_irq_db *irq_db_indices;
	uint32_t num_ntfy_blks;
	struct gve_ptype_lut *ptype_lut_dqo;
	int resources_configured;

	/* RSS state. Sizes come from the RSS_CONFIG device option when
	 * present, else from the GVE_RSS_* defaults. */
	int rss_opt_present;
	uint16_t rss_key_size;
	uint16_t rss_lut_size;

	/* Queue pairs accepted by gve_netdev_configure. The datapath
	 * and start path iterate this many entries. Ticket a9b1936640. */
	uint16_t queue_pairs;
	struct uk_netdev_tx_queue *txq[GVE_MAX_QUEUES];
	struct uk_netdev_rx_queue *rxq[GVE_MAX_QUEUES];
};

#define to_gvedev(ndev) \
	__containerof(ndev, struct gve_device, netdev)

static inline uint16_t bswap16(uint16_t x)
{
	return __builtin_bswap16(x);
}

static inline uint32_t bswap32(uint32_t x)
{
	return __builtin_bswap32(x);
}

static inline uint64_t bswap64(uint64_t x)
{
	return __builtin_bswap64(x);
}

static inline size_t align_up_page(size_t sz)
{
	return (sz + (GVE_PAGE_SIZE - 1UL)) & ~(GVE_PAGE_SIZE - 1UL);
}

/*
 * Return the PDPT entry this driver uses for a BAR slot, or NULL
 * when the slot index is invalid. Slots 0 and 1 map PDPT entries 4
 * and 5, covering the 4 GiB and 5 GiB virtual regions.
 *
 * Assumption: the Unikraft 0.21 boot page table leaves PDPT entries
 * 4 and 5 empty. The present-bit check in gve_map_bar turns a broken
 * assumption into a clean failure: a second gVNIC device fails its
 * map instead of clobbering the first device's window.
 */
static volatile uint64_t *gve_pdpt_entry(int slot)
{
	uint64_t cr3;
	uint64_t *pml4;
	uint64_t *pdpt0;

	if (slot < 0 || slot > 1)
		return NULL;

	__asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
	pml4 = (uint64_t *)(uintptr_t)(cr3 & ~0xfffULL);
	pdpt0 = (uint64_t *)(uintptr_t)(pml4[0] & ~0xfffULL);
	return &pdpt0[4 + slot];
}

/*
 * Map a 32-bit or 64-bit physical BAR address into pdpt0[4 + slot]
 * using 2 MiB pages with uncacheable RW attributes (0x9b). Refuse a
 * slot whose entry is already present.
 */
static volatile void *gve_map_bar(uint64_t paddr, int slot)
{
	volatile uint64_t *entry;
	uint64_t cr3;
	uint64_t gb_base;
	uint64_t vaddr_base;
	int i;

	if (paddr == 0)
		return NULL;

	entry = gve_pdpt_entry(slot);
	if (!entry)
		return NULL;
	if (*entry & 0x1ULL)
		return NULL;

	__asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));

	gb_base = paddr & ~0x3fffffffULL;
	for (i = 0; i < 512; i++)
		gve_bar_pd[slot][i] = (gb_base + ((uint64_t)i << 21)) | 0x9bULL;

	*entry = ((uint64_t)(uintptr_t)gve_bar_pd[slot]) | 0x03ULL;
	__asm__ __volatile__("movq %0, %%cr3" :: "r"(cr3) : "memory");

	vaddr_base = ((uint64_t)(4 + slot)) << 30;
	return (volatile void *)(uintptr_t)(vaddr_base + (paddr & 0x3fffffffULL));
}

/*
 * Clear a BAR slot mapping and flush the TLB with a cr3 reload. The
 * caller must not touch the mapped window afterwards.
 */
static void gve_unmap_bar(int slot)
{
	volatile uint64_t *entry;
	uint64_t cr3;

	entry = gve_pdpt_entry(slot);
	if (!entry || !(*entry & 0x1ULL))
		return;

	*entry = 0ULL;
	__asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
	__asm__ __volatile__("movq %0, %%cr3" :: "r"(cr3) : "memory");
}

/*
 * Read one BAR size with the standard config-space probe: save the
 * BAR, write all ones, read the decode mask, and restore the BAR.
 * Nothing maps or accesses the BAR while it holds the probe value,
 * so the write cannot disturb the running system. Return the size
 * in bytes, decoded by gve_bar_size_from_mask(). Ticket ad4bd33ab8.
 */
static uint64_t gve_bar_size(struct pci_device *pdev, uint32_t bar_off)
{
	uint32_t orig = uk_pci_read_config32(&pdev->addr, bar_off);
	uint32_t mask;

	uk_pci_write_config32(&pdev->addr, bar_off, 0xffffffffU);
	mask = uk_pci_read_config32(&pdev->addr, bar_off);
	uk_pci_write_config32(&pdev->addr, bar_off, orig);
	return gve_bar_size_from_mask(mask);
}

static inline uint32_t mmio_read32(const volatile uint32_t *reg)
{
	uint32_t val = *reg;

	__asm__ __volatile__("" ::: "memory");
	return val;
}

static inline void mmio_write32(volatile uint32_t *reg, uint32_t val)
{
	__asm__ __volatile__("" ::: "memory");
	*reg = val;
	__asm__ __volatile__("mfence" ::: "memory");
}

static inline void mmio_write16(volatile uint16_t *reg, uint16_t val)
{
	__asm__ __volatile__("" ::: "memory");
	*reg = val;
	__asm__ __volatile__("mfence" ::: "memory");
}

static inline void mmio_write8(volatile uint8_t *reg, uint8_t val)
{
	__asm__ __volatile__("" ::: "memory");
	*reg = val;
	__asm__ __volatile__("mfence" ::: "memory");
}

static const char *gve_option_name(uint16_t opt_id)
{
	switch (opt_id) {
	case GVE_DEV_OPT_ID_GQI_RAW_ADDRESSING:
		return "GQI_RAW_ADDRESSING";
	case GVE_DEV_OPT_ID_GQI_RDA:
		return "GQI_RDA";
	case GVE_DEV_OPT_ID_GQI_QPL:
		return "GQI_QPL";
	case GVE_DEV_OPT_ID_DQO_RDA:
		return "DQO_RDA";
	case GVE_DEV_OPT_ID_MODIFY_RING:
		return "MODIFY_RING";
	case GVE_DEV_OPT_ID_DQO_QPL:
		return "DQO_QPL";
	case GVE_DEV_OPT_ID_JUMBO_FRAMES:
		return "JUMBO_FRAMES";
	case GVE_DEV_OPT_ID_BUFFER_SIZES:
		return "BUFFER_SIZES";
	case GVE_DEV_OPT_ID_FLOW_STEERING:
		return "FLOW_STEERING";
	case GVE_DEV_OPT_ID_RSS_CONFIG:
		return "RSS_CONFIG";
	default:
		return "UNKNOWN";
	}
}

static const char *gve_queue_format_name(enum gve_queue_format fmt)
{
	switch (fmt) {
	case GVE_GQI_RDA_FORMAT:
		return "GQI_RDA";
	case GVE_GQI_QPL_FORMAT:
		return "GQI_QPL";
	case GVE_DQO_RDA_FORMAT:
		return "DQO_RDA";
	case GVE_DQO_QPL_FORMAT:
		return "DQO_QPL";
	default:
		return "UNSPECIFIED";
	}
}

static int gve_adminq_exec(volatile struct gve_registers *reg_bar0,
			   union gve_adminq_command *adminq,
			   uint32_t *prod_cnt,
			   const union gve_adminq_command *cmd_in,
			   uint32_t *status_out)
{
	uint32_t mask = (GVE_PAGE_SIZE / sizeof(union gve_adminq_command)) - 1U;
	uint32_t slot = (*prod_cnt) & mask;
	uint32_t target;
	uint32_t ev_cnt = 0;
	uint32_t status;
	__nsec deadline;

	memcpy(&adminq[slot], cmd_in, sizeof(*cmd_in));
	adminq[slot].status = GVE_ADMINQ_COMMAND_UNSET;
	__asm__ __volatile__("mfence" ::: "memory");

	(*prod_cnt)++;
	target = *prod_cnt;
	mmio_write32(&reg_bar0->adminq_doorbell, bswap32(target));

	/*
	 * Time-based wait. ukplat_monotonic_clock() reads the
	 * calibrated TSC clock, so the bound does not depend on the
	 * loop speed or the CPU frequency of a particular machine
	 * size.
	 */
	deadline = ukplat_monotonic_clock() +
		   ukarch_time_msec_to_nsec(GVE_ADMINQ_TIMEOUT_MSEC);
	for (;;) {
		ev_cnt = bswap32(mmio_read32(&reg_bar0->adminq_event_counter));
		if (ev_cnt == target)
			break;
		if (ukplat_monotonic_clock() >= deadline)
			break;
		__asm__ __volatile__("pause" ::: "memory");
	}

	if (ev_cnt != target) {
		uk_pr_err("gve: AdminQ timeout after %u ms: doorbell=%u event_counter=%u\n",
			  (unsigned int)GVE_ADMINQ_TIMEOUT_MSEC, target, ev_cnt);
		return -ETIMEDOUT;
	}

	status = bswap32(adminq[slot].status);
	if (status_out)
		*status_out = status;

	return (status == GVE_ADMINQ_COMMAND_PASSED) ? 0 : -EIO;
}

/*
 * Teardown commands. They run on error unwinding and on queue
 * reconfiguration. They return the AdminQ status so the caller can
 * decide whether the device still holds the memory the queue owns.
 * A failed destroy or unregister means the device may still DMA
 * into those buffers.
 */
static int gve_adminq_destroy_tx_queue(struct gve_device *gdev,
				       uint16_t queue_id)
{
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	int rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_DESTROY_TX_QUEUE);
	cmd.destroy_tx_queue.queue_id = bswap32(queue_id);
	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: DESTROY_TX_QUEUE qid=%u rc=%d status=0x%x\n",
		   queue_id, rc, cmd_status);
	return rc;
}

static int gve_adminq_destroy_rx_queue(struct gve_device *gdev,
				       uint16_t queue_id)
{
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	int rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_DESTROY_RX_QUEUE);
	cmd.destroy_rx_queue.queue_id = bswap32(queue_id);
	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: DESTROY_RX_QUEUE qid=%u rc=%d status=0x%x\n",
		   queue_id, rc, cmd_status);
	return rc;
}

static int gve_adminq_unregister_page_list(struct gve_device *gdev,
					   uint32_t qpl_id,
					   uint32_t num_pages)
{
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	int rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_UNREGISTER_PAGE_LIST);
	cmd.unreg_page_list.page_list_id = bswap32(qpl_id);
	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: UNREGISTER_PAGE_LIST id=%u rc=%d status=0x%x\n",
		   qpl_id, rc, cmd_status);
	/*
	 * Release the driver-side budget share on success only. The
	 * count mirrors what gve_register_qpl added on success. When
	 * the unregister fails, the caller leaks the page list, so
	 * the budget share stays charged: that memory never returns
	 * to the pool.
	 */
	if (rc == 0) {
		if (gdev->registered_pages >= num_pages)
			gdev->registered_pages -= num_pages;
		else
			gdev->registered_pages = 0;
	}
	return rc;
}

static int gve_register_qpl(struct gve_device *gdev, struct uk_alloc *a,
			    uint32_t qpl_id, uint32_t num_pages,
			    uint8_t **pages_out,
			    uint64_t **page_bus_out)
{
	uint8_t *pages;
	uint64_t *page_bus;
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	uint32_t i;
	int rc;

	/*
	 * Enforce the device page budget before any allocation. A
	 * device that reports 0 sets no limit. The passing c3 device
	 * reports 16384 pages, far above this driver's worst case of
	 * 1024 RX plus 64 TX pages, so the check only refuses broken
	 * or hostile descriptors.
	 */
	if (!gve_qpl_within_limit(gdev->registered_pages, num_pages,
				  gdev->max_registered_pages)) {
		uk_pr_err("gve: QPL id %u wants %u pages, budget is %lu of %llu used\n",
			  qpl_id, num_pages,
			  (unsigned long)gdev->registered_pages,
			  (unsigned long long)gdev->max_registered_pages);
		return -ENOMEM;
	}

	pages = uk_memalign(a, GVE_PAGE_SIZE,
			    (size_t)num_pages * GVE_PAGE_SIZE);
	if (!pages)
		return -ENOMEM;
	memset(pages, 0, (size_t)num_pages * GVE_PAGE_SIZE);
	page_bus = uk_memalign(a, GVE_PAGE_SIZE,
			       align_up_page((size_t)num_pages * sizeof(uint64_t)));
	if (!page_bus) {
		uk_free(a, pages);
		return -ENOMEM;
	}
	memset(page_bus, 0, align_up_page((size_t)num_pages * sizeof(uint64_t)));

	for (i = 0; i < num_pages; i++) {
		uint64_t paddr = (uint64_t)(uintptr_t)(pages + (size_t)i * GVE_PAGE_SIZE);

		page_bus[i] = bswap64(paddr);
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_REGISTER_PAGE_LIST);
	cmd.reg_page_list.page_list_id = bswap32(qpl_id);
	cmd.reg_page_list.num_pages = bswap32(num_pages);
	cmd.reg_page_list.page_address_list_addr =
		bswap64((uint64_t)(uintptr_t)page_bus);
	cmd.reg_page_list.page_size = bswap64(GVE_PAGE_SIZE);

	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: REGISTER_PAGE_LIST id=%u pages=%u rc=%d status=0x%x\n",
		   qpl_id, num_pages, rc, cmd_status);
	if (rc != 0) {
		/*
		 * A timeout does not cancel the command. The device
		 * can still read the bus list and DMA into those
		 * pages, so leak both buffers on -ETIMEDOUT. A
		 * returned status (-EIO) means the device finished
		 * the command, so the buffers are safe to free.
		 * Ticket 6e2054b369.
		 */
		if (rc == -ETIMEDOUT) {
			uk_pr_err("gve: REGISTER_PAGE_LIST id=%u timed out, leaking page list buffers\n",
				  qpl_id);
			return rc;
		}
		uk_free(a, page_bus);
		uk_free(a, pages);
		return rc;
	}

	gdev->registered_pages += num_pages;
	*pages_out = pages;
	*page_bus_out = page_bus;
	return 0;
}

/*
 * Program RSS so the device spreads flows across every configured RX
 * queue: Toeplitz algorithm, the Linux default hash-type set (TCP and
 * UDP over IPv4 and IPv6), a fixed key, and the default indirection
 * table (entry i selects queue i % queue_pairs). Sizes come from the
 * RSS_CONFIG device option when the device advertised one.
 *
 * The key and LUT buffers only need to stay valid until the command
 * completes, the same contract the Linux driver relies on. A timeout
 * does not cancel the command, so that path leaks the buffers instead
 * of freeing memory the device may still read (ticket 6e2054b369). A
 * rejected command is not fatal: steering then follows whatever the
 * device had configured before. Ticket a9b1936640.
 */
static int gve_configure_rss(struct gve_device *gdev)
{
	uint16_t key_size = gdev->rss_opt_present ? gdev->rss_key_size
						   : GVE_RSS_KEY_SIZE;
	uint16_t lut_size = gdev->rss_opt_present ? gdev->rss_lut_size
						   : GVE_RSS_INDIR_SIZE;
	uint32_t pairs = gdev->queue_pairs ? gdev->queue_pairs : 1U;
	uint16_t hash_types = (uint16_t)((1U << GVE_RSS_HASH_TCPV4) |
					 (1U << GVE_RSS_HASH_UDPV4) |
					 (1U << GVE_RSS_HASH_TCPV6) |
					 (1U << GVE_RSS_HASH_UDPV6));
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	uint8_t *key;
	uint32_t *lut;
	int rc;

	if (key_size == 0U || key_size > GVE_RSS_KEY_SIZE_MAX ||
	    lut_size == 0U || lut_size > GVE_RSS_INDIR_SIZE_MAX) {
		uk_pr_err("gve: RSS sizes out of driver bounds (key=%u lut=%u), not configuring RSS\n",
			  key_size, lut_size);
		return -EINVAL;
	}

	key = uk_memalign(drv_alloc, GVE_PAGE_SIZE, align_up_page(key_size));
	lut = uk_memalign(drv_alloc, GVE_PAGE_SIZE,
			  align_up_page((size_t)lut_size * sizeof(uint32_t)));
	if (!key || !lut) {
		if (key)
			uk_free(drv_alloc, key);
		if (lut)
			uk_free(drv_alloc, lut);
		return -ENOMEM;
	}
	gve_rss_fill_key(key, key_size);
	gve_rss_fill_lut(lut, lut_size, pairs);

	gve_adminq_fill_configure_rss(&cmd, hash_types,
				      GVE_RSS_HASH_ALG_TOEPLITZ,
				      (uint64_t)(uintptr_t)key, key_size,
				      (uint64_t)(uintptr_t)lut, lut_size);
	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: CONFIGURE_RSS types=0x%x alg=top key=%u lut=%u queues=%u rc=%d status=0x%x\n",
		   hash_types, key_size, lut_size, pairs, rc, cmd_status);
	if (rc == -ETIMEDOUT) {
		uk_pr_err("gve: CONFIGURE_RSS timed out, leaking rss buffers\n");
		return rc;
	}
	uk_free(drv_alloc, key);
	uk_free(drv_alloc, lut);
	return rc;
}

static int gve_configure_resources(struct gve_device *gdev)
{
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	uint32_t ntfy_candidates[2];
	uint32_t ntfy_want;
	int cand_idx;
	int rc = -EIO;
	int rss_rc;
	int got_counters = 0;	/* allocated by this call: free on failure */
	int got_dbs = 0;
	int got_ptype = 0;
	int cfg_timed_out = 0;	/* a CONFIGURE command may still run */

	if (gdev->resources_configured)
		return 0;

	if (!gdev->counter_array) {
		size_t cnt_sz = align_up_page((size_t)gdev->num_event_counters *
					      sizeof(uint32_t));

		if (cnt_sz == 0)
			cnt_sz = GVE_PAGE_SIZE;
		gdev->counter_array = uk_memalign(drv_alloc, GVE_PAGE_SIZE, cnt_sz);
		if (!gdev->counter_array)
			return -ENOMEM;
		memset(gdev->counter_array, 0, cnt_sz);
		got_counters = 1;
	}

	if (!gdev->irq_db_indices) {
		size_t db_sz = align_up_page((size_t)gdev->num_event_counters *
					     sizeof(struct gve_irq_db));

		if (db_sz == 0)
			db_sz = GVE_PAGE_SIZE;
		gdev->irq_db_indices = uk_memalign(drv_alloc, GVE_PAGE_SIZE, db_sz);
		if (!gdev->irq_db_indices) {
			rc = -ENOMEM;
			goto fail;
		}
		memset(gdev->irq_db_indices, 0, db_sz);
		got_dbs = 1;
	}

	/*
	 * The device may write one irq db index and one counter per
	 * notification block, so the advertised block count must fit
	 * the arrays sized from num_event_counters above. Cap both
	 * candidates. The target is one block pair per queue pair;
	 * refuse a device whose budget cannot support even one pair.
	 * Ticket 0b923396ef, multi-queue rebase ticket a9b1936640.
	 */
	ntfy_want = 2U * (gdev->queue_pairs ? gdev->queue_pairs : 1U);
	if (gdev->num_event_counters < ntfy_want) {
		printf("GATE_FAIL: gve device reports %u event counters, need at least %u for %u queue pairs\n",
		       (unsigned int)gdev->num_event_counters,
		       (unsigned int)ntfy_want,
		       (unsigned int)(ntfy_want / 2U));
		return -ENODEV;
	}
	ntfy_candidates[0] = gve_ntfy_cap(ntfy_want, gdev->num_event_counters);
	ntfy_candidates[1] = gve_ntfy_cap(gdev->max_tx_queues +
					  gdev->max_rx_queues,
					  gdev->num_event_counters);
	if (ntfy_candidates[1] < ntfy_candidates[0])
		ntfy_candidates[1] = ntfy_candidates[0];

	for (cand_idx = 0; cand_idx < 2; cand_idx++) {
		uint32_t num_ntfy = ntfy_candidates[cand_idx];

		gve_adminq_fill_configure_resources(
			&cmd,
			(uint64_t)(uintptr_t)gdev->counter_array,
			(uint64_t)(uintptr_t)gdev->irq_db_indices,
			gdev->num_event_counters,
			num_ntfy,
			(uint8_t)gdev->queue_format);

		rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
				     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
		uk_pr_info("gve: CONFIGURE_DEVICE_RESOURCES fmt=%s(%u) counters=%u irq_dbs=%u rc=%d status=0x%x\n",
			   gve_queue_format_name(gdev->queue_format),
			   (unsigned int)gdev->queue_format,
			   gdev->num_event_counters, num_ntfy, rc, cmd_status);
		if (rc == -ETIMEDOUT)
			cfg_timed_out = 1;
		if (rc == 0) {
			gdev->num_ntfy_blks = num_ntfy;
			break;
		}
	}

	if (rc != 0 && gdev->queue_format != GVE_GQI_QPL_FORMAT &&
	    gdev->has_gqi_qpl) {
		uk_pr_info("gve: Falling back from %s to GQI_QPL\n",
			   gve_queue_format_name(gdev->queue_format));
		gdev->queue_format = GVE_GQI_QPL_FORMAT;
		gve_adminq_fill_configure_resources(
			&cmd,
			(uint64_t)(uintptr_t)gdev->counter_array,
			(uint64_t)(uintptr_t)gdev->irq_db_indices,
			gdev->num_event_counters,
			ntfy_candidates[0],
			(uint8_t)gdev->queue_format);
		rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
				     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
		uk_pr_info("gve: CONFIGURE_DEVICE_RESOURCES (fallback GQI_QPL) rc=%d status=0x%x\n",
			   rc, cmd_status);
		if (rc == -ETIMEDOUT)
			cfg_timed_out = 1;
		if (rc == 0)
			gdev->num_ntfy_blks = ntfy_candidates[0];
	}

	if (rc != 0)
		goto fail;

	if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
		if (!gdev->ptype_lut_dqo) {
			gdev->ptype_lut_dqo = uk_memalign(
				drv_alloc, GVE_PAGE_SIZE,
				align_up_page(sizeof(struct gve_ptype_lut)));
			if (!gdev->ptype_lut_dqo) {
				rc = -ENOMEM;
				goto fail;
			}
			memset(gdev->ptype_lut_dqo, 0,
			       align_up_page(sizeof(struct gve_ptype_lut)));
			got_ptype = 1;
		}

		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = bswap32(GVE_ADMINQ_GET_PTYPE_MAP);
		cmd.get_ptype_map.ptype_map_len =
			bswap64(sizeof(struct gve_ptype_lut));
		cmd.get_ptype_map.ptype_map_addr =
			bswap64((uint64_t)(uintptr_t)gdev->ptype_lut_dqo);

		rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
				     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
		uk_pr_info("gve: GET_PTYPE_MAP rc=%d status=0x%x\n",
			   rc, cmd_status);
		if (rc != 0) {
			uk_pr_info("gve: Warning: GET_PTYPE_MAP returned status=0x%x\n",
				   cmd_status);
		}
	}

	/*
	 * Program RSS so the device spreads flows across the RX queues.
	 * Non-fatal: the hardware default already spreads flows, so a
	 * rejected or unimplemented command still leaves a working
	 * multi-queue device. Ticket a9b1936640.
	 */
	rss_rc = gve_configure_rss(gdev);
	if (rss_rc != 0 && rss_rc != -ETIMEDOUT)
		uk_pr_info("gve: Continuing with device default RSS (rc=%d)\n",
			   rss_rc);

	gdev->resources_configured = 1;
	return 0;

fail:
	/*
	 * Release everything this call allocated. State the device
	 * already had stays in place for a later retry.
	 *
	 * A CONFIGURE timeout does not cancel the command. The device
	 * can still write the counter array and the irq db indices
	 * later, and both addresses live in that command. Leak the
	 * arrays and the ptype map on timeout instead of handing the
	 * allocator memory the device may write. The pointers stay
	 * set, so a retry reuses the same arrays. Ticket 6e2054b369.
	 */
	if (cfg_timed_out) {
		uk_pr_err("gve: CONFIGURE_DEVICE_RESOURCES timed out, leaking counter, irq, and ptype buffers\n");
		return rc;
	}
	if (got_ptype) {
		uk_free(drv_alloc, gdev->ptype_lut_dqo);
		gdev->ptype_lut_dqo = NULL;
	}
	if (got_dbs) {
		uk_free(drv_alloc, gdev->irq_db_indices);
		gdev->irq_db_indices = NULL;
	}
	if (got_counters) {
		uk_free(drv_alloc, gdev->counter_array);
		gdev->counter_array = NULL;
	}
	return rc;
}

static int gve_netdev_probe(struct uk_netdev *dev)
{
	struct gve_device *gdev = to_gvedev(dev);

	if (!gdev)
		return -EINVAL;
	return 0;
}

static void gve_netdev_info_get(struct uk_netdev *dev,
				struct uk_netdev_info *dev_info)
{
	struct gve_device *gdev = to_gvedev(dev);
	uint32_t budget;

	if (!dev_info || !gdev)
		return;

	budget = gve_queue_pairs_budget(gdev->max_tx_queues,
					gdev->max_rx_queues,
					gdev->num_event_counters);
	if (budget > GVE_MAX_QUEUES)
		budget = GVE_MAX_QUEUES;

	memset(dev_info, 0, sizeof(*dev_info));
	dev_info->max_rx_queues = (uint8_t)budget;
	dev_info->max_tx_queues = (uint8_t)budget;
	dev_info->max_mtu = gdev->max_mtu ? gdev->max_mtu : gdev->mtu;
	dev_info->nb_encap_tx = 0;
	dev_info->nb_encap_rx = 0;
	dev_info->ioalign = GVE_PKT_BUF_ALIGN;
	dev_info->features = 0;
}

static int gve_netdev_configure(struct uk_netdev *dev,
				const struct uk_netdev_conf *conf)
{
	struct gve_device *gdev = to_gvedev(dev);
	uint32_t budget;
	uint16_t pairs;

	if (!gdev || !conf)
		return -EINVAL;

	/*
	 * The driver always pairs TX queue q with RX queue q and gives
	 * the pair one notification block each, so the two directions
	 * must be configured with the same count. The count is bounded
	 * by the device queue maxima and half the event counters, the
	 * one-pair-per-worker budget. Ticket a9b1936640.
	 */
	if (conf->nb_rx_queues != conf->nb_tx_queues)
		return -EINVAL;
	pairs = conf->nb_rx_queues;

	budget = gve_queue_pairs_budget(gdev->max_tx_queues,
					gdev->max_rx_queues,
					gdev->num_event_counters);
	if (budget > GVE_MAX_QUEUES)
		budget = GVE_MAX_QUEUES;
	if (pairs == 0U || (uint32_t)pairs > budget) {
		uk_pr_err("gve: request for %u queue pairs out of budget %u (max_tx=%u max_rx=%u counters=%u)\n",
			  pairs, budget, gdev->max_tx_queues,
			  gdev->max_rx_queues, gdev->num_event_counters);
		return -EINVAL;
	}

	gdev->queue_pairs = pairs;
	return gve_configure_resources(gdev);
}

static int gve_netdev_txq_info_get(struct uk_netdev *dev, uint16_t queue_id,
				   struct uk_netdev_queue_info *qinfo)
{
	struct gve_device *gdev = to_gvedev(dev);

	if (!gdev || !qinfo ||
	    queue_id >= (gdev->queue_pairs ? gdev->queue_pairs : 1U))
		return -EINVAL;

	memset(qinfo, 0, sizeof(*qinfo));
	qinfo->nb_min = gdev->tx_desc_cnt;
	qinfo->nb_max = gdev->tx_desc_cnt;
	qinfo->nb_align = 1;
	qinfo->nb_is_power_of_two = 1;
	return 0;
}

static int gve_netdev_rxq_info_get(struct uk_netdev *dev, uint16_t queue_id,
				   struct uk_netdev_queue_info *qinfo)
{
	struct gve_device *gdev = to_gvedev(dev);

	if (!gdev || !qinfo ||
	    queue_id >= (gdev->queue_pairs ? gdev->queue_pairs : 1U))
		return -EINVAL;

	memset(qinfo, 0, sizeof(*qinfo));
	qinfo->nb_min = gdev->rx_desc_cnt;
	qinfo->nb_max = gdev->rx_desc_cnt;
	qinfo->nb_align = 1;
	qinfo->nb_is_power_of_two = 1;
	return 0;
}

/*
 * Release one TX queue. A live queue is destroyed on the device
 * first. A half-built queue, whose CREATE_TX_QUEUE never passed, is
 * released with destroy_queue set to 0. A registered QPL is always
 * unregistered before its memory returns.
 */
static void gve_txq_free(struct gve_device *gdev,
			 struct uk_netdev_tx_queue *txq, int destroy_queue)
{
	struct uk_alloc *a;
	int rc = 0;

	if (!txq)
		return;
	a = txq->alloc ? txq->alloc : drv_alloc;
	if (destroy_queue)
		rc |= gve_adminq_destroy_tx_queue(gdev, txq->qid);
	if (txq->qpl_registered)
		rc |= gve_adminq_unregister_page_list(gdev, txq->qpl_id,
						      txq->num_qpl_pages);
	if (rc != 0) {
		/*
		 * The device did not confirm that it released the
		 * queue or the page list. It can still DMA into these
		 * buffers, so freeing them would corrupt whatever the
		 * allocator hands out next. Leak the whole queue on
		 * purpose: the pointers simply go out of scope.
		 * Ticket 888a2b2e1d.
		 */
		uk_pr_err("gve: TX queue qid=%u teardown failed, leaking queue memory\n",
			  txq->qid);
		return;
	}
	if (txq->qpl_page_bus)
		uk_free(a, txq->qpl_page_bus);
	if (txq->qpl_pages)
		uk_free(a, txq->qpl_pages);
	if (txq->bounce_bufs)
		uk_free(a, txq->bounce_bufs);
	if (txq->dqo_compl_ring)
		uk_free(a, txq->dqo_compl_ring);
	if (txq->dqo_tx_ring)
		uk_free(a, txq->dqo_tx_ring);
	if (txq->gqi_tx_ring)
		uk_free(a, txq->gqi_tx_ring);
	if (txq->q_resources)
		uk_free(a, txq->q_resources);
	uk_free(a, txq);
}

static struct uk_netdev_tx_queue *gve_netdev_txq_configure(
	struct uk_netdev *dev, uint16_t queue_id, uint16_t nb_desc,
	struct uk_netdev_txqueue_conf *conf)
{
	struct gve_device *gdev = to_gvedev(dev);
	struct uk_netdev_tx_queue *txq;
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	struct uk_alloc *a;
	size_t bounce_sz;
	int rc;

	if (!gdev || queue_id >= (gdev->queue_pairs ? gdev->queue_pairs : 1U))
		return ERR2PTR(-EINVAL);
	/*
	 * The driver creates the ring at the device-reported size and
	 * advertises that size through txq_info_get. nb_desc 0 selects
	 * the driver default. Anything else breaks the advertised
	 * window, so reject it.
	 */
	if (!gve_ring_request_ok(nb_desc, gdev->tx_desc_cnt))
		return ERR2PTR(-EINVAL);
	a = (conf && conf->a) ? conf->a : drv_alloc;

	rc = gve_configure_resources(gdev);
	if (rc != 0)
		return ERR2PTR(rc);

	/* Reconfiguration: tear the live queue down before recreating. */
	if (gdev->txq[queue_id]) {
		gve_txq_free(gdev, gdev->txq[queue_id], 1);
		gdev->txq[queue_id] = NULL;
	}

	txq = uk_zalloc(a, sizeof(*txq));
	if (!txq)
		return ERR2PTR(-ENOMEM);
	txq->alloc = a;
	txq->gdev = gdev;
	txq->qid = queue_id;
	txq->cnt = gdev->tx_desc_cnt;
	txq->mask = txq->cnt - 1U;
	txq->ntfy_id = (uint32_t)queue_id;

	txq->q_resources = uk_memalign(a, GVE_PAGE_SIZE, GVE_PAGE_SIZE);
	if (!txq->q_resources) {
		rc = -ENOMEM;
		goto err_free;
	}
	memset(txq->q_resources, 0, GVE_PAGE_SIZE);

	if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
		size_t tx_ring_sz = align_up_page(
			(size_t)txq->cnt * sizeof(union gve_tx_desc_dqo));
		size_t comp_ring_sz;

		txq->comp_cnt = gdev->tx_comp_ring_entries ?
				gdev->tx_comp_ring_entries : txq->cnt;
		txq->comp_mask = txq->comp_cnt - 1U;
		comp_ring_sz = align_up_page(
			(size_t)txq->comp_cnt * sizeof(struct gve_tx_compl_desc));

		txq->dqo_tx_ring = uk_memalign(a, GVE_PAGE_SIZE, tx_ring_sz);
		txq->dqo_compl_ring = uk_memalign(a, GVE_PAGE_SIZE, comp_ring_sz);
		bounce_sz = align_up_page((size_t)txq->cnt * GVE_DEFAULT_RX_BUFFER_SIZE);
		txq->bounce_bufs = uk_memalign(a, GVE_PAGE_SIZE, bounce_sz);
		if (!txq->dqo_tx_ring || !txq->dqo_compl_ring || !txq->bounce_bufs) {
			rc = -ENOMEM;
			goto err_free;
		}

		memset(txq->dqo_tx_ring, 0, tx_ring_sz);
		memset(txq->dqo_compl_ring, 0, comp_ring_sz);
		memset(txq->bounce_bufs, 0, bounce_sz);
		txq->dqo_cur_gen_bit = 1;

		gve_adminq_fill_create_tx_queue(
			&cmd, queue_id,
			(uint64_t)(uintptr_t)txq->q_resources,
			(uint64_t)(uintptr_t)txq->dqo_tx_ring,
			GVE_RAW_ADDRESSING_QPL_ID,
			txq->ntfy_id,
			(uint64_t)(uintptr_t)txq->dqo_compl_ring,
			txq->cnt,
			txq->comp_cnt);
	} else {
		size_t tx_ring_sz = align_up_page(
			(size_t)txq->cnt * sizeof(union gve_tx_desc));
		uint32_t qpl_id = GVE_RAW_ADDRESSING_QPL_ID;

		txq->gqi_tx_ring = uk_memalign(a, GVE_PAGE_SIZE, tx_ring_sz);
		if (!txq->gqi_tx_ring) {
			rc = -ENOMEM;
			goto err_free;
		}
		memset(txq->gqi_tx_ring, 0, tx_ring_sz);

		if (gdev->queue_format == GVE_GQI_QPL_FORMAT) {
			txq->qpl_id = queue_id;
			txq->num_qpl_pages = gdev->tx_pages_per_qpl ?
					     gdev->tx_pages_per_qpl : 64U;
			rc = gve_register_qpl(gdev, a, txq->qpl_id,
					      txq->num_qpl_pages,
					      &txq->qpl_pages,
					      &txq->qpl_page_bus);
			if (rc != 0)
				goto err_free;
			txq->qpl_registered = 1;
			qpl_id = txq->qpl_id;
		} else {
			bounce_sz = align_up_page(
				(size_t)txq->cnt * GVE_DEFAULT_RX_BUFFER_SIZE);
			txq->bounce_bufs = uk_memalign(a, GVE_PAGE_SIZE,
						       bounce_sz);
			if (!txq->bounce_bufs) {
				rc = -ENOMEM;
				goto err_free;
			}
			memset(txq->bounce_bufs, 0, bounce_sz);
		}

		gve_adminq_fill_create_tx_queue(
			&cmd, queue_id,
			(uint64_t)(uintptr_t)txq->q_resources,
			(uint64_t)(uintptr_t)txq->gqi_tx_ring,
			qpl_id,
			txq->ntfy_id,
			0,
			txq->cnt,
			0);
	}

	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: CREATE_TX_QUEUE qid=%u fmt=%s rc=%d status=0x%x\n",
		   queue_id, gve_queue_format_name(gdev->queue_format),
		   rc, cmd_status);
	if (rc == -ETIMEDOUT) {
		/*
		 * The CREATE command can still run: the device may
		 * write q_resources and the rings at any time. Leak
		 * the whole queue instead of freeing memory the
		 * device can touch. The QPL pages are already leaked
		 * by gve_register_qpl if that command timed out.
		 * Ticket 6e2054b369.
		 */
		uk_pr_err("gve: CREATE_TX_QUEUE timed out, leaking queue buffers\n");
		return ERR2PTR(rc);
	}
	if (rc != 0)
		goto err_free;

	/*
	 * The device filled q_resources during CREATE. Validate its
	 * doorbell index once here and keep the host-endian value in
	 * txq->db_idx for every later BAR2 write. An index whose
	 * 32-bit word falls beyond BAR2 must fail the queue setup
	 * instead of writing to other MMIO or RAM. The device
	 * allocates sparse indexes, so the window bound is the check;
	 * see gve_doorbell_in_range. Ticket 7af135385d.
	 */
	txq->db_idx = bswap32(txq->q_resources->db_index);
	if (!gve_doorbell_in_range(txq->db_idx, gdev->bar2_size)) {
		printf("GATE_FAIL: gve TX doorbell index %u out of range (bar2_size=%llu)\n",
		       txq->db_idx, (unsigned long long)gdev->bar2_size);
		rc = -EIO;
		goto err_destroy;
	}
	uk_pr_info("gve: TX%u doorbell index %u accepted (bar2_size=%llu)\n",
		   queue_id, txq->db_idx,
		   (unsigned long long)gdev->bar2_size);

	gdev->txq[queue_id] = txq;
	return txq;

err_destroy:
	/* CREATE succeeded, so the device holds the queue: destroy it. */
	gve_txq_free(gdev, txq, 1);
	return ERR2PTR(rc);

err_free:
	/* The queue never started: release the partial build only. */
	gve_txq_free(gdev, txq, 0);
	return ERR2PTR(rc);
}

/*
 * Release one RX queue. Mirrors gve_txq_free: destroy the live queue
 * on the device first, unregister a registered QPL, then free the
 * memory through the queue allocator.
 */
static void gve_rxq_free(struct gve_device *gdev,
			 struct uk_netdev_rx_queue *rxq, int destroy_queue)
{
	struct uk_alloc *a;
	int rc = 0;

	if (!rxq)
		return;
	a = rxq->alloc ? rxq->alloc : drv_alloc;
	if (destroy_queue)
		rc |= gve_adminq_destroy_rx_queue(gdev, rxq->qid);
	if (rxq->qpl_registered)
		rc |= gve_adminq_unregister_page_list(gdev, rxq->qpl_id,
						      rxq->num_qpl_pages);
	if (rc != 0) {
		/*
		 * Same rule as gve_txq_free: the device may still DMA
		 * into these buffers after a failed teardown, so leak
		 * the queue on purpose instead of freeing live DMA
		 * memory. Ticket 888a2b2e1d.
		 */
		uk_pr_err("gve: RX queue qid=%u teardown failed, leaking queue memory\n",
			  rxq->qid);
		return;
	}
	if (rxq->qpl_page_bus)
		uk_free(a, rxq->qpl_page_bus);
	if (rxq->qpl_pages)
		uk_free(a, rxq->qpl_pages);
	if (rxq->dqo_rx_bufs)
		uk_free(a, rxq->dqo_rx_bufs);
	if (rxq->dqo_complq_ring)
		uk_free(a, rxq->dqo_complq_ring);
	if (rxq->dqo_bufq_ring)
		uk_free(a, rxq->dqo_bufq_ring);
	if (rxq->gqi_data_ring)
		uk_free(a, rxq->gqi_data_ring);
	if (rxq->gqi_desc_ring)
		uk_free(a, rxq->gqi_desc_ring);
	if (rxq->q_resources)
		uk_free(a, rxq->q_resources);
	uk_free(a, rxq);
}

static struct uk_netdev_rx_queue *gve_netdev_rxq_configure(
	struct uk_netdev *dev, uint16_t queue_id, uint16_t nb_desc,
	struct uk_netdev_rxqueue_conf *conf)
{
	struct gve_device *gdev = to_gvedev(dev);
	struct uk_netdev_rx_queue *rxq;
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	struct uk_alloc *a;
	uint32_t i;
	int rc;

	if (!gdev || !conf || !conf->alloc_rxpkts ||
	    queue_id >= (gdev->queue_pairs ? gdev->queue_pairs : 1U))
		return ERR2PTR(-EINVAL);
	/* Same policy as the TX queue: device size or driver default. */
	if (!gve_ring_request_ok(nb_desc, gdev->rx_desc_cnt))
		return ERR2PTR(-EINVAL);
	a = (conf->a) ? conf->a : drv_alloc;

	rc = gve_configure_resources(gdev);
	if (rc != 0)
		return ERR2PTR(rc);

	/* Reconfiguration: tear the live queue down before recreating. */
	if (gdev->rxq[queue_id]) {
		gve_rxq_free(gdev, gdev->rxq[queue_id], 1);
		gdev->rxq[queue_id] = NULL;
	}

	rxq = uk_zalloc(a, sizeof(*rxq));
	if (!rxq)
		return ERR2PTR(-ENOMEM);
	rxq->alloc = a;
	rxq->gdev = gdev;
	rxq->qid = queue_id;
	rxq->cnt = gdev->rx_desc_cnt;
	rxq->mask = rxq->cnt - 1U;
	rxq->ntfy_id = (gdev->num_ntfy_blks / 2U) + (uint32_t)queue_id;
	rxq->alloc_rxpkts = conf->alloc_rxpkts;
	rxq->alloc_rxpkts_argp = conf->alloc_rxpkts_argp;

	rxq->q_resources = uk_memalign(a, GVE_PAGE_SIZE, GVE_PAGE_SIZE);
	if (!rxq->q_resources) {
		rc = -ENOMEM;
		goto err_free;
	}
	memset(rxq->q_resources, 0, GVE_PAGE_SIZE);

	if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
		size_t bufq_sz, complq_sz, bufs_sz;

		rxq->buf_cnt = gdev->rx_buff_ring_entries ?
			       gdev->rx_buff_ring_entries : rxq->cnt;
		rxq->buf_mask = rxq->buf_cnt - 1U;

		bufq_sz = align_up_page(
			(size_t)rxq->buf_cnt * sizeof(struct gve_rx_desc_dqo));
		complq_sz = align_up_page(
			(size_t)rxq->cnt * sizeof(struct gve_rx_compl_desc_dqo));
		bufs_sz = align_up_page(
			(size_t)rxq->buf_cnt * GVE_DEFAULT_RX_BUFFER_SIZE);

		rxq->dqo_bufq_ring = uk_memalign(a, GVE_PAGE_SIZE, bufq_sz);
		rxq->dqo_complq_ring = uk_memalign(a, GVE_PAGE_SIZE, complq_sz);
		rxq->dqo_rx_bufs = uk_memalign(a, GVE_PAGE_SIZE, bufs_sz);
		if (!rxq->dqo_bufq_ring || !rxq->dqo_complq_ring || !rxq->dqo_rx_bufs) {
			rc = -ENOMEM;
			goto err_free;
		}

		memset(rxq->dqo_bufq_ring, 0, bufq_sz);
		memset(rxq->dqo_complq_ring, 0, complq_sz);
		memset(rxq->dqo_rx_bufs, 0, bufs_sz);
		rxq->dqo_cur_gen_bit = 1;

		for (i = 0; i < (uint32_t)(rxq->buf_cnt - 1U); i++) {
			uint64_t baddr = (uint64_t)(uintptr_t)(
				rxq->dqo_rx_bufs + (size_t)i * GVE_DEFAULT_RX_BUFFER_SIZE);

			rxq->dqo_bufq_ring[i].buf_id = (uint16_t)i;
			rxq->dqo_bufq_ring[i].buf_addr = baddr;
			rxq->dqo_bufq_ring[i].header_buf_addr = 0;
		}
		rxq->dqo_bufq_tail = (uint32_t)(rxq->buf_cnt - 1U);

		gve_adminq_fill_create_rx_queue(
			&cmd, queue_id, 0, rxq->ntfy_id,
			(uint64_t)(uintptr_t)rxq->q_resources,
			(uint64_t)(uintptr_t)rxq->dqo_complq_ring,
			(uint64_t)(uintptr_t)rxq->dqo_bufq_ring,
			GVE_RAW_ADDRESSING_QPL_ID,
			rxq->cnt,
			GVE_DEFAULT_RX_BUFFER_SIZE,
			rxq->buf_cnt);
	} else {
		size_t desc_sz = align_up_page(
			(size_t)rxq->cnt * sizeof(struct gve_rx_desc));
		size_t data_sz = align_up_page(
			(size_t)rxq->cnt * sizeof(union gve_rx_data_slot));
		uint32_t qpl_id = GVE_RAW_ADDRESSING_QPL_ID;

		rxq->gqi_desc_ring = uk_memalign(a, GVE_PAGE_SIZE, desc_sz);
		rxq->gqi_data_ring = uk_memalign(a, GVE_PAGE_SIZE, data_sz);
		if (!rxq->gqi_desc_ring || !rxq->gqi_data_ring) {
			rc = -ENOMEM;
			goto err_free;
		}

		memset(rxq->gqi_desc_ring, 0, desc_sz);
		memset(rxq->gqi_data_ring, 0, data_sz);
		rxq->gqi_seqno = 1;

		rxq->num_qpl_pages = rxq->cnt;
		if (gdev->queue_format == GVE_GQI_QPL_FORMAT) {
			rxq->qpl_id = gdev->max_tx_queues + queue_id;
			rc = gve_register_qpl(gdev, a, rxq->qpl_id,
					      rxq->num_qpl_pages,
					      &rxq->qpl_pages,
					      &rxq->qpl_page_bus);
			if (rc != 0)
				goto err_free;
			rxq->qpl_registered = 1;
			qpl_id = rxq->qpl_id;

			for (i = 0; i < rxq->cnt; i++) {
				rxq->gqi_data_ring[i].qpl_offset =
					bswap64((uint64_t)i * GVE_PAGE_SIZE);
			}
		} else {
			rxq->qpl_pages = uk_memalign(
				a, GVE_PAGE_SIZE,
				(size_t)rxq->num_qpl_pages * GVE_PAGE_SIZE);
			if (!rxq->qpl_pages) {
				rc = -ENOMEM;
				goto err_free;
			}
			memset(rxq->qpl_pages, 0,
			       (size_t)rxq->num_qpl_pages * GVE_PAGE_SIZE);

			for (i = 0; i < rxq->cnt; i++) {
				uint64_t paddr = (uint64_t)(uintptr_t)(
					rxq->qpl_pages + (size_t)i * GVE_PAGE_SIZE);

				rxq->gqi_data_ring[i].addr = bswap64(paddr);
			}
		}
		rxq->gqi_fill_cnt = rxq->cnt;

		gve_adminq_fill_create_rx_queue(
			&cmd, queue_id, queue_id, rxq->ntfy_id,
			(uint64_t)(uintptr_t)rxq->q_resources,
			(uint64_t)(uintptr_t)rxq->gqi_desc_ring,
			(uint64_t)(uintptr_t)rxq->gqi_data_ring,
			qpl_id,
			rxq->cnt,
			GVE_DEFAULT_RX_BUFFER_SIZE,
			0);
	}

	rc = gve_adminq_exec(gdev->reg_bar0, gdev->adminq,
			     &gdev->adminq_prod_cnt, &cmd, &cmd_status);
	uk_pr_info("gve: CREATE_RX_QUEUE qid=%u fmt=%s rc=%d status=0x%x\n",
		   queue_id, gve_queue_format_name(gdev->queue_format),
		   rc, cmd_status);
	if (rc == -ETIMEDOUT) {
		/* Same rule as the TX path. Ticket 6e2054b369. */
		uk_pr_err("gve: CREATE_RX_QUEUE timed out, leaking queue buffers\n");
		return ERR2PTR(rc);
	}
	if (rc != 0)
		goto err_free;

	/*
	 * Same rule as the TX path: validate the device-reported RX
	 * doorbell index once and store it for the datapath. Ticket
	 * 7af135385d.
	 */
	rxq->db_idx = bswap32(rxq->q_resources->db_index);
	if (!gve_doorbell_in_range(rxq->db_idx, gdev->bar2_size)) {
		printf("GATE_FAIL: gve RX doorbell index %u out of range (bar2_size=%llu)\n",
		       rxq->db_idx, (unsigned long long)gdev->bar2_size);
		rc = -EIO;
		goto err_destroy;
	}
	uk_pr_info("gve: RX%u doorbell index %u accepted (bar2_size=%llu)\n",
		   queue_id, rxq->db_idx, (unsigned long long)gdev->bar2_size);

	gdev->rxq[queue_id] = rxq;
	return rxq;

err_destroy:
	/* CREATE succeeded, so the device holds the queue: destroy it. */
	gve_rxq_free(gdev, rxq, 1);
	return ERR2PTR(rc);

err_free:
	/* The queue never started: release the partial build only. */
	gve_rxq_free(gdev, rxq, 0);
	return ERR2PTR(rc);
}

static int gve_netdev_start(struct uk_netdev *dev)
{
	struct gve_device *gdev = to_gvedev(dev);
	uint32_t tx_db_idx, tx_cnt_idx;
	uint32_t rx_db_idx, rx_cnt_idx;
	uint32_t dev_status;
	uint16_t q;
	__nsec deadline;
	int link_up = 0;

	if (!gdev || gdev->queue_pairs == 0U)
		return -EINVAL;

	/*
	 * Every configured pair must have both halves. Catch a partial
	 * setup here, before any doorbell write.
	 */
	for (q = 0U; q < gdev->queue_pairs; q++) {
		if (!gdev->txq[q] || !gdev->rxq[q])
			return -EINVAL;
	}

	__asm__ __volatile__("mfence" ::: "memory");

	for (q = 0U; q < gdev->queue_pairs; q++) {
		tx_db_idx = gdev->txq[q]->db_idx;
		tx_cnt_idx = bswap32(gdev->txq[q]->q_resources->counter_index);
		rx_db_idx = gdev->rxq[q]->db_idx;
		rx_cnt_idx = bswap32(gdev->rxq[q]->q_resources->counter_index);

		/*
		 * Defense in depth: the queue configure path already
		 * checked both indices, so this only catches a queue
		 * that reached start without that check. The first RX
		 * doorbell write below must never leave the BAR2
		 * window. Ticket 7af135385d.
		 */
		if (!gve_doorbell_in_range(tx_db_idx, gdev->bar2_size) ||
		    !gve_doorbell_in_range(rx_db_idx, gdev->bar2_size)) {
			printf("GATE_FAIL: gve doorbell index out of range at start: q=%u tx=%u rx=%u bar2_size=%llu\n",
			       q, tx_db_idx, rx_db_idx,
			       (unsigned long long)gdev->bar2_size);
			return -EIO;
		}

		/* Post this RX queue's initial buffers via BAR2 doorbell. */
		if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
			mmio_write32(&gdev->db_bar2[rx_db_idx],
				     gdev->rxq[q]->dqo_bufq_tail);
			/*
			 * The driver never arms the interrupt translation
			 * rings. It enables no MSI-X vector and receives
			 * every packet by polling, so an ITR write would
			 * only let the device raise interrupts nobody
			 * handles. The irq door block array stays
			 * allocated because CONFIGURE_DEVICE_RESOURCES
			 * requires it.
			 */
		} else {
			mmio_write32(&gdev->db_bar2[rx_db_idx],
				     bswap32(gdev->rxq[q]->gqi_fill_cnt));
		}

		uk_pr_info("gve: Queue resources ready: TX%u(db=%u, cnt=%u) RX%u(db=%u, cnt=%u)\n",
			   q, tx_db_idx, tx_cnt_idx, q, rx_db_idx, rx_cnt_idx);
	}

	uk_pr_info("gve: %u queue pair(s) ready, format=%s\n",
		   gdev->queue_pairs, gve_queue_format_name(gdev->queue_format));

	/*
	 * Do not print the gate marker for a link-down device. Poll
	 * the BAR0 link bit for a short bounded time: a freshly
	 * created GCP instance reports the link up within
	 * milliseconds. If the bit stays clear, fail the gate with
	 * the register value in the log so the serial console shows
	 * why.
	 */
	deadline = ukplat_monotonic_clock() +
		   ukarch_time_msec_to_nsec(GVE_LINK_TIMEOUT_MSEC);
	for (;;) {
		dev_status = bswap32(mmio_read32(&gdev->reg_bar0->device_status));
		if (gve_link_is_up(dev_status)) {
			link_up = 1;
			break;
		}
		if (ukplat_monotonic_clock() >= deadline)
			break;
		__asm__ __volatile__("pause" ::: "memory");
	}
	if (!link_up) {
		printf("GATE_FAIL: gve link is down, device_status=0x%08x\n",
		       dev_status);
		return -ENOLINK;
	}

	printf("========================================\n");
	printf("GATE_GVE_PASS\n");
	printf("========================================\n");

	return 0;
}

static const struct uk_hwaddr *gve_netdev_hwaddr_get(struct uk_netdev *dev)
{
	struct gve_device *gdev = to_gvedev(dev);

	if (!gdev)
		return NULL;
	return &gdev->hw_addr;
}

static uint16_t gve_netdev_mtu_get(struct uk_netdev *dev)
{
	struct gve_device *gdev = to_gvedev(dev);

	if (!gdev)
		return 0;
	return gdev->mtu;
}

static unsigned int gve_netdev_promiscuous_get(struct uk_netdev *dev __unused)
{
	return 0;
}

/*
 * Consume DQO TX completions. A DESC-type completion reports the
 * device TX head in tx_head: the slot after the last descriptor the
 * device fetched. The driver moves dqo_head to that slot and
 * reclaims the entries below it. The driver consumes other
 * completion types without reclaim. The step cap bounds one call
 * even if the device writes bogus generations.
 */
static void gve_poll_dqo_tx_completions(struct uk_netdev_tx_queue *queue)
{
	uint32_t steps;

	for (steps = 0; steps < (uint32_t)queue->comp_cnt; steps++) {
		struct gve_tx_compl_desc *cdesc =
			&queue->dqo_compl_ring[queue->dqo_complq_head];
		uint8_t gen;

		__asm__ __volatile__("" ::: "memory");
		gen = cdesc->generation;
		if (gen != queue->dqo_cur_gen_bit)
			break;

		__asm__ __volatile__("lfence" ::: "memory");
		if (cdesc->type == GVE_COMPL_TYPE_DQO_DESC)
			queue->dqo_head = gve_ring_advance_head(
				queue->dqo_head, queue->dqo_tail,
				queue->cnt, cdesc->tx_head);

		queue->dqo_complq_head =
			(queue->dqo_complq_head + 1U) & queue->comp_mask;
		if (queue->dqo_complq_head == 0)
			queue->dqo_cur_gen_bit ^= 1U;
	}
}

/*
 * Update gqi_done from the device TX event counter. The device keeps
 * the counter in counter_array as a big-endian count of completed
 * TX packets for this queue.
 */
static void gve_gqi_poll_tx_done(struct uk_netdev_tx_queue *queue)
{
	struct gve_device *gdev = queue->gdev;
	uint32_t idx = bswap32(queue->q_resources->counter_index);
	uint32_t done;

	if (!gdev->counter_array ||
	    idx >= (uint32_t)gdev->num_event_counters)
		return;

	done = bswap32(gdev->counter_array[idx]);
	__asm__ __volatile__("" ::: "memory");
	if ((int32_t)(done - queue->gqi_done) > 0)
		queue->gqi_done = done;
}

static int gve_netdev_xmit(struct uk_netdev *dev,
			   struct uk_netdev_tx_queue *queue,
			   struct uk_netbuf *pkt)
{
	struct gve_device *gdev = to_gvedev(dev);
	struct uk_netbuf *iter;
	struct gve_chain_acc acc;
	uint32_t tot_len;
	uint32_t padded_len;
	uint32_t tx_db_idx;

	if (!gdev || !queue || !pkt)
		return -EINVAL;

	/*
	 * Sum the chain in uint32 space. Reject any segment or total
	 * above the device buffer size. The caller keeps ownership of
	 * a rejected chain, so do not free the packet here.
	 */
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	for (iter = pkt; iter != NULL; iter = iter->next) {
		if (gve_chain_acc_add(&acc, iter->len) != 0)
			return -EMSGSIZE;
	}
	tot_len = acc.total;
	padded_len = gve_tx_pad_len(tot_len);

	tx_db_idx = queue->db_idx;	/* validated at queue configure */

	if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
		uint32_t ctx_slot;
		uint32_t pkt_slot;
		uint8_t *dst;
		uint32_t off = 0;

		/*
		 * Reclaim completed slots first. Refuse the send while
		 * the two target slots or the cache-overlap gap are
		 * not free. Writing an entry the device still fetches
		 * would let it read a mixed descriptor. The capacity
		 * bound of cnt - 1 matches num_avail_tx_slots() in
		 * the Linux gve driver.
		 */
		gve_poll_dqo_tx_completions(queue);
		if (!gve_ring_has_space(queue->dqo_head, queue->dqo_tail,
					GVE_TX_DQO_WRITE_RESERVE,
					(uint32_t)queue->cnt - 1U))
			return -ENOSPC;

		ctx_slot = queue->dqo_tail & queue->mask;
		pkt_slot = (queue->dqo_tail + 1U) & queue->mask;
		dst = queue->bounce_bufs +
		      (size_t)pkt_slot * GVE_DEFAULT_RX_BUFFER_SIZE;

		for (iter = pkt; iter != NULL; iter = iter->next) {
			memcpy(dst + off, iter->data, iter->len);
			off += iter->len;
		}

		if (padded_len != tot_len)
			memset(dst + tot_len, 0, padded_len - tot_len);

		gve_dqo_fill_tx_general_ctx_desc(
			&queue->dqo_tx_ring[ctx_slot].general_ctx);
		gve_dqo_fill_tx_pkt_desc(&queue->dqo_tx_ring[pkt_slot].pkt,
					 (uint64_t)(uintptr_t)dst,
					 (uint16_t)pkt_slot,
					 (uint16_t)padded_len);
		__asm__ __volatile__("mfence" ::: "memory");

		queue->dqo_tail += 2U;
		mmio_write32(&gdev->db_bar2[tx_db_idx],
			     queue->dqo_tail & queue->mask);
	} else {
		uint32_t slot;
		uint32_t capacity;
		uint8_t *dst;
		uint64_t addr;
		uint32_t off = 0;

		/*
		 * GQI posts one descriptor per packet. A QPL page can
		 * back several ring slots, so cap the window by the
		 * page count too. That stops the driver from overwriting
		 * a page that the device still sends.
		 */
		capacity = gve_gqi_tx_capacity((uint32_t)queue->cnt,
					       queue->num_qpl_pages,
					       gdev->queue_format ==
					       GVE_GQI_QPL_FORMAT);

		gve_gqi_poll_tx_done(queue);
		if (!gve_ring_has_space(queue->gqi_done, queue->gqi_req,
					1U, capacity))
			return -ENOSPC;

		slot = queue->gqi_req & queue->mask;
		if (gdev->queue_format == GVE_GQI_QPL_FORMAT) {
			uint32_t page_idx = slot % queue->num_qpl_pages;

			dst = queue->qpl_pages + (size_t)page_idx * GVE_PAGE_SIZE;
			addr = (uint64_t)page_idx * GVE_PAGE_SIZE;
		} else {
			dst = queue->bounce_bufs +
			      (size_t)slot * GVE_DEFAULT_RX_BUFFER_SIZE;
			addr = (uint64_t)(uintptr_t)dst;
		}

		for (iter = pkt; iter != NULL; iter = iter->next) {
			memcpy(dst + off, iter->data, iter->len);
			off += iter->len;
		}

		if (padded_len != tot_len)
			memset(dst + tot_len, 0, padded_len - tot_len);

		gve_gqi_fill_tx_pkt_desc(&queue->gqi_tx_ring[slot], addr,
					 (uint16_t)padded_len);
		__asm__ __volatile__("mfence" ::: "memory");

		queue->gqi_req++;
		mmio_write32(&gdev->db_bar2[tx_db_idx],
			     bswap32(queue->gqi_req));
	}

	uk_netbuf_free(pkt);
	return UK_NETDEV_STATUS_SUCCESS;
}

static int gve_netdev_recv(struct uk_netdev *dev,
			   struct uk_netdev_rx_queue *queue,
			   struct uk_netbuf **pkt)
{
	struct gve_device *gdev = to_gvedev(dev);
	struct uk_netbuf *nb = NULL;
	uint32_t rx_db_idx;
	uint16_t alloc_cnt;

	if (!gdev || !queue || !pkt)
		return -EINVAL;

	*pkt = NULL;
	rx_db_idx = queue->db_idx;	/* validated at queue configure */

	if (gdev->queue_format == GVE_DQO_RDA_FORMAT) {
		struct gve_rx_compl_desc_dqo *cdesc =
			&queue->dqo_complq_ring[queue->dqo_complq_head];
		uint16_t pkt_len;
		uint16_t buf_id;
		uint8_t rx_err;
		uint8_t *src;
		uint32_t bslot;

		__asm__ __volatile__("" ::: "memory");
		if (cdesc->generation != queue->dqo_cur_gen_bit)
			return 0;

		__asm__ __volatile__("lfence" ::: "memory");
		pkt_len = cdesc->packet_len;
		buf_id = cdesc->buf_id;
		rx_err = cdesc->rx_error;

		queue->dqo_complq_head =
			(queue->dqo_complq_head + 1U) & queue->mask;
		if (queue->dqo_complq_head == 0)
			queue->dqo_cur_gen_bit ^= 1U;

		if (!rx_err && pkt_len > 0 &&
		    pkt_len <= GVE_DEFAULT_RX_BUFFER_SIZE &&
		    buf_id < queue->buf_cnt) {
			alloc_cnt = queue->alloc_rxpkts(
				queue->alloc_rxpkts_argp, &nb, 1);
			if (alloc_cnt >= 1 && nb != NULL) {
				if (gve_rx_fits_netbuf(
					    (uint32_t)pkt_len,
					    (uint32_t)(nb->buflen -
						       uk_netbuf_headroom(nb)))) {
					src = queue->dqo_rx_bufs +
					      (size_t)buf_id * GVE_DEFAULT_RX_BUFFER_SIZE;
					memcpy(nb->data, src, pkt_len);
					nb->len = pkt_len;
					*pkt = nb;
				} else {
					/*
					 * The frame exceeds the usable
					 * data area. Drop it. The buffer
					 * still returns to the ring below.
					 */
					uk_netbuf_free(nb);
				}
			}
		}

		/* Repost buffer to DQO bufq ring */
		if (buf_id < queue->buf_cnt) {
			bslot = queue->dqo_bufq_tail;
			queue->dqo_bufq_ring[bslot].buf_id = buf_id;
			queue->dqo_bufq_ring[bslot].buf_addr =
				(uint64_t)(uintptr_t)(queue->dqo_rx_bufs +
				(size_t)buf_id * GVE_DEFAULT_RX_BUFFER_SIZE);
			queue->dqo_bufq_ring[bslot].header_buf_addr = 0;
			__asm__ __volatile__("mfence" ::: "memory");

			queue->dqo_bufq_tail =
				(queue->dqo_bufq_tail + 1U) & queue->buf_mask;
			queue->dqo_refill_pending++;
			if (gve_rx_refill_should_ring(
				    queue->dqo_refill_pending,
				    GVE_RX_BUF_THRESH_DQO)) {
				mmio_write32(&gdev->db_bar2[rx_db_idx],
					     queue->dqo_bufq_tail);
				queue->dqo_refill_pending = 0;
			}
		}

		/*
		 * Flush reposts that the threshold did not cover. The
		 * driver never returns to the stack with an unnotified
		 * buffer, so the device cannot stall on a hidden buffer.
		 */
		if (queue->dqo_refill_pending != 0U) {
			mmio_write32(&gdev->db_bar2[rx_db_idx],
				     queue->dqo_bufq_tail);
			queue->dqo_refill_pending = 0;
		}

		if (*pkt == NULL)
			return 0;
		return UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_MORE;
	} else {
		uint32_t slot = queue->gqi_cnt & queue->mask;
		struct gve_rx_desc *rdesc = &queue->gqi_desc_ring[slot];
		uint16_t flags_seq;
		uint16_t raw_len, pkt_len;
		uint8_t *src;

		__asm__ __volatile__("" ::: "memory");
		flags_seq = rdesc->flags_seq;
		if (GVE_SEQNO(flags_seq) != queue->gqi_seqno)
			return 0;

		__asm__ __volatile__("lfence" ::: "memory");
		raw_len = bswap16(rdesc->len);
		if (!(GVE_RXFLG(flags_seq) & GVE_RXF_ERR) &&
		    raw_len > GVE_RX_PAD &&
		    raw_len <= GVE_DEFAULT_RX_BUFFER_SIZE) {
			pkt_len = raw_len - GVE_RX_PAD;
			alloc_cnt = queue->alloc_rxpkts(
				queue->alloc_rxpkts_argp, &nb, 1);
			if (alloc_cnt >= 1 && nb != NULL) {
				if (gve_rx_fits_netbuf(
					    (uint32_t)pkt_len,
					    (uint32_t)(nb->buflen -
						       uk_netbuf_headroom(nb)))) {
					src = queue->qpl_pages +
					      (size_t)slot * GVE_PAGE_SIZE + GVE_RX_PAD;
					memcpy(nb->data, src, pkt_len);
					nb->len = pkt_len;
					*pkt = nb;
				} else {
					/* Frame exceeds tailroom. Drop it. */
					uk_netbuf_free(nb);
				}
			}
		}

		queue->gqi_cnt++;
		queue->gqi_seqno = gve_next_seqno(queue->gqi_seqno);
		queue->gqi_fill_cnt++;
		mmio_write32(&gdev->db_bar2[rx_db_idx],
			     bswap32(queue->gqi_fill_cnt));

		if (*pkt == NULL)
			return 0;
		return UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_MORE;
	}
}

static const struct uk_netdev_ops gve_netdev_ops = {
	.probe = gve_netdev_probe,
	.info_get = gve_netdev_info_get,
	.configure = gve_netdev_configure,
	.txq_info_get = gve_netdev_txq_info_get,
	.rxq_info_get = gve_netdev_rxq_info_get,
	.txq_configure = gve_netdev_txq_configure,
	.rxq_configure = gve_netdev_rxq_configure,
	.start = gve_netdev_start,
	.hwaddr_get = gve_netdev_hwaddr_get,
	.mtu_get = gve_netdev_mtu_get,
	.promiscuous_get = gve_netdev_promiscuous_get,
};

static int gve_add_dev(struct pci_device *pdev)
{
	struct gve_device *gdev;
	uint8_t bus, dev, func, rev;
	uint32_t cmd_reg;
	uint64_t bar0_paddr, bar2_paddr;
	volatile struct gve_registers *reg_bar0;
	volatile uint32_t *db_bar2;
	uint32_t dev_status, max_tx_q, max_rx_q;
	const char *ver_str = "GVE-1.0.0\n";
	const char *p;
	union gve_adminq_command *adminq = NULL;
	uint64_t adminq_paddr;
	struct gve_driver_info *drv_info = NULL;
	uint8_t *desc_buf = NULL;
	struct gve_device_descriptor *desc;
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	uint16_t num_opts, total_len, i;
	uint32_t opt_off, desc_region;
	int mapped_bar0 = 0;
	int mapped_bar2 = 0;
	int has_gqi_raw = 0;
	int cmd_may_run = 0;	/* a timeout may have left a command pending */
	int rc;

	if (!pdev)
		return -EINVAL;

	gdev = uk_zalloc(drv_alloc, sizeof(*gdev));
	if (!gdev)
		return -ENOMEM;

	gdev->pdev = pdev;
	bus = pdev->addr.bus;
	dev = pdev->addr.devid;
	func = pdev->addr.function;

	/*
	 * Default to a single queue pair. gve_netdev_configure normally
	 * overrides this. The default keeps start and queue configure
	 * correct for any caller that does not go through configure.
	 * Ticket a9b1936640.
	 */
	gdev->queue_pairs = 1U;

	uk_pr_info("gve: Probing gVNIC PCI device %04x:%02x:%02x.%x (1ae0:0042)\n",
		   pdev->addr.domain, bus, dev, func);

	/* Enable PCI memory space and bus mastering */
	cmd_reg = uk_pci_read_config32(&pdev->addr, PCI_COMMAND);
	cmd_reg |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	uk_pci_write_config32(&pdev->addr, PCI_COMMAND, cmd_reg);

	rev = (uint8_t)(uk_pci_read_config32(&pdev->addr,
					     PCI_CLASS_REVISION) & 0xffU);
	bar0_paddr = uk_pci_read_bar(&pdev->addr, PCI_BASE_ADDRESS_0);
	bar2_paddr = uk_pci_read_bar(&pdev->addr, PCI_BASE_ADDRESS_2);
	gdev->bar0_size = gve_bar_size(pdev, PCI_BASE_ADDRESS_0);
	gdev->bar2_size = gve_bar_size(pdev, PCI_BASE_ADDRESS_2);

	uk_pr_info("gve: PCI Revision: 0x%02x, BAR0 paddr: 0x%llx size: %llu, BAR2 paddr: 0x%llx size: %llu\n",
		   rev, (unsigned long long)bar0_paddr,
		   (unsigned long long)gdev->bar0_size,
		   (unsigned long long)bar2_paddr,
		   (unsigned long long)gdev->bar2_size);

	/*
	 * gve_map_bar maps one 1 GiB-aligned window per slot, so each
	 * BAR must sit fully inside one window. BAR0 must also cover
	 * the whole register layout, or a short BAR would let register
	 * reads run past the device memory. Refuse the device
	 * otherwise. Ticket ad4bd33ab8.
	 */
	if (!gve_bar_fits_window(bar0_paddr, gdev->bar0_size) ||
	    gdev->bar0_size < sizeof(struct gve_registers)) {
		printf("GATE_FAIL: gve BAR0 unusable paddr=0x%llx size=%llu\n",
		       (unsigned long long)bar0_paddr,
		       (unsigned long long)gdev->bar0_size);
		rc = -ENODEV;
		goto err;
	}
	if (!gve_bar_fits_window(bar2_paddr, gdev->bar2_size)) {
		printf("GATE_FAIL: gve BAR2 unusable paddr=0x%llx size=%llu\n",
		       (unsigned long long)bar2_paddr,
		       (unsigned long long)gdev->bar2_size);
		rc = -ENODEV;
		goto err;
	}

	reg_bar0 = (volatile struct gve_registers *)gve_map_bar(bar0_paddr, 0);
	if (!reg_bar0) {
		printf("GATE_FAIL: gve BAR0 mapping failed\n");
		rc = -ENOMEM;
		goto err;
	}
	mapped_bar0 = 1;
	db_bar2 = (volatile uint32_t *)gve_map_bar(bar2_paddr, 1);
	if (!db_bar2) {
		printf("GATE_FAIL: gve BAR2 mapping failed\n");
		rc = -ENOMEM;
		goto err;
	}
	mapped_bar2 = 1;
	gdev->reg_bar0 = reg_bar0;
	gdev->db_bar2 = db_bar2;

	uk_pr_info("gve: Mapped BAR0 vaddr=%p, BAR2 vaddr=%p\n",
		   (const void *)reg_bar0, (const void *)db_bar2);

	dev_status = bswap32(mmio_read32(&reg_bar0->device_status));
	max_tx_q = bswap32(mmio_read32(&reg_bar0->max_tx_queues));
	max_rx_q = bswap32(mmio_read32(&reg_bar0->max_rx_queues));
	gdev->max_tx_queues = max_tx_q;
	gdev->max_rx_queues = max_rx_q;
	uk_pr_info("gve: BAR0 device_status=0x%08x max_tx_queues=%u max_rx_queues=%u\n",
		   dev_status, max_tx_q, max_rx_q);

	/* Write driver version string byte-by-byte to BAR0 offset 0x1f */
	for (p = ver_str; *p; p++)
		mmio_write8(&reg_bar0->driver_version, (uint8_t)*p);

	/* Allocate 4096-byte aligned Admin Queue page */
	adminq = uk_memalign(drv_alloc, GVE_PAGE_SIZE, GVE_PAGE_SIZE);
	if (!adminq) {
		printf("GATE_FAIL: gve adminq allocation failed\n");
		rc = -ENOMEM;
		goto err;
	}
	memset(adminq, 0, GVE_PAGE_SIZE);
	gdev->adminq = adminq;
	adminq_paddr = (uint64_t)(uintptr_t)adminq;

	if (rev < 0x1) {
		mmio_write32(&reg_bar0->adminq_pfn,
			     bswap32((uint32_t)(adminq_paddr >> 12)));
	} else {
		mmio_write16(&reg_bar0->adminq_length, bswap16(GVE_PAGE_SIZE));
		mmio_write32(&reg_bar0->adminq_base_address_hi,
			     bswap32((uint32_t)(adminq_paddr >> 32)));
		mmio_write32(&reg_bar0->adminq_base_address_lo,
			     bswap32((uint32_t)(adminq_paddr & 0xffffffffU)));
		mmio_write32(&reg_bar0->driver_status,
			     bswap32(GVE_DRIVER_STATUS_RUN_MASK));
	}
	uk_pr_info("gve: Admin Queue initialized at paddr=0x%llx\n",
		   (unsigned long long)adminq_paddr);

	/* Issue VERIFY_DRIVER_COMPATIBILITY (0xF) advertising all queue formats */
	drv_info = uk_memalign(drv_alloc, GVE_PAGE_SIZE, GVE_PAGE_SIZE);
	if (!drv_info) {
		printf("GATE_FAIL: gve driver_info allocation failed\n");
		rc = -ENOMEM;
		goto err;
	}
	memset(drv_info, 0, GVE_PAGE_SIZE);
	drv_info->os_type = 1; /* Linux */
	drv_info->driver_major = 1;
	drv_info->driver_minor = 0;
	drv_info->driver_sub = 0;
	drv_info->os_version_major = bswap32(6);
	drv_info->os_version_minor = bswap32(8);
	drv_info->os_version_sub = bswap32(0);
	/*
	 * Advertise the queue formats this driver can run as
	 * big-endian capability bits. The whole flag word is
	 * big-endian on the wire. DQO_QPL stays out: the datapath
	 * does not implement it, so the driver must not claim it.
	 */
	{
		uint64_t caps = (1ULL << GVE_DRIVER_CAPABILITY_GQI_QPL) |
				(1ULL << GVE_DRIVER_CAPABILITY_GQI_RDA) |
				(1ULL << GVE_DRIVER_CAPABILITY_DQO_RDA);

		drv_info->driver_capability_flags[0] = bswap64(caps);
	}
	memcpy(drv_info->os_version_str1, "Unikraft 0.21.0",
	       sizeof("Unikraft 0.21.0") - 1);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_VERIFY_DRIVER_COMPATIBILITY);
	cmd.verify_driver_compatibility.driver_info_len =
		bswap64(sizeof(*drv_info));
	cmd.verify_driver_compatibility.driver_info_addr =
		bswap64((uint64_t)(uintptr_t)drv_info);

	rc = gve_adminq_exec(reg_bar0, adminq, &gdev->adminq_prod_cnt, &cmd,
			     &cmd_status);
	uk_pr_info("gve: VERIFY_DRIVER_COMPATIBILITY rc=%d status=0x%x\n",
		   rc, cmd_status);
	if (rc == -ETIMEDOUT)
		cmd_may_run = 1;
	if (rc != 0 && cmd_status != GVE_ADMINQ_COMMAND_ERROR_UNIMPLEMENTED) {
		uk_pr_info("gve: Warning: VERIFY_DRIVER_COMPATIBILITY returned 0x%x\n",
			   cmd_status);
	}

	/* Issue DESCRIBE_DEVICE (0x1) */
	desc_buf = uk_memalign(drv_alloc, GVE_PAGE_SIZE, GVE_PAGE_SIZE);
	if (!desc_buf) {
		printf("GATE_FAIL: gve desc_buf allocation failed\n");
		rc = -ENOMEM;
		goto err;
	}
	memset(desc_buf, 0, GVE_PAGE_SIZE);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_DESCRIBE_DEVICE);
	cmd.describe_device.device_descriptor_addr =
		bswap64((uint64_t)(uintptr_t)desc_buf);
	cmd.describe_device.device_descriptor_version = bswap32(1);
	cmd.describe_device.available_length = bswap32(GVE_PAGE_SIZE);

	rc = gve_adminq_exec(reg_bar0, adminq, &gdev->adminq_prod_cnt, &cmd,
			     &cmd_status);
	uk_pr_info("gve: DESCRIBE_DEVICE rc=%d status=0x%x\n", rc, cmd_status);
	if (rc == -ETIMEDOUT)
		cmd_may_run = 1;
	if (rc != 0) {
		printf("GATE_FAIL: DESCRIBE_DEVICE failed with status=0x%x\n",
		       cmd_status);
		goto err;
	}

	desc = (struct gve_device_descriptor *)desc_buf;
	num_opts = bswap16(desc->num_device_options);
	total_len = bswap16(desc->total_length);

	memcpy(gdev->hw_addr.addr_bytes, desc->mac, 6);
	gdev->mtu = bswap16(desc->mtu);
	gdev->max_mtu = gdev->mtu;
	gdev->tx_desc_cnt = bswap16(desc->tx_queue_entries);
	gdev->rx_desc_cnt = bswap16(desc->rx_queue_entries);
	gdev->num_event_counters = bswap16(desc->counters);
	gdev->tx_pages_per_qpl = bswap16(desc->tx_pages_per_qpl);
	gdev->rx_pages_per_qpl = bswap16(desc->rx_pages_per_qpl);
	gdev->max_registered_pages = bswap64(desc->max_registered_pages);

	uk_pr_info("gve: Device Descriptor:\n");
	uk_pr_info("  MAC address:          %02x:%02x:%02x:%02x:%02x:%02x\n",
		   desc->mac[0], desc->mac[1], desc->mac[2],
		   desc->mac[3], desc->mac[4], desc->mac[5]);
	uk_pr_info("  MTU:                  %u\n", gdev->mtu);
	uk_pr_info("  tx_queue_entries:     %u\n", gdev->tx_desc_cnt);
	uk_pr_info("  rx_queue_entries:     %u\n", gdev->rx_desc_cnt);
	uk_pr_info("  default_num_queues:   %u\n", bswap16(desc->default_num_queues));
	uk_pr_info("  max_registered_pages: %llu\n",
		   (unsigned long long)gdev->max_registered_pages);
	uk_pr_info("  counters:             %u\n", gdev->num_event_counters);
	uk_pr_info("  tx_pages_per_qpl:     %u\n", gdev->tx_pages_per_qpl);
	uk_pr_info("  rx_pages_per_qpl:     %u\n", gdev->rx_pages_per_qpl);
	uk_pr_info("  num_device_options:   %u (total_length=%u)\n",
		   num_opts, total_len);

	/*
	 * Walk the option TLVs inside a bounded region. The device
	 * writes into the 4096-byte descriptor page and reports the
	 * used length, header included, in total_length. Trust the
	 * smaller of the two bounds. A parser may read a fixed struct
	 * from a body only when that struct lies fully inside the
	 * region and the declared body length covers it.
	 */
	desc_region = (total_len >= (uint16_t)sizeof(*desc) &&
		       total_len <= (uint16_t)GVE_PAGE_SIZE)
		      ? (uint32_t)total_len : (uint32_t)GVE_PAGE_SIZE;
	opt_off = (uint32_t)sizeof(*desc);
	for (i = 0; i < num_opts; i++) {
		struct gve_device_option *opt;
		uint16_t opt_id, opt_len;
		uint32_t req_feat;
		uint8_t *opt_body;
		uint32_t sup_feat = 0;
		uint16_t pair1 = 0;
		uint16_t pair2 = 0;
		int has_sup = 0;
		int pair_kind = 0;	/* 1: comp ring, 2: qpl pages, 3: max_mtu */

		if (!gve_tlv_hdr_fits(opt_off, desc_region, sizeof(*opt)))
			break;

		opt = (struct gve_device_option *)(desc_buf + opt_off);
		opt_id = bswap16(opt->option_id);
		opt_len = bswap16(opt->option_length);
		req_feat = bswap32(opt->required_features_mask);
		opt_body = desc_buf + opt_off + sizeof(*opt);

		if (opt_id == GVE_DEV_OPT_ID_GQI_RAW_ADDRESSING) {
			has_gqi_raw = 1;
		} else if (opt_id == GVE_DEV_OPT_ID_GQI_RDA) {
			gdev->has_gqi_rda = 1;
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_gqi_rda),
					      desc_region)) {
				struct gve_device_option_gqi_rda *r =
					(struct gve_device_option_gqi_rda *)opt_body;
				sup_feat = bswap32(r->supported_features_mask);
				has_sup = 1;
			}
		} else if (opt_id == GVE_DEV_OPT_ID_GQI_QPL) {
			gdev->has_gqi_qpl = 1;
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_gqi_qpl),
					      desc_region)) {
				struct gve_device_option_gqi_qpl *q =
					(struct gve_device_option_gqi_qpl *)opt_body;
				sup_feat = bswap32(q->supported_features_mask);
				has_sup = 1;
			}
		} else if (opt_id == GVE_DEV_OPT_ID_DQO_RDA) {
			gdev->has_dqo_rda = 1;
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_dqo_rda),
					      desc_region)) {
				struct gve_device_option_dqo_rda *d =
					(struct gve_device_option_dqo_rda *)opt_body;
				gdev->tx_comp_ring_entries =
					bswap16(d->tx_comp_ring_entries);
				gdev->rx_buff_ring_entries =
					bswap16(d->rx_buff_ring_entries);
				sup_feat = bswap32(d->supported_features_mask);
				has_sup = 1;
				pair_kind = 1;
			}
		} else if (opt_id == GVE_DEV_OPT_ID_DQO_QPL) {
			gdev->has_dqo_qpl = 1;
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_dqo_qpl),
					      desc_region)) {
				struct gve_device_option_dqo_qpl *dq =
					(struct gve_device_option_dqo_qpl *)opt_body;
				sup_feat = bswap32(dq->supported_features_mask);
				pair1 = bswap16(dq->tx_pages_per_qpl);
				pair2 = bswap16(dq->rx_pages_per_qpl);
				has_sup = 1;
				pair_kind = 2;
			}
		} else if (opt_id == GVE_DEV_OPT_ID_JUMBO_FRAMES) {
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_jumbo_frames),
					      desc_region)) {
				struct gve_device_option_jumbo_frames *jf =
					(struct gve_device_option_jumbo_frames *)opt_body;
				gdev->max_mtu = bswap16(jf->max_mtu);
				pair1 = gdev->max_mtu;
				pair_kind = 3;
			}
		} else if (opt_id == GVE_DEV_OPT_ID_RSS_CONFIG) {
			if (gve_tlv_body_fits(opt_off + sizeof(*opt), opt_len,
					      sizeof(struct gve_device_option_rss_config),
					      desc_region)) {
				struct gve_device_option_rss_config *rs =
					(struct gve_device_option_rss_config *)opt_body;
				uint32_t rss_sup =
					bswap32(rs->supported_features_mask);

				sup_feat = rss_sup;
				has_sup = 1;
				if (rss_sup & GVE_SUP_RSS_CONFIG_MASK) {
					gdev->rss_opt_present = 1;
					gdev->rss_key_size =
						bswap16(rs->hash_key_size);
					gdev->rss_lut_size =
						bswap16(rs->hash_lut_size);
				}
				pair1 = gdev->rss_key_size;
				pair2 = gdev->rss_lut_size;
				pair_kind = 4;
			}
		}

		switch (pair_kind) {
		case 1:
			uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x sup_feat=0x%x tx_comp_entries=%u rx_buff_entries=%u\n",
				   i, opt_id, gve_option_name(opt_id),
				   opt_len, req_feat, sup_feat,
				   gdev->tx_comp_ring_entries,
				   gdev->rx_buff_ring_entries);
			break;
		case 2:
			uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x sup_feat=0x%x tx_pages_qpl=%u rx_pages_qpl=%u\n",
				   i, opt_id, gve_option_name(opt_id),
				   opt_len, req_feat, sup_feat, pair1, pair2);
			break;
		case 3:
			uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x max_mtu=%u\n",
				   i, opt_id, gve_option_name(opt_id),
				   opt_len, req_feat, pair1);
			break;
		case 4:
			uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x sup_feat=0x%x hash_key_size=%u hash_lut_size=%u\n",
				   i, opt_id, gve_option_name(opt_id),
				   opt_len, req_feat, sup_feat, pair1, pair2);
			break;
		default:
			if (has_sup)
				uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x sup_feat=0x%x\n",
					   i, opt_id, gve_option_name(opt_id),
					   opt_len, req_feat, sup_feat);
			else
				uk_pr_info("gve:   [Option %u] id=0x%x (%s) len=%u req_feat=0x%x\n",
					   i, opt_id, gve_option_name(opt_id),
					   opt_len, req_feat);
			break;
		}

		opt_off += (uint32_t)sizeof(*opt) + opt_len;
	}

	if (gdev->has_dqo_rda)
		gdev->queue_format = GVE_DQO_RDA_FORMAT;
	else if (gdev->has_gqi_rda || has_gqi_raw)
		gdev->queue_format = GVE_GQI_RDA_FORMAT;
	else if (gdev->has_gqi_qpl)
		gdev->queue_format = GVE_GQI_QPL_FORMAT;
	/*
	 * A DQO_QPL-only device is refused. The datapath else-branches
	 * speak GQI, so selecting DQO_QPL here would build GQI rings
	 * under a DQO_QPL label. Reject the format until a real DQO_QPL
	 * path exists.
	 */
	if (gdev->queue_format == GVE_QUEUE_FORMAT_UNSPECIFIED) {
		printf("GATE_FAIL: gve no supported queue format (dqo_qpl=%d not implemented)\n",
		       gdev->has_dqo_qpl);
		rc = -ENODEV;
		goto err;
	}

	uk_pr_info("gve: Negotiated queue format: %s (%d)\n",
		   gve_queue_format_name(gdev->queue_format), gdev->queue_format);

	/*
	 * All ring indexing uses cnt - 1 as a mask. Reject a device
	 * that reports a zero or non-power-of-two ring size. The two
	 * DQO ring sizes only feed masks in the DQO_RDA format, so
	 * validate them when the device negotiated that format.
	 */
	if (!gve_ring_size_valid(gdev->tx_desc_cnt) ||
	    !gve_ring_size_valid(gdev->rx_desc_cnt)) {
		printf("GATE_FAIL: gve invalid ring size tx=%u rx=%u\n",
		       gdev->tx_desc_cnt, gdev->rx_desc_cnt);
		rc = -EINVAL;
		goto err;
	}
	if (gdev->queue_format == GVE_DQO_RDA_FORMAT &&
	    (!gve_ring_size_valid(gdev->tx_comp_ring_entries) ||
	     !gve_ring_size_valid(gdev->rx_buff_ring_entries))) {
		printf("GATE_FAIL: gve invalid DQO ring size tx_comp=%u rx_buff=%u\n",
		       gdev->tx_comp_ring_entries, gdev->rx_buff_ring_entries);
		rc = -EINVAL;
		goto err;
	}

	if (gdev->mtu == 0) {
		printf("GATE_FAIL: gve invalid MTU 0\n");
		rc = -EINVAL;
		goto err;
	}

	/*
	 * The receive path delivers one frame per 2048-byte buffer.
	 * A jumbo advertisement would promise frames the receive path
	 * cannot produce, so cap what info_get may advertise. Raise
	 * the cap only together with multi-buffer RX.
	 */
	{
		uint16_t capped = gve_max_mtu_capped(
			gdev->max_mtu, GVE_DEFAULT_RX_BUFFER_SIZE);

		if (capped != gdev->max_mtu)
			uk_pr_info("gve: max_mtu clamped %u -> %u for single-buffer RX\n",
				   gdev->max_mtu, capped);
		gdev->max_mtu = capped;
		if (gdev->mtu > gdev->max_mtu)
			uk_pr_info("gve: Warning: device MTU %u exceeds the receive cap %u\n",
				   gdev->mtu, gdev->max_mtu);
	}

	/* Register with uk_netdev */
	gdev->netdev.ops = &gve_netdev_ops;
	gdev->netdev.rx_one = gve_netdev_recv;
	gdev->netdev.tx_one = gve_netdev_xmit;

	rc = uk_netdev_drv_register(&gdev->netdev, drv_alloc, DRIVER_NAME);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_drv_register failed rc=%d\n", rc);
		goto err;
	}
	uk_pr_info("gve: Registered uk_netdev id=%d (%s)\n", rc, DRIVER_NAME);

	/*
	 * Print the gate marker only after every fallible step of the
	 * probe succeeded. Registration was the last one, so no
	 * GATE_FAIL can follow this marker in the same probe. Ticket
	 * 06ea1706a7.
	 */
	printf("========================================\n");
	printf("GATE_GVE_ADMINQ_PASS\n");
	printf("========================================\n");

	/*
	 * The driver info and device descriptor only served the
	 * handshake and parsing above. Nothing references them after
	 * registration, so release them.
	 */
	uk_free(drv_alloc, desc_buf);
	uk_free(drv_alloc, drv_info);
	return 0;

err:
	/*
	 * Unwind every allocation and mapping this call made. A
	 * timeout does not cancel the AdminQ command: the device can
	 * still write the adminq entry and the descriptor buffer, and
	 * still read the driver info buffer. Leak those three on
	 * timeout instead of returning device-touched or
	 * device-readable memory to the allocator. Ticket 6e2054b369.
	 */
	if (!cmd_may_run) {
		if (desc_buf)
			uk_free(drv_alloc, desc_buf);
		if (drv_info)
			uk_free(drv_alloc, drv_info);
		if (adminq)
			uk_free(drv_alloc, adminq);
	} else {
		uk_pr_err("gve: probe aborted with a command possibly pending, leaking adminq, driver info, and descriptor buffers\n");
	}
	if (mapped_bar2)
		gve_unmap_bar(1);
	if (mapped_bar0)
		gve_unmap_bar(0);
	uk_free(drv_alloc, gdev);
	return rc;
}

static int gve_drv_init(struct uk_alloc *a)
{
	if (!a)
		return -EINVAL;

	drv_alloc = a;
	return 0;
}

static const struct pci_device_id gve_pci_ids[] = {
	{ PCI_DEVICE_ID(GVE_PCI_VENDOR_ID, GVE_PCI_DEVICE_ID) },
	{ PCI_ANY_DEVICE_ID },
};

static struct pci_driver gve_pci_drv = {
	.device_ids = gve_pci_ids,
	.init = gve_drv_init,
	.add_dev = gve_add_dev,
};
PCI_REGISTER_DRIVER(&gve_pci_drv);
