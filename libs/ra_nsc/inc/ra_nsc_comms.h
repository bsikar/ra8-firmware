/**
 * @file ra_nsc_comms.h
 * @brief NSC veneers for the Wave 3 communications drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wave 9.3 retrofit veneers for the comms drivers (ra_sci,
 * ra_iic, ra_spi, ra_usb). Each function here is a Non-Secure
 * Callable entry point that validates pointer arguments via
 * ``cmse_check_address_range`` (under TrustZone) and forwards
 * to the matching Ring-3 driver primitive in the secure world.
 *
 * Wave 9.3 ships a representative subset -- init + the most
 * common transfer primitive per driver. The remaining surface
 * (interrupt callbacks, DMA paths, error introspection) is
 * straightforward to add by following the same pattern; it is
 * deferred to Wave 9.3b alongside the first NS-world consumer
 * that actually exercises those paths.
 *
 * ## Coverage
 *
 *   ra_nsc_sci_init       wraps ra_sci_init
 *   ra_nsc_sci_putc       wraps ra_sci_putc_polling
 *   ra_nsc_sci_getc       wraps ra_sci_getc_polling
 *   ra_nsc_iic_init       wraps ra_iic_init
 *   ra_nsc_iic_write      wraps ra_iic_write
 *   ra_nsc_iic_read       wraps ra_iic_read
 *   ra_nsc_spi_init       wraps ra_spi_init
 *   ra_nsc_spi_xfer8      wraps ra_spi_xfer8
 *   ra_nsc_usb_init       wraps ra_usb_device_init
 *   ra_nsc_usb_attach     wraps ra_usb_device_attach
 *
 * @par TrustZone Safety:
 *  - **Validates:** every config struct pointer + every byte
 *    buffer is in NS-readable / NS-writable memory.
 *  - **Trusts:** the secure-side driver state machines.
 *  - **Denies:** raw struct pass-by-value crossing the boundary
 *    (the cfg pointers stay opaque to the secure side until the
 *    veneer copies what it needs).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_iic.h"
#include "ra_nsc_veneer.h"
#include "ra_sci.h"
#include "ra_spi.h"
#include "ra_usb.h"

/* =============================================================================
 * SCI veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SCI channel from NS code.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[in] cfg     Configuration descriptor in NS memory.
 *
 * @return ``ra_err_t`` error code from the underlying ra_sci_init.
 *
 * @par TrustZone Safety:
 *  - Validates the cfg pointer covers ``sizeof(ra_sci_cfg_t)`` of
 *    NS-readable memory before forwarding.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_sci_init(uint8_t channel, const ra_sci_cfg_t* cfg);

/**
 * @brief NSC veneer: blocking single-byte SCI write.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[in] byte    Byte to send.
 *
 * @return ``ra_err_t`` error code.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_sci_putc(uint8_t channel, uint8_t byte);

/**
 * @brief NSC veneer: blocking single-byte SCI read.
 *
 * @param[in]  channel  SCI channel 0..9.
 * @param[out] out_byte Destination in NS memory.
 *
 * @return ``ra_err_t`` error code.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_sci_getc(uint8_t channel, uint8_t* out_byte);

/* =============================================================================
 * IIC veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an IIC channel.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg);

/**
 * @brief NSC veneer: blocking I2C write to a 7-bit target.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_iic_write(uint8_t        channel,
                                                      uint8_t        target_7b,
                                                      const uint8_t* data,
                                                      uint32_t       len);

/**
 * @brief NSC veneer: blocking I2C read from a 7-bit target.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_iic_read(uint8_t  channel,
                                                     uint8_t  target_7b,
                                                     uint8_t* out_buf,
                                                     uint32_t len);

/* =============================================================================
 * SPI veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SPI master channel.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg);

/**
 * @brief NSC veneer: full-duplex single-byte SPI exchange.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

/* =============================================================================
 * USB veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the USB device controller.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_usb_init(ra_usb_speed_t speed);

/**
 * @brief NSC veneer: raise / drop USB D+ pull-up.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_usb_attach(ra_usb_speed_t speed, bool attached);

#ifdef __cplusplus
}
#endif
