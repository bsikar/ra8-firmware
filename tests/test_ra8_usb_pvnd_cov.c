/**
 * @file test_ra8_usb_pvnd_cov.c
 * @brief Coverage top-up for the native USB device-Vendor class layer
 *
 * @details
 * Targets the residual uncovered lines in
 * ``libs/ra8_hal/src/ra8_usb_pvnd.c`` that the primary suite
 * (``test_ra8_usb_pvnd.c``) does not reach:
 *   - ``ra8_usb_pvnd_set_descriptors``: the NULL-pointer reject, the
 *     zero-length reject, and the store-and-succeed happy path (lines
 *     209-215) -- the primary suite only calls it pre-init.
 *   - ``ra8_usb_pvnd_recv``: both legs of the queue drain -- the
 *     empty-pipe leg (``ra8_usb_queue_out`` returns ``k_ra8_err_no_data``
 *     and ``*got_len`` is zeroed) and the drained-bank leg (queue_out
 *     returns ``k_ra8_ok`` and ``*got_len`` reflects the byte count),
 *     lines 247-255.
 *   - ``ra8_usb_pvnd_handle_setup``: the application-callback-returned-error
 *     leg that stalls EP0 (line 293).
 *   - ``ra8_usb_pvnd_init``: the ``ra8_usb_device_init`` failure leg
 *     (lines 170-171) driven by saturating the USBFS module-stop
 *     ref-count so ``ra8_mstp_enable`` inside ``ra8_usb_device_init``
 *     returns an error.
 *
 * Every leg is driven deterministically by pre-seeding the fake's
 * register RAM (BRDYSTS / CFIFOCTR for the OUT-pipe drain) or the
 * module-stop ref-count; no timing injection (SIGALRM) is used.
 * ``internal_wait_frdy`` converges on its first poll via the unarmed
 * ra8_fake_mmio seam, so the FIFO drain converges without a real
 * controller.
 *
 * The one leg NOT reachable from the host -- the
 * ``internal_configure_pipes`` failure return in ``ra8_usb_pvnd_init``
 * (lines 177-178) -- is documented in the source with a GCOVR_EXCL_LINE
 * justification: that helper only fails on an invalid endpoint argument,
 * and the class layer feeds it compile-time-constant, always-valid pipe
 * / endpoint / packet parameters, so no host input can make it fail.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_pvnd.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_pvnd_cov_t
 * @brief Transferred-byte out-parameter seed.
 */
typedef enum : uint16_t {
  k_t_got_unset = 0xBEEFU, /**< Pre-set transferred count; a transfer that fails
                                must leave it rather than report zero.           */
} t_pvnd_cov_t;

/**
 * @enum test_pvnd_cov_const_t
 * @brief Named literals for the coverage-driving register seeds.
 */
typedef enum : uint16_t {
  k_test_pipe1_brdy_bit = 0x0002U, /**< 1U << 1 : PIPE1 (bulk OUT) status bit.  */
  k_test_recv_avail     = 4U,      /**< DTLN byte count staged in CFIFOCTR.     */
  k_test_recv_capacity  = 16U,     /**< recv() buffer capacity (>= avail).      */
  k_test_mstp_saturate  = 255U,    /**< Enables needed to pin USBFS ref at max. */
  k_test_desc_len       = 8U,      /**< Non-zero descriptor length.             */
  k_test_setup_breq     = 0x42U,   /**< Arbitrary vendor bRequest.              */
} test_pvnd_cov_const_t;

/** @brief Minimal descriptor blob; only the pointer + length are stored. */
static const uint8_t s_sample_desc[k_test_desc_len] = {
  0x09U,
  0x02U,
  0x12U,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
};

static int32_t   s_setup_cb_calls       = 0;
static ra8_err_t s_setup_cb_return_code = k_ra8_ok;

/**
 * @brief Test setup handler with a caller-configurable return code.
 *
 * @details Records the invocation count so the dispatch legs can be
 * verified, and returns ::s_setup_cb_return_code so a test can force the
 * class layer down its callback-error branch.
 *
 * @param[in] ctx   Unused caller context.
 * @param[in] setup Unused SETUP envelope (contents are irrelevant here).
 *
 * @return ::s_setup_cb_return_code.
 * @retval k_ra8_ok Default accept path.
 */
static ra8_err_t test_setup_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  (void)setup;
  s_setup_cb_calls++;
  return s_setup_cb_return_code;
}

/**
 * @brief Reset the fake MMIO, module-stop ref-counts, and class
 *        state before each case.
 *
 * @details ``ra8_mstp_init`` clears every module-stop ref-count to zero,
 * which is essential for the ref-count-saturation case: each test starts
 * from a clean count. ``ra8_usb_pvnd_close`` drops any prior init.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pvnd_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_return_code = k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no compound decision exercised -- ``RA8_CHECK_NULL_PTR`` and
 * ``if (desc_len == 0U)`` are each single conditions; this case drives
 * both outcomes of each in turn plus the store-and-succeed tail)
 *
 * @details Covers the descriptor-handoff body (lines 209-215): the
 * NULL-pointer reject, the zero-length reject, and the successful
 * pointer + length store.
 */
static void test_set_descriptors_paths(void)
{
  TEST_BEGIN("ra8_usb_pvnd_set_descriptors: NULL / zero-len / store");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  /* NULL blob -> RA8_CHECK_NULL_PTR reject (line 209). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_pvnd_set_descriptors(nullptr, (uint16_t)k_test_desc_len));
  /* Zero length -> invalid_arg (lines 210-211). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_set_descriptors(s_sample_desc, 0U));
  /* Valid blob -> stored, k_ra8_ok (lines 213-215). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pvnd_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc)));

  TEST_END("ra8_usb_pvnd_set_descriptors: NULL / zero-len / store");
}

/**
 * @par MC/DC:
 * (no compound decision exercised -- ``if (err == k_ra8_ok)`` at the
 * drain site is a single condition; this case pins it FALSE via an
 * empty pipe and the sibling case pins it TRUE)
 *
 * @details With BRDYSTS clear (the reset default), ``ra8_usb_queue_out``
 * takes its no-data fast path and returns ``k_ra8_err_no_data``. The
 * class layer then zeroes ``*got_len`` (lines 252-253) and returns the
 * error (lines 247-250, 255).
 */
static void test_recv_empty_pipe(void)
{
  TEST_BEGIN("ra8_usb_pvnd_recv reports no_data and zeroes got_len on empty pipe");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  uint8_t  buf[k_test_recv_capacity] = {};
  uint16_t got                       = k_t_got_unset;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_pvnd_recv(buf, (uint16_t)k_test_recv_capacity, &got));
  /* Error leg must clear the out-count (line 253). */
  TEST_ASSERT_EQ(0U, got);

  TEST_END("ra8_usb_pvnd_recv reports no_data and zeroes got_len on empty pipe");
}

/**
 * @par MC/DC:
 * (no compound decision exercised -- ``if (err == k_ra8_ok)`` at the
 * drain site is a single condition; this case pins it TRUE)
 *
 * @details Pre-seeds BRDYSTS bit 1 (PIPE1 / bulk OUT) so the queue_out
 * no-data fast path is bypassed, and stages CFIFOCTR with FRDY asserted
 * and DTLN == ::k_test_recv_avail so the drain observes a non-zero
 * bank. ``ra8_usb_queue_out`` then returns ``k_ra8_ok`` and the class
 * layer copies the drained byte count into ``*got_len`` (line 251).
 */
static void test_recv_drains_bank(void)
{
  TEST_BEGIN("ra8_usb_pvnd_recv drains a staged bank and reports the byte count");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* BRDY latched for PIPE1 -> skip the no-data fast path. */
  reg->BRDYSTS = (uint16_t)k_test_pipe1_brdy_bit;
  /* FRDY set with DTLN == avail -> the drain sees `avail` ready bytes. */
  reg->CFIFOCTR = (uint16_t)((uint16_t)k_ra8_fifoctr_frdy | (uint16_t)k_test_recv_avail);

  uint8_t  buf[k_test_recv_capacity] = {};
  uint16_t got                       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_recv(buf, (uint16_t)k_test_recv_capacity, &got));
  /* Success leg copies the drained length into *got_len (line 251). */
  TEST_ASSERT_EQ(k_test_recv_avail, got);

  TEST_END("ra8_usb_pvnd_recv drains a staged bank and reports the byte count");
}

/**
 * @par MC/DC:
 * (no compound decision exercised -- ``if (cb_err != k_ra8_ok)`` is a
 * single condition; this case pins it TRUE while the primary suite pins
 * it FALSE)
 *
 * @details Registers a vendor setup handler that returns an error, then
 * dispatches a vendor-recipient SETUP. The class layer takes the
 * callback-error branch and stalls EP0 via ``ra8_usb_control_response``
 * (line 293). Off-target that stall request itself succeeds, so
 * ``ra8_usb_pvnd_handle_setup`` returns ``k_ra8_ok`` even though the
 * application rejected the request; the callback-was-invoked count
 * confirms the error branch was taken.
 */
static void test_handle_setup_cb_error_stalls(void)
{
  TEST_BEGIN("ra8_usb_pvnd_handle_setup stalls EP0 when the vendor callback fails");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));

  s_setup_cb_return_code = k_ra8_err_timeout; /* Force the error branch. */
  ra8_usb_setup_t setup  = {
    .bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_dev_in,
    .b_request       = (uint8_t)k_test_setup_breq,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  /* The stall response itself returns k_ra8_ok off-target. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  /* The callback must have run exactly once to reach the error branch. */
  TEST_ASSERT_EQ(1, s_setup_cb_calls);

  TEST_END("ra8_usb_pvnd_handle_setup stalls EP0 when the vendor callback fails");
}

/**
 * @par MC/DC:
 * (no compound decision exercised -- ``if (usb_err != k_ra8_ok)`` is a
 * single condition; this case pins it TRUE while every other init case
 * pins it FALSE)
 *
 * @details Drives the ``ra8_usb_device_init`` failure leg of
 * ``ra8_usb_pvnd_init`` (lines 170-171). ``ra8_usb_device_init`` calls
 * ``ra8_mstp_enable`` for the USBFS module; that helper returns
 * ``k_ra8_err_invalid_state`` once the module's ref-count is saturated at
 * ``UINT8_MAX``. Saturating it here (255 enables, no matching disables)
 * makes the device-init call fail, so ``ra8_usb_pvnd_init`` logs the
 * failure and returns ``k_ra8_err_hw_init_failed`` without ever setting
 * ``s_state.initialized``. ``ra8_mstp_init`` in ``prep`` restores a clean
 * ref-count for every other case.
 */
static void test_init_device_init_failure(void)
{
  TEST_BEGIN("ra8_usb_pvnd_init maps a device-init failure to hw_init_failed");
  prep();

  /* Pin the USBFS module-stop ref-count at its ceiling so the enable
   * inside ra8_usb_device_init trips the saturation guard. */
  for (uint16_t i = 0U; i < (uint16_t)k_test_mstp_saturate; ++i) {
    (void)ra8_mstp_enable(k_ra8_mstp_usbfs);
  }

  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  TEST_END("ra8_usb_pvnd_init maps a device-init failure to hw_init_failed");
}

int32_t main(void)
{
  test_set_descriptors_paths();
  test_recv_empty_pipe();
  test_recv_drains_bank();
  test_handle_setup_cb_error_stalls();
  test_init_device_init_failure();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_pvnd_cov.c\n");
  return 0;
}
