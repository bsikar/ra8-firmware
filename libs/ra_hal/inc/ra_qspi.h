/**
 * @file ra_qspi.h
 * @brief Legacy QSPI driver -- mirrors FSP ``r_qspi``
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Compact polling-mode driver mirroring the FSP ``r_qspi`` API minus
 * the dedicated erase/write/auto-calibrate paths. The RA8D2 silicon
 * does not actually carry the legacy QSPI block (it ships an OSPI
 * controller covered by ``ra_ospi_b``); this driver provides the
 * matching public surface so applications that previously targeted
 * RX or RA4/RA6 keep their compile shape.
 *
 * Public surface (mirrors FSP ``R_QSPI_*``):
 *   - ``ra_qspi_open``   -- programme mode/clock/protocol cfg
 *   - ``ra_qspi_close``  -- gate the channel
 *   - ``ra_qspi_send``   -- issue a command + payload write
 *   - ``ra_qspi_receive`` -- memory-mapped read out of the XIP window
 *   - ``ra_qspi_status`` -- snapshot SFMSST
 *   - ``ra_qspi_xip_enter`` / ``_exit``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_qspi_regs.h"
#include "ra_err.h"

/**
 * @enum ra_qspi_protocol_t
 * @brief Protocol-configuration constants matching FSP ``spi_flash_protocol_t``.
 */
typedef enum : uint8_t {
  k_ra_qspi_protocol_extended = 0U, /**< Single-lane SPI (1-1-1).      */
  k_ra_qspi_protocol_dual     = 1U, /**< Dual-IO SPI (1-1-2).          */
  k_ra_qspi_protocol_quad     = 2U, /**< Quad-IO SPI (1-1-4).          */
  k_ra_qspi_protocol_qpi      = 3U, /**< Quad-Peripheral Interface.    */
} ra_qspi_protocol_t;

/**
 * @struct ra_qspi_cfg_t
 * @brief Configuration descriptor for ``ra_qspi_open``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_qspi_open`` /
 * ``libs/ra_hal/src/ra_qspi.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t           clock_div;    /**< SFMSKC divisor (1..255).        */
  uint8_t            cpol;         /**< 0 / 1 -- SPI clock polarity.    */
  uint8_t            cpha;         /**< 0 / 1 -- SPI clock phase.       */
  uint8_t            dummy_cycles; /**< Dummy cycles for XIP read.      */
  ra_qspi_protocol_t protocol;     /**< SPI protocol mode.              */
  uint8_t            xip_read_op;  /**< XIP read instruction byte.      */
} ra_qspi_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_qspi_status_mask_t
 * @brief Bit mask returned by ``ra_qspi_status``.
 */
typedef enum : uint8_t {
  k_ra_qspi_status_idle    = 0x00U, /**< Channel idle.                */
  k_ra_qspi_status_busy    = 0x01U, /**< SFMSST.BUSY set.             */
  k_ra_qspi_status_tx_full = 0x02U, /**< SFMSST.TXFULL set.           */
  k_ra_qspi_status_rx_rdy  = 0x04U, /**< SFMSST.RXRDY set.            */
  k_ra_qspi_status_xip     = 0x08U, /**< SFMCST shows XIP active.     */
} ra_qspi_status_mask_t;

/**
 * @brief Open a QSPI channel.
 *
 * @param[in] channel Channel index (must be 0).
 * @param[in] cfg     Configuration (non-NULL, ``clock_div != 0``).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel open and ready.
 * @retval k_ra_err_null_ptr     ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg  Channel/clock_div/protocol out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_open(uint8_t channel, const ra_qspi_cfg_t* cfg);

/**
 * @brief Close the QSPI channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel closed.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_close(uint8_t channel);

/**
 * @brief Issue a direct-mode command + payload write.
 *
 * @param[in] channel Channel index.
 * @param[in] data    Payload buffer (non-NULL when ``len > 0``).
 * @param[in] len     Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Bytes pushed into SFMCOM.
 * @retval k_ra_err_null_ptr     ``data`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 * @retval k_ra_err_hw_timeout   TXFULL never cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_send(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Memory-mapped read from the XIP window.
 *
 * @param[in]  channel  Channel index.
 * @param[in]  offset   Byte offset within the XIP window.
 * @param[out] buf      Destination buffer (non-NULL when ``len > 0``).
 * @param[in]  len      Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Bytes copied.
 * @retval k_ra_err_null_ptr     ``buf`` is NULL with non-zero len.
 * @retval k_ra_err_invalid_arg  Channel/offset/len out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_qspi_receive(uint8_t channel, uint32_t offset, uint8_t* buf, uint32_t len);

/**
 * @brief Snapshot SFMSST + SFMCST into a status mask.
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra_qspi_status_*`` bits (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Snapshot populated.
 * @retval k_ra_err_null_ptr     ``out_mask`` is NULL.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Trigger XIP entry (memory-mapped read mode).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_xip_enter(uint8_t channel);

/**
 * @brief Exit XIP / direct-communication mode.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_qspi_xip_exit(uint8_t channel);

#ifdef __cplusplus
}
#endif
