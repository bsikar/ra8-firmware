/**
 * @file ra_touch.h
 * @brief Multi-touch input driver -- GoodIX GT911 backend
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 has no on-die capacitive touch sensing unit (no CTSU);
 * touch on the EK-RA8D2 ereader carrier is provided by an off-chip
 * GoodIX GT911 IC sitting on the I2C0 bus. ``ra_touch`` exposes a
 * narrow, vendor-agnostic surface so a future FT5x06 / Atmel maXTouch
 * backend can be slotted in without changing application code.
 *
 * Lifecycle:
 *
 * 1. ``ra_touch_open(cfg)`` configures the underlying IIC_B channel
 *    (channel 0 on RA8D2), wakes the GT911 by reading its product id
 *    string, programmes the GT911 status register to "clear", and
 *    -- if ``cfg->irq_pin`` is in range -- programmes the matching
 *    ICU IRQ pin for falling-edge detection.
 * 2. ``ra_touch_attach_handler(fn, ctx)`` installs an event callback
 *    fired by ``ra_touch_dispatch_irq()`` (the test-callable shim) when
 *    the GT911 INT pin asserts. The callback is expected to call
 *    ``ra_touch_read()`` to drain the touch frame.
 * 3. ``ra_touch_read(out, max_count, *got_count)`` reads the GT911
 *    status byte, copies up to ``max_count`` decoded points into
 *    ``out``, and writes the GT911 command register with
 *    ``k_ra_touch_gt911_cmd_clear_status`` so the IC can latch the
 *    next frame.
 * 4. ``ra_touch_calibrate()`` is a no-op for GT911 (factory-calibrated)
 *    and returns ``k_ra_ok`` -- it exists in the API so future
 *    resistive backends can plug in.
 * 5. ``ra_touch_close()`` returns the IIC_B channel to idle and clears
 *    the registered handler.
 *
 * Static-allocation footprint:
 *   - One ``ra_touch_state_t`` slot (driver-private, file-scope).
 *   - No runtime allocation; ``out_points`` is caller-provided.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra_touch_limits_t
 * @brief Static-allocation caps for the touch driver.
 */
typedef enum : uint8_t {
  k_ra_touch_max_points    = 5U,  /**< Hard cap (matches GT911 capacity). */
  k_ra_touch_irq_pin_unset = 32U, /**< Sentinel for "no IRQ pin attached". */
} ra_touch_limits_t;

/* ===========================================================================
 * Public types
 * ===========================================================================
 */

/**
 * @struct ra_touch_point_t
 * @brief One decoded touch contact.
 *
 * @details
 * Coordinates are reported in panel-native units (the GT911 reports the
 * X/Y resolution it was configured for; ``ra_touch`` does not transform
 * them). ``track_id`` is persistent across consecutive
 * ``ra_touch_read`` calls for the same physical contact, so callers can
 * implement gesture recognisers without re-keying.
 *
 * @invariant ``track_id`` < 16 (GT911 stores it in 4 bits).
 */
typedef struct {
  uint16_t x;        /**< Panel X coordinate.                              */
  uint16_t y;        /**< Panel Y coordinate.                              */
  uint8_t  track_id; /**< Persistent contact id (0..15).                   */
  uint8_t  pressure; /**< Approximate pressure / contact-area, 0..255.     */
} ra_touch_point_t;

/**
 * @struct ra_touch_cfg_t
 * @brief Configuration descriptor for ``ra_touch_open``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra_touch_open`` in
 * ``libs/ra_hal/src/ra_touch.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t i2c_channel; /**< IIC_B channel (only ``0`` on RA8D2).           */
  uint8_t target_7b;   /**< 7-bit GT911 address (0x5D or 0x14).            */
  uint8_t irq_pin;     /**< IRQ pin 0..31, or
                            ::k_ra_touch_irq_pin_unset for polling-only.   */
  uint8_t max_points;  /**< Cap on touches reported (clamped to 5).        */
} ra_touch_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_touch_event_fn_t
 * @brief Touch-event callback signature.
 *
 * @details
 * Fired by ``ra_touch_dispatch_irq()`` when the GT911 INT pin asserts.
 * The callback is expected to call ``ra_touch_read()`` to drain the
 * frame; ``ra_touch`` does not pre-decode points before dispatch
 * because the driver does not know how many of them the application
 * actually wants.
 *
 * @param[in] ctx Caller-supplied context.
 */
typedef void (*ra_touch_event_fn_t)(void* ctx);

/* ===========================================================================
 * Lifecycle
 * ===========================================================================
 */

/**
 * @brief Bring up the touch IC and the underlying I2C transport.
 *
 * @details
 * Steps:
 *   1. Validate ``cfg`` and clamp ``max_points`` to
 *      ::k_ra_touch_max_points.
 *   2. Bring up IIC_B channel ``cfg->i2c_channel`` at fast-mode speed
 *      via ``ra_iic_b_init``.
 *   3. Read the GT911 PRODUCT_ID register (4 ASCII bytes) to confirm
 *      the IC is alive. If the read fails or the id does not start
 *      with ``'9'``, return ``k_ra_err_hw_init_failed``.
 *   4. Clear the GT911 status byte so the next interrupt corresponds
 *      to a fresh frame.
 *   5. If ``cfg->irq_pin`` is in range, configure that ICU IRQ pin
 *      for falling-edge detection.
 *
 * @param[in] cfg Configuration descriptor (non-NULL).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 IC alive, driver ready.
 * @retval k_ra_err_null_ptr       ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg    ``cfg->i2c_channel`` out of range or
 *                                 ``cfg->target_7b`` not 0x5D/0x14.
 * @retval k_ra_err_invalid_state  ``ra_touch_open`` was already called.
 * @retval k_ra_err_hw_init_failed I2C transport or product-id check
 *                                 failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` already called.
 * @post On success the driver is in the open state.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_touch_open(const ra_touch_cfg_t* cfg);

/**
 * @brief Tear the driver down and detach any registered handler.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Driver closed.
 * @retval k_ra_err_not_initialized ``ra_touch_open`` was not called.
 *
 * @pre Driver is currently open.
 * @post No handler is registered; the IIC_B channel is deinitialised.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_touch_close(void);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Handler stored.
 * @retval k_ra_err_not_initialized Driver is not open.
 *
 * @pre Driver is open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_touch_attach_handler(ra_touch_event_fn_t fn, void* ctx);

/**
 * @brief Test-callable IRQ dispatch shim.
 *
 * @details
 * Mirrors the ERI dispatch helper from ``ra_iic_b``: this is the
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
void ra_touch_dispatch_irq(void);

/* ===========================================================================
 * Touch-point read
 * ===========================================================================
 */

/**
 * @brief Drain the current touch frame.
 *
 * @details
 * Sequence:
 *   1. ``ra_iic_b_transfer`` with the 16-bit register pointer to
 *      ::k_ra_touch_gt911_reg_status, reads 1 status byte.
 *   2. If bit7 is clear (no frame ready), set ``*got_count = 0`` and
 *      return ``k_ra_ok``.
 *   3. Otherwise read up to ``min(status & 0x0F, max_count)`` 8-byte
 *      point records starting at ::k_ra_touch_gt911_reg_point0 and
 *      decode them into ``out_points``.
 *   4. Write ``k_ra_touch_gt911_cmd_clear_status`` (0x00) into the
 *      status register so the IC can latch the next frame.
 *
 * @param[out] out_points Destination buffer (non-NULL, must hold
 *                        ``max_count`` entries).
 * @param[in]  max_count  Capacity of ``out_points``, in entries.
 * @param[out] got_count  Number of valid entries written.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Frame drained (possibly empty).
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_invalid_arg     ``max_count == 0``.
 * @retval k_ra_err_not_initialized Driver is not open.
 * @retval k_ra_err_hw_error        I2C transport failed.
 *
 * @pre Driver is open.
 * @post On success ``*got_count <= max_count``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_touch_read(ra_touch_point_t* out_points, uint8_t max_count, uint8_t* got_count);

/**
 * @brief Run any backend-specific calibration routine.
 *
 * @details
 * No-op for GT911 (factory-calibrated). Returns ``k_ra_ok`` so callers
 * can wire it unconditionally; resistive backends added later will
 * actually drive the calibration sequence here.
 *
 * @return ``k_ra_ok`` unconditionally for the GT911 backend.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_touch_calibrate(void);

#ifdef UNIT_TEST
/**
 * @brief Decode a raw GT911 point block into ``ra_touch_point_t``.
 *
 * @details
 * Test-only entry point. Production code reaches the same parser via
 * ``ra_touch_read``; tests use this entry to inject synthetic GT911
 * payloads without driving the full I2C transport.
 *
 * @param[in]  raw         Source buffer holding ``n_points * 8`` bytes
 *                         in GT911 wire format.
 * @param[in]  n_points    Number of point records present in ``raw``.
 * @param[out] out_points  Destination buffer.
 * @param[in]  max_count   Capacity of ``out_points``.
 * @param[out] got_count   Entries actually written.
 *
 * @return ``k_ra_ok`` on success, ``k_ra_err_null_ptr`` if any pointer
 *         is NULL, ``k_ra_err_invalid_arg`` if ``max_count == 0``.
 *
 * @note Test-only; not compiled into firmware builds.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_touch_test_decode(const uint8_t*    raw,
                                            uint8_t           n_points,
                                            ra_touch_point_t* out_points,
                                            uint8_t           max_count,
                                            uint8_t*          got_count);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif
