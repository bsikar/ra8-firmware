/**
 * @file ra8_i2c_bus_ops.h
 * @brief Injected I2C-bus seam consumed by Ring-3 I2C device drivers.
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 implements I2C twice: the classic RIIC block (`ra8_i2c.h`,
 * three channels) and the I3C block's I2C-compatibility mode (`ra8_i3c.h`,
 * one channel). Device drivers at [Ring 3 / HAL] (the GT911 touch driver
 * in `ra8_touch.h`, the SMBus 3.2 layer in `ra8_smbus.h`) must not care
 * which of the two electrically-equivalent peripherals a board revision
 * wires their device to, so they take this small function-pointer seam
 * in their config instead of naming `ra8_i2c_*` or `ra8_i3c_*` directly.
 *
 * The seam deliberately lives at Ring 3, NOT in the `ra8_io` fabric: the
 * peripheral-agnostic bus facade `ra8_io_i2c_bus.h` sits at [Ring 4 / PAL],
 * and a Ring-3 driver depending on it would invert ring ordering (see
 * `docs/RING_AND_WORLD.md` and the identical `ra8_vsource` /
 * `ra8_io_blockdev` split). The sanctioned bridge is the Ring-4 adapter
 * `ra8_io_i2c_bus_as_ops()`, which fills this struct with trampolines into
 * a bound ::ra8_io_i2c_bus_t; the app owns the bus, binds a backend, and
 * hands the filled seam to the device driver's `_init()`/`_open()` cfg.
 *
 * The seam is transfer-only by design: bus bring-up (peripheral init,
 * bit-rate) and teardown are the app's responsibility, because only the
 * app knows which peripheral the board wires to the device and which
 * other devices share that bus.
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

/**
 * @struct ra8_i2c_bus_ops_t
 * @brief Caller-filled I2C controller-transfer seam (write / read /
 *        write-then-read).
 *
 * @details
 * A device driver stores this struct by value from its config and routes
 * every bus transaction through the three callbacks. The `ctx` cookie is
 * opaque to the driver; the binder that filled the seam decides what it
 * points at (the Ring-4 `ra8_io_i2c_bus_as_ops()` points it at the bound
 * ::ra8_io_i2c_bus_t handle, which must therefore out-live the driver).
 * Tests may fill the seam directly with local trampolines.
 *
 * @invariant `write`, `read`, and `transfer` are non-NULL once filled.
 * @invariant Whatever `ctx` references out-lives every callback call.
 *
 * @par Example:
 * @code
 * ra8_io_i2c_bus_t bus = {};
 * (void)ra8_io_i2c_bus_bind_i3c_compat(&bus, 0U);
 * ra8_i2c_bus_ops_t ops = {};
 * (void)ra8_io_i2c_bus_as_ops(&bus, &ops);
 * ra8_touch_cfg_t cfg = { .bus = ops, ... };
 * @endcode
 *
 * @see ra8_io_i2c_bus_as_ops  Ring-4 binder over the `ra8_io` I2C facade.
 * @see ra8_touch_cfg_t        Ring-3 consumer (GT911 touch driver).
 * @see ra8_smbus_cfg_t        Ring-3 consumer (SMBus 3.2 layer).
 *
 * @since 0.1.0
 */
typedef struct {
  /**
   * @brief Controller write of `len` bytes to 7-bit address `addr`.
   *
   * @details
   * When `send_stop` is true the transaction ends with STOP and the bus
   * is released; when false the bus is held so the next call on the same
   * bus injects a repeated-START. Matches the `ra8_i2c_write` contract
   * (`ra8_i3c_write` uses the inverted `restart` flag; binders adapt).
   *
   * @param[in] ctx       Binder-owned cookie (the `ctx` member below).
   * @param[in] addr      7-bit peripheral address.
   * @param[in] data      Bytes to send (non-NULL when `len` > 0).
   * @param[in] len       Byte count.
   * @param[in] send_stop true = STOP after the payload; false = hold.
   *
   * @return `ra8_err_t` forwarded from the bound transport.
   */
  ra8_err_t (*write)(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop);

  /**
   * @brief Controller read of `len` bytes from 7-bit address `addr`.
   *
   * @details
   * Issues START, the address byte with R/W# = 1, drains `len` bytes,
   * then issues STOP and releases the bus.
   *
   * @param[in]  ctx  Binder-owned cookie (the `ctx` member below).
   * @param[in]  addr 7-bit peripheral address.
   * @param[out] data Destination buffer (non-NULL).
   * @param[in]  len  Byte count (non-zero).
   *
   * @return `ra8_err_t` forwarded from the bound transport.
   */
  ra8_err_t (*read)(void* ctx, uint8_t addr, uint8_t* data, uint32_t len);

  /**
   * @brief Combined write-then-RESTART-then-read in one transaction.
   *
   * @details
   * The classic "address a register, read its contents" shape: writes
   * `wr_len` bytes, injects a repeated START, reads `rd_len` bytes, then
   * issues STOP. Matches `ra8_i2c_transfer` / `ra8_i3c_transfer`.
   *
   * @param[in]  ctx    Binder-owned cookie (the `ctx` member below).
   * @param[in]  addr   7-bit peripheral address.
   * @param[in]  wr     Bytes to send first (NULL only when `wr_len` 0).
   * @param[in]  wr_len Number of bytes to send.
   * @param[out] rd     Destination buffer (NULL only when `rd_len` 0).
   * @param[in]  rd_len Number of bytes to read.
   *
   * @return `ra8_err_t` forwarded from the bound transport.
   */
  ra8_err_t (*transfer)(void*          ctx,
                        uint8_t        addr,
                        const uint8_t* wr,
                        uint32_t       wr_len,
                        uint8_t*       rd,
                        uint32_t       rd_len);

  void* ctx; /**< Opaque cookie handed to every callback (binder-owned). */
} ra8_i2c_bus_ops_t;

#ifdef __cplusplus
}
#endif
