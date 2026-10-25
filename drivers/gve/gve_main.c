/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Google Virtual Ethernet (gVNIC) PCI driver for Unikraft (Phase B1).
 * Probes PCI 1ae0:0042, maps 64-bit BARs, initializes Admin Queue,
 * and executes DESCRIBE_DEVICE.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <uk/config.h>
#include <uk/alloc.h>
#include <uk/print.h>
#include <uk/bus/pci.h>
#include <gve.h>

static struct uk_alloc *drv_alloc;

/* 2 MiB page directories for mapping BAR0 (slot 0) and BAR2 (slot 1) */
static uint64_t gve_bar_pd[2][512] __attribute__((aligned(4096)));

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

static inline void outl_port(uint16_t port, uint32_t val)
{
	__asm__ __volatile__("outl %0, %w1" :: "a"(val), "Nd"(port));
}

static inline uint32_t inl_port(uint16_t port)
{
	uint32_t val;

	__asm__ __volatile__("inl %w1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func,
			       uint8_t offset)
{
	uint32_t addr;

	addr = (1U << 31) |
	       ((uint32_t)bus << 16) |
	       ((uint32_t)dev << 11) |
	       ((uint32_t)func << 8) |
	       ((uint32_t)offset & 0xfcU);
	outl_port(PCI_CONFIG_ADDR, addr);
	return inl_port(PCI_CONFIG_DATA);
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func,
			    uint8_t offset, uint32_t val)
{
	uint32_t addr;

	addr = (1U << 31) |
	       ((uint32_t)bus << 16) |
	       ((uint32_t)dev << 11) |
	       ((uint32_t)func << 8) |
	       ((uint32_t)offset & 0xfcU);
	outl_port(PCI_CONFIG_ADDR, addr);
	outl_port(PCI_CONFIG_DATA, val);
}

static uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func,
			     uint8_t reg)
{
	uint32_t lo = pci_cfg_read32(bus, dev, func, reg);
	uint64_t addr;

	if (lo & 0x1U)
		return (uint64_t)(lo & ~0x3U);

	addr = (uint64_t)(lo & ~0xfU);
	if (((lo >> 1) & 0x3U) == 0x2U) {
		uint32_t hi = pci_cfg_read32(bus, dev, func, reg + 4);

		addr |= ((uint64_t)hi << 32);
	}
	return addr;
}

/*
 * Map a 32-bit or 64-bit physical BAR address into pdpt0[4 + slot]
 * using 2 MiB pages with uncacheable RW attributes (0x9b).
 */
static volatile void *gve_map_bar(uint64_t paddr, int slot)
{
	uint64_t cr3;
	uint64_t *pml4;
	uint64_t *pdpt0;
	uint64_t gb_base;
	uint64_t vaddr_base;
	int i;

	if (slot < 0 || slot > 1 || paddr == 0)
		return NULL;

	__asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
	pml4 = (uint64_t *)(uintptr_t)(cr3 & ~0xfffULL);
	pdpt0 = (uint64_t *)(uintptr_t)(pml4[0] & ~0xfffULL);

	gb_base = paddr & ~0x3fffffffULL;
	for (i = 0; i < 512; i++)
		gve_bar_pd[slot][i] = (gb_base + ((uint64_t)i << 21)) | 0x9bULL;

	pdpt0[4 + slot] = ((uint64_t)(uintptr_t)gve_bar_pd[slot]) | 0x03ULL;
	__asm__ __volatile__("movq %0, %%cr3" :: "r"(cr3) : "memory");

	vaddr_base = ((uint64_t)(4 + slot)) << 30;
	return (volatile void *)(uintptr_t)(vaddr_base + (paddr & 0x3fffffffULL));
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
	uint32_t mask = (4096 / sizeof(union gve_adminq_command)) - 1;
	uint32_t slot = (*prod_cnt) & mask;
	uint32_t target;
	uint32_t ev_cnt;
	uint32_t status;
	int i;

	memcpy(&adminq[slot], cmd_in, sizeof(*cmd_in));
	adminq[slot].status = GVE_ADMINQ_COMMAND_UNSET;
	__asm__ __volatile__("mfence" ::: "memory");

	(*prod_cnt)++;
	target = *prod_cnt;
	mmio_write32(&reg_bar0->adminq_doorbell, bswap32(target));

	for (i = 0; i < 10000000; i++) {
		ev_cnt = bswap32(mmio_read32(&reg_bar0->adminq_event_counter));
		if (ev_cnt == target)
			break;
		__asm__ __volatile__("pause" ::: "memory");
	}

	if (ev_cnt != target) {
		printf("[gve] AdminQ timeout: doorbell=%u event_counter=%u\n",
		       target, ev_cnt);
		return -ETIMEDOUT;
	}

	status = bswap32(adminq[slot].status);
	if (status_out)
		*status_out = status;

	return (status == GVE_ADMINQ_COMMAND_PASSED) ? 0 : -EIO;
}

static int gve_add_dev(struct pci_device *pdev)
{
	uint8_t bus, dev, func, rev;
	uint32_t cmd_reg;
	uint64_t bar0_paddr, bar2_paddr;
	volatile struct gve_registers *reg_bar0;
	volatile uint32_t *db_bar2;
	uint32_t dev_status, max_tx_q, max_rx_q;
	const char *ver_str = "GVE-1.0.0\n";
	const char *p;
	union gve_adminq_command *adminq;
	uint64_t adminq_paddr;
	uint32_t adminq_prod_cnt = 0;
	struct gve_driver_info *drv_info;
	uint8_t *desc_buf;
	struct gve_device_descriptor *desc;
	union gve_adminq_command cmd;
	uint32_t cmd_status = 0;
	uint16_t num_opts, total_len, i;
	uint8_t *opt_ptr;
	int has_gqi_raw = 0, has_gqi_rda = 0, has_gqi_qpl = 0;
	int has_dqo_rda = 0, has_dqo_qpl = 0;
	enum gve_queue_format selected_fmt = GVE_QUEUE_FORMAT_UNSPECIFIED;
	int rc;

	if (!pdev)
		return -EINVAL;

	bus = pdev->addr.bus;
	dev = pdev->addr.devid;
	func = pdev->addr.function;

	printf("\n========================================\n");
	printf("[gve] Probing gVNIC PCI device %04x:%02x:%02x.%x (1ae0:0042)\n",
	       pdev->addr.domain, bus, dev, func);

	/* Enable PCI memory space and bus mastering */
	cmd_reg = pci_cfg_read32(bus, dev, func, PCI_COMMAND);
	cmd_reg |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	pci_cfg_write32(bus, dev, func, PCI_COMMAND, cmd_reg);

	rev = (uint8_t)(pci_cfg_read32(bus, dev, func, PCI_CLASS_REVISION) & 0xffU);
	bar0_paddr = pci_read_bar(bus, dev, func, PCI_BASE_ADDRESS_0);
	bar2_paddr = pci_read_bar(bus, dev, func, PCI_BASE_ADDRESS_2);

	printf("[gve] PCI Revision: 0x%02x, BAR0 paddr: 0x%llx, BAR2 paddr: 0x%llx\n",
	       rev, (unsigned long long)bar0_paddr,
	       (unsigned long long)bar2_paddr);

	reg_bar0 = (volatile struct gve_registers *)gve_map_bar(bar0_paddr, 0);
	db_bar2 = (volatile uint32_t *)gve_map_bar(bar2_paddr, 1);
	if (!reg_bar0 || !db_bar2) {
		printf("GATE_FAIL: gve BAR mapping failed\n");
		return -ENOMEM;
	}

	printf("[gve] Mapped BAR0 vaddr=%p, BAR2 vaddr=%p\n",
	       (const void *)reg_bar0, (const void *)db_bar2);

	dev_status = bswap32(mmio_read32(&reg_bar0->device_status));
	max_tx_q = bswap32(mmio_read32(&reg_bar0->max_tx_queues));
	max_rx_q = bswap32(mmio_read32(&reg_bar0->max_rx_queues));
	printf("[gve] BAR0 device_status=0x%08x max_tx_queues=%u max_rx_queues=%u\n",
	       dev_status, max_tx_q, max_rx_q);

	/* Write driver version string byte-by-byte to BAR0 offset 0x1f */
	for (p = ver_str; *p; p++)
		mmio_write8(&reg_bar0->driver_version, (uint8_t)*p);

	/* Allocate 4096-byte aligned Admin Queue page */
	adminq = uk_memalign(drv_alloc, 4096, 4096);
	if (!adminq) {
		printf("GATE_FAIL: gve adminq allocation failed\n");
		return -ENOMEM;
	}
	memset(adminq, 0, 4096);
	adminq_paddr = (uint64_t)(uintptr_t)adminq;

	if (rev < 0x1) {
		mmio_write32(&reg_bar0->adminq_pfn,
			     bswap32((uint32_t)(adminq_paddr >> 12)));
	} else {
		mmio_write16(&reg_bar0->adminq_length, bswap16(4096));
		mmio_write32(&reg_bar0->adminq_base_address_hi,
			     bswap32((uint32_t)(adminq_paddr >> 32)));
		mmio_write32(&reg_bar0->adminq_base_address_lo,
			     bswap32((uint32_t)(adminq_paddr & 0xffffffffU)));
		mmio_write32(&reg_bar0->driver_status,
			     bswap32(GVE_DRIVER_STATUS_RUN_MASK));
	}
	printf("[gve] Admin Queue initialized at paddr=0x%llx\n",
	       (unsigned long long)adminq_paddr);

	/* Issue VERIFY_DRIVER_COMPATIBILITY (0xF) advertising all queue formats */
	drv_info = uk_memalign(drv_alloc, 4096, 4096);
	if (!drv_info) {
		printf("GATE_FAIL: gve driver_info allocation failed\n");
		return -ENOMEM;
	}
	memset(drv_info, 0, 4096);
	drv_info->os_type = 1; /* Linux */
	drv_info->driver_major = 1;
	drv_info->driver_minor = 0;
	drv_info->driver_sub = 0;
	drv_info->os_version_major = bswap32(6);
	drv_info->os_version_minor = bswap32(8);
	drv_info->os_version_sub = bswap32(0);
	/* Advertise gqi_qpl (0) | gqi_rda (1) | dqo_qpl (2) | dqo_rda (3) = 0x0f */
	drv_info->driver_capability_flags[0] = bswap64(0x0fULL) | 0x0fULL;
	memcpy(drv_info->os_version_str1, "Unikraft 0.21.0", 15);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_VERIFY_DRIVER_COMPATIBILITY);
	cmd.verify_driver_compatibility.driver_info_len =
		bswap64(sizeof(*drv_info));
	cmd.verify_driver_compatibility.driver_info_addr =
		bswap64((uint64_t)(uintptr_t)drv_info);

	rc = gve_adminq_exec(reg_bar0, adminq, &adminq_prod_cnt, &cmd,
			     &cmd_status);
	printf("[gve] VERIFY_DRIVER_COMPATIBILITY rc=%d status=0x%x\n",
	       rc, cmd_status);
	if (rc != 0 && cmd_status != GVE_ADMINQ_COMMAND_ERROR_UNIMPLEMENTED) {
		printf("[gve] Warning: VERIFY_DRIVER_COMPATIBILITY returned 0x%x\n",
		       cmd_status);
	}

	/* Issue DESCRIBE_DEVICE (0x1) */
	desc_buf = uk_memalign(drv_alloc, 4096, 4096);
	if (!desc_buf) {
		printf("GATE_FAIL: gve desc_buf allocation failed\n");
		return -ENOMEM;
	}
	memset(desc_buf, 0, 4096);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = bswap32(GVE_ADMINQ_DESCRIBE_DEVICE);
	cmd.describe_device.device_descriptor_addr =
		bswap64((uint64_t)(uintptr_t)desc_buf);
	cmd.describe_device.device_descriptor_version = bswap32(1);
	cmd.describe_device.available_length = bswap32(4096);

	rc = gve_adminq_exec(reg_bar0, adminq, &adminq_prod_cnt, &cmd,
			     &cmd_status);
	printf("[gve] DESCRIBE_DEVICE rc=%d status=0x%x\n", rc, cmd_status);
	if (rc != 0) {
		printf("GATE_FAIL: DESCRIBE_DEVICE failed with status=0x%x\n",
		       cmd_status);
		return rc;
	}

	desc = (struct gve_device_descriptor *)desc_buf;
	num_opts = bswap16(desc->num_device_options);
	total_len = bswap16(desc->total_length);

	printf("[gve] Device Descriptor:\n");
	printf("  MAC address:          %02x:%02x:%02x:%02x:%02x:%02x\n",
	       desc->mac[0], desc->mac[1], desc->mac[2],
	       desc->mac[3], desc->mac[4], desc->mac[5]);
	printf("  MTU:                  %u\n", bswap16(desc->mtu));
	printf("  tx_queue_entries:     %u\n", bswap16(desc->tx_queue_entries));
	printf("  rx_queue_entries:     %u\n", bswap16(desc->rx_queue_entries));
	printf("  default_num_queues:   %u\n", bswap16(desc->default_num_queues));
	printf("  max_registered_pages: %llu\n",
	       (unsigned long long)bswap64(desc->max_registered_pages));
	printf("  counters:             %u\n", bswap16(desc->counters));
	printf("  tx_pages_per_qpl:     %u\n", bswap16(desc->tx_pages_per_qpl));
	printf("  rx_pages_per_qpl:     %u\n", bswap16(desc->rx_pages_per_qpl));
	printf("  num_device_options:   %u (total_length=%u)\n",
	       num_opts, total_len);

	opt_ptr = desc_buf + sizeof(*desc);
	for (i = 0; i < num_opts; i++) {
		struct gve_device_option *opt;
		uint16_t opt_id, opt_len;
		uint32_t req_feat;
		uint8_t *opt_body;

		if ((size_t)(opt_ptr - desc_buf) + sizeof(*opt) > 4096)
			break;

		opt = (struct gve_device_option *)opt_ptr;
		opt_id = bswap16(opt->option_id);
		opt_len = bswap16(opt->option_length);
		req_feat = bswap32(opt->required_features_mask);
		opt_body = opt_ptr + sizeof(*opt);

		printf("  [Option %u] id=0x%x (%s) len=%u req_feat=0x%x",
		       i, opt_id, gve_option_name(opt_id), opt_len, req_feat);

		if (opt_id == GVE_DEV_OPT_ID_GQI_RAW_ADDRESSING) {
			has_gqi_raw = 1;
		} else if (opt_id == GVE_DEV_OPT_ID_GQI_RDA) {
			has_gqi_rda = 1;
			if (opt_len >= sizeof(struct gve_device_option_gqi_rda)) {
				struct gve_device_option_gqi_rda *r =
					(struct gve_device_option_gqi_rda *)opt_body;
				printf(" sup_feat=0x%x",
				       bswap32(r->supported_features_mask));
			}
		} else if (opt_id == GVE_DEV_OPT_ID_GQI_QPL) {
			has_gqi_qpl = 1;
			if (opt_len >= sizeof(struct gve_device_option_gqi_qpl)) {
				struct gve_device_option_gqi_qpl *q =
					(struct gve_device_option_gqi_qpl *)opt_body;
				printf(" sup_feat=0x%x",
				       bswap32(q->supported_features_mask));
			}
		} else if (opt_id == GVE_DEV_OPT_ID_DQO_RDA) {
			has_dqo_rda = 1;
			if (opt_len >= sizeof(struct gve_device_option_dqo_rda)) {
				struct gve_device_option_dqo_rda *d =
					(struct gve_device_option_dqo_rda *)opt_body;
				printf(" sup_feat=0x%x tx_comp_entries=%u rx_buff_entries=%u",
				       bswap32(d->supported_features_mask),
				       bswap16(d->tx_comp_ring_entries),
				       bswap16(d->rx_buff_ring_entries));
			}
		} else if (opt_id == GVE_DEV_OPT_ID_DQO_QPL) {
			has_dqo_qpl = 1;
			if (opt_len >= sizeof(struct gve_device_option_dqo_qpl)) {
				struct gve_device_option_dqo_qpl *dq =
					(struct gve_device_option_dqo_qpl *)opt_body;
				printf(" sup_feat=0x%x tx_pages_qpl=%u rx_pages_qpl=%u",
				       bswap32(dq->supported_features_mask),
				       bswap16(dq->tx_pages_per_qpl),
				       bswap16(dq->rx_pages_per_qpl));
			}
		} else if (opt_id == GVE_DEV_OPT_ID_JUMBO_FRAMES) {
			if (opt_len >= sizeof(struct gve_device_option_jumbo_frames)) {
				struct gve_device_option_jumbo_frames *jf =
					(struct gve_device_option_jumbo_frames *)opt_body;
				printf(" max_mtu=%u", bswap16(jf->max_mtu));
			}
		}
		printf("\n");

		opt_ptr += sizeof(*opt) + opt_len;
	}

	if (has_dqo_rda)
		selected_fmt = GVE_DQO_RDA_FORMAT;
	else if (has_dqo_qpl)
		selected_fmt = GVE_DQO_QPL_FORMAT;
	else if (has_gqi_rda || has_gqi_raw)
		selected_fmt = GVE_GQI_RDA_FORMAT;
	else if (has_gqi_qpl)
		selected_fmt = GVE_GQI_QPL_FORMAT;

	printf("[gve] Negotiated queue format: %s (%d)\n",
	       gve_queue_format_name(selected_fmt), selected_fmt);

	if (bswap16(desc->mtu) == 0) {
		printf("GATE_FAIL: gve invalid MTU 0\n");
		return -EINVAL;
	}

	printf("========================================\n");
	printf("GATE_GVE_ADMINQ_PASS\n");
	printf("========================================\n");

	return 0;
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
