/**
 * @file test_ra8_usb_hmsc_cov.c
 * @brief Coverage-focused unit tests for the native USB host-side MSC
 *        (Bulk-Only-Transport) SCSI command path
 *
 * @details
 * The base suite (`test_ra8_usb_hmsc.c`) exercises the pure protocol units
 * (CBW build / CSW decode) and the pre-init / pre-attach guards, but it
 * can never drive the SCSI command entry points past
 * `internal_check_ready`, because a real attach only happens at the end of
 * the hardware enumeration ladder (`ra8_usb_hmsc_enumerate`), which the
 * plain-RAM register mirror cannot answer.
 *
 * This suite injects the post-enumeration precondition -- the driver's
 * `attached` shadow flag -- directly through the module-private singleton
 * `s_usb_hmsc_state` (shared via `ra8_hal_internal.h`, the same object the
 * enumeration ladder writes on a real attach). With the device marked
 * attached, every SCSI entry point (INQUIRY / READ_CAPACITY(10) /
 * READ(10) / WRITE(10)) runs its full CDB assembly and Bulk-Only-Transport
 * (BOT) command-block-wrapper (CBW) push.
 *
 * The bulk-OUT completion wait (`internal_host_wait_pipe` on `BEMPSTS`)
 * cannot signal success under the host fake: `ra8_usb_host_bulk_out`
 * W0C-clears the pipe's `BEMPSTS` bit before waiting, and the plain-RAM
 * `ra8_fake_mmap` backing has no active USB SIE model to re-assert it, so
 * the CBW push deterministically returns `k_ra8_err_hw_timeout`. That is
 * exactly the leg these tests drive: the CDB builders, the CBW assembly,
 * the pipe push, and the error propagation back out of every entry point.
 * The command-completion tail (data-IN pull, CSW read/validate, response
 * decode) lives strictly downstream of a successful CBW push and is
 * therefore not reachable on the bare-metal host -- those lines are marked
 * `GCOVR_EXCL_*` in the source with a per-region justification.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_hal_internal.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_hmsc_cov_const_t
 * @brief Local fixture constants for the coverage suite.
 */
typedef enum : uint8_t {
  k_test_lun_ok      = 0U,  /**< In-range logical unit.                   */
  k_test_lun_bad     = 5U,  /**< Out-of-range LUN (> k_ra8_hmsc_max_lun). */
  k_test_block_count = 1U,  /**< One-block READ(10) / WRITE(10).          */
  k_test_scratch_len = 16U, /**< Scratch data buffer bytes.               */
} test_hmsc_cov_const_t;

/**
 * @brief Reset the register mirror and driver state to a known baseline.
 *
 * @details Mirrors `test_ra8_usb_hmsc.c::prep`: wipes the fake MMIO
 * windows, brings the module-stop gate up, and force-closes any prior
 * host-MSC session so each test starts from a clean, uninitialised
 * driver.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hmsc_close();
}

/**
 * @brief Inject the post-enumeration "device attached" precondition.
 *
 * @details On real hardware `ra8_usb_hmsc_enumerate` sets this flag after a
 * successful descriptor walk + SET_CONFIGURATION + pipe open. The plain-RAM
 * register mirror cannot answer that ladder, so the tests write the shadow
 * flag directly -- the same singleton object the ladder mutates -- to reach
 * the SCSI command path. `initialized` is already true from
 * `ra8_usb_hmsc_init`; only `attached` needs injecting.
 */
static void mark_attached(void)
{
  s_usb_hmsc_state.attached = true;
}

/* ---- SCSI INQUIRY: CDB build + CBW push (times out at the bulk-OUT wait) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in the code this case touches -- the SCSI entry
 * point, `internal_check_ready` LUN-in-range leg, the INQUIRY CDB builder,
 * and the BOT CBW push are all straight-line; the bulk-OUT completion wait
 * returns `k_ra8_err_hw_timeout` because the fake cannot re-assert the
 * pipe's BEMPSTS bit)
 */
static void test_inquiry_attached_drives_bot_push(void)
{
  TEST_BEGIN("inquiry (attached) builds CDB + pushes CBW, bulk-OUT wait times out");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  mark_attached();

  ra8_usb_hmsc_inquiry_response_t resp = {};
  /* Reaches internal_check_ready (LUN in range -> k_ra8_ok),
   * internal_build_inquiry_cdb, internal_run_data_in -> internal_issue_cbw
   * -> internal_send_cbw (bulk-OUT). The CBW push cannot complete under the
   * fake, so the whole command reports the bulk-OUT timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_inquiry(k_test_lun_ok, &resp));

  TEST_END("inquiry (attached) builds CDB + pushes CBW, bulk-OUT wait times out");
}

/* ---- SCSI READ_CAPACITY(10): CDB build + CBW push ---- */

/**
 * @par MC/DC:
 * (no compound decisions in the code this case touches -- the read-capacity
 * entry point, its CDB builder, and the BOT CBW push are straight-line;
 * the decode of the 8-byte capacity response is downstream of a successful
 * push and is not reached)
 */
static void test_read_capacity_attached_drives_bot_push(void)
{
  TEST_BEGIN("read_capacity (attached) builds CDB + pushes CBW, bulk-OUT wait times out");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  mark_attached();

  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_hmsc_read_capacity(k_test_lun_ok, &block_count, &block_size));

  TEST_END("read_capacity (attached) builds CDB + pushes CBW, bulk-OUT wait times out");
}

/* ---- SCSI READ(10): READ(10) CDB build + data-IN CBW push ---- */

/**
 * @par MC/DC:
 * (the only decision touched is `if (block_count == 0U)` -- a single
 * condition, driven false with block_count = 1; the base suite already
 * drives it true. The READ(10) CDB builder and CBW push are straight-line)
 */
static void test_read10_attached_drives_bot_push(void)
{
  TEST_BEGIN("read10 (attached) builds READ(10) CDB + pushes CBW, bulk-OUT wait times out");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  mark_attached();

  uint8_t buf[k_test_scratch_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_hmsc_read10(k_test_lun_ok, 0U, k_test_block_count, buf));

  TEST_END("read10 (attached) builds READ(10) CDB + pushes CBW, bulk-OUT wait times out");
}

/* ---- SCSI WRITE(10): WRITE(10) CDB build + data-OUT CBW push ---- */

/**
 * @par MC/DC:
 * (the only decision touched is `if (block_count == 0U)` -- single
 * condition, driven false with block_count = 1. The WRITE(10) CDB builder
 * and the data-OUT run (`internal_run_data_out`) up to and including the
 * CBW push are straight-line; the per-packet data chunk loop is downstream
 * of a successful push and is not reached)
 */
static void test_write10_attached_drives_bot_push(void)
{
  TEST_BEGIN("write10 (attached) builds WRITE(10) CDB + pushes CBW, bulk-OUT wait times out");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  mark_attached();

  uint8_t buf[k_test_scratch_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_hmsc_write10(k_test_lun_ok, 0U, k_test_block_count, buf));

  TEST_END("write10 (attached) builds WRITE(10) CDB + pushes CBW, bulk-OUT wait times out");
}

/* ---- internal_check_ready LUN-out-of-range leg (attached, LUN > max) ---- */

/**
 * @par MC/DC:
 * (no compound decisions -- `internal_check_ready` is three independent
 * single-condition guards. The base suite covers the !initialized and
 * !attached legs; this case covers the `target_lun > k_ra8_hmsc_max_lun`
 * leg with an attached device and an out-of-range LUN, so the check
 * reaches and rejects on the LUN guard rather than short-circuiting on the
 * attach state. Every SCSI entry point funnels through the same guard.)
 */
static void test_scsi_lun_out_of_range_rejected(void)
{
  TEST_BEGIN("attached SCSI ops reject an out-of-range LUN via internal_check_ready");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  mark_attached();

  ra8_usb_hmsc_inquiry_response_t resp                    = {};
  uint32_t                        block_count             = 0U;
  uint32_t                        block_size              = 0U;
  uint8_t                         buf[k_test_scratch_len] = {};

  /* Non-NULL args clear the null guards; the attached state clears the
   * state guards; the LUN guard (target_lun > k_ra8_hmsc_max_lun) then
   * rejects -- the leg the base suite cannot reach. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_inquiry(k_test_lun_bad, &resp));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_read_capacity(k_test_lun_bad, &block_count, &block_size));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_read10(k_test_lun_bad, 0U, k_test_block_count, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_write10(k_test_lun_bad, 0U, k_test_block_count, buf));

  TEST_END("attached SCSI ops reject an out-of-range LUN via internal_check_ready");
}

int32_t main(void)
{
  test_inquiry_attached_drives_bot_push();
  test_read_capacity_attached_drives_bot_push();
  test_read10_attached_drives_bot_push();
  test_write10_attached_drives_bot_push();
  test_scsi_lun_out_of_range_rejected();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hmsc_cov.c\n");
  return 0;
}
