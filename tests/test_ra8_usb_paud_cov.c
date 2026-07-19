/**
 * @file test_ra8_usb_paud_cov.c
 * @brief Line-coverage top-up for the native USB device-side Audio (UAC1)
 *        class layer (`libs/ra8_hal/src/ra8_usb_paud.c`).
 *
 * @details
 * The sibling functional/MC-DC suite `test_ra8_usb_paud.c` exercises the
 * public contract but only ever drives the descriptor / recv-frame /
 * setup-callback-error entry points BEFORE `ra8_usb_paud_init` (the
 * `k_ra8_err_invalid_state` rejection legs). This file drives the same
 * entry points AFTER a successful init so the post-init statement bodies
 * are covered, and seeds the FS controller's OUT-pipe FIFO registers so
 * the isochronous drain success leg runs to completion. gcovr merges the
 * `.gcda` produced here with the sibling target's for the same source.
 *
 * All hardware effects are produced by pre-seeding the memory-mapped
 * register RAM exposed by `RA8_SIMULATOR_MODE` + `ra8_sim_mmap` (BRDYSTS /
 * CFIFOCTR / CFIFO). `internal_wait_frdy` converges on its first poll
 * via the unarmed ra8_sim_mmio seam, so no timing/SIGALRM injection is
 * used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_paud.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum usb_paud_cov_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_usb_paud_cov_got_ffff = 0xFFFFU,
} usb_paud_cov_uint16_const_t;

/**
 * @enum test_paud_cov_t
 * @brief Numeric fixtures for the coverage vectors below.
 *
 * @details Tests are exempt from the magic-number gate; these names keep
 * the intent readable rather than to satisfy the enum-constant rule.
 */
typedef enum : uint16_t {
  k_test_paud_iso_out_pipe  = 2U,      /**< PIPE2 carries iso OUT.        */
  k_test_paud_iso_out_bit   = 0x0004U, /**< BRDYSTS bit for PIPE2 (1<<2). */
  k_test_paud_drain_len     = 1U,      /**< One byte staged in the FIFO.  */
  k_test_paud_fifo_word     = 0xBBAAU, /**< CFIFO data word (LSB=0xAA).   */
  k_test_paud_fifo_lsb      = 0x00AAU, /**< Expected first drained byte.  */
  k_test_paud_recv_cap      = 8U,      /**< recv_frame buffer capacity.   */
  k_test_paud_desc_len      = 4U,      /**< Non-zero descriptor length.   */
  k_test_paud_desc_len_zero = 0U,      /**< Zero descriptor length.       */
  k_test_paud_ctl_iface_out = 0x21U,   /**< Class | Iface | Out envelope. */
} test_paud_cov_t;

/** @brief Tiny descriptor blob used by the set_descriptors happy path. */
static const uint8_t s_desc_blob[k_test_paud_desc_len] = {0x09U, 0x04U, 0x00U, 0x00U};

static int32_t   s_cb_calls  = 0;
static ra8_err_t s_cb_return = k_ra8_ok;

/**
 * @brief Class-setup callback whose return code the test controls.
 *
 * @param[in] ctx   Unused caller context.
 * @param[in] setup Unused SETUP envelope.
 * @return The value staged in `s_cb_return`.
 */
static ra8_err_t test_paud_cov_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  (void)setup;
  s_cb_calls++;
  return s_cb_return;
}

/** @brief Reset the simulator, module-stop table, and class singleton. */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_paud_close();
  s_cb_calls  = 0;
  s_cb_return = k_ra8_ok;
}

/**
 * @test test_set_descriptors_post_init
 *
 * @par MC/DC:
 * (single-condition decisions only -- `RA8_CHECK_NULL_PTR(desc)` and
 * `if (desc_len == 0U)`; no `&&`/`||` in the covered statements, so
 * per-condition independence is trivially the branch pair below)
 *
 * @details Drives `ra8_usb_paud_set_descriptors` after a successful init:
 *  - NULL desc  -> `k_ra8_err_null_ptr`   (null guard, false leg of the ptr check)
 *  - len == 0   -> `k_ra8_err_invalid_arg` (zero-length guard true leg)
 *  - valid pair -> `k_ra8_ok`              (pointer + length stored)
 */
static void test_set_descriptors_post_init(void)
{
  TEST_BEGIN("ra8_usb_paud_set_descriptors stores a valid blob after init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_paud_set_descriptors(nullptr, (uint16_t)k_test_paud_desc_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_paud_set_descriptors(s_desc_blob, (uint16_t)k_test_paud_desc_len_zero));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_paud_set_descriptors(s_desc_blob, (uint16_t)k_test_paud_desc_len));

  TEST_END("ra8_usb_paud_set_descriptors stores a valid blob after init");
}

/**
 * @test test_recv_frame_post_init
 *
 * @par MC/DC:
 * (single-condition decisions only -- `if (max_len == 0U)` and
 * `if (err == k_ra8_ok)`; both branch legs are taken across the three
 * calls below, giving full branch coverage of each 1-condition decision)
 *
 * @details Drives `ra8_usb_paud_recv_frame` after init:
 *  - max_len == 0                -> `k_ra8_err_invalid_arg` (arg guard true leg)
 *  - BRDYSTS clear (empty pipe)  -> `k_ra8_err_no_data`, `*got_len == 0`
 *    (queue_out no-data fast path -> `err != k_ra8_ok` else leg, line 333)
 *  - BRDYSTS + CFIFOCTR seeded   -> `k_ra8_ok`, one byte drained
 *    (queue_out success -> `err == k_ra8_ok` then leg, line 331)
 */
static void test_recv_frame_post_init(void)
{
  TEST_BEGIN("ra8_usb_paud_recv_frame drains a seeded iso-OUT FIFO");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  uint8_t  buf[k_test_paud_recv_cap] = {};
  uint16_t got                       = k_usb_paud_cov_got_ffff;

  /* max_len == 0 -> invalid_arg, no pipe access. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_recv_frame(buf, 0U, &got));

  volatile r_usb_regs_t* reg = ra8_usb_fs();

  /* Empty pipe: BRDYSTS clear for PIPE2 -> queue_out no-data else leg. */
  reg->BRDYSTS = 0U;
  got          = k_usb_paud_cov_got_ffff;
  TEST_ASSERT_EQ(k_ra8_err_no_data,
                 ra8_usb_paud_recv_frame(buf, (uint16_t)k_test_paud_recv_cap, &got));
  TEST_ASSERT_EQ(0U, got);

  /* Staged OUT packet: BRDYSTS latched + FRDY asserted + one byte in the
   * FIFO -> queue_out success leg copies `inout_len` into `*got_len`. */
  reg->BRDYSTS  = (uint16_t)k_test_paud_iso_out_bit;
  reg->CFIFOCTR = (uint16_t)(k_ra8_fifoctr_frdy | (uint16_t)k_test_paud_drain_len);
  reg->CFIFO    = (uint16_t)k_test_paud_fifo_word;
  got           = k_usb_paud_cov_got_ffff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_recv_frame(buf, (uint16_t)k_test_paud_recv_cap, &got));
  TEST_ASSERT_EQ(k_test_paud_drain_len, got);
  TEST_ASSERT_EQ(k_test_paud_fifo_lsb, buf[0]);

  TEST_END("ra8_usb_paud_recv_frame drains a seeded iso-OUT FIFO");
}

/**
 * @test test_handle_setup_callback_stall
 *
 * @par MC/DC:
 * (single-condition decision `if (cb_err != k_ra8_ok)`; the sibling suite
 * covers the `== k_ra8_ok` leg, this case covers the `!= k_ra8_ok` leg,
 * completing branch coverage of the 1-condition decision on line 426)
 *
 * @details A registered class handler that returns a non-`k_ra8_ok` code
 * makes `ra8_usb_paud_handle_setup` STALL EP0 via
 * `ra8_usb_control_response(speed, false)`. Under simulation that helper
 * returns `k_ra8_ok` (it only drives the DCPCTR PID=STALL register), so
 * the dispatcher itself reports `k_ra8_ok` while the callback fired once.
 */
static void test_handle_setup_callback_stall(void)
{
  TEST_BEGIN("ra8_usb_paud_handle_setup stalls EP0 when the callback rejects");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_attach_setup_handler(test_paud_cov_cb, nullptr));

  s_cb_return           = k_ra8_err_not_supported;
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_paud_ctl_iface_out,
    .b_request       = (uint8_t)k_ra8_paud_req_set_cur,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_cb_calls);

  TEST_END("ra8_usb_paud_handle_setup stalls EP0 when the callback rejects");
}

int32_t main(void)
{
  test_set_descriptors_post_init();
  test_recv_frame_post_init();
  test_handle_setup_callback_stall();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_paud_cov.c\n");
  return 0;
}
