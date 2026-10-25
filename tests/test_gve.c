/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host unit tests for Google Virtual Ethernet (gVNIC) protocol structures,
 * Admin Queue command builders, and GQI/DQO descriptor ring logic.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <gve.h>

static void test_struct_sizes(void)
{
	assert(sizeof(struct gve_registers) == 42);
	assert(sizeof(struct gve_driver_info) == 304);
	assert(sizeof(struct gve_device_descriptor) == 40);
	assert(sizeof(struct gve_device_option) == 8);
	assert(sizeof(struct gve_irq_db) == 64);
	assert(sizeof(struct gve_queue_resources) == 64);
	assert(sizeof(struct gve_ptype_lut) == 2048);
	assert(sizeof(struct gve_adminq_configure_device_resources) == 40);
	assert(sizeof(struct gve_adminq_register_page_list) == 24);
	assert(sizeof(struct gve_adminq_create_tx_queue) == 48);
	assert(sizeof(struct gve_adminq_create_rx_queue) == 56);
	assert(sizeof(struct gve_adminq_get_ptype_map) == 16);
	assert(sizeof(union gve_adminq_command) == 64);
	assert(sizeof(union gve_tx_desc) == 16);
	assert(sizeof(union gve_rx_data_slot) == 8);
	assert(sizeof(struct gve_rx_desc) == 64);
	assert(sizeof(struct gve_tx_pkt_desc_dqo) == 16);
	assert(sizeof(struct gve_tx_general_context_desc_dqo) == 16);
	assert(sizeof(struct gve_tx_compl_desc) == 8);
	assert(sizeof(struct gve_rx_desc_dqo) == 32);
	assert(sizeof(struct gve_rx_compl_desc_dqo) == 32);
}

static void test_adminq_configure_resources(void)
{
	union gve_adminq_command cmd;

	gve_adminq_fill_configure_resources(&cmd, 0x100000ULL, 0x200000ULL,
					    8U, 2U, GVE_DQO_RDA_FORMAT);

	assert(__builtin_bswap32(cmd.opcode) ==
	       GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES);
	assert(cmd.status == GVE_ADMINQ_COMMAND_UNSET);
	assert(__builtin_bswap64(cmd.configure_device_resources.counter_array) ==
	       0x100000ULL);
	assert(__builtin_bswap64(cmd.configure_device_resources.irq_db_addr) ==
	       0x200000ULL);
	assert(__builtin_bswap32(cmd.configure_device_resources.num_counters) == 8U);
	assert(__builtin_bswap32(cmd.configure_device_resources.num_irq_dbs) == 2U);
	assert(__builtin_bswap32(cmd.configure_device_resources.irq_db_stride) == 64U);
	assert(cmd.configure_device_resources.queue_format == GVE_DQO_RDA_FORMAT);
}

static void test_adminq_create_queues(void)
{
	union gve_adminq_command tx_cmd;
	union gve_adminq_command rx_cmd;

	gve_adminq_fill_create_tx_queue(&tx_cmd, 0U, 0x300000ULL, 0x301000ULL,
					GVE_RAW_ADDRESSING_QPL_ID, 0U,
					0x305000ULL, 1024U, 1024U);

	assert(__builtin_bswap32(tx_cmd.opcode) == GVE_ADMINQ_CREATE_TX_QUEUE);
	assert(__builtin_bswap32(tx_cmd.create_tx_queue.queue_id) == 0U);
	assert(__builtin_bswap64(tx_cmd.create_tx_queue.queue_resources_addr) ==
	       0x300000ULL);
	assert(__builtin_bswap64(tx_cmd.create_tx_queue.tx_ring_addr) ==
	       0x301000ULL);
	assert(__builtin_bswap32(tx_cmd.create_tx_queue.queue_page_list_id) ==
	       GVE_RAW_ADDRESSING_QPL_ID);
	assert(__builtin_bswap32(tx_cmd.create_tx_queue.ntfy_id) == 0U);
	assert(__builtin_bswap64(tx_cmd.create_tx_queue.tx_comp_ring_addr) ==
	       0x305000ULL);
	assert(__builtin_bswap16(tx_cmd.create_tx_queue.tx_ring_size) == 1024U);
	assert(__builtin_bswap16(tx_cmd.create_tx_queue.tx_comp_ring_size) == 1024U);

	gve_adminq_fill_create_rx_queue(&rx_cmd, 0U, 0U, 1U, 0x400000ULL,
					0x401000ULL, 0x410000ULL,
					GVE_RAW_ADDRESSING_QPL_ID,
					1024U, GVE_DEFAULT_RX_BUFFER_SIZE,
					1024U);

	assert(__builtin_bswap32(rx_cmd.opcode) == GVE_ADMINQ_CREATE_RX_QUEUE);
	assert(__builtin_bswap32(rx_cmd.create_rx_queue.queue_id) == 0U);
	assert(__builtin_bswap32(rx_cmd.create_rx_queue.index) == 0U);
	assert(__builtin_bswap32(rx_cmd.create_rx_queue.ntfy_id) == 1U);
	assert(__builtin_bswap64(rx_cmd.create_rx_queue.queue_resources_addr) ==
	       0x400000ULL);
	assert(__builtin_bswap64(rx_cmd.create_rx_queue.rx_desc_ring_addr) ==
	       0x401000ULL);
	assert(__builtin_bswap64(rx_cmd.create_rx_queue.rx_data_ring_addr) ==
	       0x410000ULL);
	assert(__builtin_bswap32(rx_cmd.create_rx_queue.queue_page_list_id) ==
	       GVE_RAW_ADDRESSING_QPL_ID);
	assert(__builtin_bswap16(rx_cmd.create_rx_queue.rx_ring_size) == 1024U);
	assert(__builtin_bswap16(rx_cmd.create_rx_queue.packet_buffer_size) ==
	       GVE_DEFAULT_RX_BUFFER_SIZE);
	assert(__builtin_bswap16(rx_cmd.create_rx_queue.rx_buff_ring_size) ==
	       1024U);
	assert(rx_cmd.create_rx_queue.enable_rsc == 0U);
}

static void test_gqi_datapath_descriptors(void)
{
	union gve_tx_desc tx_desc;
	uint8_t seq = 1U;
	int i;

	gve_gqi_fill_tx_pkt_desc(&tx_desc, 0x4000ULL, 64U);
	assert(tx_desc.pkt.type_flags == GVE_TXD_STD);
	assert(tx_desc.pkt.desc_cnt == 1U);
	assert(__builtin_bswap16(tx_desc.pkt.len) == 64U);
	assert(__builtin_bswap16(tx_desc.pkt.seg_len) == 64U);
	assert(__builtin_bswap64(tx_desc.pkt.addr) == 0x4000ULL);

	/* Verify 3-bit GQI sequence number cycles 1..7 and never hits 0 */
	for (i = 0; i < 14; i++) {
		uint16_t raw_flags_seq = __builtin_bswap16((uint16_t)seq);

		assert(GVE_SEQNO(raw_flags_seq) == seq);
		assert(seq >= 1U && seq <= 7U);
		seq = gve_next_seqno(seq);
	}
	assert(seq == 1U);
}

static void test_dqo_datapath_descriptors(void)
{
	struct gve_tx_pkt_desc_dqo tx_desc;
	struct gve_rx_compl_desc_dqo rx_ring[4];
	uint32_t head = 0;
	uint8_t cur_gen = 1;
	int step;

	gve_dqo_fill_tx_pkt_desc(&tx_desc, 0xdeadbeef000ULL, 42U, 1514U);
	assert(tx_desc.buf_addr == 0xdeadbeef000ULL);
	assert(tx_desc.dtype == GVE_TX_PKT_DESC_DTYPE_DQO);
	assert(tx_desc.end_of_packet == 1U);
	assert(tx_desc.report_event == 1U);
	assert(tx_desc.compl_tag == 42U);
	assert(tx_desc.buf_size == 1514U);

	/* Simulate DQO completion ring generation-bit flip across wrap-around */
	memset(rx_ring, 0, sizeof(rx_ring));
	for (step = 0; step < 8; step++) {
		uint8_t hw_gen = (step < 4) ? 1U : 0U;

		rx_ring[head].generation = hw_gen;
		rx_ring[head].packet_len = 60U;
		rx_ring[head].buf_id = (uint16_t)head;

		assert(rx_ring[head].generation == cur_gen);
		head = (head + 1U) & 3U;
		if (head == 0U)
			cur_gen ^= 1U;
	}
	assert(head == 0U);
	assert(cur_gen == 1U);
}

int main(void)
{
	test_struct_sizes();
	test_adminq_configure_resources();
	test_adminq_create_queues();
	test_gqi_datapath_descriptors();
	test_dqo_datapath_descriptors();
	printf("test_gve: all unit tests passed\n");
	return 0;
}
