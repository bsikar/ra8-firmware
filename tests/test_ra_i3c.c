/**
 * @file test_ra_i3c.c
 * @brief Unit tests for ra_i3c.c (I3C Bus Interface driver)
 *
 * @details
 * Covers the lifecycle / status / IRQ surface that the original
 * scaffold provided, plus the CCC engine (ENTDAA, SETDASA, RSTDAA,
 * generic send/recv), private read / write, and the IBI inbound
 * queue.  Every test resets the simulated MMIO backing store via
 * ``ra_sim_mmap_reset`` so the FIFO ports start zeroed, then drives
 * the driver through one verifiable register sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_i3c_regs.h"
#include "ra_err.h"
#include "ra_i3c.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_i3c_cb_count;
static uint32_t s_i3c_cb_last_mask;

static void stub_i3c_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_i3c_cb_count;
  s_i3c_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_i3c_cb_count     = 0U;
  s_i3c_cb_last_mask = 0U;
}

static void test_init(void)
{
  TEST_BEGIN("i3c init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  /* CECTL.CLKE must be set after init. */
  TEST_ASSERT_EQ((int32_t)1, (int32_t)(ra_i3c()->CECTL & 0x1U));
  /* RSTCTL must be released after init. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_i3c()->RSTCTL);
  TEST_END("i3c init");
}

static void test_deinit(void)
{
  TEST_BEGIN("i3c deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(ra_i3c()->CECTL & 0x1U));
  TEST_END("i3c deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("i3c status read + clear");
  prep();
  ra_i3c()->INST = 0xDEADBEEFU;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_clear_status(0xF0F0F0F0U));
  /* Bits in mask 0xF0F0F0F0 must have been cleared. */
  TEST_ASSERT_EQ((int32_t)(0xDEADBEEFU & ~0xF0F0F0F0U), (int32_t)ra_i3c()->INST);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_get_status(nullptr));
  TEST_END("i3c status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("i3c attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_i3c_attach_handler(stub_i3c_cb, (void*)(uintptr_t)0x13U));
  ra_i3c()->INST = 0xCAFEU;
  ra_i3c_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_i3c_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_i3c_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_attach_handler(nullptr, nullptr));
  ra_i3c()->INST = 0xBEEFU;
  ra_i3c_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_i3c_cb_count);
  TEST_END("i3c attach + dispatch");
}

static void test_set_address(void)
{
  TEST_BEGIN("i3c set address");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  /* Valid 7-bit address: bits [22:16] hold the value, bit 31 marks valid. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_set_address(0x42U));
  const uint32_t expect = (0x42U << 16) | 0x80000000U;
  TEST_ASSERT_EQ((int32_t)expect, (int32_t)ra_i3c()->MSDVAD);
  /* Out-of-range 7-bit dynamic address must be rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_set_address(0x80U));
  TEST_END("i3c set address");
}

static void test_bus_enable(void)
{
  TEST_BEGIN("i3c bus enable");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_bus_enable(true));
  /* BCTL.BUSE is bit 31 per FSP R_I3C0_BCTL_BUSE_Pos = 31. */
  TEST_ASSERT_EQ((int32_t)1, (int32_t)((ra_i3c()->BCTL >> 31) & 0x1U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_bus_enable(false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)((ra_i3c()->BCTL >> 31) & 0x1U));
  TEST_END("i3c bus enable");
}

static void test_power_transition(void)
{
  TEST_BEGIN("i3c power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_exit_stop());
  TEST_END("i3c power transition");
}

/* ---------------------------------------------------------------------------
 * CCC engine: ENTDAA, SETDASA, RSTDAA, generic send/recv.
 * --------------------------------------------------------------------------- */

static void test_dynamic_address_assign(void)
{
  TEST_BEGIN("i3c dynamic_address_assign (ENTDAA)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());

  /* Pre-stage one PID/BCR/DCR response in the simulated NTDTBP0 cell --
   * with a single backing word the driver's two consecutive reads return
   * the same value, so the test only verifies the *first* drained 4 bytes
   * landed in the target.pid[]. */
  ra_i3c()->NTDTBP0 = 0x44332211U;

  ra_i3c_daa_target_t targets[2] = {{.dynamic_address = 0x10U}, {.dynamic_address = 0x11U}};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_dynamic_address_assign(targets, 2U));

  /* Verify the driver issued at least the two-word command descriptor (the
   * cell was zeroed by ``prep()`` and the second word the driver writes is
   * 0, leaving NCMDQP == 0).  The interesting side-effect is the populated
   * targets[] array: pid[0..3] should equal the staged 32-bit word LE. */
  TEST_ASSERT_EQ((int32_t)0x11U, (int32_t)targets[0].pid[0]);
  TEST_ASSERT_EQ((int32_t)0x22U, (int32_t)targets[0].pid[1]);
  TEST_ASSERT_EQ((int32_t)0x33U, (int32_t)targets[0].pid[2]);
  TEST_ASSERT_EQ((int32_t)0x44U, (int32_t)targets[0].pid[3]);
  /* Caller-supplied dynamic_address values must be preserved. */
  TEST_ASSERT_EQ((int32_t)0x10U, (int32_t)targets[0].dynamic_address);
  TEST_ASSERT_EQ((int32_t)0x11U, (int32_t)targets[1].dynamic_address);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_dynamic_address_assign(nullptr, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_i3c_dynamic_address_assign(targets, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_i3c_dynamic_address_assign(targets, 99U));
  TEST_END("i3c dynamic_address_assign (ENTDAA)");
}

static void test_set_dynamic_address(void)
{
  TEST_BEGIN("i3c set_dynamic_address (SETDASA)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_set_dynamic_address(0x12U, 0x34U));
  /* Last write to NCMDQP was the dynamic-address payload byte (cmd2). */
  TEST_ASSERT_EQ((int32_t)0x34U, (int32_t)ra_i3c()->NCMDQP);
  /* Bounds check on both addresses. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_set_dynamic_address(0x80U, 0x10U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_set_dynamic_address(0x10U, 0x80U));
  TEST_END("i3c set_dynamic_address (SETDASA)");
}

static void test_reset_dynamic_addresses(void)
{
  TEST_BEGIN("i3c reset_dynamic_addresses (RSTDAA)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  /* Pre-seed NCMDQP with a sentinel so the driver's two writes (cmd1 +
   * cmd2=0) result in NCMDQP == 0 by the end -- this proves the driver
   * touched the queue at least twice. */
  ra_i3c()->NCMDQP = 0xDEADBEEFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_reset_dynamic_addresses());
  /* RSTDAA cmd2 is zero -> last write. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_i3c()->NCMDQP);
  TEST_END("i3c reset_dynamic_addresses (RSTDAA)");
}

static void test_send_ccc_broadcast(void)
{
  TEST_BEGIN("i3c send_ccc broadcast (ENEC, no payload)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  /* Pre-seed NCMDQP with a sentinel; broadcast ENEC + zero payload causes
   * the driver to write cmd1 then cmd2 = 0, so the cell ends as 0 -- proof
   * the queue saw at least one write. */
  ra_i3c()->NCMDQP = 0xCAFEBABEU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_send_ccc(0x00U, 0U, nullptr, 0U));
  /* Last NCMDQP write was the immediate-payload word (zero for empty). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_i3c()->NCMDQP);
  TEST_END("i3c send_ccc broadcast");
}

static void test_send_ccc_directed_with_payload(void)
{
  TEST_BEGIN("i3c send_ccc directed with payload");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  const uint8_t payload[3] = {0xAAU, 0xBBU, 0xCCU};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_send_ccc(0x80U, 0x42U, payload, 3U));
  /* Last NCMDQP write was the immediate-data word (LE pack of payload). */
  const uint32_t expect_data = 0x00CCBBAAU;
  TEST_ASSERT_EQ((int32_t)expect_data, (int32_t)ra_i3c()->NCMDQP);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_send_ccc(0x80U, 0x42U, nullptr, 3U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_i3c_send_ccc(0x80U, 0x80U, payload, 3U));
  TEST_END("i3c send_ccc directed with payload");
}

static void test_recv_ccc_directed(void)
{
  TEST_BEGIN("i3c recv_ccc directed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  /* Pre-load NTDTBP0 with one word of receive data. */
  ra_i3c()->NTDTBP0 = 0x11223344U;

  uint8_t buf[4]  = {};
  uint8_t got_len = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_recv_ccc(0x8DU, 0x42U, buf, 4U, &got_len));
  TEST_ASSERT_EQ((int32_t)4, (int32_t)got_len);
  TEST_ASSERT_EQ((int32_t)0x44U, (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)0x33U, (int32_t)buf[1]);
  TEST_ASSERT_EQ((int32_t)0x22U, (int32_t)buf[2]);
  TEST_ASSERT_EQ((int32_t)0x11U, (int32_t)buf[3]);
  /* Broadcast CCC must be rejected by recv. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_i3c_recv_ccc(0x07U, 0x42U, buf, 4U, &got_len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_i3c_recv_ccc(0x8DU, 0x42U, nullptr, 4U, &got_len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_i3c_recv_ccc(0x8DU, 0x42U, buf, 4U, nullptr));
  TEST_END("i3c recv_ccc directed");
}

static void test_write_immediate(void)
{
  TEST_BEGIN("i3c write (immediate-data)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  const uint8_t data[2] = {0xDEU, 0xADU};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_write(0x55U, data, 2U));
  /* Last NCMDQP write is the immediate data packed LE. */
  TEST_ASSERT_EQ((int32_t)0x0000ADDEU, (int32_t)ra_i3c()->NCMDQP);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_write(0x80U, data, 2U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_write(0x55U, nullptr, 2U));
  TEST_END("i3c write (immediate-data)");
}

static void test_write_regular(void)
{
  TEST_BEGIN("i3c write (regular FIFO)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_write(0x55U, data, sizeof(data)));
  /* The FIFO backing memory holds the LAST word that was written --
   * with the simulated MMIO NTDTBP0 maps to a single 32-bit cell, so
   * the second word ({5,6,7,8}) is what remains. */
  TEST_ASSERT_EQ((int32_t)0x08070605U, (int32_t)ra_i3c()->NTDTBP0);
  TEST_END("i3c write (regular FIFO)");
}

static void test_read_happy(void)
{
  TEST_BEGIN("i3c read (happy path)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  ra_i3c()->NTDTBP0 = 0xCAFEBABEU;
  uint8_t buf[4]    = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_read(0x12U, buf, 4U));
  TEST_ASSERT_EQ((int32_t)0xBEU, (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)0xBAU, (int32_t)buf[1]);
  TEST_ASSERT_EQ((int32_t)0xFEU, (int32_t)buf[2]);
  TEST_ASSERT_EQ((int32_t)0xCAU, (int32_t)buf[3]);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_read(0x12U, nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_read(0x12U, buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_i3c_read(0x80U, buf, 4U));
  TEST_END("i3c read (happy path)");
}

static void test_ibi_read_empty(void)
{
  TEST_BEGIN("i3c ibi_read empty queue");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  ra_i3c_ibi_t ibi = {};
  /* NTST.IBIQEFF (bit 2) is zero by default -> queue empty. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_ibi_read(nullptr));
  TEST_END("i3c ibi_read empty queue");
}

static void test_ibi_read_one(void)
{
  TEST_BEGIN("i3c ibi_read one IBI");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());

  /* Flag the queue as non-empty (NTST.IBIQEFF bit 2). */
  ra_i3c()->NTST = (uint32_t)k_ra_i3c_ntst_ibiqeff_mask;

  /* Build an IBI status descriptor: length=2, IBI ID=0x84 (addr 0x42 + RnW),
   * IBI_ST=0 (slave-initiated IBI). */
  const uint32_t ibi_status = (2U << 0) | ((uint32_t)0x84U << 8);
  ra_i3c()->NIBIQP          = ibi_status;
  /* Stage the 2-byte IBI payload in NTDTBP0. */
  ra_i3c()->NTDTBP0 = 0x0000BEEFU;

  ra_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ((int32_t)0x42U, (int32_t)ibi.address);
  TEST_ASSERT_EQ((int32_t)k_ra_i3c_ibi_type_interrupt, (int32_t)ibi.type);
  TEST_ASSERT_EQ((int32_t)2, (int32_t)ibi.payload_len);
  TEST_ASSERT_EQ((int32_t)0xEFU, (int32_t)ibi.payload[0]);
  TEST_ASSERT_EQ((int32_t)0xBEU, (int32_t)ibi.payload[1]);
  TEST_END("i3c ibi_read one IBI");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_set_address();
  test_bus_enable();
  test_power_transition();
  test_dynamic_address_assign();
  test_set_dynamic_address();
  test_reset_dynamic_addresses();
  test_send_ccc_broadcast();
  test_send_ccc_directed_with_payload();
  test_recv_ccc_directed();
  test_write_immediate();
  test_write_regular();
  test_read_happy();
  test_ibi_read_empty();
  test_ibi_read_one();
  (void)fprintf(stderr, "[OK  ] test_ra_i3c.c\n");
  return 0;
}
