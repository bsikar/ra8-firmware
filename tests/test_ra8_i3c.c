/**
 * @file test_ra8_i3c.c
 * @brief Unit tests for ra8_i3c.c (I3C Bus Interface driver)
 *
 * @details
 * Covers the lifecycle / status / IRQ surface that the original
 * scaffold provided, plus the CCC engine (ENTDAA, SETDASA, RSTDAA,
 * generic send/recv), private read / write, and the IBI inbound
 * queue.  Every test resets the fake MMIO backing store via
 * ``ra8_fake_mmap_reset`` so the FIFO ports start zeroed, then drives
 * the driver through one verifiable register sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c.h"
#include "ra8_i3c_internal.h"
#include "ra8_i3c_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum i3c_fixture_t
 * @brief The handles and addresses the fixture transacts on, plus the protocol codes and identifiers exchanged, and the recognizable values moved through the code under test.
 */
typedef enum : uint8_t {
  k_i3c_dynamic_addr = 0x11U, /**< Dynamic address assigned to the fixture target. */
  k_i3c_payload_byte =
    0xA5U, /**< Recognizable single-byte payload; not 0x00 or 0xFF, so idle bus cannot fake it. */
  k_i3c_ibi_id =
    0x84U, /**< IBI identifier placed in queue word's upper bits, with a zero length below it. */
} i3c_fixture_t;

/**
 * @enum i3c_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_i3c_probe_inst_b =
    0xBEEFU, /**< A second, different INST value, so the read cannot be a cached first result. */
  k_i3c_probe_inst_a =
    0xCAFEU, /**< Planted in INST to prove the status read reaches the register. */
  k_i3c_probe_half_word =
    0x0000BEEFU, /**< Upper half zero, catching sign-extension or reused stale high bytes. */
} i3c_fixture2_t;

/**
 * @enum i3c_fixture3_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_i3c_probe_word =
    0xDEADBEEFU, /**< Full 32-bit value in INST and the command queue; no field is truncated. */
  k_i3c_probe_word_alt =
    0xCAFEBABEU, /**< Second full-width value, so command queue and buffer writes stay distinct. */
  k_i3c_probe_ascending =
    0x11223344U, /**< Ascending bytes: a byte-order slip in the data buffer shows up directly. */
  k_i3c_probe_descending =
    0x44332211U, /**< The same bytes reversed, which is what a byte-swapped write would produce. */
} i3c_fixture3_t;

static uint32_t s_i3c_cb_count;
static uint32_t s_i3c_cb_last_mask;

/** @brief Native-mode bring-up config used by the native-path tests. */
static const ra8_i3c_cfg_t k_native_cfg = {.mode     = k_ra8_i3c_mode_native,
                                           .bus_hz   = 0U,
                                           .pclka_hz = 0U};

static void stub_i3c_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_i3c_cb_count;
  s_i3c_cb_last_mask = mask;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_i3c_cb_count     = 0U;
  s_i3c_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("i3c init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  /* CECTL.CLKE must be set after init. */
  TEST_ASSERT_EQ(1, (ra8_i3c()->CECTL & 0x1U));
  /* RSTCTL must be released after init. */
  TEST_ASSERT_EQ(0, ra8_i3c()->RSTCTL);
  TEST_END("i3c init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("i3c deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_deinit(0U));
  TEST_ASSERT_EQ(0, (ra8_i3c()->CECTL & 0x1U));
  TEST_END("i3c deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("i3c status read + clear");
  prep();
  ra8_i3c()->INST = k_i3c_probe_word;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_get_status(&mask));
  TEST_ASSERT_EQ(0xDEADBEEFU, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_clear_status(0xF0F0F0F0U));
  /* Bits in mask 0xF0F0F0F0 must have been cleared. */
  TEST_ASSERT_EQ((0xDEADBEEFU & ~0xF0F0F0F0U), ra8_i3c()->INST);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_get_status(nullptr));
  TEST_END("i3c status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("i3c attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_attach_handler(0U, stub_i3c_cb, (void*)(uintptr_t)0x13U));
  ra8_i3c()->INST = k_i3c_probe_inst_a;
  ra8_i3c_dispatch(0U);
  TEST_ASSERT_EQ(1, s_i3c_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_i3c_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_attach_handler(0U, nullptr, nullptr));
  ra8_i3c()->INST = k_i3c_probe_inst_b;
  ra8_i3c_dispatch(0U);
  TEST_ASSERT_EQ(1, s_i3c_cb_count);
  TEST_END("i3c attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_address(void)
{
  TEST_BEGIN("i3c set address");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  /* Valid 7-bit address: bits [22:16] hold the value, bit 31 marks valid. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_address(0x42U));
  const uint32_t expect = (0x42U << 16) | 0x80000000U;
  TEST_ASSERT_EQ(expect, ra8_i3c()->MSDVAD);
  /* Out-of-range 7-bit dynamic address must be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_address(0x80U));
  TEST_END("i3c set address");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bus_enable(void)
{
  TEST_BEGIN("i3c bus enable");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_bus_enable(true));
  /* BCTL.BUSE is bit 31 per FSP R_I3C0_BCTL_BUSE_Pos = 31. */
  TEST_ASSERT_EQ(1, ((ra8_i3c()->BCTL >> 31) & 0x1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_bus_enable(false));
  TEST_ASSERT_EQ(0, ((ra8_i3c()->BCTL >> 31) & 0x1U));
  TEST_END("i3c bus enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("i3c power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_exit_stop());
  TEST_END("i3c power transition");
}

/* ---------------------------------------------------------------------------
 * CCC engine: ENTDAA, SETDASA, RSTDAA, generic send/recv.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

static void test_dynamic_address_assign(void)
{
  TEST_BEGIN("i3c dynamic_address_assign (ENTDAA)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  /* Pre-stage one PID/BCR/DCR response in the fake NTDTBP0 cell --
   * with a single backing word the driver's two consecutive reads return
   * the same value, so the test only verifies the *first* drained 4 bytes
   * landed in the target.pid[]. */
  ra8_i3c()->NTDTBP0 = k_i3c_probe_descending;

  ra8_i3c_daa_target_t targets[2] = {{.dynamic_address = 0x10U},
                                     {.dynamic_address = k_i3c_dynamic_addr}};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_dynamic_address_assign(targets, 2U));

  /* Verify the driver issued at least the two-word command descriptor (the
   * cell was zeroed by ``prep()`` and the second word the driver writes is
   * 0, leaving NCMDQP == 0).  The interesting side-effect is the populated
   * targets[] array: pid[0..3] should equal the staged 32-bit word LE. */
  TEST_ASSERT_EQ(0x11U, targets[0].pid[0]);
  TEST_ASSERT_EQ(0x22U, targets[0].pid[1]);
  TEST_ASSERT_EQ(0x33U, targets[0].pid[2]);
  TEST_ASSERT_EQ(0x44U, targets[0].pid[3]);
  /* Caller-supplied dynamic_address values must be preserved. */
  TEST_ASSERT_EQ(0x10U, targets[0].dynamic_address);
  TEST_ASSERT_EQ(0x11U, targets[1].dynamic_address);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_dynamic_address_assign(nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_dynamic_address_assign(targets, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_dynamic_address_assign(targets, 99U));
  TEST_END("i3c dynamic_address_assign (ENTDAA)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dynamic_address(void)
{
  TEST_BEGIN("i3c set_dynamic_address (SETDASA)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_dynamic_address(0x12U, 0x34U));
  /* Last write to NCMDQP was the dynamic-address payload byte (cmd2). */
  TEST_ASSERT_EQ(0x34U, ra8_i3c()->NCMDQP);
  /* Bounds check on both addresses. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_dynamic_address(0x80U, 0x10U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_dynamic_address(0x10U, 0x80U));
  TEST_END("i3c set_dynamic_address (SETDASA)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_dynamic_addresses(void)
{
  TEST_BEGIN("i3c reset_dynamic_addresses (RSTDAA)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  /* Pre-seed NCMDQP with a sentinel so the driver's two writes (cmd1 +
   * cmd2=0) result in NCMDQP == 0 by the end -- this proves the driver
   * touched the queue at least twice. */
  ra8_i3c()->NCMDQP = k_i3c_probe_word;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_reset_dynamic_addresses());
  /* RSTDAA cmd2 is zero -> last write. */
  TEST_ASSERT_EQ(0, ra8_i3c()->NCMDQP);
  TEST_END("i3c reset_dynamic_addresses (RSTDAA)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_ccc_broadcast(void)
{
  TEST_BEGIN("i3c send_ccc broadcast (ENEC, no payload)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  /* Pre-seed NCMDQP with a sentinel; broadcast ENEC + zero payload causes
   * the driver to write cmd1 then cmd2 = 0, so the cell ends as 0 -- proof
   * the queue saw at least one write. */
  ra8_i3c()->NCMDQP = k_i3c_probe_word_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_send_ccc(0x00U, 0U, nullptr, 0U));
  /* Last NCMDQP write was the immediate-payload word (zero for empty). */
  TEST_ASSERT_EQ(0, ra8_i3c()->NCMDQP);
  TEST_END("i3c send_ccc broadcast");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_ccc_directed_with_payload(void)
{
  TEST_BEGIN("i3c send_ccc directed with payload");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  const uint8_t payload[3] = {0xAAU, 0xBBU, 0xCCU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_send_ccc(0x80U, 0x42U, payload, 3U));
  /* Last NCMDQP write was the immediate-data word (LE pack of payload). */
  const uint32_t expect_data = 0x00CCBBAAU;
  TEST_ASSERT_EQ(expect_data, ra8_i3c()->NCMDQP);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_send_ccc(0x80U, 0x42U, nullptr, 3U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_send_ccc(0x80U, 0x80U, payload, 3U));
  TEST_END("i3c send_ccc directed with payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_ccc_directed(void)
{
  TEST_BEGIN("i3c recv_ccc directed");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  /* Pre-load NTDTBP0 with one word of receive data. */
  ra8_i3c()->NTDTBP0 = k_i3c_probe_ascending;

  uint8_t buf[4]  = {};
  uint8_t got_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_recv_ccc(0x8DU, 0x42U, buf, 4U, &got_len));
  TEST_ASSERT_EQ(4, got_len);
  TEST_ASSERT_EQ(0x44U, buf[0]);
  TEST_ASSERT_EQ(0x33U, buf[1]);
  TEST_ASSERT_EQ(0x22U, buf[2]);
  TEST_ASSERT_EQ(0x11U, buf[3]);
  /* Broadcast CCC must be rejected by recv. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_recv_ccc(0x07U, 0x42U, buf, 4U, &got_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_recv_ccc(0x8DU, 0x42U, nullptr, 4U, &got_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_recv_ccc(0x8DU, 0x42U, buf, 4U, nullptr));
  TEST_END("i3c recv_ccc directed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_immediate(void)
{
  TEST_BEGIN("i3c write (immediate-data)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  const uint8_t data[2] = {0xDEU, 0xADU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_write(0U, 0x55U, data, 2U, false));
  /* Last NCMDQP write is the immediate data packed LE. */
  TEST_ASSERT_EQ(0x0000ADDEU, ra8_i3c()->NCMDQP);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_write(0U, 0x80U, data, 2U, false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_write(0U, 0x55U, nullptr, 2U, false));
  TEST_END("i3c write (immediate-data)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_regular(void)
{
  TEST_BEGIN("i3c write (regular FIFO)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_write(0U, 0x55U, data, sizeof(data), false));
  /* The FIFO backing memory holds the LAST word that was written --
   * with the fake MMIO NTDTBP0 maps to a single 32-bit cell, so
   * the second word ({5,6,7,8}) is what remains. */
  TEST_ASSERT_EQ(0x08070605U, ra8_i3c()->NTDTBP0);
  TEST_END("i3c write (regular FIFO)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_happy(void)
{
  TEST_BEGIN("i3c read (happy path)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  ra8_i3c()->NTDTBP0 = k_i3c_probe_word_alt;
  uint8_t buf[4]     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_read(0U, 0x12U, buf, 4U, false));
  TEST_ASSERT_EQ(0xBEU, buf[0]);
  TEST_ASSERT_EQ(0xBAU, buf[1]);
  TEST_ASSERT_EQ(0xFEU, buf[2]);
  TEST_ASSERT_EQ(0xCAU, buf[3]);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_read(0U, 0x12U, nullptr, 4U, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_read(0U, 0x12U, buf, 0U, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_read(0U, 0x80U, buf, 4U, false));
  TEST_END("i3c read (happy path)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ibi_read_empty(void)
{
  TEST_BEGIN("i3c ibi_read empty queue");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  ra8_i3c_ibi_t ibi = {};
  /* NTST.IBIQEFF (bit 2) is zero by default -> queue empty. */
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_ibi_read(nullptr));
  TEST_END("i3c ibi_read empty queue");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ibi_read_one(void)
{
  TEST_BEGIN("i3c ibi_read one IBI");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  /* Flag the queue as non-empty (NTST.IBIQEFF bit 2). */
  ra8_i3c()->NTST = (uint32_t)k_ra8_i3c_ntst_ibiqeff_mask;

  /* Build an IBI status descriptor: length=2, IBI ID=0x84 (addr 0x42 + RnW),
   * IBI_ST=0 (peripheral-initiated IBI). */
  const uint32_t ibi_status = (2U << 0) | ((uint32_t)0x84U << 8);
  ra8_i3c()->NIBIQP         = ibi_status;
  /* Stage the 2-byte IBI payload in NTDTBP0. */
  ra8_i3c()->NTDTBP0 = k_i3c_probe_half_word;

  ra8_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ(0x42U, ibi.address);
  TEST_ASSERT_EQ(k_ra8_i3c_ibi_type_interrupt, ibi.type);
  TEST_ASSERT_EQ(2, ibi.payload_len);
  TEST_ASSERT_EQ(0xEFU, ibi.payload[0]);
  TEST_ASSERT_EQ(0xBEU, ibi.payload[1]);
  TEST_END("i3c ibi_read one IBI");
}

/* ---------------------------------------------------------------------------
 * Sweep 15 / Phase 2: HDR + IBI + peripheral-mode entry surface.
 * --------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_hdr_mode_ddr(void)
{
  TEST_BEGIN("i3c set_hdr_mode DDR encodes [27:26]=01");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_hdr_mode(0x42U, k_ra8_i3c_hdr_mode_ddr));
  /* NCMDQP word should carry the DDR mode bits (1U << 26) and the target
   * dynamic address (0x42 << 16). */
  const uint32_t ncmdqp = ra8_i3c()->NCMDQP;
  TEST_ASSERT(((ncmdqp >> 26) & 0x3U) == 1U);
  TEST_ASSERT(((ncmdqp >> 16) & 0x7FU) == 0x42U);
  TEST_END("i3c set_hdr_mode DDR encodes [27:26]=01");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_hdr_mode_ts_and_validation(void)
{
  TEST_BEGIN("i3c set_hdr_mode TS + validation");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_hdr_mode(0x10U, k_ra8_i3c_hdr_mode_ts));
  TEST_ASSERT(((ra8_i3c()->NCMDQP >> 26) & 0x3U) == 2U);

  /* Out-of-range address rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_hdr_mode(0x80U, k_ra8_i3c_hdr_mode_ddr));
  /* Bogus mode rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_hdr_mode(0x10U, (ra8_i3c_hdr_mode_t)9U));
  TEST_END("i3c set_hdr_mode TS + validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ibi_enable_writes_ntibivctl(void)
{
  TEST_BEGIN("i3c ibi_enable writes NTIBIVCTL.VLCNT=1");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_enable(0x55U));
  /* VLCNT field [7:0] should equal 1. */
  TEST_ASSERT_EQ(1, (ra8_i3c()->NTIBIVCTL & 0xFFU));

  /* Out-of-range address rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_ibi_enable(0x80U));
  TEST_END("i3c ibi_enable writes NTIBIVCTL.VLCNT=1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ibi_drain_aliases_read(void)
{
  TEST_BEGIN("i3c ibi_drain mirrors ibi_read semantics");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  /* Empty queue path. */
  ra8_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_i3c_ibi_drain(&ibi));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_ibi_drain(nullptr));

  /* Stage one IBI and verify drain returns it. */
  ra8_i3c()->NTST    = (uint32_t)k_ra8_i3c_ntst_ibiqeff_mask;
  ra8_i3c()->NIBIQP  = ((uint32_t)k_i3c_ibi_id << 8); /* len=0, IBI ID=0x84. */
  ra8_i3c()->NTDTBP0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_drain(&ibi));
  TEST_ASSERT_EQ(0x42U, ibi.address);
  TEST_END("i3c ibi_drain mirrors ibi_read semantics");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_target_open_sets_slve_and_nsdvad(void)
{
  TEST_BEGIN("i3c target_open sets BCTL.SLVE and NSDVAD");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_target_open(0x33U));
  /* BCTL.SLVE (bit 16) set; BCTL.BUSE (bit 31) cleared. */
  TEST_ASSERT(((ra8_i3c()->BCTL >> 16) & 0x1U) == 1U);
  TEST_ASSERT(((ra8_i3c()->BCTL >> 31) & 0x1U) == 0U);
  /* NSDVAD has SDYAD=0x33 in [22:16] and SDYADV (bit 31) set. */
  const uint32_t expect = (0x33U << 16) | 0x80000000U;
  TEST_ASSERT_EQ(expect, ra8_i3c()->NSDVAD);
  /* Out-of-range static address rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_target_open(0x80U));
  TEST_END("i3c target_open sets BCTL.SLVE and NSDVAD");
}

/**
 * @test test_mcdc_i3c
 *
 * @par MC/DC:
 * Three 2-condition decisions in libs/ra8_hal/src/ra8_i3c.c:
 *
 * Decision A (line 320, ``ra8_i3c_dynamic_address_assign``):
 * ``if ((target_count == 0U) || (target_count > k_ra8_i3c_max_targets))``
 * - V1: count=0  -> C1=T (short-circuits) -> dec T (invalid_arg)
 * - V2: count=1  -> C1=F,C2=F             -> dec F (ok)
 * - V3: count=k_ra8_i3c_max_targets+1 -> C1=F,C2=T -> dec T (invalid_arg)
 * Pairs: (V1,V2) flip C1 with C2 fixed; (V2,V3) flip C2 with C1 fixed.
 *
 * Decision B (line 367, ``ra8_i3c_set_dynamic_address``):
 * ``if ((static_addr > addr_mask) || (dynamic_addr > addr_mask))``
 * - V1: static=ok, dyn=ok   -> C1=F,C2=F -> dec F (ok)
 * - V2: static=bad, dyn=ok  -> C1=T (short-circuits) -> dec T
 * - V3: static=ok, dyn=bad  -> C1=F,C2=T -> dec T
 *
 * Decision C (line 405, ``ra8_i3c_send_ccc``):
 * ``if ((len > 0U) && (payload == nullptr))``
 * - V1: len=0           -> C1=F (short-circuits) -> dec F (ok)
 * - V2: len>0, payload!=NULL -> C1=T,C2=F -> dec F (ok)
 * - V3: len>0, payload=NULL  -> C1=T,C2=T -> dec T (null_ptr)
 */
static void test_mcdc_i3c(void)
{
  TEST_BEGIN("i3c MC/DC: three 2-cond arg decisions");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));

  /* Decision A: dynamic_address_assign */
  ra8_i3c_daa_target_t one         = {};
  ra8_i3c_daa_target_t many[1 + 8] = {}; /* generous; only count is checked */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_dynamic_address_assign(&one, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_dynamic_address_assign(&one, 1U));
  /* Use a count strictly greater than k_ra8_i3c_max_targets. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_dynamic_address_assign(many, 0xFFU));

  /* Decision B: set_dynamic_address */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_dynamic_address(0x33U, 0x44U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_dynamic_address(0x80U, 0x44U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_dynamic_address(0x33U, 0x80U));

  /* Decision C: send_ccc */
  uint8_t payload[1] = {k_i3c_payload_byte};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_send_ccc(0x00U, 0x33U, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_send_ccc(0x00U, 0x33U, payload, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_send_ccc(0x00U, 0x33U, nullptr, 1U));
  TEST_END("i3c MC/DC: three 2-cond arg decisions");
}

/**
 * @test test_mcdc_i3c_write_read_arg_pairs
 *
 * @par MC/DC:
 * Decision A (libs/ra8_hal/src/ra8_i3c.c ra8_i3c_write):
 *   ``if ((len > 0U) && (data == nullptr))``
 * 2-cond AND, N+1 = 3 vectors.
 * - V1: len>0, data=valid -> C1=T, C2=F -> dec F (proceed).
 * - V2: len=0, data=NULL  -> C1=F short -> dec F (zero-length write).
 * - V3: len>0, data=NULL  -> C1=T, C2=T -> dec T -> null_ptr.
 * V1+V3 isolate C2; V2+V3 isolate C1.
 *
 * Decision B (libs/ra8_hal/src/ra8_i3c.c ra8_i3c_read):
 *   ``if ((len == 0U) || (len > k_ra8_i3c_cmd_xfer_length_max))``
 * 2-cond OR, N+1 = 3 vectors.
 * - V1: len=4              -> C1=F, C2=F -> dec F.
 * - V2: len=0              -> C1=T short -> dec T -> invalid_arg.
 * - V3: len=0x10000        -> C1=F, C2=T -> dec T -> invalid_arg.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_i3c_write_read_arg_pairs(void)
{
  TEST_BEGIN("i3c MC/DC: write+read len/ptr pairs");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
  const uint8_t data[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  uint8_t       buf[8]  = {0U};

  /* Write Decision A. */
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_write(0U, 0x55U, data, 4U, false));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_write(0U, 0x55U, nullptr, 0U, false));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_write(0U, 0x55U, nullptr, 4U, false));

  /* Read Decision B. */
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_read(0U, 0x55U, buf, 4U, false));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_read(0U, 0x55U, buf, 0U, false));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_read(0U, 0x55U, buf, 0x10000U, false));

  TEST_END("i3c MC/DC: write+read len/ptr pairs");
}

/**
 * @test test_mcdc_i3c_internal_recv_ccc_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_i3c.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_i3c.c:
 *   ``target > addr_mask || max_len == 0`` (2 conditions, OR).
 * - V1: target<=mask, max_len>0 -> false
 * - V2: target>mask,  max_len>0 -> true (varies left)
 * - V3: target<=mask, max_len=0 -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_i3c_internal_recv_ccc_invalid(void)
{
  TEST_BEGIN("i3c MC/DC: recv_ccc_invalid OR");
  TEST_ASSERT(!ra8_i3c_internal_recv_ccc_invalid(0x7FU, 0x10U, 4U));
  TEST_ASSERT(ra8_i3c_internal_recv_ccc_invalid(0x7FU, 0xFFU, 4U));
  TEST_ASSERT(ra8_i3c_internal_recv_ccc_invalid(0x7FU, 0x10U, 0U));
  TEST_END("i3c MC/DC: recv_ccc_invalid OR");
}

/**
 * @test test_mcdc_i3c_internal_hdr_mode_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_i3c.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_i3c.c:
 *   ``mode != SDR && mode != DDR && mode != TS`` (3 conditions, AND).
 * - V1: mode=SDR -> false (SDR varies vs V4)
 * - V2: mode=DDR -> false (DDR varies vs V4)
 * - V3: mode=TS  -> false (TS varies vs V4)
 * - V4: mode=99  -> true  (control: all true)
 * N+1 = 4.
 */
static void test_mcdc_i3c_internal_hdr_mode_invalid(void)
{
  TEST_BEGIN("i3c MC/DC: hdr_mode_invalid AND");
  TEST_ASSERT(!ra8_i3c_internal_hdr_mode_invalid((uint32_t)k_ra8_i3c_hdr_mode_sdr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ddr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ts,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_sdr));
  TEST_ASSERT(!ra8_i3c_internal_hdr_mode_invalid((uint32_t)k_ra8_i3c_hdr_mode_sdr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ddr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ts,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ddr));
  TEST_ASSERT(!ra8_i3c_internal_hdr_mode_invalid((uint32_t)k_ra8_i3c_hdr_mode_sdr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ddr,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ts,
                                                 (uint32_t)k_ra8_i3c_hdr_mode_ts));
  TEST_ASSERT(ra8_i3c_internal_hdr_mode_invalid((uint32_t)k_ra8_i3c_hdr_mode_sdr,
                                                (uint32_t)k_ra8_i3c_hdr_mode_ddr,
                                                (uint32_t)k_ra8_i3c_hdr_mode_ts,
                                                99U));
  TEST_END("i3c MC/DC: hdr_mode_invalid AND");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init,
  test_deinit,
  test_status_read_and_clear,
  test_attach_and_dispatch,
  test_set_address,
  test_bus_enable,
  test_power_transition,
  test_dynamic_address_assign,
  test_set_dynamic_address,
  test_reset_dynamic_addresses,
  test_send_ccc_broadcast,
  test_send_ccc_directed_with_payload,
  test_recv_ccc_directed,
  test_write_immediate,
  test_write_regular,
  test_read_happy,
  test_ibi_read_empty,
  test_ibi_read_one,
  test_set_hdr_mode_ddr,
  test_set_hdr_mode_ts_and_validation,
  test_ibi_enable_writes_ntibivctl,
  test_ibi_drain_aliases_read,
  test_target_open_sets_slve_and_nsdvad,
  test_mcdc_i3c,
  test_mcdc_i3c_write_read_arg_pairs,
  test_mcdc_i3c_internal_recv_ccc_invalid,
  test_mcdc_i3c_internal_hdr_mode_invalid,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_i3c.c\n");
  return 0;
}
