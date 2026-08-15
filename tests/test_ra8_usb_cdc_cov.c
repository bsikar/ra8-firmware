/**
 * @file test_ra8_usb_cdc_cov.c
 * @brief Coverage-focused unit tests for the native USB CDC ACM class
 *        layer (`libs/ra8_hal/src/ra8_usb_cdc.c`)
 *
 * @details
 * Companion to `test_ra8_usb_cdc.c`. That file covers the public-API
 * happy path and argument rejection; this file drives the remaining
 * class-specific SETUP dispatch legs and the bulk-OUT drain path that
 * the original suite never exercised:
 *
 *  - SET_LINE_CODING (bRequest 0x20) -- runs the control-write case in
 *    `internal_dispatch_class_setup`, which in turn calls the
 *    data-stage-pull stub `internal_pull_data_stage`.
 *  - An unrecognised class bRequest -- runs the `default` leg of the
 *    dispatch switch.
 *  - `ra8_usb_cdc_recv` forwarding to `ra8_usb_queue_out` on an empty
 *    bulk-OUT pipe (the no-data drain leg).
 *
 * All hardware is the host register-window model brought up by
 * `ra8_fake_mmap_reset` + `ra8_mstp_init`; no live controller, no timers,
 * no SIGALRM. Register pre-seeding is used where a specific controller
 * status is required to make a leg deterministic.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_cdc.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_cdc_cov_t
 * @brief Fixed SETUP-packet field values used by the coverage cases.
 *
 * @details Mirrors the internal (file-static) constants of
 * `ra8_usb_cdc.c` that are not exported through the public header so the
 * tests can build class-specific SETUP envelopes.
 */
typedef enum : uint16_t {
  k_test_cdc_iface_class    = 0x21U, /**< bmRequestType class|iface|out. */
  k_test_cdc_iface_class_in = 0xA1U, /**< bmRequestType class|iface|in.  */
  k_test_cdc_unknown_req    = 0x99U, /**< bRequest not handled by CDC.   */
  k_test_cdc_line_coding_wl = 7U,    /**< wLength of a line-coding xfer. */
  k_test_cdc_recv_cap       = 8U,    /**< Bulk-OUT drain buffer size.    */
} test_cdc_cov_t;

/**
 * @brief Reset the fake MMIO window and CDC singleton to a known
 * pre-init state.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_cdc_deinit();
}

/**
 * @test test_handle_setup_set_line_coding
 *
 * @details Sends SET_LINE_CODING (bRequest 0x20) with a valid
 * class|interface bmRequestType. This runs the control-write case of
 * `internal_dispatch_class_setup`: it allocates the local payload
 * buffer, calls `internal_pull_data_stage` (the EP0 OUT data-stage
 * stub), observes that the stub reports `k_ra8_err_not_supported`
 * (so the `if (pull == k_ra8_ok)` guard is false and the line-coding
 * apply is skipped), and finishes by ACKing the status stage via
 * `ra8_usb_control_response`.
 *
 * @par MC/DC:
 * Decision `(bm != iface) && (bm != iface_in)` in
 * `ra8_usb_cdc_handle_setup` -- this case exercises bm == iface, so the
 * first condition is false and the `&&` short-circuits (decision =
 * false, request dispatched). The complementary vectors that make the
 * decision true (non-class bm) live in `test_ra8_usb_cdc.c`
 * (`test_handle_setup_rejects_standard`) and in this file's
 * `test_handle_setup_unknown_class_request` (bm == iface_in, second
 * condition false). Together they satisfy the N+1 = 3 pattern.
 */
static void test_handle_setup_set_line_coding(void)
{
  TEST_BEGIN("ra8_usb_cdc_handle_setup drives SET_LINE_CODING dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_cdc_iface_class,
    .b_request       = (uint8_t)k_ra8_cdc_req_set_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = (uint16_t)k_test_cdc_line_coding_wl,
  };
  /* The data-stage stub returns k_ra8_err_not_supported, so the apply
   * branch is skipped and the handler ACKs the status stage: k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));

  /* The stored line coding must be untouched (still the 9600/8/N/1
   * seed) because no payload was applied on this path. */
  ra8_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ(9600U, coding.dte_rate);
  TEST_ASSERT_EQ(8U, coding.data_bits);
  TEST_END("ra8_usb_cdc_handle_setup drives SET_LINE_CODING dispatch");
}

/**
 * @test test_handle_setup_unknown_class_request
 *
 * @details Sends a class|interface SETUP whose bRequest is not one of
 * the three CDC ACM requests the layer handles (0x20/0x21/0x22). The
 * envelope check passes (bmRequestType is a class request), so control
 * reaches `internal_dispatch_class_setup`, whose `switch` falls to the
 * `default` leg and returns `k_ra8_err_not_supported`. This is the only
 * path that runs that leg -- non-class SETUPs are rejected earlier at
 * the envelope and never enter the dispatch switch.
 *
 * @par MC/DC:
 * Decision `(bm != iface) && (bm != iface_in)` -- this case uses
 * bm == iface_in, so the first condition is true and the second is
 * false (decision = false, request dispatched). Paired with the
 * bm == iface vector in `test_handle_setup_set_line_coding` and the
 * bm == 0x80 (both true) vector in `test_ra8_usb_cdc.c`, the second
 * condition is shown to independently affect the outcome.
 */
static void test_handle_setup_unknown_class_request(void)
{
  TEST_BEGIN("ra8_usb_cdc_handle_setup default-rejects unknown class req");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_cdc_iface_class_in,
    .b_request       = (uint8_t)k_test_cdc_unknown_req,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_cdc_handle_setup(&setup));
  TEST_END("ra8_usb_cdc_handle_setup default-rejects unknown class req");
}

/**
 * @test test_recv_forwards_to_queue_out
 *
 * @details After init, calls `ra8_usb_cdc_recv` with a non-NULL buffer
 * and a non-zero capacity so both `RA8_CHECK_NULL_PTR` guards and the
 * `*inout_len == 0` guard pass and control reaches the
 * `ra8_usb_queue_out` forward. The bulk-OUT pipe's BRDYSTS status bit is
 * pre-cleared so `ra8_usb_queue_out` takes its fast no-data leg
 * (BRDYSTS bit for the pipe clear -> `k_ra8_err_no_data`) without
 * entering any FRDY spin. The original suite only ever called `recv`
 * before init or with bad arguments, so the forwarding statement was
 * never executed.
 *
 * @par MC/DC:
 * (no compound decision in the code under test on this path -- the
 * `recv` guards are independent single-condition `if`s and NULL-ptr
 * macros; `ra8_usb_queue_out`'s own compound argument check is covered
 * in the `ra8_usb_xfer` test suite)
 */
static void test_recv_forwards_to_queue_out(void)
{
  TEST_BEGIN("ra8_usb_cdc_recv forwards to ra8_usb_queue_out (no-data leg)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  /* Present an empty bulk-OUT pipe: clear BRDYSTS so the pipe-2 status
   * bit reads 0 and ra8_usb_queue_out reports no data. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->BRDYSTS               = 0U;

  uint8_t  buf[k_test_cdc_recv_cap] = {};
  uint16_t len                      = (uint16_t)k_test_cdc_recv_cap;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_cdc_recv(buf, &len));
  /* The no-data leg zeroes the reported length. */
  TEST_ASSERT_EQ(0U, len);
  TEST_END("ra8_usb_cdc_recv forwards to ra8_usb_queue_out (no-data leg)");
}

int32_t main(void)
{
  test_handle_setup_set_line_coding();
  test_handle_setup_unknown_class_request();
  test_recv_forwards_to_queue_out();
  return 0;
}
