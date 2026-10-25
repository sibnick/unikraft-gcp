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
	uint8_t padding[5];
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
			struct gve_adminq_verify_driver_compatibility verify_driver_compatibility;
		};
	} __attribute__((packed));
	uint8_t reserved[64];
} __attribute__((packed));

#endif /* __UK_DRV_GVE_H__ */
