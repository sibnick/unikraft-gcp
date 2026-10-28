/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Google Virtual Ethernet (gVNIC) driver definitions for Unikraft.
 */

#ifndef __UK_DRV_GVE_H__
#define __UK_DRV_GVE_H__

#include <stdint.h>
#include <stddef.h>

#define GVE_PCI_VENDOR_ID		0x1ae0
#define GVE_PCI_DEVICE_ID		0x0042

/*
 * BAR0 register layout (big-endian fields). Natural alignment
 * already reproduces the hardware offsets, so the struct stays
 * unpacked: passing &reg_bar0->member to the mmio helpers then
 * causes no -Waddress-of-packed-member warning. The offsetof checks
 * below pin the layout against the device contract.
 */
struct gve_registers {
	uint32_t device_status;
	uint32_t driver_status;
	uint32_t max_tx_queues;
	uint32_t max_rx_queues;
	uint32_t adminq_pfn;
	uint32_t adminq_doorbell;
	uint32_t adminq_event_counter;
	uint8_t reserved[3];
	uint8_t driver_version;
	uint32_t adminq_base_address_hi;
	uint32_t adminq_base_address_lo;
	uint16_t adminq_length;
};

/* Driver status flags */
#define GVE_DRIVER_STATUS_RUN_MASK	(1U << 0)
#define GVE_DRIVER_STATUS_RESET_MASK	(1U << 1)

/* Device status flags */
#define GVE_DEVICE_STATUS_RESET_MASK		(1U << 1)
#define GVE_DEVICE_STATUS_LINK_STATUS_MASK	(1U << 2)
#define GVE_DEVICE_STATUS_REPORT_STATS_MASK	(1U << 3)
#define GVE_DEVICE_STATUS_DEVICE_IS_RESET	(1U << 4)

/* Admin Queue opcodes */
enum gve_adminq_opcodes {
	GVE_ADMINQ_DESCRIBE_DEVICE		= 0x1,
	GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES	= 0x2,
	GVE_ADMINQ_REGISTER_PAGE_LIST		= 0x3,
	GVE_ADMINQ_UNREGISTER_PAGE_LIST		= 0x4,
	GVE_ADMINQ_CREATE_TX_QUEUE		= 0x5,
	GVE_ADMINQ_CREATE_RX_QUEUE		= 0x6,
	GVE_ADMINQ_DESTROY_TX_QUEUE		= 0x7,
	GVE_ADMINQ_DESTROY_RX_QUEUE		= 0x8,
	GVE_ADMINQ_DECONFIGURE_DEVICE_RESOURCES	= 0x9,
	GVE_ADMINQ_SET_DRIVER_PARAMETER		= 0xB,
	GVE_ADMINQ_REPORT_STATS			= 0xC,
	GVE_ADMINQ_REPORT_LINK_SPEED		= 0xD,
	GVE_ADMINQ_GET_PTYPE_MAP		= 0xE,
	GVE_ADMINQ_VERIFY_DRIVER_COMPATIBILITY	= 0xF,
};

/* Admin Queue status codes */
enum gve_adminq_statuses {
	GVE_ADMINQ_COMMAND_UNSET			= 0x0,
	GVE_ADMINQ_COMMAND_PASSED			= 0x1,
	GVE_ADMINQ_COMMAND_ERROR_ABORTED		= 0xFFFFFFF0U,
	GVE_ADMINQ_COMMAND_ERROR_ALREADY_EXISTS		= 0xFFFFFFF1U,
	GVE_ADMINQ_COMMAND_ERROR_CANCELLED		= 0xFFFFFFF2U,
	GVE_ADMINQ_COMMAND_ERROR_DATALOSS		= 0xFFFFFFF3U,
	GVE_ADMINQ_COMMAND_ERROR_DEADLINE_EXCEEDED	= 0xFFFFFFF4U,
	GVE_ADMINQ_COMMAND_ERROR_FAILED_PRECONDITION	= 0xFFFFFFF5U,
	GVE_ADMINQ_COMMAND_ERROR_INTERNAL_ERROR		= 0xFFFFFFF6U,
	GVE_ADMINQ_COMMAND_ERROR_INVALID_ARGUMENT	= 0xFFFFFFF7U,
	GVE_ADMINQ_COMMAND_ERROR_NOT_FOUND		= 0xFFFFFFF8U,
	GVE_ADMINQ_COMMAND_ERROR_OUT_OF_RANGE		= 0xFFFFFFF9U,
	GVE_ADMINQ_COMMAND_ERROR_PERMISSION_DENIED	= 0xFFFFFFFAU,
	GVE_ADMINQ_COMMAND_ERROR_UNAUTHENTICATED	= 0xFFFFFFFBU,
	GVE_ADMINQ_COMMAND_ERROR_RESOURCE_EXHAUSTED	= 0xFFFFFFFCU,
	GVE_ADMINQ_COMMAND_ERROR_UNAVAILABLE		= 0xFFFFFFFDU,
	GVE_ADMINQ_COMMAND_ERROR_UNIMPLEMENTED		= 0xFFFFFFFEU,
	GVE_ADMINQ_COMMAND_ERROR_UNKNOWN_ERROR		= 0xFFFFFFFFU,
};

/* Queue formats */
enum gve_queue_format {
	GVE_QUEUE_FORMAT_UNSPECIFIED	= 0x0,
	GVE_GQI_RDA_FORMAT		= 0x1,
	GVE_GQI_QPL_FORMAT		= 0x2,
	GVE_DQO_RDA_FORMAT		= 0x3,
	GVE_DQO_QPL_FORMAT		= 0x4,
};

/* Device option IDs */
enum gve_dev_opt_id {
	GVE_DEV_OPT_ID_GQI_RAW_ADDRESSING	= 0x1,
	GVE_DEV_OPT_ID_GQI_RDA			= 0x2,
	GVE_DEV_OPT_ID_GQI_QPL			= 0x3,
	GVE_DEV_OPT_ID_DQO_RDA			= 0x4,
	GVE_DEV_OPT_ID_MODIFY_RING		= 0x6,
	GVE_DEV_OPT_ID_DQO_QPL			= 0x7,
	GVE_DEV_OPT_ID_JUMBO_FRAMES		= 0x8,
	GVE_DEV_OPT_ID_BUFFER_SIZES		= 0xa,
};

/* Driver capability bit positions in driver_capability_flags[0] */
enum gve_driver_capability {
	GVE_DRIVER_CAPABILITY_GQI_QPL		= 0,
	GVE_DRIVER_CAPABILITY_GQI_RDA		= 1,
	GVE_DRIVER_CAPABILITY_DQO_QPL		= 2,
	GVE_DRIVER_CAPABILITY_DQO_RDA		= 3,
	GVE_DRIVER_CAPABILITY_ALT_MISS_COMPL	= 4,
};

struct gve_driver_info {
	uint8_t os_type;
	uint8_t driver_major;
	uint8_t driver_minor;
	uint8_t driver_sub;
	uint32_t os_version_major;
	uint32_t os_version_minor;
	uint32_t os_version_sub;
	uint64_t driver_capability_flags[4];
	uint8_t os_version_str1[128];
	uint8_t os_version_str2[128];
} __attribute__((packed));

struct gve_device_descriptor {
	uint64_t max_registered_pages;
	uint16_t reserved1;
	uint16_t tx_queue_entries;
	uint16_t rx_queue_entries;
	uint16_t default_num_queues;
	uint16_t mtu;
	uint16_t counters;
	uint16_t tx_pages_per_qpl;
	uint16_t rx_pages_per_qpl;
	uint8_t mac[6];
	uint16_t num_device_options;
	uint16_t total_length;
	uint8_t reserved2[6];
} __attribute__((packed));

struct gve_device_option {
	uint16_t option_id;
	uint16_t option_length;
	uint32_t required_features_mask;
} __attribute__((packed));

struct gve_device_option_gqi_rda {
	uint32_t supported_features_mask;
} __attribute__((packed));

struct gve_device_option_gqi_qpl {
	uint32_t supported_features_mask;
} __attribute__((packed));

struct gve_device_option_dqo_rda {
	uint32_t supported_features_mask;
	uint16_t tx_comp_ring_entries;
	uint16_t rx_buff_ring_entries;
} __attribute__((packed));

struct gve_device_option_dqo_qpl {
	uint32_t supported_features_mask;
	uint16_t tx_pages_per_qpl;
	uint16_t rx_pages_per_qpl;
} __attribute__((packed));

struct gve_device_option_jumbo_frames {
	uint32_t supported_features_mask;
	uint16_t max_mtu;
	uint8_t padding[2];
} __attribute__((packed));

struct gve_adminq_describe_device {
	uint64_t device_descriptor_addr;
	uint32_t device_descriptor_version;
	uint32_t available_length;
} __attribute__((packed));

struct gve_adminq_configure_device_resources {
	uint64_t counter_array;
	uint64_t irq_db_addr;
	uint32_t num_counters;
	uint32_t num_irq_dbs;
	uint32_t irq_db_stride;
	uint32_t ntfy_blk_msix_base_idx;
	uint8_t queue_format;
	uint8_t padding[7];
} __attribute__((packed));

struct gve_adminq_register_page_list {
	uint32_t page_list_id;
	uint32_t num_pages;
	uint64_t page_address_list_addr;
	uint64_t page_size;
} __attribute__((packed));

struct gve_adminq_unregister_page_list {
	uint32_t page_list_id;
} __attribute__((packed));

struct gve_adminq_create_tx_queue {
	uint32_t queue_id;
	uint32_t reserved;
	uint64_t queue_resources_addr;
	uint64_t tx_ring_addr;
	uint32_t queue_page_list_id;
	uint32_t ntfy_id;
	uint64_t tx_comp_ring_addr;
	uint16_t tx_ring_size;
	uint16_t tx_comp_ring_size;
	uint8_t padding[4];
} __attribute__((packed));

#define GVE_RAW_ADDRESSING_QPL_ID	0xFFFFFFFFU
#define GVE_DEFAULT_RX_BUFFER_SIZE	2048U
#define GVE_RX_PAD			2U
#define GVE_RX_BUF_THRESH_DQO		32U
#define GVE_NUM_PTYPES			1024U

struct gve_irq_db {
	uint32_t index;
	uint8_t reserved[60];
} __attribute__((packed, aligned(64)));

struct gve_queue_resources {
	uint32_t db_index;
	uint32_t counter_index;
	uint8_t reserved[56];
} __attribute__((packed, aligned(64)));

struct gve_ptype_entry {
	uint8_t l3_type;
	uint8_t l4_type;
} __attribute__((packed));

struct gve_ptype_lut {
	struct gve_ptype_entry ptypes[GVE_NUM_PTYPES];
} __attribute__((packed));

struct gve_adminq_create_rx_queue {
	uint32_t queue_id;
	uint32_t index;
	uint32_t reserved;
	uint32_t ntfy_id;
	uint64_t queue_resources_addr;
	uint64_t rx_desc_ring_addr;
	uint64_t rx_data_ring_addr;
	uint32_t queue_page_list_id;
	uint16_t rx_ring_size;
	uint16_t packet_buffer_size;
	uint16_t rx_buff_ring_size;
	uint8_t enable_rsc;
	uint8_t padding1;
	uint16_t header_buffer_size;
	uint8_t padding2[2];
} __attribute__((packed));

struct gve_adminq_destroy_tx_queue {
	uint32_t queue_id;
} __attribute__((packed));

struct gve_adminq_destroy_rx_queue {
	uint32_t queue_id;
} __attribute__((packed));

struct gve_adminq_set_driver_parameter {
	uint32_t parameter_type;
	uint8_t reserved[4];
	uint64_t parameter_value;
} __attribute__((packed));

struct gve_adminq_get_ptype_map {
	uint64_t ptype_map_len;
	uint64_t ptype_map_addr;
} __attribute__((packed));

struct gve_adminq_verify_driver_compatibility {
	uint64_t driver_info_len;
	uint64_t driver_info_addr;
} __attribute__((packed));

union gve_adminq_command {
	struct {
		uint32_t opcode;
		uint32_t status;
		union {
			struct gve_adminq_configure_device_resources configure_device_resources;
			struct gve_adminq_create_tx_queue create_tx_queue;
			struct gve_adminq_create_rx_queue create_rx_queue;
			struct gve_adminq_destroy_tx_queue destroy_tx_queue;
			struct gve_adminq_destroy_rx_queue destroy_rx_queue;
			struct gve_adminq_describe_device describe_device;
			struct gve_adminq_register_page_list reg_page_list;
			struct gve_adminq_unregister_page_list unreg_page_list;
			struct gve_adminq_set_driver_parameter set_driver_param;
			struct gve_adminq_get_ptype_map get_ptype_map;
			struct gve_adminq_verify_driver_compatibility verify_driver_compatibility;
		};
	} __attribute__((packed));
	uint8_t reserved[64];
} __attribute__((packed));

/* GQI datapath descriptors (big-endian fields) */
#define GVE_TXF_STD		0x00U
#define GVE_TXD_STD		GVE_TXF_STD

struct gve_tx_pkt_desc {
	uint8_t type_flags;
	uint8_t checksum_offset;
	uint8_t l4_csum_offset;
	uint8_t desc_cnt;
	uint16_t len;
	uint16_t seg_len;
	uint64_t addr;
} __attribute__((packed));

struct gve_tx_mtd_desc {
	uint8_t type_flags;
	uint8_t path_state;
	uint16_t reserved0;
	uint32_t path_hash;
	uint64_t reserved1;
} __attribute__((packed));

struct gve_tx_seg_desc {
	uint8_t type_flags;
	uint8_t l3_offset;
	uint16_t reserved;
	uint16_t mss;
	uint16_t seg_len;
	uint64_t addr;
} __attribute__((packed));

union gve_tx_desc {
	struct gve_tx_pkt_desc pkt;
	struct gve_tx_mtd_desc mtd;
	struct gve_tx_seg_desc seg;
} __attribute__((packed));

union gve_rx_data_slot {
	uint64_t qpl_offset;
	uint64_t addr;
} __attribute__((packed));

#define GVE_RXF_FRAG		(1U << 3)
#define GVE_RXF_IPV4		(1U << 4)
#define GVE_RXF_IPV6		(1U << 5)
#define GVE_RXF_TCP		(1U << 6)
#define GVE_RXF_UDP		(1U << 7)
#define GVE_RXF_ERR		(1U << 11)
/*
 * flags_seq holds the RX flags and the sequence number in
 * big-endian byte order. GVE_RXFLG() byteswaps the whole word so
 * the host-bit GVE_RXF_* masks above can be applied directly. The
 * macro performs no other transformation. GVE_SEQNO() extracts the
 * low three bits from the same swapped word.
 */
#define GVE_RXFLG(x)		__builtin_bswap16(x)
#define GVE_SEQNO(x)		((uint8_t)(__builtin_bswap16(x) & 0x7U))

static inline uint8_t gve_next_seqno(uint8_t seq)
{
	return (seq + 1U == 8U) ? 1U : (uint8_t)(seq + 1U);
}

struct gve_rx_desc {
	uint8_t padding[48];
	uint32_t rss_hash;
	uint16_t mss;
	uint16_t reserved;
	uint8_t hdr_len;
	uint8_t hdr_off;
	uint16_t csum;
	uint16_t len;
	uint16_t flags_seq;
} __attribute__((packed, aligned(64)));

/* DQO datapath descriptors (little-endian fields) */
#define GVE_TX_PKT_DESC_DTYPE_DQO		0xcU
#define GVE_TX_GENERAL_CTX_DESC_DTYPE_DQO	0x4U

#define GVE_COMPL_TYPE_DQO_PKT			0x2U
#define GVE_COMPL_TYPE_DQO_DESC			0x4U
#define GVE_COMPL_TYPE_DQO_MISS			0x1U
#define GVE_COMPL_TYPE_DQO_REINJECTION		0x3U

struct gve_tx_pkt_desc_dqo {
	uint64_t buf_addr;
	uint8_t dtype:5;
	uint8_t end_of_packet:1;
	uint8_t checksum_offload_enable:1;
	uint8_t report_event:1;
	uint8_t reserved0;
	uint16_t reserved1;
	uint16_t compl_tag;
	uint16_t buf_size:14;
	uint16_t reserved2:2;
} __attribute__((packed));

struct gve_tx_context_cmd_dtype {
	uint8_t dtype:5;
	uint8_t tso:1;
	uint8_t reserved1:2;
	uint8_t reserved2;
} __attribute__((packed));

struct gve_tx_general_context_desc_dqo {
	uint8_t flex4;
	uint8_t flex5;
	uint8_t flex6;
	uint8_t flex7;
	uint8_t flex8;
	uint8_t flex9;
	uint8_t flex10;
	uint8_t flex11;
	struct gve_tx_context_cmd_dtype cmd_dtype;
	uint16_t reserved;
	uint8_t flex0;
	uint8_t flex1;
	uint8_t flex2;
	uint8_t flex3;
} __attribute__((packed));

union gve_tx_desc_dqo {
	struct gve_tx_pkt_desc_dqo pkt;
	struct gve_tx_general_context_desc_dqo general_ctx;
} __attribute__((packed));

/*
 * DQO TX completion descriptor. Little-endian bitfields, matching
 * struct gve_tx_compl_desc in the Linux gve driver. In the first
 * 16-bit word the TX queue id sits at bits 10:0, the completion
 * type at bits 13:11, and the generation bit at bit 15. Bit 14 is
 * reserved by the device. The id field names the queue, not a ring
 * slot. A DESC-type completion reports the device TX head in
 * tx_head: the ring slot after the last descriptor the device
 * fetched. The field is __le16 in Linux, so a plain read is correct
 * on x86. The driver reclaims all ring slots below that index.
 */
struct gve_tx_compl_desc {
	uint16_t id:11;
	uint16_t type:3;
	uint16_t reserved0:1;
	uint16_t generation:1;
	union {
		uint16_t completion_tag;	/* PKT-type, le16 */
		uint16_t tx_head;			/* DESC-type, le16 */
	};
	uint32_t reserved1;
} __attribute__((packed));

struct gve_rx_desc_dqo {
	uint16_t buf_id;
	uint16_t reserved0;
	uint32_t reserved1;
	uint64_t buf_addr;
	uint64_t header_buf_addr;
	uint64_t reserved2;
} __attribute__((packed, aligned(32)));

struct gve_rx_compl_desc_dqo {
	uint8_t rxdid:4;                  /* Offset 0 [3:0] */
	uint8_t reserved0:4;              /* Offset 0 [7:4] */
	uint8_t loopback:1;               /* Offset 1 [0] */
	uint8_t ipv6_ex_add:1;            /* Offset 1 [1] */
	uint8_t rx_error:1;               /* Offset 1 [2] */
	uint8_t reserved1:5;              /* Offset 1 [7:3] */
	uint16_t packet_type:10;          /* Offset 2..3 [9:0] */
	uint16_t ip_hdr_err:1;            /* Offset 2..3 [10] */
	uint16_t udp_len_err:1;           /* Offset 2..3 [11] */
	uint16_t raw_cs_invalid:1;        /* Offset 2..3 [12] */
	uint16_t reserved2:3;             /* Offset 2..3 [15:13] */
	uint16_t packet_len:14;           /* Offset 4..5 [13:0] */
	uint16_t generation:1;            /* Offset 4..5 [14] */
	uint16_t buffer_queue_id:1;       /* Offset 4..5 [15] */
	uint16_t header_len:10;           /* Offset 6..7 [9:0] */
	uint16_t rsc:1;                   /* Offset 6..7 [10] */
	uint16_t split_header:1;          /* Offset 6..7 [11] */
	uint16_t reserved3:4;             /* Offset 6..7 [15:12] */
	uint8_t descriptor_done:1;        /* Offset 8 [0] */
	uint8_t end_of_packet:1;          /* Offset 8 [1] */
	uint8_t header_buffer_overflow:1;/* Offset 8 [2] */
	uint8_t l3_l4_processed:1;        /* Offset 8 [3] */
	uint8_t csum_ip_err:1;            /* Offset 8 [4] */
	uint8_t csum_l4_err:1;            /* Offset 8 [5] */
	uint8_t csum_external_ip_err:1;   /* Offset 8 [6] */
	uint8_t csum_external_udp_err:1;  /* Offset 8 [7] */
	uint8_t status_error1;            /* Offset 9 */
	uint8_t reserved5;                /* Offset 10 */
	uint8_t ts_sub_nsecs_low;         /* Offset 11 */
	uint16_t buf_id;                  /* Offset 12..13 */
	union {
		uint16_t raw_cs;              /* Offset 14..15 */
		uint16_t rsc_seg_len;
	};
	uint32_t hash;                    /* Offset 16..19 */
	uint32_t reserved6;               /* Offset 20..23 */
	uint32_t reserved7;               /* Offset 24..27 */
	uint32_t ts;                      /* Offset 28..31 */
} __attribute__((packed, aligned(32)));

_Static_assert(offsetof(struct gve_registers, device_status) == 0,
	       "device_status offset");
_Static_assert(offsetof(struct gve_registers, driver_status) == 4,
	       "driver_status offset");
_Static_assert(offsetof(struct gve_registers, max_tx_queues) == 8,
	       "max_tx_queues offset");
_Static_assert(offsetof(struct gve_registers, max_rx_queues) == 12,
	       "max_rx_queues offset");
_Static_assert(offsetof(struct gve_registers, adminq_pfn) == 16,
	       "adminq_pfn offset");
_Static_assert(offsetof(struct gve_registers, adminq_doorbell) == 20,
	       "adminq_doorbell offset");
_Static_assert(offsetof(struct gve_registers, adminq_event_counter) == 24,
	       "adminq_event_counter offset");
_Static_assert(offsetof(struct gve_registers, driver_version) == 31,
	       "driver_version offset");	/* BAR0 + 0x1f */
_Static_assert(offsetof(struct gve_registers, adminq_base_address_hi) == 32,
	       "adminq_base_address_hi offset");
_Static_assert(offsetof(struct gve_registers, adminq_base_address_lo) == 36,
	       "adminq_base_address_lo offset");
_Static_assert(offsetof(struct gve_registers, adminq_length) == 40,
	       "adminq_length offset");
_Static_assert(sizeof(struct gve_driver_info) == 304, "gve_driver_info size");
_Static_assert(sizeof(struct gve_device_descriptor) == 40, "gve_device_descriptor size");
_Static_assert(sizeof(struct gve_device_option) == 8, "gve_device_option size");
_Static_assert(sizeof(struct gve_irq_db) == 64, "gve_irq_db size");
_Static_assert(sizeof(struct gve_queue_resources) == 64, "gve_queue_resources size");
_Static_assert(sizeof(struct gve_ptype_lut) == 2048, "gve_ptype_lut size");
_Static_assert(sizeof(struct gve_adminq_configure_device_resources) == 40, "configure_device_resources size");
_Static_assert(sizeof(struct gve_adminq_register_page_list) == 24, "register_page_list size");
_Static_assert(sizeof(struct gve_adminq_create_tx_queue) == 48, "create_tx_queue size");
_Static_assert(sizeof(struct gve_adminq_create_rx_queue) == 56, "create_rx_queue size");
_Static_assert(sizeof(struct gve_adminq_get_ptype_map) == 16, "get_ptype_map size");
_Static_assert(sizeof(union gve_adminq_command) == 64, "gve_adminq_command size");
_Static_assert(sizeof(union gve_tx_desc) == 16, "gve_tx_desc size");
_Static_assert(sizeof(union gve_rx_data_slot) == 8, "gve_rx_data_slot size");
_Static_assert(sizeof(struct gve_rx_desc) == 64, "gve_rx_desc size");
_Static_assert(sizeof(struct gve_tx_pkt_desc_dqo) == 16, "gve_tx_pkt_desc_dqo size");
_Static_assert(sizeof(struct gve_tx_general_context_desc_dqo) == 16, "gve_tx_general_context_desc_dqo size");
_Static_assert(sizeof(union gve_tx_desc_dqo) == 16, "union gve_tx_desc_dqo size");
_Static_assert(sizeof(struct gve_tx_compl_desc) == 8, "gve_tx_compl_desc size");
_Static_assert(sizeof(struct gve_rx_desc_dqo) == 32, "gve_rx_desc_dqo size");
_Static_assert(sizeof(struct gve_rx_compl_desc_dqo) == 32, "gve_rx_compl_desc_dqo size");

static inline void gve_adminq_fill_configure_resources(
	union gve_adminq_command *cmd,
	uint64_t counter_array_paddr,
	uint64_t irq_db_paddr,
	uint32_t num_counters,
	uint32_t num_irq_dbs,
	uint8_t queue_format)
{
	__builtin_memset(cmd, 0, sizeof(*cmd));
	cmd->opcode = __builtin_bswap32(GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES);
	cmd->configure_device_resources.counter_array =
		__builtin_bswap64(counter_array_paddr);
	cmd->configure_device_resources.irq_db_addr =
		__builtin_bswap64(irq_db_paddr);
	cmd->configure_device_resources.num_counters =
		__builtin_bswap32(num_counters);
	cmd->configure_device_resources.num_irq_dbs =
		__builtin_bswap32(num_irq_dbs);
	cmd->configure_device_resources.irq_db_stride =
		__builtin_bswap32(sizeof(struct gve_irq_db));
	cmd->configure_device_resources.ntfy_blk_msix_base_idx =
		__builtin_bswap32(0);
	cmd->configure_device_resources.queue_format = queue_format;
}

static inline void gve_adminq_fill_create_tx_queue(
	union gve_adminq_command *cmd,
	uint32_t queue_id,
	uint64_t q_res_paddr,
	uint64_t tx_ring_paddr,
	uint32_t qpl_id,
	uint32_t ntfy_id,
	uint64_t tx_comp_ring_paddr,
	uint16_t tx_ring_size,
	uint16_t tx_comp_ring_size)
{
	__builtin_memset(cmd, 0, sizeof(*cmd));
	cmd->opcode = __builtin_bswap32(GVE_ADMINQ_CREATE_TX_QUEUE);
	cmd->create_tx_queue.queue_id = __builtin_bswap32(queue_id);
	cmd->create_tx_queue.queue_resources_addr =
		__builtin_bswap64(q_res_paddr);
	cmd->create_tx_queue.tx_ring_addr =
		__builtin_bswap64(tx_ring_paddr);
	cmd->create_tx_queue.queue_page_list_id =
		__builtin_bswap32(qpl_id);
	cmd->create_tx_queue.ntfy_id = __builtin_bswap32(ntfy_id);
	cmd->create_tx_queue.tx_comp_ring_addr =
		__builtin_bswap64(tx_comp_ring_paddr);
	cmd->create_tx_queue.tx_ring_size =
		__builtin_bswap16(tx_ring_size);
	cmd->create_tx_queue.tx_comp_ring_size =
		__builtin_bswap16(tx_comp_ring_size);
}

static inline void gve_adminq_fill_create_rx_queue(
	union gve_adminq_command *cmd,
	uint32_t queue_id,
	uint32_t index,
	uint32_t ntfy_id,
	uint64_t q_res_paddr,
	uint64_t rx_desc_ring_paddr,
	uint64_t rx_data_ring_paddr,
	uint32_t qpl_id,
	uint16_t rx_ring_size,
	uint16_t packet_buf_size,
	uint16_t rx_buff_ring_size)
{
	__builtin_memset(cmd, 0, sizeof(*cmd));
	cmd->opcode = __builtin_bswap32(GVE_ADMINQ_CREATE_RX_QUEUE);
	cmd->create_rx_queue.queue_id = __builtin_bswap32(queue_id);
	cmd->create_rx_queue.index = __builtin_bswap32(index);
	cmd->create_rx_queue.ntfy_id = __builtin_bswap32(ntfy_id);
	cmd->create_rx_queue.queue_resources_addr =
		__builtin_bswap64(q_res_paddr);
	cmd->create_rx_queue.rx_desc_ring_addr =
		__builtin_bswap64(rx_desc_ring_paddr);
	cmd->create_rx_queue.rx_data_ring_addr =
		__builtin_bswap64(rx_data_ring_paddr);
	cmd->create_rx_queue.queue_page_list_id =
		__builtin_bswap32(qpl_id);
	cmd->create_rx_queue.rx_ring_size =
		__builtin_bswap16(rx_ring_size);
	cmd->create_rx_queue.packet_buffer_size =
		__builtin_bswap16(packet_buf_size);
	cmd->create_rx_queue.rx_buff_ring_size =
		__builtin_bswap16(rx_buff_ring_size);
	cmd->create_rx_queue.enable_rsc = 0;
}

static inline void gve_dqo_fill_tx_general_ctx_desc(
	struct gve_tx_general_context_desc_dqo *desc)
{
	__builtin_memset(desc, 0, sizeof(*desc));
	desc->cmd_dtype.dtype = GVE_TX_GENERAL_CTX_DESC_DTYPE_DQO;
}

static inline void gve_dqo_fill_tx_pkt_desc(
	struct gve_tx_pkt_desc_dqo *desc,
	uint64_t buf_addr,
	uint16_t compl_tag,
	uint16_t buf_size)
{
	__builtin_memset(desc, 0, sizeof(*desc));
	desc->buf_addr = buf_addr;
	desc->dtype = GVE_TX_PKT_DESC_DTYPE_DQO;
	desc->end_of_packet = 1;
	desc->checksum_offload_enable = 0;
	desc->report_event = ((compl_tag & 31U) == 1U) ? 1U : 0U;
	desc->compl_tag = compl_tag;
	desc->buf_size = buf_size & 0x3fffU;
}

static inline void gve_gqi_fill_tx_pkt_desc(
	union gve_tx_desc *desc,
	uint64_t addr,
	uint16_t len)
{
	__builtin_memset(desc, 0, sizeof(*desc));
	desc->pkt.type_flags = GVE_TXD_STD;
	desc->pkt.desc_cnt = 1;
	desc->pkt.len = __builtin_bswap16(len);
	desc->pkt.seg_len = __builtin_bswap16(len);
	desc->pkt.addr = __builtin_bswap64(addr);
}

/*
 * Shared datapath logic helpers. Both the driver and the host unit
 * tests use these pure functions. Keep them free of device, memory,
 * and MMIO access.
 */

/*
 * Check a device-reported ring size. The driver masks every ring
 * index with cnt - 1, so cnt must be a non-zero power of two.
 * Return 1 when the size is usable and 0 otherwise.
 */
static inline int gve_ring_size_valid(uint16_t cnt)
{
	return (cnt != 0U) && (((uint32_t)cnt & ((uint32_t)cnt - 1U)) == 0U);
}

/* Ethernet header length without VLAN tags. */
#define GVE_ETH_HLEN			14U

/*
 * Cap the advertised maximum MTU to what one receive buffer can
 * carry. The receive path delivers only frames that fit a single
 * buffer, so the payload bound is the buffer size minus the
 * Ethernet header. Jumbo advertisements above the cap must not
 * reach the stack, or oversized frames arrive as bogus packets
 * once a peer sends them. VLAN tags eat four more bytes from the
 * margin. Return the smaller of advertised and the cap.
 */
static inline uint16_t gve_max_mtu_capped(uint32_t advertised,
					  uint32_t rx_buf)
{
	uint32_t cap = (rx_buf > GVE_ETH_HLEN) ? (rx_buf - GVE_ETH_HLEN) : 0U;

	return (uint16_t)((advertised < cap) ? advertised : cap);
}

/*
 * Bound check for one TLV header inside the device-descriptor region.
 * off is the byte offset from the start of the descriptor buffer.
 * region is the usable byte count. hdr_sz is the TLV header size.
 * Return 1 when the whole header is inside the region.
 */
static inline int gve_tlv_hdr_fits(uint32_t off, uint32_t region,
				   uint32_t hdr_sz)
{
	return (off <= region) && ((region - off) >= hdr_sz);
}

/*
 * Bound check for the bytes a parser reads from one TLV body. The
 * parser reads struct_sz bytes, so require body_len to cover them
 * and require struct_sz to stay inside the region. A body_len that
 * reaches past the region still allows the read: the bytes the
 * parser touches stay inside the region, and the walk stops when it
 * advances past the region.
 */
static inline int gve_tlv_body_fits(uint32_t off, uint32_t body_len,
				    uint32_t struct_sz, uint32_t region)
{
	return (body_len >= struct_sz) &&
	       gve_tlv_hdr_fits(off, region, struct_sz);
}

/*
 * Check a caller queue-size request against the driver policy.
 * The driver creates each ring at the size the device descriptor
 * reports, and queue_info_get advertises that size as both nb_min
 * and nb_max. nb_desc 0 asks for the driver default. Return 1 when
 * the request is 0 or the fixed size, and 0 for anything else.
 */
static inline int gve_ring_request_ok(uint32_t nb_desc, uint32_t fixed)
{
	return (nb_desc == 0U) || (nb_desc == fixed);
}

/*
 * Report whether the producer may write need more ring entries.
 * head and tail are free-running entry counters. capacity is the
 * largest number of entries that may be outstanding. Callers must
 * keep capacity at or below UINT32_MAX / 2. The uint32 subtraction
 * stays correct when the counters wrap at 2^32.
 */
static inline int gve_ring_has_space(uint32_t head, uint32_t tail,
				     uint32_t need, uint32_t capacity)
{
	return (tail + need - head) <= capacity;
}

/*
 * Producer reserve for one DQO send: two ring entries for the
 * context and packet descriptor pair, plus a gap between device
 * head and driver tail. The gap mirrors GVE_TX_MIN_DESC_PREVENT_
 * CACHE_OVERLAP in the Linux gve driver: the device fetches
 * descriptors a few entries ahead of its reported head.
 */
#define GVE_TX_DQO_CACHE_GAP		4U
#define GVE_TX_DQO_WRITE_RESERVE	(2U + GVE_TX_DQO_CACHE_GAP)

/*
 * Move the TX head toward the ring index that the device reports.
 * new_idx is the absolute ring slot index from a DESC completion
 * tx_head field. The helper picks the free-running counter
 * congruent to new_idx between head and tail. It never moves head
 * past tail. A report congruent with the current head advances
 * nothing, so an ambiguous full-ring report keeps the driver on the
 * safe side.
 */
static inline uint32_t gve_ring_advance_head(uint32_t head, uint32_t tail,
					     uint16_t cnt, uint32_t new_idx)
{
	uint32_t mask = (uint32_t)cnt - 1U;
	uint32_t adv = (new_idx - (head & mask)) & mask;
	uint32_t inflight = tail - head;

	if (adv > inflight)
		return tail;
	return head + adv;
}

/*
 * Chain length accumulator with a hard bound. The total stays in
 * uint32 space, so a chain cannot wrap past a 16-bit bound check.
 */
struct gve_chain_acc {
	uint32_t total;
	uint32_t max;
};

static inline void gve_chain_acc_init(struct gve_chain_acc *acc, uint32_t max)
{
	acc->total = 0U;
	acc->max = max;
}

/*
 * Add one segment length to the chain total. Reject a zero segment,
 * a segment larger than max, and a total larger than max. Return 0
 * on success and -1 on rejection. On rejection the total is unchanged.
 */
static inline int gve_chain_acc_add(struct gve_chain_acc *acc, uint32_t seg_len)
{
	if (seg_len == 0U || seg_len > acc->max)
		return -1;
	if (acc->total + seg_len > acc->max)
		return -1;
	acc->total += seg_len;
	return 0;
}

/*
 * Return the maximum number of GQI TX packets that may be in
 * flight. One descriptor uses one ring slot. In QPL mode the pages
 * repeat every nqp slots when nqp is a power of two, so the smaller
 * bound wins. When nqp is zero or not a power of two the driver
 * cannot bound page aliasing cheaply, so it serializes sends.
 */
static inline uint32_t gve_gqi_tx_capacity(uint32_t cnt, uint32_t nqp,
					   int qpl)
{
	if (!qpl)
		return cnt;
	if (nqp == 0U || (nqp & (nqp - 1U)) != 0U)
		return 1U;
	return (nqp < cnt) ? nqp : cnt;
}

/* Ethernet minimum frame length. The driver pads short TX frames up. */
#define GVE_TX_MIN_FRAME_LEN		60U

/* Return the transmit length after minimum-frame padding. */
static inline uint32_t gve_tx_pad_len(uint32_t total)
{
	return (total < GVE_TX_MIN_FRAME_LEN) ? GVE_TX_MIN_FRAME_LEN : total;
}

/*
 * Decide whether a received frame fits the netbuf data area.
 * pkt_len is the frame length. cap is the writable space from the
 * netbuf data pointer to the end of the buffer: buflen minus
 * headroom. Do not pass uk_netbuf_tailroom() here. The lwIP glue
 * presets len to the whole receive area (lwip_alloc_netbuf), so
 * tailroom is always 0 on an RX netbuf and would drop every frame.
 * Return 1 when the copy is safe and 0 when the driver must drop
 * the frame.
 */
static inline int gve_rx_fits_netbuf(uint32_t pkt_len, uint32_t cap)
{
	return (pkt_len > 0U) && (pkt_len <= cap);
}

/*
 * Decide whether to ring the DQO RX buffer refill doorbell.
 * unnotified counts reposts since the last doorbell. Ring when the
 * count reaches the threshold. The caller also rings once per
 * receive call when unnotified reposts remain.
 */
static inline int gve_rx_refill_should_ring(uint32_t unnotified,
					    uint32_t thresh)
{
	return unnotified >= thresh;
}

/*
 * Decide whether the device reports an established link. Pass the
 * host-endian value of the big-endian BAR0 device_status register.
 * Passing cloud runs read device_status = 0x00000004 with the link
 * up, so bit GVE_DEVICE_STATUS_LINK_STATUS_MASK is the link bit on
 * this device. Return 1 when the link is up and 0 when down.
 */
static inline int gve_link_is_up(uint32_t dev_status)
{
	return (dev_status & GVE_DEVICE_STATUS_LINK_STATUS_MASK) != 0U;
}

/*
 * Decide whether one QPL registration stays inside the device page
 * budget. max_registered_pages comes from the device descriptor. A
 * device that reports 0 sets no limit, so the check passes. total is
 * the page count the driver already registered. Return 1 when the
 * registration fits and 0 when the driver must refuse it.
 */
static inline int gve_qpl_within_limit(uint64_t total, uint64_t num_pages,
				       uint64_t max_pages)
{
	if (max_pages == 0ULL)
		return 1;
	return (total + num_pages) <= max_pages;
}

#endif /* __UK_DRV_GVE_H__ */
