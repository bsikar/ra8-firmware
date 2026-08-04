/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i3c_i2c_peripheral.h
 * @brief IIC_B (I3C in I2C-only mode) peripheral driver -- mirrors FSP ``r_iic_b_peripheral``
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Peripheral-side companion to ``ra8_i3c_i2c`` (controller). The IIC_B block is
 * documented in HUM Ch 40 "I3C Bus Interface (I3C)" pages 2445..2701.
 * In peripheral mode the host bus controller addresses this device via its
 * 7-bit MTAR / SDA address; matching transactions raise NTST.RDBFF0 /
 * TDBEF0 and the polling helpers below drain or refill NTDTBP0
 * accordingly.
 *
 * Public surface (mirrors FSP ``R_IIC_B_PERIPHERAL_*``):
 *   - ``internal_i3c_i2c_peripheral_open`` / ``_close``
 *   - ``internal_i3c_i2c_peripheral_send`` -- respond to a controller-read
 *   - ``internal_i3c_i2c_peripheral_receive`` -- consume a controller-write
 *   - ``internal_i3c_i2c_peripheral_status``
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_i3c_i2c_peripheral_cfg_t
 * @brief Configuration descriptor for ``internal_i3c_i2c_peripheral_open``.
 *
 * @details
 * ``peripheral_addr_7b`` is programmed into the controller-target-address-register
 * window (FSP names it MSDVAD / SDA target address); the bus controller
 * addresses the device with that 7-bit value and the IIC_B block raises
 * NTST.RDBFF0 / TDBEF0 to flag inbound / outbound bytes.
 *
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``internal_i3c_i2c_peripheral_open`` /
 * ``libs/ra8_hal/src/ra8_i3c_i2c_peripheral.c``.
 */
typedef struct {
  uint8_t peripheral_addr_7b; /**< 7-bit own address.                       */
  uint8_t general_call;       /**< Non-zero -> answer general-call address. */
} ra8_i3c_i2c_peripheral_cfg_t;

/**
 * @enum ra8_i3c_i2c_peripheral_status_t
 * @brief Peripheral-side status mask.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_peripheral_status_idle     = 0x00U, /**< No latched event. */
  k_ra8_i3c_i2c_peripheral_status_aas      = 0x01U, /**< Address matched.  */
  k_ra8_i3c_i2c_peripheral_status_rx_full  = 0x02U, /**< NTST.RDBFF0 set.  */
  k_ra8_i3c_i2c_peripheral_status_tx_empty = 0x04U, /**< NTST.TDBEF0 set.  */
  k_ra8_i3c_i2c_peripheral_status_stop     = 0x08U, /**< BST.SPCNDDF set.  */
  k_ra8_i3c_i2c_peripheral_status_nack     = 0x10U, /**< BST.NACKDF set.   */
} ra8_i3c_i2c_peripheral_status_t;

/**
 * @brief Open a channel as an IIC_B peripheral.
 *
 * @param[in] channel Channel index (only 0 valid on RA8D2).
 * @param[in] cfg     Configuration descriptor (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               Channel up; address programmed.
 * @retval k_ra8_err_null_ptr     ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg  Channel/address out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t internal_i3c_i2c_peripheral_open(uint8_t channel,
                                                         const ra8_i3c_i2c_peripheral_cfg_t* cfg);

/**
 * @brief Close the peripheral channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               Channel torn down.
 * @retval k_ra8_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t internal_i3c_i2c_peripheral_close(uint8_t channel);

/**
 * @brief Push ``len`` bytes into NTDTBP0 in response to a controller-read.
 *
 * @param[in] channel Channel index.
 * @param[in] data    Buffer (non-NULL when ``len > 0``).
 * @param[in] len     Byte count.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               All bytes loaded.
 * @retval k_ra8_err_null_ptr     ``data`` is NULL with non-zero len.
 * @retval k_ra8_err_invalid_arg  Channel out of range.
 * @retval k_ra8_err_hw_timeout   TDBEF0 wait exhausted spin budget.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
internal_i3c_i2c_peripheral_send(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Drain ``len`` bytes from NTDTBP0 after a controller-write.
 *
 * @param[in]  channel Channel index.
 * @param[out] buf     Destination buffer (non-NULL when ``len > 0``).
 * @param[in]  len     Byte count.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               All bytes consumed.
 * @retval k_ra8_err_null_ptr     ``buf`` is NULL with non-zero len.
 * @retval k_ra8_err_invalid_arg  Channel out of range.
 * @retval k_ra8_err_hw_timeout   RDBFF0 wait exhausted spin budget.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
internal_i3c_i2c_peripheral_receive(uint8_t channel, uint8_t* buf, uint32_t len);

/**
 * @brief Read latched peripheral-side status mask.
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra8_i3c_i2c_peripheral_status_*`` bits (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               Snapshot populated.
 * @retval k_ra8_err_null_ptr     ``out_mask`` is NULL.
 * @retval k_ra8_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t internal_i3c_i2c_peripheral_status(uint8_t channel, uint8_t* out_mask);

#ifdef __cplusplus
}
#endif
