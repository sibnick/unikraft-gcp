/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host unit tests for Google Virtual Ethernet (gVNIC) protocol structures,
 * Admin Queue command builders, GQI/DQO descriptor ring logic, TX free-slot
 * accounting, chain bound checks, RX tailroom decisions, and refill
 * doorbell thresholds.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <gve.h>

/*
 * Struct layout stays verified by the _Static_assert checks in
 * gve.h. They fire at compile time on every build that includes the
 * header, this test included. A runtime copy repeated them without
 * adding coverage. Ticket c1a76baff0 N3.
 */

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
	assert(__builtin_bswap16(rx_cmd.create_rx_queue.rx_buff_ring_size) == 1024U);
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
	struct gve_tx_general_context_desc_dqo ctx_desc;
	struct gve_rx_compl_desc_dqo rx_ring[4];
	uint32_t head = 0;
	uint8_t cur_gen = 1;
	int step;

	gve_dqo_fill_tx_general_ctx_desc(&ctx_desc);
	assert(ctx_desc.cmd_dtype.dtype == GVE_TX_GENERAL_CTX_DESC_DTYPE_DQO);

	gve_dqo_fill_tx_pkt_desc(&tx_desc, 0xdeadbeef000ULL, 1U, 1514U);
	assert(tx_desc.buf_addr == 0xdeadbeef000ULL);
	assert(tx_desc.dtype == GVE_TX_PKT_DESC_DTYPE_DQO);
	assert(tx_desc.end_of_packet == 1U);
	assert(tx_desc.report_event == 1U);
	assert(tx_desc.compl_tag == 1U);
	assert(tx_desc.buf_size == 1514U);

	gve_dqo_fill_tx_pkt_desc(&tx_desc, 0xdeadbeef000ULL, 42U, 1514U);
	assert(tx_desc.report_event == 0U);
	assert(tx_desc.compl_tag == 42U);

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

/*
 * Pin the DQO TX completion word layout: queue id bits 10:0, type
 * bits 13:11, generation bit 15, bit 14 reserved. The device writes
 * this word in little-endian order. The layout matches the struct
 * gve_tx_compl_desc bitfields in the Linux gve driver.
 * Regression for the Path B3 freeze on run ukg-1027-224408-b3f: a
 * struct with type at bits 14:12 misreads a DESC completion (type
 * 4) as PKT (type 2), so the driver never reclaims TX slots and the
 * ring wedges at capacity.
 */
static void test_dqo_tx_compl_layout(void)
{
	union {
		struct gve_tx_compl_desc c;
		uint16_t w[4];
	} u;

	memset(&u, 0, sizeof(u));
	u.c.id = 0x2a5U;
	u.c.type = GVE_COMPL_TYPE_DQO_DESC;
	u.c.generation = 1U;
	/* gen bit 15, type 4 at bits 13:11 (0x2000), id 0x2a5. */
	assert(u.w[0] == (uint16_t)0xa2a5U);
	assert(u.c.id == 0x2a5U);
	assert(u.c.type == GVE_COMPL_TYPE_DQO_DESC);
	assert(u.c.generation == 1U);

	/*
	 * A DESC completion as the device writes it: type 4 at bits
	 * 13:11, queue id 3. The old struct read bits 14:12 and saw
	 * type 2 (PKT), which skipped reclaim.
	 */
	memset(&u, 0, sizeof(u));
	u.w[0] = (uint16_t)((GVE_COMPL_TYPE_DQO_DESC << 11) | 3U);
	assert(((u.w[0] >> 12) & 7U) == GVE_COMPL_TYPE_DQO_PKT);
	assert(u.c.type == GVE_COMPL_TYPE_DQO_DESC);
	assert(u.c.id == 3U);

	/* Device value with type PKT (0x2 << 11) and id 1. */
	memset(&u, 0, sizeof(u));
	u.w[0] = (uint16_t)0x1001U;
	assert(u.c.generation == 0U);
	assert(u.c.type == GVE_COMPL_TYPE_DQO_PKT);
	assert(u.c.id == 1U);

	/* REINJECTION (0x3) must land in bits 13:11, not 14:12. */
	memset(&u, 0, sizeof(u));
	u.c.type = GVE_COMPL_TYPE_DQO_REINJECTION;
	assert(u.w[0] == (uint16_t)0x1800U);

	/*
	 * A DESC completion reports the device head in tx_head, the
	 * little-endian second word. The id stays a queue number and
	 * must never be used as a ring index.
	 */
	memset(&u, 0, sizeof(u));
	u.c.type = GVE_COMPL_TYPE_DQO_DESC;
	u.c.tx_head = 77U;
	assert(u.w[1] == 77U);
	assert(u.c.tx_head == 77U);
	assert(u.c.completion_tag == 77U);
	assert(u.c.id == 0U);
}

static void test_ring_size_validation(void)
{
	/* Real GCP devices report 1024 for every ring. */
	assert(gve_ring_size_valid(1024U));
	assert(gve_ring_size_valid(1U));
	assert(gve_ring_size_valid(2U));
	assert(gve_ring_size_valid(32U));
	assert(gve_ring_size_valid(32768U));

	/* Zero and non-power-of-two sizes must be rejected. Ticket 9d1de75d2c. */
	assert(!gve_ring_size_valid(0U));
	assert(!gve_ring_size_valid(3U));
	assert(!gve_ring_size_valid(100U));
	assert(!gve_ring_size_valid(1023U));
	assert(!gve_ring_size_valid(1025U));
	assert(!gve_ring_size_valid(0xffffU));

	/* With cnt = 0 the mask becomes 0xffff. Show the hazard we reject. */
	assert((uint16_t)(0U - 1U) == 0xffffU);
}

/*
 * DQO writes two entries per packet: one context plus one packet.
 * The producer keeps the Linux gve reserve: two entries plus the
 * four-entry cache-overlap gap, against a capacity of cnt - 1.
 */
static void test_tx_ring_full_refusal_dqo(void)
{
	const uint16_t cnt = 16;
	const uint32_t cap = (uint32_t)cnt - 1U;
	uint32_t head = 0;
	uint32_t tail = 0;
	int sent = 0;

	/* Fill the ring. Entries up to 9 outstanding allow 5 sends. */
	while (gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE, cap)) {
		assert(sent < 5);
		tail += 2U;
		sent++;
	}
	assert(sent == 5);
	assert(tail == 10U);
	/* No room: tail + 6 must stay inside the cnt - 1 window. */
	assert(!gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE, cap));

	/*
	 * The device consumed the whole ring and reports tx_head 0.
	 * That report is congruent with the current head, so the
	 * driver keeps head at 0 and still refuses. Safe-side policy.
	 */
	head = gve_ring_advance_head(head, tail, cnt, 0U);
	assert(head == 0U);
	assert(!gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE, cap));

	/* Device consumed two entries: one packet slot pair frees up. */
	head = gve_ring_advance_head(head, tail, cnt, 2U);
	assert(head == 2U);
	assert(gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE, cap));
	tail += 2U;
	assert(!gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE, cap));

	/* A report at ring index 4 reclaims linearly: head 2 -> 4. */
	head = gve_ring_advance_head(head, tail, cnt, 4U);
	assert(head == 4U);
	/* A legal lagging report: head 4 -> 11, still behind tail 12. */
	head = gve_ring_advance_head(head, tail, cnt, 11U);
	assert(head == 11U);
	/* Bogus report far ahead: congruence puts it past tail. Clamp. */
	head = gve_ring_advance_head(head, tail, cnt, 10U);
	assert(head == tail);
}

/*
 * Soak model of the DQO TX ring. The device consumes a pseudo-random
 * number of entries and reports DESC completions. Assert the driver
 * never writes a slot whose previous occupant is still in flight.
 */
static void test_dqo_tx_ring_soak(void)
{
	const uint16_t cnt = 16;
	const uint32_t mask = cnt - 1U;
	uint32_t head = 0;
	uint32_t tail = 0;
	uint32_t dev_done = 0;
	int32_t writer[16];
	uint32_t rnd = 12345U;
	int round;
	uint32_t s0;
	uint32_t s1;

	for (round = 0; round < (int)cnt; round++)
		writer[round] = -1;

	for (round = 0; round < 5000; round++) {
		/* Device drains zero to three entries. */
		rnd = rnd * 1103515245U + 12345U;
		if (dev_done < tail) {
			uint32_t drain = (rnd >> 16) & 3U;

			if (drain > tail - dev_done)
				drain = tail - dev_done;
			dev_done += drain;
			/* Device reports tx_head in a DESC completion. */
			head = gve_ring_advance_head(head, tail, cnt,
						     dev_done & mask);
			assert(head <= tail);
		}

		if (!gve_ring_has_space(head, tail, GVE_TX_DQO_WRITE_RESERVE,
					(uint32_t)cnt - 1U))
			continue;	/* full ring: refuse the send */

		/* Both target slots must be reclaimed before the write. */
		s0 = tail & mask;
		s1 = (tail + 1U) & mask;
		assert(writer[s0] < 0 || (uint32_t)writer[s0] < head);
		assert(writer[s1] < 0 || (uint32_t)writer[s1] < head);
		writer[s0] = (int32_t)tail;
		writer[s1] = (int32_t)(tail + 1U);
		tail += 2U;
	}
	assert(tail > 500U);	/* forward progress despite random drains */
}

/*
 * GQI posts one descriptor per packet. The window is bounded by the
 * ring size and, for GQI_QPL, also by the QPL page count. A shared
 * page must not be rewritten while its older packet is still in
 * flight.
 */
static void test_gqi_tx_ring_window(void)
{
	uint32_t cnt = 8;
	uint32_t nqp = 2;
	uint32_t cap = gve_gqi_tx_capacity(cnt, nqp, 1);
	uint32_t req = 0;
	uint32_t done = 0;
	int sent = 0;

	assert(cap == 2U);
	while (gve_ring_has_space(done, req, 1U, cap)) {
		assert(sent < 2);
		req++;
		sent++;
	}
	assert(sent == 2);
	assert(!gve_ring_has_space(done, req, 1U, cap));

	/* Counter reports one completed packet: window opens by one. */
	done = 1U;
	assert(gve_ring_has_space(done, req, 1U, cap));
	req++;
	assert(!gve_ring_has_space(done, req, 1U, cap));

	/* Ring-sized window when the format owns private buffers. */
	cap = gve_gqi_tx_capacity(cnt, nqp, 0);
	assert(cap == 8U);
	req = 0;
	done = 0;
	sent = 0;
	while (gve_ring_has_space(done, req, 1U, cap)) {
		req++;
		sent++;
	}
	assert(sent == 8);
	done = 1U;
	assert(gve_ring_has_space(done, req, 1U, cap));

	/* QPL window sizing. Real devices report 64 pages with 1024 slots. */
	assert(gve_gqi_tx_capacity(1024U, 64U, 1) == 64U);
	/* More pages than slots: the ring bound wins. */
	assert(gve_gqi_tx_capacity(8U, 16U, 1) == 8U);
	/* Zero or non-power-of-two page counts serialize the sends. */
	assert(gve_gqi_tx_capacity(1024U, 0U, 1) == 1U);
	assert(gve_gqi_tx_capacity(8U, 3U, 1) == 1U);

	/* Free-running counters stay correct across the 2^32 wrap. */
	assert(gve_ring_has_space(0xfffffff0U, 0x00000006U, 1U, 32U));
	assert(!gve_ring_has_space(0xfffffff0U, 0x00000012U, 1U, 32U));
}

/* Ethernet minimum-frame padding around the 60-byte boundary. */
static void test_tx_pad_len(void)
{
	assert(gve_tx_pad_len(1U) == 60U);
	assert(gve_tx_pad_len(59U) == 60U);
	assert(gve_tx_pad_len(60U) == 60U);
	assert(gve_tx_pad_len(61U) == 61U);
	assert(gve_tx_pad_len(GVE_DEFAULT_RX_BUFFER_SIZE) ==
	       GVE_DEFAULT_RX_BUFFER_SIZE);
}

/*
 * Chain totals must not wrap. Ticket 816c608e68: a chain of 33 x
 * 2048 bytes totals 2048 + 65536. A uint16 accumulator wraps to 2048
 * and passes the old bound check. The uint32 accumulator rejects it.
 */
static void test_chain_total_wrap(void)
{
	struct gve_chain_acc acc;
	uint32_t i;

	/* Show the historic 16-bit wrap that passed the check. */
	assert((uint16_t)(2048U + 65536U) == 2048U);

	/* 64 x 1056 = 67584 = 2048 + 65536. Reject at the second segment. */
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	for (i = 0; i < 64U; i++) {
		if (gve_chain_acc_add(&acc, 1056U) != 0)
			break;
	}
	assert(i == 1U);
	assert(acc.total == 1056U);

	/* A chain that totals exactly 65536 would wrap to 0. Reject early. */
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	for (i = 0; i < 64U; i++) {
		if (gve_chain_acc_add(&acc, 1024U) != 0)
			break;
	}
	assert(i == 2U);

	/* Segment bounds: max passes, max + 1 fails, zero fails. */
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	assert(gve_chain_acc_add(&acc, GVE_DEFAULT_RX_BUFFER_SIZE) == 0);
	assert(gve_chain_acc_add(&acc, 1U) != 0);
	assert(acc.total == GVE_DEFAULT_RX_BUFFER_SIZE);
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	assert(gve_chain_acc_add(&acc, GVE_DEFAULT_RX_BUFFER_SIZE + 1U) != 0);
	assert(gve_chain_acc_add(&acc, 60000U) != 0);
	assert(gve_chain_acc_add(&acc, 0U) != 0);
	assert(acc.total == 0U);

	/* A valid small chain still passes. */
	gve_chain_acc_init(&acc, GVE_DEFAULT_RX_BUFFER_SIZE);
	assert(gve_chain_acc_add(&acc, 10U) == 0);
	assert(gve_chain_acc_add(&acc, 20U) == 0);
	assert(gve_chain_acc_add(&acc, 1500U) == 0);
	assert(acc.total == 1530U);
}

/*
 * The RX copy must respect the netbuf tailroom, not the 2048-byte
 * device buffer. The driver advertises max_mtu up to 8892.
 * Ticket 2c87f0d431.
 */
static void test_rx_fits_netbuf(void)
{
	assert(gve_rx_fits_netbuf(60U, 2048U));
	assert(gve_rx_fits_netbuf(1514U, 1514U));
	assert(gve_rx_fits_netbuf(2048U, 2048U));

	/* Frame one byte past the usable data area: drop decision. */
	assert(!gve_rx_fits_netbuf(1515U, 1514U));
	assert(!gve_rx_fits_netbuf(2048U, 2047U));
	assert(!gve_rx_fits_netbuf(2048U, 1514U));

	/* Zero-length frame is never delivered. */
	assert(!gve_rx_fits_netbuf(0U, 2048U));
}

/*
 * The advertised MTU must never exceed one receive buffer minus the
 * Ethernet header. Jumbo advertisements are clipped; smaller ones
 * pass through. Ticket 7d17e44863 item M4.
 */
static void test_max_mtu_cap(void)
{
	/* Jumbo 8892 clipped to 2048 - 14. */
	assert(gve_max_mtu_capped(8892U, 2048U) == 2034U);
	/* Exactly at the cap and below: unchanged. */
	assert(gve_max_mtu_capped(2034U, 2048U) == 2034U);
	assert(gve_max_mtu_capped(1410U, 2048U) == 1410U);
	/* Degenerate buffer sizes cap to zero, never to a large value. */
	assert(gve_max_mtu_capped(1500U, 14U) == 0U);
	assert(gve_max_mtu_capped(1500U, 0U) == 0U);
}

/*
 * TLV region bound checks. A parser must read a fixed struct only
 * when it is covered by the body length and stays inside the region.
 * Ticket 7d17e44863 item M3.
 */
static void test_tlv_bounds(void)
{
	const uint32_t region = 152U;	/* real device total_length */

	/* Header at the very end of the region still fits exactly. */
	assert(gve_tlv_hdr_fits(region - 8U, region, 8U));
	/* One byte short of a header: refuse. */
	assert(!gve_tlv_hdr_fits(region - 7U, region, 8U));
	/* Offset past the region: refuse, no wraparound. */
	assert(!gve_tlv_hdr_fits(region + 1U, region, 8U));

	/* Body fully covered: 8 bytes of body with a 4-byte struct. */
	assert(gve_tlv_body_fits(48U, 8U, 4U, region));
	/* Struct bigger than declared body: refuse. */
	assert(!gve_tlv_body_fits(48U, 2U, 4U, region));
	/* Struct would cross the region end: refuse. */
	assert(!gve_tlv_body_fits(region - 8U, 16U, 12U, region));
	/* Declared body past the region, struct inside the region: the
	 * read is safe, so it passes. The walk stops at the next header
	 * check. */
	assert(gve_tlv_body_fits(region - 4U, 64U, 4U, region));
}

/*
 * Queue size requests: only the driver default (0) or the fixed
 * device size pass. Ticket 7d17e44863 item M6.
 */
static void test_ring_request(void)
{
	assert(gve_ring_request_ok(0U, 1024U));	/* driver default */
	assert(gve_ring_request_ok(1024U, 1024U));
	assert(!gve_ring_request_ok(32U, 1024U));
	assert(!gve_ring_request_ok(2048U, 1024U));
	assert(!gve_ring_request_ok(1U, 0U));
}

/*
 * Regression for the Path B3 failure on run ukg-1027-221230. The lwIP
 * glue (lwip_alloc_netbuf) presets len to buflen minus headroom, so
 * uk_netbuf_tailroom() returns 0 on every RX netbuf. A tailroom-based
 * guard drops all received traffic. The driver must pass the capacity
 * from the data pointer to the end of the buffer instead.
 */
static void test_rx_fits_netbuf_lwip_glue(void)
{
	uint32_t buflen = 2048U;
	uint32_t headroom = 0U;
	uint32_t len = buflen - headroom;   /* glue preset */
	uint32_t tailroom = buflen - headroom - len;
	uint32_t cap = buflen - headroom;   /* what the driver passes */

	assert(tailroom == 0U);
	/* The old tailroom check would drop a 60-byte echo reply. */
	assert(!gve_rx_fits_netbuf(60U, tailroom));
	/* The capacity check accepts it. */
	assert(gve_rx_fits_netbuf(60U, cap));
	assert(gve_rx_fits_netbuf(2048U, cap));
	assert(!gve_rx_fits_netbuf(2049U, cap));
}

/*
 * DQO RX refill doorbell policy. Ring at GVE_RX_BUF_THRESH_DQO and
 * flush unnotified reposts at the end of the receive call.
 * Ticket 4181b38c69.
 */
static void test_rx_refill_doorbell(void)
{
	uint32_t pending = 0;
	uint32_t rings = 0;
	int i;

	/* 31 reposts stay below the threshold. No threshold doorbell. */
	for (i = 0; i < 31; i++) {
		pending++;
		if (gve_rx_refill_should_ring(pending, GVE_RX_BUF_THRESH_DQO)) {
			rings++;
			pending = 0;
		}
	}
	assert(rings == 0U);
	assert(pending == 31U);

	/* End-of-call flush must notify the remainder. */
	if (pending != 0U) {
		rings++;
		pending = 0;
	}
	assert(rings == 1U);
	assert(pending == 0U);

	/* Exactly 32 reposts ring once via the threshold path. */
	pending = 0;
	rings = 0;
	for (i = 0; i < 32; i++) {
		pending++;
		if (gve_rx_refill_should_ring(pending, GVE_RX_BUF_THRESH_DQO)) {
			rings++;
			pending = 0;
		}
	}
	assert(rings == 1U);
	assert(pending == 0U);

	/* 33 reposts: one threshold ring plus one pending for the flush. */
	pending = 0;
	rings = 0;
	for (i = 0; i < 33; i++) {
		pending++;
		if (gve_rx_refill_should_ring(pending, GVE_RX_BUF_THRESH_DQO)) {
			rings++;
			pending = 0;
		}
	}
	assert(rings == 1U);
	assert(pending == 1U);

	assert(!gve_rx_refill_should_ring(0U, GVE_RX_BUF_THRESH_DQO));
	assert(gve_rx_refill_should_ring(31U, GVE_RX_BUF_THRESH_DQO) == 0);
	assert(gve_rx_refill_should_ring(32U, GVE_RX_BUF_THRESH_DQO) != 0);
	assert(gve_rx_refill_should_ring(33U, GVE_RX_BUF_THRESH_DQO) != 0);
}

/*
 * Link state gate for GATE_GVE_PASS in netdev_start. The caller
 * passes the host-endian BAR0 device_status value. Passing cloud
 * runs report 0x00000004 with the link up. Only
 * GVE_DEVICE_STATUS_LINK_STATUS_MASK decides. Ticket c1a76baff0 L3.
 */
static void test_link_state(void)
{
	assert(gve_link_is_up(0x00000004U));	/* passing cloud runs */
	assert(gve_link_is_up(0x0000000CU));	/* link plus other bits */
	assert(!gve_link_is_up(0x00000000U));	/* link down */
	assert(!gve_link_is_up(0x00000001U));	/* done bit only */
	assert(!gve_link_is_up(0x00000002U));	/* reset bit only */
}

/*
 * QPL page budget enforcement before REGISTER_PAGE_LIST. The
 * passing c3 device reports 16384 pages. A device that reports 0
 * sets no limit. Ticket c1a76baff0 L4.
 */
static void test_qpl_limit(void)
{
	/* Driver worst case: 1024 RX pages plus 64 TX pages. */
	assert(gve_qpl_within_limit(0ULL, 1024ULL, 16384ULL));
	assert(gve_qpl_within_limit(1024ULL, 64ULL, 16384ULL));
	/* The last page inside the budget passes. */
	assert(gve_qpl_within_limit(16320ULL, 64ULL, 16384ULL));
	/* One page over the budget fails. */
	assert(!gve_qpl_within_limit(16321ULL, 64ULL, 16384ULL));
	assert(!gve_qpl_within_limit(16384ULL, 1ULL, 16384ULL));
	/* max_pages 0 means the device set no limit. */
	assert(gve_qpl_within_limit(0xFFFFFFFFFFFFFFFFULL, 1ULL, 0ULL));
}

int main(void)
{
	test_adminq_configure_resources();
	test_adminq_create_queues();
	test_gqi_datapath_descriptors();
	test_dqo_datapath_descriptors();
	test_dqo_tx_compl_layout();
	test_ring_size_validation();
	test_tx_ring_full_refusal_dqo();
	test_dqo_tx_ring_soak();
	test_gqi_tx_ring_window();
	test_tx_pad_len();
	test_chain_total_wrap();
	test_rx_fits_netbuf();
	test_rx_fits_netbuf_lwip_glue();
	test_rx_refill_doorbell();
	test_max_mtu_cap();
	test_tlv_bounds();
	test_ring_request();
	test_link_state();
	test_qpl_limit();
	printf("test_gve: all unit tests passed\n");
	return 0;
}
