/**
 * @file ra8_nsc_comms.h
 * @brief NSC veneers for the communications drivers
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Retrofit veneers for the comms drivers (ra8_sci,
 * ra8_iic, ra8_spi, ra8_usb). Each function here is a Non-Secure
 * Callable entry point that validates pointer arguments via
 * ``cmse_check_address_range`` (under TrustZone) and forwards
 * to the matching Ring-3 driver primitive in the secure world.
 *
 * This layer ships a representative subset -- init + the most
 * common transfer primitive per driver. The remaining surface
 * (interrupt callbacks, DMA paths, error introspection) is
 * straightforward to add by following the same pattern; it is
 * deferred to land alongside the first NS-world consumer
 * that actually exercises those paths.
 *
 * ## Coverage
 *
 * ra8_nsc_sci_init wraps ra8_sci_init
 * ra8_nsc_sci_putc wraps ra8_sci_putc_polling
 * ra8_nsc_sci_getc wraps ra8_sci_getc_polling
 * ra8_nsc_iic_init wraps ra8_i3c_init
 * ra8_nsc_iic_write wraps ra8_i3c_write
 * ra8_nsc_iic_read wraps ra8_i3c_read
 * ra8_nsc_spi_init wraps ra8_spi_init
 * ra8_nsc_spi_xfer8 wraps ra8_spi_xfer8
 * ra8_nsc_usb_init wraps ra8_usb_device_init
 * ra8_nsc_usb_attach wraps ra8_usb_device_attach
 *
 * @par TrustZone Safety:
 * - **Validates:** every config struct pointer + every byte
 * buffer is in NS-readable / NS-writable memory.
 * - **Trusts:** the secure-side driver state machines.
 * - **Denies:** raw struct pass-by-value crossing the boundary
 * (the cfg pointers stay opaque to the secure side until the
 * veneer copies what it needs).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_nsc_veneer.h"
#include "ra8_sci.h"
#include "ra8_spi.h"
#include "ra8_usb.h"

/* =============================================================================
 * SCI veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SCI channel from NS code.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[in] cfg Configuration descriptor in NS memory.
 *
 * @return ``ra8_err_t`` error code from the underlying ra8_sci_init.
 *
 * @par TrustZone Safety:
 * - Validates the cfg pointer covers ``sizeof(ra8_sci_cfg_t)`` of
 * NS-readable memory before forwarding.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg);

/**
 * @brief NSC veneer: blocking single-byte SCI write.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[in] byte Byte to send.
 *
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_putc(uint8_t channel, uint8_t byte);

/**
 * @brief NSC veneer: blocking single-byte SCI read.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[out] out_byte Destination in NS memory.
 *
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_getc(uint8_t channel, uint8_t* out_byte);

/* =============================================================================
 * IIC veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an IIC channel.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_init(uint8_t channel, const ra8_i3c_cfg_t* cfg);

/**
 * @brief NSC veneer: blocking I2C write to a 7-bit target.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_write(uint8_t        channel,
                                                         uint8_t        target_7b,
                                                         const uint8_t* data,
                                                         uint32_t       len);

/**
 * @brief NSC veneer: blocking I2C read from a 7-bit target.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_read(uint8_t  channel,
                                                        uint8_t  target_7b,
                                                        uint8_t* out_buf,
                                                        uint32_t len);

/* =============================================================================
 * SPI veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SPI controller channel.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_init(uint8_t channel, const ra8_spi_cfg_t* cfg);

/**
 * @brief NSC veneer: full-duplex single-byte SPI exchange.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

/**
 * @brief NSC veneer: multi-frame TX-only polling SPI write.
 *
 * @par TrustZone Safety:
 * - Validates ``tx`` covers ``len * sizeof(unit)`` bytes of NS-readable
 *   memory before forwarding.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_write(uint8_t             channel,
                                                         const void*         tx,
                                                         uint32_t            len,
                                                         ra8_spi_bit_width_t bit_width);

/**
 * @brief NSC veneer: multi-frame RX-only polling SPI read.
 *
 * @par TrustZone Safety:
 * - Validates ``rx`` covers ``len * sizeof(unit)`` bytes of NS-writable
 *   memory before forwarding.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_read(uint8_t             channel,
                                                        void*               rx,
                                                        uint32_t            len,
                                                        ra8_spi_bit_width_t bit_width);

/**
 * @enum ra8_nsc_spi_pack_const_t
 * @brief Bit layout for ``ra8_nsc_spi_ch_bw()`` packed argument.
 */
typedef enum : uint32_t {
  k_ra8_nsc_spi_byte_msk = 0x000000FFU, /**< Byte mask within the packed word. */
  k_ra8_nsc_spi_bw_shift = 8U,          /**< Bit position of ``bit_width``.    */
} ra8_nsc_spi_pack_const_t;

/**
 * @brief Pack ``channel`` + ``bit_width`` into a single 32-bit argument.
 *
 * @details
 * ``ra8_nsc_spi_write_read`` would naturally have five arguments
 * (channel, tx, rx, len, bit_width) but ``cmse_nonsecure_entry``
 * requires every parameter to fit in a register (AAPCS R0..R3). Five
 * args spill to the stack and the compiler rejects the attribute. We
 * encode ``channel`` in bits [7:0] and ``bit_width`` in bits [15:8] so
 * the veneer takes four register-sized arguments.
 *
 * @param[in] channel   SPI channel id.
 * @param[in] bit_width Bus bit width enum value.
 *
 * @return Packed ``ch_bw`` value suitable for ``ra8_nsc_spi_write_read``.
 * @retval value ``(channel & 0xFF) | (bit_width << 8)``.
 *
 * @pre None.
 * @pre None.
 * @post Returned value round-trips: ``(v & 0xFF) == channel`` and
 *       ``((v >> 8) & 0xFF) == (uint8_t)bit_width``.
 * @post No side effects.
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
static inline uint32_t ra8_nsc_spi_ch_bw(uint8_t channel, ra8_spi_bit_width_t bit_width)
{
  return ((uint32_t)channel & (uint32_t)k_ra8_nsc_spi_byte_msk) |
         ((uint32_t)bit_width << (uint32_t)k_ra8_nsc_spi_bw_shift);
}

/**
 * @brief NSC veneer: multi-frame full-duplex polling SPI exchange.
 *
 * @details
 * The veneer takes four register-sized arguments; pack ``channel`` and
 * ``bit_width`` into ``ch_bw`` with ``ra8_nsc_spi_ch_bw()``.
 *
 * @par TrustZone Safety:
 * - Validates both buffer ranges are NS-accessible for the requested
 *   direction before forwarding.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_write_read(uint32_t    ch_bw,
                                                              const void* tx,
                                                              void*       rx,
                                                              uint32_t    len);

/* =============================================================================
 * USB veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the USB device controller.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_usb_init(ra8_usb_speed_t speed);

/**
 * @brief NSC veneer: raise / drop USB D+ pull-up.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_usb_attach(ra8_usb_speed_t speed, bool attached);

#ifdef __cplusplus
}
#endif
