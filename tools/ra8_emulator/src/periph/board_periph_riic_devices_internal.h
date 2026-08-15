/**
 * @file board_periph_riic_devices_internal.h
 * @brief Internal RIIC bus-device registry and board-device models
 * @details Declares the bounded bus registry shared with the RIIC controller
 * and owns lifecycle/reporting seams for the PI4IOE5V6408 expander and OV5640
 * SCCB sensor that populate the EK-RA8D2 system bus.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief One device registered on the modelled RIIC bus.
 * @details Binds a 7-bit address to bounded byte-transfer callbacks and their
 * module-owned context; the RIIC controller never assumes a concrete device
 * representation.
 */
typedef struct {
  bool    present;                                         /**< Slot occupied.      */
  uint8_t addr_7b;                                         /**< 7-bit address.      */
  void (*write)(void* ctx, uint8_t byte);                  /**< Controller->device. */
  uint32_t (*read)(void* ctx, uint8_t* buf, uint32_t max); /**< Device->controller. */
  void (*stop)(void* ctx);                                 /**< STOP/transfer end.  */
  void* ctx;                                               /**< Device state.       */
} riic_device_t;

/**
 * @brief Find the registered RIIC device answering one 7-bit address.
 * @details Searches the fixed-capacity board-device registry without changing
 * transfer state or acquiring storage.
 * @param[in] addr_7b Seven-bit bus address to resolve.
 * @return Pointer to the matching registry entry, or nullptr when unclaimed.
 * @retval nullptr No registered device claims @p addr_7b; otherwise the
 * returned pointer identifies the matching entry.
 * @pre @p addr_7b is limited to the seven-bit I2C address domain.
 * @pre The RIIC device registry has been reset for the current emulator run.
 * @post The registry and all device states are unchanged.
 * @post Ownership of the returned module-owned entry remains with the model.
 * @note The returned pointer stays valid until the next device reset.
 * @since 0.1.0
 */
RA8_PRIV riic_device_t* priv_riic_device_find(uint8_t addr_7b);

/**
 * @brief Reset and repopulate the board's RIIC device registry.
 * @details Clears all registry and device state, seeds the expander identity
 * register, and installs the PI4IOE5V6408 and OV5640 callback entries.
 * @pre No RIIC device callback is executing during the reset.
 * @pre The call executes on the emulator's single owning thread.
 * @post Only the two board devices occupy registry entries.
 * @post Both device models reflect their documented reset values.
 * @note The operation uses only fixed-capacity module storage.
 * @since 0.1.0
 */
RA8_PRIV void priv_riic_devices_reset(void);

/**
 * @brief Report activity observed by the registered board devices.
 * @details Emits one diagnostic line for an active expander or camera model,
 * including accepted writes and OV5640 identity-register reads.
 * @pre The injected emulator diagnostic sink is initialized.
 * @pre The call executes on the emulator's single owning thread.
 * @post Device and registry state are unchanged.
 * @post Diagnostic output contains only devices with observed activity.
 * @note Sink failures follow the emulator's existing diagnostic policy.
 * @since 0.1.0
 */
RA8_PRIV void priv_riic_devices_report(void);
