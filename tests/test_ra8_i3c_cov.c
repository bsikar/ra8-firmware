/**
 * @file test_ra8_i3c_cov.c
 * @brief Coverage supplement for ra8_i3c.c -- drives branches not yet
 *        exercised by test_ra8_i3c.c.
 *
 * @details
 * Targets uncovered lines 250-258, 319, 345, 412, 422, 428-434, 575-577,
 * 593, 615, 618, 632, 660, 663, 701, 704, 709-718, 723, 726, 731-740,
 * 742-751, 753-762, 787-796, 798-807, 809-818, 823, 826, 939-941, 947-948.
 *
 * Harness: identical to test_ra8_i3c.c -- RA8_SIMULATOR_MODE mmap backing
 * store, ra8_mstp_init() before each case, unity_minimal.h assertions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_i3c_internal.h"
#include "ra8_i3c_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum i3c_cov_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_i3c_payload_byte = 0xAAU, /**< A recognizable single-byte payload; neither 0x00 nor 0xFF. */
  k_i3c_irq_out_of_range =
    255U, /**< An interrupt number past the last real source; dispatch must ignore it. */
} i3c_cov_fixture_t;

/**
 * @enum i3c_cov_fixture2_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_i3c_ibi_status_with_len =
    0x00008409U, /**< An IBI queue word carrying a non-zero payload length, so the reader must drain the data buffer. */
} i3c_cov_fixture2_t;

/**
 * @enum i3c_cov_fixture3_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_i3c_ibi_status_error_a = 0x80000200U, /**< An IBI queue word with the error bit set. */
  k_i3c_ibi_status_error_b =
    0x80000400U, /**< A second error word with a different cause field, so the two are not conflated. */
  k_i3c_probe_word =
    0xDEADBEEFU, /**< Payload word in the data buffer, read out four bytes at a time. */
} i3c_cov_fixture3_t;

/* ---------------------------------------------------------------------------
 * Shared callbacks and config helpers
 * ---------------------------------------------------------------------------
 */

/** @brief Callback hit count for dispatch i2c-mode test. */
static uint32_t s_cb_count;
/** @brief Last mask received by the callback. */
static uint32_t s_cb_mask;

/** @brief Stub callback that records invocations. */
static void stub_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_cb_count;
  s_cb_mask = mask;
}

/** @brief Native-mode bring-up configuration. */
static const ra8_i3c_cfg_t k_native_cfg = {
  .mode     = k_ra8_i3c_mode_native,
  .bus_hz   = 0U,
  .pclka_hz = 0U,
};

/** @brief I2C-compat bring-up configuration (valid clocks). */
static const ra8_i3c_cfg_t k_i2c_cfg = {
  .mode     = k_ra8_i3c_mode_i2c,
  .bus_hz   = 100000U,
  .pclka_hz = 8000000U,
};

/**
 * @brief Reset the simulated MMIO and re-initialise the MSTP mock.
 *
 * @details Mirrors the prep() helper in test_ra8_i3c.c.  Does NOT reset
 * the module-level s_i3c_chan[] state; callers must call ra8_i3c_deinit()
 * or ra8_i3c_init() themselves to reach the desired channel state.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_cb_count = 0U;
  s_cb_mask  = 0U;
}

/**
 * @brief prep() + native-mode init on channel 0.
 *
 * @details Guarantees channel 0 is initialised in native mode.
 * Used by tests that need the native code path.
 */
static void prep_native(void)
{
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_native_cfg));
}

/**
 * @brief prep() + i2c-mode init on channel 0.
 *
 * @details Guarantees channel 0 is initialised in I2C-compat mode.
 * Used by tests that exercise I2C-only APIs (set_clock, get_errors, ...).
 */
static void prep_i2c(void)
{
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &k_i2c_cfg));
}

/**
 * @brief prep() then deinit channel 0 to leave it uninitialized.
 *
 * @details Ensures s_i3c_chan[0].initialized == false regardless of the
 * state left by the previous test.
 */
static void prep_uninit(void)
{
  prep();
  (void)ra8_i3c_deinit(0U); /* clears .initialized */
}

/* ---------------------------------------------------------------------------
 * Test: priv_ra8_i3c_fifo_write partial-word path (lines 250-258) and
 *       ra8_i3c_send_ccc regular-transfer path (lines 575-577).
 *
 * @par MC/DC:
 * Decision A (ra8_i3c_send_ccc, line 561): ``len <= k_ra8_i3c_immediate_max_bytes``
 * - V1: len=3  -> C1=T -> immediate path (covered by test_ra8_i3c.c)
 * - V2: len=5  -> C1=F -> regular-FIFO path (this test, covers lines 575-577)
 * V1+V2 prove the single condition independently affects the branch.
 *
 * The regular-FIFO path calls priv_ra8_i3c_fifo_write(reg, payload, 5);
 * 5 = 4 (full word) + 1 (partial): the partial branch at lines 249-258 is
 * taken once, covering lines 250-258.
 * ---------------------------------------------------------------------------
 */
static void test_send_ccc_large_payload_and_fifo_partial(void)
{
  TEST_BEGIN("i3c send_ccc large payload covers regular path + fifo partial");
  prep_native();
  const uint8_t payload[5] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
  /* len=5 > k_ra8_i3c_immediate_max_bytes=4: takes the regular-transfer
   * branch (lines 575-577).  The FIFO write of 5 bytes yields one full
   * 32-bit word plus one partial byte, covering lines 250-258. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_send_ccc(0x80U, 0x42U, payload, 5U));
  /* Last FIFO write was the partial byte (0x55) in the low byte of a
   * zero-padded word. */
  TEST_ASSERT_EQ(0x55U, ra8_i3c()->NTDTBP0 & 0xFFU);
  TEST_END("i3c send_ccc large payload covers regular path + fifo partial");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_init I2C-mode error propagation (line 319).
 *
 * @par MC/DC:
 * (no new compound decisions; single conditional guard on return value)
 * ---------------------------------------------------------------------------
 */
static void test_init_i2c_mode_error(void)
{
  TEST_BEGIN("i3c init i2c mode propagates internal error (line 319)");
  prep();
  /* bus_hz=0 causes internal_i3c_i2c_init to return k_ra8_err_invalid_arg;
   * ra8_i3c_init's guard at line 318 is true and returns on line 319. */
  const ra8_i3c_cfg_t i2c_fail_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = 0U,
    .pclka_hz = 8000000U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_init(0U, &i2c_fail_cfg));
  TEST_END("i3c init i2c mode propagates internal error (line 319)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_deinit out-of-range channel (line 345).
 *
 * @par MC/DC:
 * (no new compound decisions; single channel-range guard)
 * ---------------------------------------------------------------------------
 */
static void test_deinit_oob(void)
{
  TEST_BEGIN("i3c deinit rejects out-of-range channel (line 345)");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_deinit(255U));
  TEST_END("i3c deinit rejects out-of-range channel (line 345)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_attach_handler out-of-range channel (line 412).
 *
 * @par MC/DC:
 * (no new compound decisions; single channel-range guard)
 * ---------------------------------------------------------------------------
 */
static void test_attach_handler_oob(void)
{
  TEST_BEGIN("i3c attach_handler rejects out-of-range channel (line 412)");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_attach_handler(255U, nullptr, nullptr));
  TEST_END("i3c attach_handler rejects out-of-range channel (line 412)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_dispatch out-of-range channel (line 422) and i2c-mode
 *       dispatch (lines 428-434).
 *
 * @par MC/DC:
 * Decision A (ra8_i3c_dispatch line 431): ``fn != nullptr``
 * - V1: fn=nullptr -> false -> skip callback  (lines 428-431 + 434)
 * - V2: fn=stub_cb -> true  -> invoke fn      (lines 428-434)
 * V1+V2 prove the single condition independently affects the branch.
 * ---------------------------------------------------------------------------
 */
static void test_dispatch_oob_and_i2c_mode(void)
{
  TEST_BEGIN("i3c dispatch: out-of-range channel + i2c-mode path");
  /* Part 1: channel 255 hits the early return at line 422. */
  prep();
  ra8_i3c_dispatch(k_i3c_irq_out_of_range);
  /* Part 2: channel 0 in i2c mode, callback attached -- covers 428-434.
   * MC/DC V2 (fn!=nullptr): fn is invoked. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_attach_handler(0U, stub_cb, nullptr));
  ra8_i3c_dispatch(0U);
  TEST_ASSERT_EQ(1U, s_cb_count);
  /* Part 3: MC/DC V1 -- same channel but callback detached (fn=nullptr):
   * the if-body at lines 431-433 is skipped. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_attach_handler(0U, nullptr, nullptr));
  ra8_i3c_dispatch(0U);
  TEST_ASSERT_EQ(1U, s_cb_count); /* count unchanged */
  TEST_END("i3c dispatch: out-of-range channel + i2c-mode path");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_recv_ccc returns invalid_arg via internal_recv_ccc_invalid
 *       (line 593) when target_addr > addr_mask with max_len > 0.
 *
 * @par MC/DC:
 * Decision at ra8_i3c_recv_ccc line 592 (call to
 * ra8_i3c_internal_recv_ccc_invalid): see test_ra8_i3c.c for the full
 * 3-vector MC/DC coverage of the inner OR.  Here we exercise the site
 * where the guard returns true and produces the line-593 return.
 * ---------------------------------------------------------------------------
 */
static void test_recv_ccc_internal_invalid(void)
{
  TEST_BEGIN("i3c recv_ccc: internal predicate triggers invalid_arg (line 593)");
  prep_native();
  uint8_t buf[4]  = {};
  uint8_t got_len = 0U;
  /* direct CCC (bit 7 set), target_addr=0xFF > 0x7F: the helper returns
   * true -> line 593 executed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_recv_ccc(0x8DU, 0xFFU, buf, 4U, &got_len));
  /* max_len=0 with valid target also triggers the helper (varies right
   * condition of the internal OR -- complements the above vector). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_recv_ccc(0x8DU, 0x10U, buf, 0U, &got_len));
  TEST_END("i3c recv_ccc: internal predicate triggers invalid_arg (line 593)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_write error paths (lines 615, 618, 632).
 *
 * @par MC/DC:
 * Decision (ra8_i3c_write line 628): ``(len > 0U) && (data == nullptr)``
 * is already fully covered by test_ra8_i3c.c.  The three new guards here
 * are single-condition checks, not compound decisions.
 * ---------------------------------------------------------------------------
 */
static void test_write_error_paths(void)
{
  TEST_BEGIN("i3c write: oob channel / uninit / len overflow errors");
  uint8_t dummy = k_i3c_payload_byte;

  /* Line 615: channel out of range. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_write(255U, 0x10U, &dummy, 1U, false));

  /* Line 618: channel 0 not yet initialized. */
  prep_uninit();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_write(0U, 0x10U, &dummy, 1U, false));

  /* Line 632: len > k_ra8_i3c_cmd_xfer_length_max (0xFFFF). */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_write(0U, 0x10U, &dummy, 0x10000U, false));
  TEST_END("i3c write: oob channel / uninit / len overflow errors");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_read error paths (lines 660, 663).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards)
 * ---------------------------------------------------------------------------
 */
static void test_read_error_paths(void)
{
  TEST_BEGIN("i3c read: oob channel / uninit errors");
  uint8_t buf[4] = {};

  /* Line 660: channel out of range. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_read(255U, 0x10U, buf, 4U, false));

  /* Line 663: channel 0 not initialized. */
  prep_uninit();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_read(0U, 0x10U, buf, 4U, false));
  TEST_END("i3c read: oob channel / uninit errors");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_transfer validation (lines 701, 704).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards)
 * ---------------------------------------------------------------------------
 */
static void test_transfer_error_paths(void)
{
  TEST_BEGIN("i3c transfer: oob channel / native-mode error paths");
  uint8_t buf[2] = {};

  /* Line 701: channel out of range. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_transfer(255U, 0x10U, nullptr, 0U, buf, 2U));

  /* Line 704: channel 0 in native mode is not I2C. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_transfer(0U, 0x10U, nullptr, 0U, buf, 2U));
  TEST_END("i3c transfer: oob channel / native-mode error paths");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_set_clock -- entire function uncovered (lines 709-718).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_set_clock_coverage(void)
{
  TEST_BEGIN("i3c set_clock: full function coverage (lines 709-718)");

  /* Lines 709, 711, 712: entry + oob channel guard. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_set_clock(255U, 100000U, 8000000U));

  /* Lines 714, 715: channel 0 in native mode is not I2C. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_set_clock(0U, 100000U, 8000000U));

  /* Lines 717, 718: channel 0 in I2C mode -- happy path. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_set_clock(0U, 100000U, 8000000U));
  TEST_END("i3c set_clock: full function coverage (lines 709-718)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_scan error returns (lines 723, 726).
 * The happy path (line 728) is already covered by test_ra8_i3c_i2c.c.
 *
 * @par MC/DC:
 * (no new compound decisions; single guards)
 * ---------------------------------------------------------------------------
 */
static void test_scan_error_paths(void)
{
  TEST_BEGIN("i3c scan: oob channel / native-mode error returns");
  bool acked = false;

  /* Line 723: channel out of range. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_scan(255U, 0x10U, &acked));

  /* Line 726: channel 0 in native mode is not I2C. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_scan(0U, 0x10U, &acked));
  TEST_END("i3c scan: oob channel / native-mode error returns");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_get_errors -- entire function uncovered (lines 731-740).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_get_errors_coverage(void)
{
  TEST_BEGIN("i3c get_errors: full function coverage (lines 731-740)");
  uint8_t mask = 0U;

  /* Lines 731, 733, 734: entry + oob channel guard. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_get_errors(255U, &mask));

  /* Lines 736, 737: channel 0 in native mode. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_get_errors(0U, &mask));

  /* Lines 739, 740: channel 0 in I2C mode -- happy path. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_get_errors(0U, &mask));
  TEST_END("i3c get_errors: full function coverage (lines 731-740)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_clear_errors -- entire function uncovered (lines 742-751).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_clear_errors_coverage(void)
{
  TEST_BEGIN("i3c clear_errors: full function coverage (lines 742-751)");

  /* Lines 742, 744, 745: oob channel. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_clear_errors(255U));

  /* Lines 747, 748: native mode channel. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_clear_errors(0U));

  /* Lines 750, 751: I2C mode -- happy path. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_clear_errors(0U));
  TEST_END("i3c clear_errors: full function coverage (lines 742-751)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_abort -- entire function uncovered (lines 753-762).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_abort_coverage(void)
{
  TEST_BEGIN("i3c abort: full function coverage (lines 753-762)");

  /* Lines 753, 755, 756: oob channel. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_abort(255U));

  /* Lines 758, 759: native mode channel. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_abort(0U));

  /* Lines 761, 762: I2C mode -- happy path.  internal_i3c_i2c_abort
   * performs only register writes (no polling), safe in the sim. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_abort(0U));
  TEST_END("i3c abort: full function coverage (lines 753-762)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_peripheral_close -- entire function uncovered (lines 787-796).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_peripheral_close_coverage(void)
{
  TEST_BEGIN("i3c peripheral_close: full function coverage (lines 787-796)");

  /* Lines 787, 789, 790: oob channel. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_peripheral_close(255U));

  /* Lines 792, 793: native mode channel. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_peripheral_close(0U));

  /* Lines 795, 796: I2C mode channel -- happy path.
   * internal_i3c_i2c_peripheral_close performs register writes only. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_close(0U));
  TEST_END("i3c peripheral_close: full function coverage (lines 787-796)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_peripheral_send -- entire function uncovered (lines 798-807).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_peripheral_send_coverage(void)
{
  TEST_BEGIN("i3c peripheral_send: full function coverage (lines 798-807)");
  const uint8_t data[1] = {0xA5U};

  /* Lines 798, 800, 801: oob channel. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_peripheral_send(255U, data, 1U));

  /* Lines 803, 804: native mode channel. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_peripheral_send(0U, data, 1U));

  /* Lines 806, 807: I2C mode -- len=0 so the for-loop body is never
   * entered, avoiding any TDBEF0 spin.  Returns k_ra8_ok. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_send(0U, nullptr, 0U));
  TEST_END("i3c peripheral_send: full function coverage (lines 798-807)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_peripheral_receive -- entire function uncovered (lines 809-818).
 *
 * @par MC/DC:
 * (no new compound decisions; single guards on each return path)
 * ---------------------------------------------------------------------------
 */
static void test_peripheral_receive_coverage(void)
{
  TEST_BEGIN("i3c peripheral_receive: full function coverage (lines 809-818)");
  uint8_t buf[1] = {0U};

  /* Lines 809, 811, 812: oob channel. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_peripheral_receive(255U, buf, 1U));

  /* Lines 814, 815: native mode channel. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_peripheral_receive(0U, buf, 1U));

  /* Lines 817, 818: I2C mode -- len=0 so the for-loop body is skipped,
   * no RDBFF0 spin.  Returns k_ra8_ok. */
  prep_i2c();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_receive(0U, nullptr, 0U));
  TEST_END("i3c peripheral_receive: full function coverage (lines 809-818)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_peripheral_status error returns (lines 823, 826).
 * The happy path is already covered by test_ra8_i3c_i2c_peripheral.c.
 *
 * @par MC/DC:
 * (no new compound decisions; single guards)
 * ---------------------------------------------------------------------------
 */
static void test_peripheral_status_error_paths(void)
{
  TEST_BEGIN("i3c peripheral_status: oob / native-mode error returns");
  uint8_t mask = 0U;

  /* Line 823: channel out of range. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i3c_peripheral_status(255U, &mask));

  /* Line 826: channel 0 in native mode. */
  prep_native();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_i3c_peripheral_status(0U, &mask));
  TEST_END("i3c peripheral_status: oob / native-mode error returns");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_ibi_read -- IBI type hot-join branch (line 939).
 *
 * @par MC/DC:
 * Decision (ra8_i3c_ibi_read line 938 else-if):
 *   ``ibi_id == k_ibi_id_hot_join`` (single condition, already in else of
 *   the IBI_ST==0 guard).
 * - V1: ibi_id=0x84 (not hot-join), IBI_ST=0 -> interrupt type (line 937)
 *       [covered by test_ra8_i3c.c test_ibi_read_one]
 * - V2: ibi_id=0x02, IBI_ST=1 -> hot-join  (line 939, this test)
 * - V3: ibi_id=0x04, IBI_ST=1 -> main-request (line 941, next test)
 * Pairs (V2,V3) isolate the ibi_id condition inside the else block.
 * ---------------------------------------------------------------------------
 */
static void test_ibi_hot_join(void)
{
  TEST_BEGIN("i3c ibi_read: hot-join type (line 939)");
  prep_native();
  /* Mark the IBI queue non-empty. */
  ra8_i3c()->NTST = (uint32_t)k_ra8_i3c_ntst_ibiqeff_mask;
  /* IBI_ST=1 (bit 31), ibi_id=0x02 (hot-join), len=0. */
  ra8_i3c()->NIBIQP = k_i3c_ibi_status_error_a;

  ra8_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ(k_ra8_i3c_ibi_type_hot_join, ibi.type);
  TEST_ASSERT_EQ(0U, ibi.payload_len);
  TEST_END("i3c ibi_read: hot-join type (line 939)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_ibi_read -- IBI type main-request branch (line 941).
 *
 * @par MC/DC:
 * See test_ibi_hot_join for the vector table.  This test provides V3.
 * ---------------------------------------------------------------------------
 */
static void test_ibi_main_request(void)
{
  TEST_BEGIN("i3c ibi_read: main-request type (line 941)");
  prep_native();
  ra8_i3c()->NTST = (uint32_t)k_ra8_i3c_ntst_ibiqeff_mask;
  /* IBI_ST=1 (bit 31), ibi_id=0x04 (mainship-request, not 0x02), len=0. */
  ra8_i3c()->NIBIQP = k_i3c_ibi_status_error_b;

  ra8_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ(k_ra8_i3c_ibi_type_main_request, ibi.type);
  TEST_ASSERT_EQ(0U, ibi.payload_len);
  TEST_END("i3c ibi_read: main-request type (line 941)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_i3c_ibi_read -- payload length clamping (lines 947-948).
 *
 * @par MC/DC:
 * Decision (ra8_i3c_ibi_read line 946): ``to_copy > k_max_ibi_payload``
 * where k_max_ibi_payload = sizeof(out_ibi->payload) = 8.
 * - V1: len=2  -> 2 <= 8 -> no clamp (covered by test_ra8_i3c.c)
 * - V2: len=9  -> 9 > 8  -> clamp to 8 (lines 947-948, this test)
 * V1+V2 isolate the single condition.
 * ---------------------------------------------------------------------------
 */
static void test_ibi_payload_clamping(void)
{
  TEST_BEGIN("i3c ibi_read: payload length clamped to 8 (lines 947-948)");
  prep_native();
  ra8_i3c()->NTST = (uint32_t)k_ra8_i3c_ntst_ibiqeff_mask;
  /* IBI_ST=0 (interrupt type), ibi_id=0x84, len=9 (bits[7:0]=9).
   * 9 > sizeof(out_ibi->payload)=8 triggers the clamp on line 947. */
  ra8_i3c()->NIBIQP  = k_i3c_ibi_status_with_len;
  ra8_i3c()->NTDTBP0 = k_i3c_probe_word; /* payload data (two reads of 4 bytes) */

  ra8_i3c_ibi_t ibi = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_ibi_read(&ibi));
  TEST_ASSERT_EQ(k_ra8_i3c_ibi_type_interrupt, ibi.type);
  /* payload_len is clamped to 8. */
  TEST_ASSERT_EQ(8U, ibi.payload_len);
  TEST_END("i3c ibi_read: payload length clamped to 8 (lines 947-948)");
}

/* ---------------------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------------------
 */

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
  test_send_ccc_large_payload_and_fifo_partial,
  test_init_i2c_mode_error,
  test_deinit_oob,
  test_attach_handler_oob,
  test_dispatch_oob_and_i2c_mode,
  test_recv_ccc_internal_invalid,
  test_write_error_paths,
  test_read_error_paths,
  test_transfer_error_paths,
  test_set_clock_coverage,
  test_scan_error_paths,
  test_get_errors_coverage,
  test_clear_errors_coverage,
  test_abort_coverage,
  test_peripheral_close_coverage,
  test_peripheral_send_coverage,
  test_peripheral_receive_coverage,
  test_peripheral_status_error_paths,
  test_ibi_hot_join,
  test_ibi_main_request,
  test_ibi_payload_clamping,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_i3c_cov.c\n");
  return 0;
}
