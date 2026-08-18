/**
 * @file test_ra8_io_blockdev_usbmsc.c
 * @brief Unit tests for the USB mass-storage ra8_io block-device backend.
 *
 * @details
 * The USB-MSC backend is a thin shim: every vtable op forwards to the
 * ``ra8_usb_hmsc`` class layer on the LUN recorded at bind time, after the two
 * transport bounds the fabric cannot express itself (the 16-bit READ(10) /
 * WRITE(10) TRANSFER LENGTH, and a native block size other than 512).
 *
 * The class layer is driven the same way ``test_ra8_usb_hmsc_cov.c`` drives it:
 * ``ra8_fake_mmap`` supplies a plain-RAM register mirror, and the
 * post-enumeration ``attached`` precondition is injected through the
 * module-private singleton ``g_usb_hmsc_state`` -- the same object the
 * enumeration ladder writes on a real attach. Two class-layer legs are reached
 * from there without any bounded wait:
 *
 *   - initialised but not attached, which makes every SCSI entry point return
 *     ::k_ra8_err_invalid_state, proving the forward happened; and
 *   - attached with a LUN past the class layer's ceiling, which returns
 *     ::k_ra8_err_invalid_arg. That leg is the discriminator that the backend
 *     forwards ``state->lun`` rather than a hard-coded LUN 0: a hard-coded 0
 *     would reach the Bulk-Only Transport push instead.
 *
 * The vtable's own NULL guards on ``buf`` / ``out`` are unreachable through the
 * public ``ra8_io_blockdev_*`` dispatcher, which rejects those first, so they
 * are driven straight through the ``ra8_io_blockdev_backend.h`` seam -- the
 * implementer-facing contract those guards belong to.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_hal_internal.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_io_blockdev_usbmsc.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "unity_minimal.h"

/**
 * @enum test_usbmsc_const_t
 * @brief Fixture constants shared by every case in this suite.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_usbmsc_lun         = 3U,     /**< In-range LUN the handle binds to.    */
  k_test_usbmsc_lun_reject  = 5U,     /**< Past k_ra8_hmsc_max_lun; bind fails. */
  k_test_usbmsc_lun_poked   = 9U,     /**< Poked past the ceiling after bind.   */
  k_test_usbmsc_block_bytes = 512U,   /**< One logical block of scratch.        */
  k_test_usbmsc_one_block   = 1U,     /**< Single-block transfer length.        */
  k_test_usbmsc_over_max    = 65536U, /**< One block past the READ(10) bound.   */
} test_usbmsc_const_t;

/**
 * @brief Reset the register mirror and the class layer to a known baseline.
 *
 * @details
 * Mirrors `test_ra8_usb_hmsc_cov.c::internal_prep`: wipes the fake MMIO
 * windows, opens the module-stop gate, force-closes any prior host-MSC
 * session, then brings the class layer back up so the SCSI entry points are
 * past their "never initialised" guard.
 *
 * @pre The fake MMIO windows are mapped by the test harness.
 * @pre No other suite in this executable holds the class layer open.
 * @post The class layer is initialised and reports no attached device.
 * @post Every register window reads back as the reset mirror.
 *
 * @note File-local fixture helper; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hmsc_close();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
}

/**
 * @brief Bind a USB-MSC handle onto the in-range fixture LUN.
 *
 * @details
 * Every data-path case starts from the same bound handle, so the bind lives
 * here rather than being repeated. The caller owns both objects.
 *
 * @param[out] bd    Handle to bind.
 * @param[out] state Backend state to populate.
 *
 * @pre `bd` and `state` are caller-owned and zero-initialised.
 * @pre ::internal_prep has run, so the class layer is up.
 * @post `bd` dispatches to the USB-MSC backend on ::k_test_usbmsc_lun.
 * @post `state->lun` records that same LUN.
 *
 * @note File-local fixture helper; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bind(ra8_io_blockdev_t* bd, ra8_io_blockdev_usbmsc_state_t* state)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_usbmsc_init(bd, state, (uint8_t)k_test_usbmsc_lun));
}

/**
 * @brief Bind helper guards and the successful bind.
 *
 * @details
 * Drives all three decisions in ::ra8_io_blockdev_usbmsc_init and confirms a
 * successful bind wires the vtable, the context, and the LUN.
 *
 * @par MC/DC:
 * Three independent single-condition decisions, no compound decision exists.
 * - `bd == nullptr`: TRUE (NULL handle) and FALSE (every later call).
 * - `state == nullptr`: TRUE (NULL state) and FALSE (every later call).
 * - `lun > k_ra8_hmsc_max_lun`: TRUE (LUN 5) and FALSE (LUN 3).
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post The bound handle dispatches to the USB-MSC backend.
 * @post No class-layer state is mutated.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_bind_guards(void)
{
  TEST_BEGIN("usbmsc_init guards + bind");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state = {};
  ra8_io_blockdev_t              bd    = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_usbmsc_init(nullptr, &state, (uint8_t)k_test_usbmsc_lun));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_usbmsc_init(&bd, nullptr, (uint8_t)k_test_usbmsc_lun));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_usbmsc_init(&bd, &state, (uint8_t)k_test_usbmsc_lun_reject));
  TEST_ASSERT_NULL((const void*)bd.iface);

  internal_bind(&bd, &state);
  TEST_ASSERT_NOT_NULL((const void*)bd.iface);
  TEST_ASSERT(bd.ctx == &state);
  TEST_ASSERT_EQ(k_test_usbmsc_lun, state.lun);
  TEST_END("usbmsc_init guards + bind");
}

/**
 * @brief The read path bounds the transfer, then forwards on the bound LUN.
 *
 * @details
 * Confirms an over-long run is refused before any SCSI traffic, that a legal
 * run reaches the class layer (which reports no attached device), and that the
 * LUN carried into READ(10) is the bound one.
 *
 * @par MC/DC:
 * Decision under test: `count > k_ra8_io_usbmsc_max_transfer_blocks`, one
 * condition. TRUE vector: `count = 65536` -> ::k_ra8_err_out_of_range with no
 * class-layer call. FALSE vector: `count = 1` -> the class layer answers.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post Both legs of the transfer bound have executed.
 * @post The class layer is left attached; the next case re-preps.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_read_forwards(void)
{
  TEST_BEGIN("usbmsc read bounds the run and forwards the bound LUN");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state                          = {};
  ra8_io_blockdev_t              bd                             = {};
  uint8_t                        buf[k_test_usbmsc_block_bytes] = {};
  internal_bind(&bd, &state);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_read(&bd, 0U, (uint32_t)k_test_usbmsc_over_max, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_io_blockdev_read(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));

  state.lun                 = (uint8_t)k_test_usbmsc_lun_poked;
  g_usb_hmsc_state.attached = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_blockdev_read(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));
  TEST_END("usbmsc read bounds the run and forwards the bound LUN");
}

/**
 * @brief The write path bounds the transfer, then forwards on the bound LUN.
 *
 * @details
 * The mirror image of ::internal_test_usbmsc_read_forwards over WRITE(10).
 *
 * @par MC/DC:
 * Decision under test: `count > k_ra8_io_usbmsc_max_transfer_blocks`, one
 * condition. TRUE vector: `count = 65536` -> ::k_ra8_err_out_of_range with no
 * class-layer call. FALSE vector: `count = 1` -> the class layer answers.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post Both legs of the transfer bound have executed.
 * @post The class layer is left attached; the next case re-preps.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_write_forwards(void)
{
  TEST_BEGIN("usbmsc write bounds the run and forwards the bound LUN");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state                          = {};
  ra8_io_blockdev_t              bd                             = {};
  uint8_t                        buf[k_test_usbmsc_block_bytes] = {};
  internal_bind(&bd, &state);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_write(&bd, 0U, (uint32_t)k_test_usbmsc_over_max, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_io_blockdev_write(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));

  state.lun                 = (uint8_t)k_test_usbmsc_lun_poked;
  g_usb_hmsc_state.attached = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_blockdev_write(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));
  TEST_END("usbmsc write bounds the run and forwards the bound LUN");
}

/**
 * @brief Capability reporting propagates the class layer's refusal.
 *
 * @details
 * READ CAPACITY(10) cannot complete without a Bulk-Only Transport round trip,
 * so this drives the two legs a host can reach: the class layer refusing
 * because nothing is attached, and refusing the poked out-of-range LUN --
 * again the discriminator that the bound LUN, not a hard-coded 0, is used.
 *
 * @par MC/DC:
 * Decision under test: `cap != k_ra8_ok`, one condition. Only the TRUE arm is
 * host-reachable; the FALSE arm needs a completed SCSI command and is marked
 * `GCOVR_EXCL` in the source with that justification.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post No capability snapshot is written on either leg.
 * @post The class layer is left attached; the next case re-preps.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_get_caps_propagates(void)
{
  TEST_BEGIN("usbmsc get_caps propagates the class-layer refusal");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state = {};
  ra8_io_blockdev_t              bd    = {};
  ra8_io_blockdev_caps_t         caps  = {};
  internal_bind(&bd, &state);

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(0U, caps.block_count);

  state.lun                 = (uint8_t)k_test_usbmsc_lun_poked;
  g_usb_hmsc_state.attached = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(0U, caps.block_count);
  TEST_END("usbmsc get_caps propagates the class-layer refusal");
}

/**
 * @brief The absent optional vtable slots map to their defined results.
 *
 * @details
 * USB mass storage has no host erase primitive and the class layer buffers
 * nothing, so both optional slots are NULL and the fabric answers
 * ::k_ra8_err_not_supported and ::k_ra8_ok respectively. This pins the
 * Liskov-substitution behaviour a consumer sees when it swaps a thumb drive in
 * for an SD card.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post No SCSI traffic is issued by either call.
 * @post No backend state is mutated.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_optional_ops(void)
{
  TEST_BEGIN("usbmsc erase is unsupported and sync is a no-op");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state = {};
  ra8_io_blockdev_t              bd    = {};
  internal_bind(&bd, &state);

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_blockdev_erase(&bd, 0U, (uint32_t)k_test_usbmsc_one_block));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sync(&bd));
  TEST_END("usbmsc erase is unsupported and sync is a no-op");
}

/**
 * @brief Every vtable op rejects a handle whose context was lost.
 *
 * @details
 * The dispatcher validates the handle and its vtable but not the backend
 * cookie, so a handle whose `ctx` was cleared reaches the backend with NULL.
 * Each op must refuse rather than dereference it for the LUN.
 *
 * @par MC/DC:
 * Decision under test: `ctx == nullptr` inside each of the three vtable ops,
 * one condition each. TRUE arm here; FALSE arm on every other case's calls.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run.
 * @post No SCSI traffic is issued by any of the three calls.
 * @post No capability snapshot is written.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_null_ctx_guards(void)
{
  TEST_BEGIN("usbmsc vtable ops reject a lost backend context");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state                          = {};
  ra8_io_blockdev_t              bd                             = {};
  ra8_io_blockdev_caps_t         caps                           = {};
  uint8_t                        buf[k_test_usbmsc_block_bytes] = {};
  internal_bind(&bd, &state);
  bd.ctx = nullptr;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_read(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_write(&bd, 0U, (uint32_t)k_test_usbmsc_one_block, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_END("usbmsc vtable ops reject a lost backend context");
}

/**
 * @brief The vtable contract's own buffer guards, driven through the seam.
 *
 * @details
 * ::ra8_io_blockdev_read and friends reject a NULL buffer before forwarding, so
 * the backend's matching guards are unreachable from the public API. They are
 * still part of the ::ra8_io_blockdev_iface contract every backend signs, so
 * they are driven directly through the implementer-facing seam.
 *
 * @par MC/DC:
 * Decision under test: `buf == nullptr` in read and write, and
 * `out == nullptr` in get_caps -- one condition each. TRUE arm here; FALSE arm
 * on every other case's calls.
 *
 * @pre The fixture storage below is available on the stack.
 * @pre ::internal_prep has run, so the vtable is bound.
 * @post No SCSI traffic is issued by any of the three calls.
 * @post No capability snapshot is written.
 *
 * @note File-local test case; nothing escapes this executable.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_usbmsc_null_buffer_guards(void)
{
  TEST_BEGIN("usbmsc vtable ops reject a NULL buffer through the backend seam");
  internal_prep();

  ra8_io_blockdev_usbmsc_state_t state = {};
  ra8_io_blockdev_t              bd    = {};
  internal_bind(&bd, &state);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 bd.iface->read(bd.ctx, 0U, (uint32_t)k_test_usbmsc_one_block, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 bd.iface->write(bd.ctx, 0U, (uint32_t)k_test_usbmsc_one_block, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, bd.iface->get_caps(bd.ctx, nullptr));
  TEST_END("usbmsc vtable ops reject a NULL buffer through the backend seam");
}

/**
 * @brief Run every USB mass-storage block-device backend case.
 *
 * @return 0 on success; the harness calls exit(1) on the first failure.
 *
 * @pre The ra8_core_hal OBJECT library was built with coverage instrumentation.
 * @pre The fake MMIO windows are mapped by the test harness.
 * @post Every host-reachable body in ra8_io_blockdev_usbmsc.c has executed.
 * @post The class layer is left closed to the next executable's fixture.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_usbmsc_bind_guards();
  internal_test_usbmsc_read_forwards();
  internal_test_usbmsc_write_forwards();
  internal_test_usbmsc_get_caps_propagates();
  internal_test_usbmsc_optional_ops();
  internal_test_usbmsc_null_ctx_guards();
  internal_test_usbmsc_null_buffer_guards();
  (void)ra8_usb_hmsc_close();
  return 0;
}
