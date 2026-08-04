/**
 * @file board_periph_i2c_internal.h
 * @brief Module-private interfaces shared by the board_periph_i2c TUs
 *
 * @details
 * The I3C-in-I2C-mode bus model and its attached device models are one
 * logical module split across two translation units: the controller + GT911
 * touch model (board_periph_i2c.c) and the LSM6DSO IMU + MAX17048 fuel-gauge
 * devices (board_periph_i2c_devices.c). The bus-side device registry and the
 * device-side reset/register/telemetry entry points cross here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Byte-lane constants shared across the bus + device TUs. */
typedef enum : uint32_t {
  k_i3c_dev_byte_mask = 0xFFU, /**< One-byte mask for register-file lanes. */
} i2c_devices_const_t;

/**
 * @brief Register a device model in the first free bus slot (drop if full).
 *
 * @param[in] addr_7b 7-bit I2C address the device answers.
 * @param[in] wr      Write-phase byte sink (register pointer / config).
 * @param[in] rd      Read-phase burst source.
 * @param[in] stop    STOP-condition notification.
 * @param[in] ctx     Device state handed back to every callback.
 * @return Nothing.
 * @pre The registry has a free slot (drops silently when full).
 * @pre The callbacks and @p ctx outlive the run.
 * @post The device answers address phases at @p addr_7b.
 * @note Not thread-safe; call from reset/registration paths only.
 * @since 0.1.0
 */
RA8_PRIV void i2c_device_register(uint8_t addr_7b,
                                  void (*wr)(void*, uint8_t),
                                  uint32_t (*rd)(void*, uint8_t*, uint32_t),
                                  void (*stop)(void*),
                                  void* ctx);

/**
 * @brief Re-lay the IMU + fuel-gauge register files (block reset path).
 *
 * @return Nothing.
 * @pre The block reset is re-initialising the bus devices.
 * @pre The CLI battery state (if any) was already applied.
 * @post WHO_AM_I / VCELL / SOC / VERSION / CRATE read their seeded values.
 * @note Not thread-safe; single-threaded reset path only.
 * @since 0.1.0
 */
RA8_PRIV void board_i2c_imu_fuel_reset(void);

/**
 * @brief Register the LSM6DSO + MAX17048 devices on the modelled bus.
 *
 * @return Nothing.
 * @pre The registry was just cleared by the block reset.
 * @pre board_i2c_imu_fuel_reset() ran (register files are laid).
 * @post Both devices answer their 7-bit addresses.
 * @note Not thread-safe; single-threaded reset path only.
 * @since 0.1.0
 */
RA8_PRIV void board_i2c_imu_fuel_register(void);

/**
 * @brief IMU register reads answered this run (report telemetry).
 *
 * @return The LSM6DSO read counter.
 * @retval 0 No IMU read was served.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t board_i2c_imu_reads(void);

#ifdef __cplusplus
}
#endif
