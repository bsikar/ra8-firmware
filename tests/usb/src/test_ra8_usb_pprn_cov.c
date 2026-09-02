/**
 * @file test_ra8_usb_pprn_cov.c
 * @brief Coverage top-up for the native USB device-side Printer class
 *        layer (`libs/ra8_hal/src/ra8_usb_pprn.c`)
 *
 * @details
 * Companion to `test_ra8_usb_pprn.c`. That suite covers the public-API
 * happy path and the argument-rejection contract; this file drives the
 * residual legs the original suite never reaches:
 *
 *  - `ra8_usb_pprn_init` hardware-init-failed leg: the underlying
 *    `ra8_usb_device_init` is forced to fail by saturating the USBFS
 *    module-stop reference count (255 pending users) so the controller's
 *    `ra8_mstp_enable` returns `k_ra8_err_invalid_state`. The class layer
 *    logs the failure and remaps it to `k_ra8_err_hw_init_failed`.
 *  - `ra8_usb_pprn_recv` forwarding into `ra8_usb_queue_out`: the success
 *    drain leg (BRDY latched + a non-zero DTLN pre-seeded so bytes are
 *    delivered and the caller length is stamped) and the no-data leg
 *    (BRDY status bit cleared so the drain reports `k_ra8_err_no_data` and
 *    the caller length is zeroed). The original suite only ever exercised
 *    the pre-forward argument guards.
 *  - `ra8_usb_pprn_handle_setup` callback-error leg: an attached class
 *    handler returns a non-ok status, so the dispatcher STALLs the
 *    control endpoint via `ra8_usb_control_response(speed, false)`.
 *
 * All hardware is the host register-window model brought up by
 * `ra8_fake_mmap_reset` + `ra8_mstp_init`; no live controller, no timers,
 * no SIGALRM. Register pre-seeding (BRDYSTS / CFIFOCTR) is used where a
 * specific controller status is required to make a leg deterministic.
 *
 * The init `deinit`-on-pipe-failure leg (source lines 210-211) is not
 * reachable from the host and is branch-excluded in the source: see the
 * justification comment above `internal_configure_pipes`'s error return.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_pprn.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_pprn_cov_t
 * @brief GET_DEVICE_ID request length.
 */
typedef enum : uint8_t {
  k_t_wlen_device_id = 64U, /**< Bytes the host asks the printer class for. */
} t_pprn_cov_t;

/**
 * @enum test_pprn_cov_t
 * @brief Fixed register / packet values used by the coverage cases.
 *
 * @details Mirrors the file-static pipe assignments of `ra8_usb_pprn.c`
 * (bulk-OUT is PIPE3) so the tests can pre-seed the exact per-pipe status
 * bits the drain path reads.
 */
typedef enum : uint16_t {
  k_test_pprn_bulk_out_pipe = 3U,      /**< PIPE3: bulk-OUT (print data).      */
  k_test_pprn_brdy_bit      = 0x0008U, /**< 1U << 3 : PIPE3 BRDY status bit.   */
  k_test_pprn_dtln          = 4U,      /**< Bytes staged in the OUT FIFO bank. */
  k_test_pprn_recv_cap      = 8U,      /**< recv() buffer capacity.            */
  k_test_pprn_iface_in      = 0xA1U,   /**< bmRequestType class|iface|in.      */
  k_test_pprn_mstp_saturate = 255U,    /**< Enables to saturate the refcount.  */
} test_pprn_cov_t;

/** @brief Call count for the local class-setup callback. */
static int32_t s_cb_calls = 0;

/**
 * @brief Class-setup callback stub that always reports failure.
 *
 * @details Returns `k_ra8_err_not_supported` so the dispatcher in
 * `ra8_usb_pprn_handle_setup` takes the STALL leg. Records that it ran.
 *
 * @param[in] ctx Opaque context (unused).
 * @param[in] setup SETUP packet (unused).
 * @return Always `k_ra8_err_not_supported`.
 * @retval k_ra8_err_not_supported Forces the caller onto the STALL path.
 * @pre `setup` is non-NULL (guaranteed by the caller).
 * @pre The class layer is initialized.
 * @post `s_cb_calls` is incremented by one.
 * @post No hardware state is mutated by the stub itself.
 * @note Not thread-safe; single-threaded host test.
 * @since 0.1.0
 */
static ra8_err_t test_fail_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  (void)setup;
  s_cb_calls++;
  return k_ra8_err_not_supported;
}

/**
 * @brief Reset the fake MMIO window and the Printer singleton to a
 * known pre-init state.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pprn_close();
  s_cb_calls = 0;
}

/**
 * @test test_init_hw_init_failed
 *
 * @details Saturates the USBFS module-stop reference count (255 pending
 * enables) so the next `ra8_mstp_enable` inside `ra8_usb_device_init`
 * returns `k_ra8_err_invalid_state` without ungating the module. The
 * class layer sees the non-ok status, logs it via `ra8_log_error_val`,
 * and returns `k_ra8_err_hw_init_failed` (source lines 203-204). This is
 * the only host-reachable way to fail the underlying controller init:
 * the module-stop saturation guard is genuine defensive hardware-driver
 * code, driven here by the real reference-count path rather than by
 * splitting a statement.
 *
 * @par MC/DC:
 * (no compound decision on this leg -- `if (usb_err != k_ra8_ok)` is a
 * single condition. The `ra8_usb_pprn_init` speed gate
 * `(speed != FS) && (speed != HS)` is not the subject here: a valid FS
 * speed makes the first condition false and short-circuits; its full
 * N+1 vector set lives in `test_ra8_usb_pprn.c::test_mcdc_pprn`.)
 */
static void test_init_hw_init_failed(void)
{
  TEST_BEGIN("ra8_usb_pprn_init maps a controller-init failure to hw_init_failed");
  prep();

  /* Drive the refcount to UINT8_MAX so the next enable request (issued
   * from inside ra8_usb_device_init for the USBFS module) is rejected. */
  for (uint16_t i = 0U; i < (uint16_t)k_test_pprn_mstp_saturate; ++i) {
    (void)ra8_mstp_enable(k_ra8_mstp_usbfs);
  }

  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_usb_pprn_init(k_ra8_usb_speed_fs));
  TEST_END("ra8_usb_pprn_init maps a controller-init failure to hw_init_failed");
}

/**
 * @test test_recv_success_drain
 *
 * @details After a clean FS init, pre-seeds the bulk-OUT pipe (PIPE3):
 * its BRDY status bit is set so `ra8_usb_queue_out` bypasses the no-data
 * fast path, and CFIFOCTR is seeded with FRDY asserted and a non-zero
 * DTLN so the drain delivers `k_test_pprn_dtln` bytes. `ra8_usb_pprn_recv`
 * therefore reaches its `ra8_usb_queue_out` forward (source line 279-280),
 * observes `k_ra8_ok`, and stamps the caller length from the transfer
 * result (source line 282).
 *
 * @par MC/DC:
 * (no compound decision under test -- the `if (err == k_ra8_ok)` result
 * split in `ra8_usb_pprn_recv` is a single condition; its false leg is
 * exercised by `test_recv_no_data_drain`.)
 */
static void test_recv_success_drain(void)
{
  TEST_BEGIN("ra8_usb_pprn_recv delivers a drained OUT packet and stamps got_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* BRDY latched for PIPE3 -> queue_out skips the no-data fast path. */
  reg->BRDYSTS = (uint16_t)k_test_pprn_brdy_bit;
  /* FRDY asserted with a non-zero DTLN -> a real drain of DTLN bytes. */
  reg->CFIFOCTR = (uint16_t)((uint16_t)k_ra8_fifoctr_frdy | (uint16_t)k_test_pprn_dtln);

  uint8_t  buf[k_test_pprn_recv_cap] = {};
  uint16_t got                       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_recv(buf, (uint16_t)k_test_pprn_recv_cap, &got));
  /* The available DTLN (4) is below the buffer capacity (8), so the full
   * bank is delivered and got_len reflects the byte count. */
  TEST_ASSERT_EQ(k_test_pprn_dtln, got);
  TEST_END("ra8_usb_pprn_recv delivers a drained OUT packet and stamps got_len");
}

/**
 * @test test_recv_no_data_drain
 *
 * @details After a clean FS init, clears the bulk-OUT pipe's BRDY status
 * bit so `ra8_usb_queue_out` takes its fast no-data leg and returns
 * `k_ra8_err_no_data`. `ra8_usb_pprn_recv` propagates the error and zeroes
 * the caller length (source lines 283-284), complementing the success
 * drain above.
 *
 * @par MC/DC:
 * (no compound decision under test -- this case supplies the false
 * (`err != k_ra8_ok`) vector for the single-condition
 * `if (err == k_ra8_ok)` result split in `ra8_usb_pprn_recv`.)
 */
static void test_recv_no_data_drain(void)
{
  TEST_BEGIN("ra8_usb_pprn_recv reports no_data and zeroes got_len on an empty pipe");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* Present an empty bulk-OUT pipe: BRDY clear -> queue_out no-data leg. */
  reg->BRDYSTS = 0U;

  uint8_t  buf[k_test_pprn_recv_cap] = {};
  uint16_t got                       = (uint16_t)k_test_pprn_recv_cap;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_pprn_recv(buf, (uint16_t)k_test_pprn_recv_cap, &got));
  /* The error leg zeroes the reported length. */
  TEST_ASSERT_EQ(0U, got);
  TEST_END("ra8_usb_pprn_recv reports no_data and zeroes got_len on an empty pipe");
}

/**
 * @test test_handle_setup_callback_stall
 *
 * @details Attaches a class-setup handler that returns a non-ok status,
 * then dispatches a valid class|interface GET_DEVICE_ID request. The
 * handler runs (`s_cb_calls` increments), reports failure, and the
 * dispatcher STALLs the control endpoint via
 * `ra8_usb_control_response(speed, false)` (source line 366). Under the
 * host model `ra8_usb_control_response` sets the DCP PID to STALL and
 * returns `k_ra8_ok`, so `ra8_usb_pprn_handle_setup` returns `k_ra8_ok`.
 *
 * @par MC/DC:
 * (no compound decision under test -- `if (s_state.setup_cb != nullptr)`
 * and `if (cb_err != k_ra8_ok)` are independent single-condition guards.
 * The class-envelope decision `(bm != in) && (bm != out)` is short-
 * circuited false here by a class|interface|in bmRequestType; its full
 * N+1 vector set lives in `test_ra8_usb_pprn.c::test_mcdc_pprn`.)
 */
static void test_handle_setup_callback_stall(void)
{
  TEST_BEGIN("ra8_usb_pprn_handle_setup STALLs the control endpoint on callback error");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_attach_setup_handler(test_fail_cb, nullptr));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_pprn_iface_in,
    .b_request       = (uint8_t)k_ra8_pprn_req_get_device_id,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_t_wlen_device_id,
  };
  /* Callback returns non-ok -> STALL leg; control_response returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_cb_calls);
  TEST_END("ra8_usb_pprn_handle_setup STALLs the control endpoint on callback error");
}

int main(void)
{
  test_init_hw_init_failed();
  test_recv_success_drain();
  test_recv_no_data_drain();
  test_handle_setup_callback_stall();
  return 0;
}
