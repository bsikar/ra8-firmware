/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_touch.h
 * @brief Multi-touch input driver -- GoodIX GT911 backend
 * @ingroup grp_hal_usb
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 has no on-die capacitive touch sensing unit (no CTSU);
 * touch on the EK-RA8D2 ereader carrier is provided by an off-chip
 * GoodIX GT911 IC sitting on the I2C0 bus. ``ra8_touch`` exposes a
 * narrow, vendor-agnostic surface so a future FT5x06 / Atmel maXTouch
 * backend can be slotted in without changing application code.
 *
 * Lifecycle:
 *
 * 1. ``ra8_touch_open(cfg)`` latches the injected I2C bus seam
 *    (``cfg->bus``, app-bound -- see ``ra8_i2c_bus_ops.h``), wakes the
 *    GT911 by reading its product id string, programmes the GT911
 *    status register to "clear", and -- if ``cfg->irq_pin`` is in
 *    range -- programmes the matching ICU IRQ pin for falling-edge
 *    detection. The bus peripheral itself (IIC_B today, RIIC on a
 *    future board revision) is initialised by the app, not here.
 * 2. ``ra8_touch_attach_handler(fn, ctx)`` installs an event callback
 *    fired by ``ra8_touch_dispatch_irq()`` (the test-callable shim) when
 *    the GT911 INT pin asserts. The callback is expected to call
 *    ``ra8_touch_read()`` to drain the touch frame.
 * 3. ``ra8_touch_read(out, max_count, *got_count)`` reads the GT911
 *    status byte, copies up to ``max_count`` decoded points into
 *    ``out``, and writes the GT911 command register with
 *    ``k_ra8_touch_gt911_cmd_clear_status`` so the IC can latch the
 *    next frame.
 * 4. ``ra8_touch_calibrate()`` is a no-op for GT911 (factory-calibrated)
 *    and returns ``k_ra8_ok`` -- it exists in the API so future
 *    resistive backends can plug in.
 * 5. ``ra8_touch_close()`` clears the registered handler and releases
 *    the driver slot; the app keeps owning the bus peripheral.
 *
 * Static-allocation footprint:
 *   - One ``ra8_touch_state_t`` slot (driver-private, file-scope).
 *   - No runtime allocation; ``out_points`` is caller-provided.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra8_touch_limits_t
 * @brief Static-allocation caps for the touch driver.
 */
typedef enum : uint8_t {
  k_ra8_touch_max_points    = 5U,  /**< Hard cap (matches GT911 capacity).  */
  k_ra8_touch_irq_pin_unset = 32U, /**< Sentinel for "no IRQ pin attached". */
} ra8_touch_limits_t;

/* ===========================================================================
 * Public types
 * ===========================================================================
 */

/**
 * @struct ra8_touch_point_t
 * @brief One decoded touch contact.
 *
 * @details
 * Coordinates are reported in panel-native units (the GT911 reports the
 * X/Y resolution it was configured for; ``ra8_touch`` does not transform
 * them). ``track_id`` is persistent across consecutive
 * ``ra8_touch_read`` calls for the same physical contact, so callers can
 * implement gesture recognisers without re-keying.
 *
 * @invariant ``track_id`` < 16 (GT911 stores it in 4 bits).
 */
typedef struct {
  uint16_t x;        /**< Panel X coordinate.                          */
  uint16_t y;        /**< Panel Y coordinate.                          */
  uint8_t  track_id; /**< Persistent contact id (0..15).               */
  uint8_t  pressure; /**< Approximate pressure / contact-area, 0..255. */
} ra8_touch_point_t;

/**
 * @struct ra8_touch_cfg_t
 * @brief Configuration descriptor for ``ra8_touch_open``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra8_touch_open`` in
 * ``libs/ra8_hal/src/ra8_touch.c``.
 */
typedef struct {
  ra8_i2c_bus_ops_t bus;        /**< Injected I2C transfer seam (app-bound). */
  uint8_t           target_7b;  /**< 7-bit GT911 address (0x5D or 0x14).     */
  uint8_t           irq_pin;    /**< IRQ pin 0..31, or
                                    ::k_ra8_touch_irq_pin_unset for polling-only. */
  uint8_t           max_points; /**< Cap on touches reported (clamped to 5). */
} ra8_touch_cfg_t;

/**
 * @typedef ra8_touch_event_fn_t
 * @brief Touch-event callback signature.
 *
 * @details
 * Fired by ``ra8_touch_dispatch_irq()`` when the GT911 INT pin asserts.
 * The callback is expected to call ``ra8_touch_read()`` to drain the
 * frame; ``ra8_touch`` does not pre-decode points before dispatch
 * because the driver does not know how many of them the application
 * actually wants.
 *
 * @param[in] ctx Caller-supplied context.
 */
typedef void (*ra8_touch_event_fn_t)(void* ctx);

/* ===========================================================================
 * Lifecycle
 * ===========================================================================
 */

/**
 * @brief Bring up the touch IC over the injected I2C bus seam.
 *
 * @details
 * Steps:
 *   1. Validate ``cfg`` (non-NULL ``bus.write`` / ``bus.transfer``,
 *      known GT911 address) and clamp ``max_points`` to
 *      ::k_ra8_touch_max_points.
 *   2. Read the GT911 PRODUCT_ID register (4 ASCII bytes) to confirm
 *      the IC is alive. If the read fails or the id does not start
 *      with ``'9'``, return ``k_ra8_err_hw_init_failed``.
 *   3. Clear the GT911 status byte so the next interrupt corresponds
 *      to a fresh frame.
 *   4. If ``cfg->irq_pin`` is in range, configure that ICU IRQ pin
 *      for falling-edge detection.
 *
 * The bus peripheral behind ``cfg->bus`` (IIC_B in I2C-compat mode at
 * fast-mode speed today) is initialised by the app before this call,
 * typically bound through ``ra8_io_i2c_bus_as_ops()``.
 *
 * @param[in] cfg Configuration descriptor (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 IC alive, driver ready.
 * @retval k_ra8_err_null_ptr       ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg    ``cfg->bus`` seam incomplete or
 *                                 ``cfg->target_7b`` not 0x5D/0x14.
 * @retval k_ra8_err_invalid_state  ``ra8_touch_open`` was already called.
 * @retval k_ra8_err_hw_init_failed Product-id check failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre The bus peripheral behind ``cfg->bus`` is initialised.
 * @post On success the driver is in the open state.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_open(const ra8_touch_cfg_t* cfg);

/**
 * @brief Tear the driver down and detach any registered handler.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Driver closed.
 * @retval k_ra8_err_not_initialized ``ra8_touch_open`` was not called.
 *
 * @pre Driver is currently open.
 * @post No handler is registered and the driver slot is free; the app
 *       keeps owning the bus peripheral behind the injected seam.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_close(void);

/* ===========================================================================
 * Event handler attach / dispatch
 * ===========================================================================
 */

/**
 * @brief Register a touch event callback.
 *
 * @param[in] fn  Callback fired on GT911 INT assertion. NULL detaches.
 * @param[in] ctx Forwarded to the callback as its only argument.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Handler stored.
 * @retval k_ra8_err_not_initialized Driver is not open.
 *
 * @pre Driver is open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_attach_handler(ra8_touch_event_fn_t fn, void* ctx);

/**
 * @brief Test-callable IRQ dispatch shim.
 *
 * @details
 * Mirrors the ERI dispatch helper from ``ra8_i3c_i2c``: this is the
 * function the application's IRQ vector should call when the GT911
 * INT pin fires. The shim simply forwards to the registered handler.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_touch_dispatch_irq(void);

/* ===========================================================================
 * Touch-point read
 * ===========================================================================
 */

/**
 * @brief Drain the current touch frame.
 *
 * @details
 * Sequence:
 *   1. ``bus.transfer`` with the 16-bit register pointer to
 *      ::k_ra8_touch_gt911_reg_status, reads 1 status byte.
 *   2. If bit7 is clear (no frame ready), set ``*got_count = 0`` and
 *      return ``k_ra8_ok``.
 *   3. Otherwise read up to ``min(status & 0x0F, max_count)`` 8-byte
 *      point records starting at ::k_ra8_touch_gt911_reg_point0 and
 *      decode them into ``out_points``.
 *   4. Write ``k_ra8_touch_gt911_cmd_clear_status`` (0x00) into the
 *      status register so the IC can latch the next frame.
 *
 * @param[out] out_points Destination buffer (non-NULL, must hold
 *                        ``max_count`` entries).
 * @param[in]  max_count  Capacity of ``out_points``, in entries.
 * @param[out] got_count  Number of valid entries written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Frame drained (possibly empty).
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_invalid_arg     ``max_count == 0``.
 * @retval k_ra8_err_not_initialized Driver is not open.
 * @retval k_ra8_err_hw_error        I2C transport failed.
 *
 * @pre Driver is open.
 * @post On success ``*got_count <= max_count``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_touch_read(ra8_touch_point_t* out_points, uint8_t max_count, uint8_t* got_count);

/**
 * @brief Run any backend-specific calibration routine.
 *
 * @details
 * No-op for GT911 (factory-calibrated). Returns ``k_ra8_ok`` so callers
 * can wire it unconditionally; resistive backends added later will
 * actually drive the calibration sequence here.
 *
 * @return ``k_ra8_ok`` unconditionally for the GT911 backend.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_calibrate(void);

#ifdef UNIT_TEST
/**
 * @brief Decode a raw GT911 point block into ``ra8_touch_point_t``.
 *
 * @details
 * Test-only entry point. Production code reaches the same parser via
 * ``ra8_touch_read``; tests use this entry to inject synthetic GT911
 * payloads without driving the full I2C transport.
 *
 * @param[in]  raw         Source buffer holding ``n_points * 8`` bytes
 *                         in GT911 wire format.
 * @param[in]  n_points    Number of point records present in ``raw``.
 * @param[out] out_points  Destination buffer.
 * @param[in]  max_count   Capacity of ``out_points``.
 * @param[out] got_count   Entries actually written.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_null_ptr`` if any pointer
 *         is NULL, ``k_ra8_err_invalid_arg`` if ``max_count == 0``.
 *
 * @note Test-only; not compiled into firmware builds.
 * @since 0.1.0
 */
RA8_TEST_HELPER [[nodiscard]] ra8_err_t ra8_touch_test_decode(const uint8_t*     raw,
                                                              uint8_t            n_points,
                                                              ra8_touch_point_t* out_points,
                                                              uint8_t            max_count,
                                                              uint8_t*           got_count);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif
