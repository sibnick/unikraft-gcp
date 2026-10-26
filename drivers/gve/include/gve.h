/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Google Virtual Ethernet (gVNIC) driver definitions for Unikraft.
 */

#ifndef __UK_DRV_GVE_H__
#define __UK_DRV_GVE_H__

#include <stdint.h>

#define GVE_PCI_VENDOR_ID		0x1ae0
#define GVE_PCI_DEVICE_ID		0x0042

/* BAR0 register layout (big-endian fields) */
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
} __attribute__((packed));

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

#define GVE_ITR_NO_UPDATE_DQO			(3U << 3)
#define GVE_ITR_ENABLE_BIT_DQO			(1U << 0)

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

struct gve_tx_compl_desc {
	uint16_t id:11;
	uint16_t type:3;
	uint16_t reserved0:1;
	uint16_t generation:1;
	union {
		uint16_t completion_tag;
		uint16_t tx_head;
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

_Static_assert(sizeof(struct gve_registers) == 42, "gve_registers size");
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

#endif /* __UK_DRV_GVE_H__ */
