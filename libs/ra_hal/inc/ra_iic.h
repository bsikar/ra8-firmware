/**
 * @file ra_iic.h
 * @brief Legacy IIC (RIIC) master + slave driver -- mirrors FSP
 *        ``r_iic_master`` / ``r_iic_slave``.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public surface is split into ``ra_iic_master_*`` and
 * ``ra_iic_slave_*`` to keep the cross-world symbol space clean
 * (``ra_iic_init`` / ``ra_iic_write`` / ``ra_iic_read`` are reserved
 * for the IIC_B NSC pass-throughs in ``ra_iic_b.h``).
 *
 *  - ``ra_iic_master_open``  / ``ra_iic_master_close``
 *  - ``ra_iic_master_send`` (polling write)
 *  - ``ra_iic_master_receive`` (polling read)
 *  - ``ra_iic_master_status``
 *  - ``ra_iic_slave_open``  / ``ra_iic_slave_close``
 *  - ``ra_iic_slave_send`` (respond to controller read)
 *  - ``ra_iic_slave_receive`` (consume controller write)
 *  - ``ra_iic_slave_status``
 *
 * The RA8D2 silicon does not actually carry the legacy block; this
 * driver is exercised in the host unit-test substrate so applications
 * that previously targeted RX/RA4/RA6 keep their compile shape.
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

/**
 * @struct ra_iic_master_cfg_t
 * @brief Configuration descriptor for ``ra_iic_master_open``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_iic_master_open`` /
 * ``_send`` / ``_receive`` in ``libs/ra_hal/src/ra_iic.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t bus_hz;   /**< Target SCL clock in Hz.                */
  uint32_t pclkb_hz; /**< Current PCLKB rate in Hz (bit-rate calc).*/
} ra_iic_master_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_iic_slave_cfg_t
 * @brief Configuration descriptor for ``ra_iic_slave_open``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t slave_addr_7b; /**< 7-bit own address loaded into SAR0. */
  uint8_t general_call;  /**< Non-zero -> answer general-call.    */
} ra_iic_slave_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_iic_status_mask_t
 * @brief Bit mask returned by ``ra_iic_master_status`` / ``_slave_status``.
 */
typedef enum : uint8_t {
  k_ra_iic_status_idle    = 0x00U, /**< Bus idle, no events latched.   */
  k_ra_iic_status_busy    = 0x01U, /**< BBSY active.                   */
  k_ra_iic_status_nack    = 0x02U, /**< Last byte was NACKed.          */
  k_ra_iic_status_stop    = 0x04U, /**< STOP detected.                 */
  k_ra_iic_status_tx_done = 0x08U, /**< TEND latched after a write.    */
  k_ra_iic_status_rx_full = 0x10U, /**< RDRF latched.                  */
  k_ra_iic_status_aas     = 0x20U, /**< Slave address matched (slave). */
} ra_iic_status_mask_t;

/* =============================================================================
 * Master surface
 * =============================================================================
 */

/**
 * @brief Open a legacy-IIC channel as a master.
 *
 * @param[in] channel Channel index (0..2).
 * @param[in] cfg     Configuration (non-NULL, ``bus_hz != 0``).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel opened, ICE set, MST/TRS=1.
 * @retval k_ra_err_null_ptr     ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg  channel/bus_hz out of range.
 *
 * @pre Caller is single-threaded init context.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_master_open(uint8_t channel, const ra_iic_master_cfg_t* cfg);

/**
 * @brief Close the master channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel closed.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_master_close(uint8_t channel);

/**
 * @brief Polling write to a 7-bit target.
 *
 * @param[in] channel   Channel index.
 * @param[in] target_7b 7-bit slave address.
 * @param[in] data      Buffer (non-NULL when ``len > 0``).
 * @param[in] len       Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Transfer completed, STOP issued.
 * @retval k_ra_err_null_ptr     ``data`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 * @retval k_ra_err_nack         Slave NACKed; STOP issued.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_master_send(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len);

/**
 * @brief Polling read from a 7-bit target.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit slave address.
 * @param[out] buf       Destination buffer (non-NULL when ``len > 0``).
 * @param[in]  len       Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Transfer completed, STOP issued.
 * @retval k_ra_err_null_ptr     ``buf`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 * @retval k_ra_err_nack         Slave NACKed the address byte.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_master_receive(uint8_t channel, uint8_t target_7b, uint8_t* buf, uint32_t len);

/**
 * @brief Read latched master-side status mask.
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra_iic_status_*`` bits (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Snapshot populated.
 * @retval k_ra_err_null_ptr     ``out_mask`` is NULL.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_master_status(uint8_t channel, uint8_t* out_mask);

/* =============================================================================
 * Slave surface
 * =============================================================================
 */

/**
 * @brief Open a legacy-IIC channel as a slave.
 *
 * @param[in] channel Channel index.
 * @param[in] cfg     Configuration (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel opened, SAR0 programmed.
 * @retval k_ra_err_null_ptr     ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg  Channel/address out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_slave_open(uint8_t channel, const ra_iic_slave_cfg_t* cfg);

/**
 * @brief Close the slave channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_slave_close(uint8_t channel);

/**
 * @brief Push ``len`` bytes into the TX register in response to a controller-read.
 *
 * @param[in] channel Channel index.
 * @param[in] data    Buffer to send (non-NULL when ``len > 0``).
 * @param[in] len     Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               All bytes written into ICDRT.
 * @retval k_ra_err_null_ptr     ``data`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_slave_send(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Drain ``len`` bytes from ICDRR after a controller-write.
 *
 * @param[in]  channel Channel index.
 * @param[out] buf     Destination buffer (non-NULL when ``len > 0``).
 * @param[in]  len     Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               All bytes consumed from ICDRR.
 * @retval k_ra_err_null_ptr     ``buf`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_slave_receive(uint8_t channel, uint8_t* buf, uint32_t len);

/**
 * @brief Read slave-side status mask (AAS, RDRF, TEND, ...).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_slave_status(uint8_t channel, uint8_t* out_mask);

#ifdef __cplusplus
}
#endif
