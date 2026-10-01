/**
 * @file test_ra8_camera_stop.c
 * @brief MC/DC vectors for the camera source stop contract.
 * @ingroup grp_camera
 * @details Covers ::ra8_camera_source_stop across a null handle, an unbound
 *          handle, a backend that supplies no stop row, a backend that refuses
 *          the release, the fixed-frame backend, and the CEU backend driven
 *          against the fake register window. Kept out of `test_ra8_camera.c`
 *          and `test_ra8_ceu_cov.c` because both sit within a few dozen lines
 *          of the thousand-line file cap.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_camera.h"
#include "ra8_camera_internal.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_camera_source_memory.h"
#include "ra8_ceu.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum test_stop_dim_t
 * @brief Geometry of the UYVY fixture both backends capture.
 * @details Width is even so the packed row-size rule accepts it, and the byte
 *          count is exactly stride times height.
 * @invariant k_stop_frame_bytes == k_stop_stride * k_stop_height.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_stop_width         = 16U,  /**< Fixture frame width in pixels.  */
  k_stop_height        = 16U,  /**< Fixture frame height in pixels. */
  k_stop_stride        = 32U,  /**< Packed UYVY row stride, bytes.  */
  k_stop_frame_bytes   = 512U, /**< Whole fixture frame, bytes.     */
  k_stop_poll_ms       = 1U,   /**< Completion poll interval.       */
  k_stop_poll_attempts = 3U,   /**< Bounded completion poll budget. */
} test_stop_dim_t;

/** @brief Injected response and call count for the fake source backend. */
typedef struct {
  ra8_err_t result; /**< Result the stop row reports.   */
  uint32_t  calls;  /**< Times the stop row was called. */
} test_stop_ctx_t;

/**
 * @brief Report fixed metadata so the fake source counts as bound.
 * @details The stop cases never read the values; the row exists because the
 *          facade refuses to dispatch through a vtable missing it.
 * @param[in] ctx Unused backend context.
 * @param[out] out_info Receives the fixture geometry.
 * @return Error code.
 * @retval k_ra8_ok Metadata written.
 * @pre @p out_info points to writable storage.
 * @pre No concurrent case shares the fixture.
 * @post @p out_info describes the UYVY fixture.
 * @post No other state is modified.
 * @note Not thread-safe with respect to the shared fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t test_stop_get_info(void* ctx, ra8_camera_source_info_t* out_info)
{
  (void)ctx;
  *out_info = (ra8_camera_source_info_t){
    .frame_bytes_max = (uint32_t)k_stop_frame_bytes,
    .stride_bytes    = (uint32_t)k_stop_stride,
    .width           = (uint16_t)k_stop_width,
    .height          = (uint16_t)k_stop_height,
    .format          = k_ra8_camera_format_uyvy422,
  };
  return k_ra8_ok;
}

/**
 * @brief Refuse every capture; the stop cases never capture.
 * @details Present only so the vtable satisfies the facade's bound-source rule.
 * @param[in] ctx Unused backend context.
 * @param[in] buffer Unused capture storage.
 * @param[out] out_frame Unused frame view.
 * @return Error code.
 * @retval k_ra8_err_not_supported Always.
 * @pre The caller does not rely on a captured frame.
 * @pre No concurrent case shares the fixture.
 * @post No storage is written.
 * @post No hardware is touched.
 * @note Thread-safe; touches nothing.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
test_stop_capture(void* ctx, const ra8_camera_buffer_t* buffer, ra8_camera_frame_t* out_frame)
{
  (void)ctx;
  (void)buffer;
  (void)out_frame;
  return k_ra8_err_not_supported;
}

/**
 * @brief Release the fake source, reporting the injected result.
 * @details Counts the call so a refused release can be told apart from a
 *          release the facade never dispatched.
 * @param[in,out] ctx Live ::test_stop_ctx_t.
 * @return Error code.
 * @retval other The injected result.
 * @pre @p ctx addresses a live fixture.
 * @pre No concurrent case shares the fixture.
 * @post The call counter advances by one.
 * @post No other fixture field changes.
 * @note Not thread-safe with respect to the shared fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t test_stop_stop(void* ctx)
{
  test_stop_ctx_t* fixture = (test_stop_ctx_t*)ctx;
  fixture->calls += 1U;
  return fixture->result;
}

/** @brief Source vtable whose optional stop row is bound. */
static const ra8_camera_source_iface_t s_stop_iface = {
  .get_info = test_stop_get_info,
  .capture  = test_stop_capture,
  .stop     = test_stop_stop,
};

/** @brief Source vtable with no stop row, the pre-RA8FW-320 backend shape. */
static const ra8_camera_source_iface_t s_no_stop_iface = {
  .get_info = test_stop_get_info,
  .capture  = test_stop_capture,
  .stop     = nullptr,
};

/**
 * @brief Reset the fake register window before a CEU case.
 * @details Mirrors the preparation `test_ra8_ceu_cov.c` performs: a fresh
 *          register map, a fresh MMIO log, and module-stop released.
 * @pre The fake register window is mappable on this host.
 * @pre No CEU capture is in flight.
 * @post CEU registers read their reset values.
 * @post The CEU module clock is ungated.
 * @note Not thread-safe; single-threaded test binary only.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_stop_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a CEU source configuration the backend accepts.
 * @details Packed UYVY, one synchronous frame, geometry matching the fixture.
 * @return Populated configuration.
 * @retval ra8_camera_source_ceu_cfg_t Passes every `ra8_camera_source_ceu_init` guard.
 * @pre None.
 * @pre The caller mutates only the field its case is about.
 * @post No global state is modified.
 * @post The result references no temporary storage.
 * @note Thread-safe; returns by value.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_camera_source_ceu_cfg_t test_stop_make_cfg(void)
{
  const ra8_ceu_config_t ceu = {
    .width_px        = (uint16_t)k_stop_width,
    .height_px       = (uint16_t)k_stop_height,
    .x_capture_px    = (uint16_t)k_stop_stride,
    .y_capture_lines = (uint16_t)k_stop_height,
    .dst_stride      = (uint16_t)k_stop_stride,
    .bytes_per_pixel = 2U,
    .capture_format  = k_ra8_ceu_fmt_data_synchronous,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_32,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .byte_swap       = {false, true, true},
    .scale           = {0U, 0U, 0U, 0U, (uint16_t)k_stop_width, (uint16_t)k_stop_height},
  };
  return (ra8_camera_source_ceu_cfg_t){
    .ceu              = ceu,
    .output           = {.frame_bytes_max = (uint32_t)k_stop_frame_bytes,
                         .stride_bytes    = (uint32_t)k_stop_stride,
                         .width           = (uint16_t)k_stop_width,
                         .height          = (uint16_t)k_stop_height,
                         .format          = k_ra8_camera_format_uyvy422},
    .poll_interval_ms = (uint32_t)k_stop_poll_ms,
    .poll_attempts    = (uint32_t)k_stop_poll_attempts,
  };
}

/**
 * @brief Walk every guard of the facade stop entry point.
 * @details A null handle, a handle with no vtable, a vtable with no stop row,
 *          and a backend that refuses the release must all leave nothing
 *          released; only the accepted vector unbinds the handle.
 * @par MC/DC:
 * A bound source whose stop row succeeds is the accepted baseline; the null
 * handle, the null vtable, the absent stop row and the refused release each
 * independently flip one guard input of ra8_camera_source_stop.
 * @pre Unity test accounting is initialized.
 * @pre No capture is in flight on the fixtures.
 * @post A released handle dispatches nothing.
 * @post A refused release leaves the handle bound.
 * @since 0.1.0
 */
static void test_stop_facade_guards(void)
{
  TEST_BEGIN("camera stop: facade guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_stop(nullptr));

  ra8_camera_source_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_stop(&unbound));

  test_stop_ctx_t     no_row_ctx = {};
  ra8_camera_source_t no_row     = {.iface = &s_no_stop_iface, .ctx = &no_row_ctx};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_stop(&no_row));
  TEST_ASSERT_EQ(0U, no_row_ctx.calls);
  TEST_ASSERT_NOT_NULL(no_row.iface);

  test_stop_ctx_t     refuse_ctx = {.result = k_ra8_err_hw_error, .calls = 0U};
  ra8_camera_source_t refused    = {.iface = &s_stop_iface, .ctx = &refuse_ctx};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_camera_source_stop(&refused));
  TEST_ASSERT_EQ(1U, refuse_ctx.calls);
  TEST_ASSERT_NOT_NULL(refused.iface);
  TEST_ASSERT(refused.ctx == &refuse_ctx);

  test_stop_ctx_t     ok_ctx   = {};
  ra8_camera_source_t released = {.iface = &s_stop_iface, .ctx = &ok_ctx};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_stop(&released));
  TEST_ASSERT_EQ(1U, ok_ctx.calls);
  TEST_ASSERT_NULL(released.iface);
  TEST_ASSERT_NULL(released.ctx);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_stop(&released));
  TEST_END("camera stop: facade guards");
}

/**
 * @brief Release the fixed-frame backend through the facade.
 * @details The memory source owns no peripheral, so its release only forgets
 *          the caller's frame view; the handle must stop dispatching and the
 *          state must no longer reference the fixture storage.
 * @par MC/DC:
 * A populated state is the accepted baseline; the null-context direction of
 * internal_memory_stop is unreachable through the facade and is taken in the
 * CEU case below for the sibling backend.
 * @pre Unity test accounting is initialized.
 * @pre The frame fixture outlives the case.
 * @post The released source reports not-initialized.
 * @post The backend state no longer references the fixture bytes.
 * @since 0.1.0
 */
static void test_stop_memory_backend(void)
{
  TEST_BEGIN("camera stop: fixed-frame backend");
  uint8_t                  pixels[k_stop_stride] = {};
  const ra8_camera_frame_t frame                 = {.data         = pixels,
                                                    .bytes        = (uint32_t)sizeof pixels,
                                                    .stride_bytes = (uint32_t)sizeof pixels,
                                                    .width        = (uint16_t)k_stop_width,
                                                    .height       = 1U,
                                                    .format       = k_ra8_camera_format_uyvy422};

  ra8_camera_source_t              source = {};
  ra8_camera_source_memory_state_t state  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_memory_init(&source, &state, &frame));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_stop(&source));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT_NULL(state.frame.data);

  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&source, &info));
  TEST_END("camera stop: fixed-frame backend");
}

/**
 * @brief Prove the CEU claim really goes back on stop.
 * @details Stops a live CEU source, then initialises again from the same
 *          fixture: the second init can only succeed if the first stop closed
 *          the peripheral. Also takes the two backend refusals the facade
 *          cannot reach on its own, an absent context and a cleared
 *          initialized flag.
 * @par MC/DC:
 * A bound, initialized state is the accepted baseline; the null context and
 * the cleared initialized flag each independently flip one guard input of the
 * CEU backend's stop row.
 * @pre Unity test accounting is initialized.
 * @pre The fake register window is available.
 * @post The CEU is left closed.
 * @post Backend state is cleared after a successful release.
 * @since 0.1.0
 */
static void test_stop_ceu_backend(void)
{
  TEST_BEGIN("camera stop: ceu backend releases the peripheral");
  test_stop_prep();
  ra8_camera_source_t               source = {};
  ra8_camera_source_ceu_state_t     state  = {};
  const ra8_camera_source_ceu_cfg_t cfg    = test_stop_make_cfg();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init(&source, &state, &cfg));
  TEST_ASSERT(state.initialized);

  /* A context the backend cannot use is refused before any register moves. */
  ra8_camera_source_t no_ctx = source;
  no_ctx.ctx                 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_stop(&no_ctx));

  /* A stale handle over cleared state is refused for the same reason. */
  state.initialized = false;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_stop(&source));
  state.initialized = true;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_stop(&source));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT_NULL(source.ctx);
  TEST_ASSERT(!state.initialized);
  TEST_ASSERT_EQ(0U, state.info.frame_bytes_max);

  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&source, &info));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init(&source, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ((uint32_t)k_stop_frame_bytes, info.frame_bytes_max);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_stop(&source));
  TEST_END("camera stop: ceu backend releases the peripheral");
}

/**
 * @brief Test binary entry point.
 * @par MC/DC:
 * Decisions: ra8_camera_source_stop (Zig, libs/ra8_camera),
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_stop,
 * internal_memory_stop (Zig, libs/ra8_camera).
 * @return int32_t Zero on success; never returns on failure.
 * @pre Linked against the off-target core/HAL object library.
 * @pre The fake register window is mappable on this host.
 * @post Every registered case has run to completion.
 * @post The process exit status reflects the suite result.
 * @note Not thread-safe; single-threaded test binary only.
 * @since 0.1.0
 */
int main(void)
{
  test_stop_facade_guards();
  test_stop_memory_backend();
  test_stop_ceu_backend();
  return 0;
}
