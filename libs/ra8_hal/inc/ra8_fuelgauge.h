/**
 * @file ra8_fuelgauge.h
 * @brief Battery fuel-gauge driver -- MAX17048 backend
 * @ingroup grp_hal_usb
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * `ra8_batt` decides *when* to raise a low-battery warning, but nothing in
 * `libs/` produced the `(soc_pct, charging)` pair it consumes, so every
 * battery-powered app re-encoded the MAX17048 register map for itself --
 * over two different transports, one of them naming the I3C peripheral
 * directly from application code. `ra8_fuelgauge` is the device layer that
 * was missing underneath the policy layer.
 *
 * The driver speaks to the gauge through the injected Ring-3 I2C seam
 * (::ra8_i2c_bus_ops_t, filled by the app -- typically with
 * `ra8_io_i2c_bus_as_ops()`), exactly as `ra8_touch` and `ra8_smbus` do, so
 * whether the board routed the gauge to RIIC or to the I3C block's
 * I2C-compatibility mode is a bind-time decision and never a call-site one.
 * Bus bring-up (peripheral init, bit rate) stays with the app.
 *
 * Lifecycle:
 *
 * 1. ::ra8_fuelgauge_open latches the seam and the 7-bit address into a
 *    caller-allocated handle and reads the gauge's VERSION register as a
 *    liveness probe.
 * 2. ::ra8_fuelgauge_read performs three register reads (VCELL, SOC, CRATE)
 *    and hands back one decoded ::ra8_fuelgauge_state_t.
 * 3. ::ra8_fuelgauge_close clears the handle. The app keeps owning the bus.
 *
 * The intended pairing with the policy layer is two lines:
 *
 * @par Example:
 * @code
 * ra8_io_i2c_bus_t bus = {};
 * (void)ra8_io_i2c_bus_bind_i3c_compat(&bus, 0U);
 * ra8_i2c_bus_ops_t ops = {};
 * (void)ra8_io_i2c_bus_as_ops(&bus, &ops);
 *
 * ra8_fuelgauge_t       fg  = {};
 * ra8_fuelgauge_cfg_t   cfg = {.bus = ops, .target_7b = k_ra8_fuelgauge_default_addr_7b};
 * ra8_err_t             err = ra8_fuelgauge_open(&fg, &cfg);
 *
 * ra8_fuelgauge_state_t st = {};
 * err = ra8_fuelgauge_read(&fg, &st);
 * err = ra8_batt_update(&mon, st.soc_pct, st.charging, &nag);
 * @endcode
 *
 * Static-allocation footprint: the handle is caller-allocated and there is
 * no file-scope state, so several gauges may be open at once. No runtime
 * allocation, no loop and no recursion on any path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fuelgauge_max17048_regs.h"
#include "ra8_i2c_bus_ops.h"

/* ===========================================================================
 * Public constants
 * ===========================================================================
 */

/**
 * @enum ra8_fuelgauge_addr_t
 * @brief Backend-neutral spelling of the gauge's I2C address.
 *
 * @details
 * Provided so an app need not include the MAX17048 register header just to
 * name the address it binds.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fuelgauge_default_addr_7b =
    (uint8_t)k_ra8_fuelgauge_max17048_addr_7b, /**< MAX17048 fixed address. */
} ra8_fuelgauge_addr_t;

/* ===========================================================================
 * Public types
 * ===========================================================================
 */

/**
 * @struct ra8_fuelgauge_cfg_t
 * @brief Configuration descriptor for ::ra8_fuelgauge_open.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; both members
 * are read in ::ra8_fuelgauge_open in `libs/ra8_hal/src/ra8_fuelgauge.c`.
 *
 * @invariant Whatever `bus.ctx` references out-lives the opened handle.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_i2c_bus_ops_t bus;       /**< Injected I2C transfer seam (app-bound).  */
  uint8_t           target_7b; /**< 7-bit gauge address (0x36 for MAX17048). */
} ra8_fuelgauge_cfg_t;

/**
 * @struct ra8_fuelgauge_state_t
 * @brief One decoded gauge sample.
 *
 * @details
 * `soc_pct` is the gauge's integer percent (the SOC high byte, the same
 * value both in-tree readers already used); the 1/256 fraction in the low
 * byte is dropped. `crate_raw` is the signed CRATE register as read, in the
 * part's own 0.208 %/hr units -- the driver does not scale it, because the
 * only in-tree consumers use its sign. `charging` is that sign, decoded:
 * true when `crate_raw >= 0`, which is bit-for-bit the
 * `(crate_high_byte & 0x80) == 0` test both apps wrote by hand.
 *
 * @invariant `soc_pct` is 0..255 as reported; the gauge may report above
 *            100 after a QuickStart and the driver does not clamp it.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t vcell_mv;  /**< Cell voltage in millivolts (78.125 uV per LSB). */
  int16_t  crate_raw; /**< Signed CRATE register, 0.208 %/hr per LSB.      */
  uint8_t  soc_pct;   /**< Integer state of charge, percent.               */
  bool     charging;  /**< True when CRATE is non-negative.                */
} ra8_fuelgauge_state_t;

/**
 * @struct ra8_fuelgauge_t
 * @brief Caller-allocated gauge handle.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_fuelgauge_open. Treat the
 * fields as private; reach the gauge only through the functions below.
 *
 * @invariant `opened` is true only between a successful open and a close.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_i2c_bus_ops_t bus;       /**< Latched transfer seam (private). */
  uint8_t           target_7b; /**< Latched 7-bit address (private). */
  bool              opened;    /**< Open/closed flag (private).      */
} ra8_fuelgauge_t;

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

/**
 * @brief Latch the bus seam into `fg` and probe the gauge.
 *
 * @details
 * Copies `cfg->bus` and `cfg->target_7b` into the handle, then reads the
 * gauge's VERSION register once. The probe is a liveness check, not a
 * part-id check: a silicon revision is not a stable identity across parts,
 * so the driver only rejects the two readings a dead segment produces
 * (all-zero and all-ones), and accepts anything else.
 *
 * @param[out] fg  Caller-allocated handle to fill.
 * @param[in]  cfg Configuration descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Handle is open and the gauge answered.
 * @retval k_ra8_err_null_ptr      `fg` or `cfg` was NULL.
 * @retval k_ra8_err_invalid_arg   `cfg->bus.transfer` was NULL.
 * @retval k_ra8_err_hw_not_ready  The probe read back all-zero or all-ones.
 * @retval k_ra8_err_nack          The gauge did not acknowledge (forwarded).
 * @retval k_ra8_err_hw_timeout    The bus stalled (forwarded).
 *
 * @pre The bus peripheral has been initialised by the app.
 * @pre `cfg->bus` has been filled by a binder (or, in tests, by hand).
 * @post On success `fg` is open; on failure `fg` is left closed.
 *
 * @note Not thread-safe with respect to the same handle.
 *
 * @see ra8_fuelgauge_read   Take one sample.
 * @see ra8_io_i2c_bus_as_ops  Ring-4 binder that fills the seam.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fuelgauge_open(ra8_fuelgauge_t* fg, const ra8_fuelgauge_cfg_t* cfg);

/**
 * @brief Read VCELL, SOC and CRATE and decode them into `out`.
 *
 * @details
 * Three register reads in the part's own "write pointer, RESTART, read two
 * bytes" shape, MSB first. On any transport failure the call returns that
 * error and `out` is left untouched, so a caller cannot mistake a stale
 * sample for a fresh one.
 *
 * @param[in]  fg  Open handle.
 * @param[out] out Destination sample.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Sample decoded into `out`.
 * @retval k_ra8_err_null_ptr        `fg` or `out` was NULL.
 * @retval k_ra8_err_not_initialized `fg` is not open.
 * @retval k_ra8_err_nack            The gauge did not acknowledge (forwarded).
 * @retval k_ra8_err_hw_timeout      The bus stalled (forwarded).
 *
 * @pre `fg` was opened by ::ra8_fuelgauge_open.
 * @post On success every field of `out` is written; on failure none is.
 *
 * @note Not thread-safe with respect to the same handle.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fuelgauge_read(ra8_fuelgauge_t* fg, ra8_fuelgauge_state_t* out);

/**
 * @brief Close `fg` and release the latched seam.
 *
 * @details
 * Clears the handle. The bus peripheral is the app's and is untouched; no
 * register write is issued, so a closed gauge keeps gauging.
 *
 * @param[in,out] fg Handle to close.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Handle closed.
 * @retval k_ra8_err_null_ptr        `fg` was NULL.
 * @retval k_ra8_err_not_initialized `fg` was not open.
 *
 * @pre None beyond a non-NULL handle.
 * @post `fg` is zeroed and a further ::ra8_fuelgauge_read is rejected.
 *
 * @note Not thread-safe with respect to the same handle.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fuelgauge_close(ra8_fuelgauge_t* fg);

#ifdef __cplusplus
}
#endif
