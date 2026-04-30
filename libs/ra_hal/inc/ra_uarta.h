/**
 * @file ra_uarta.h
 * @brief Legacy UART-A (UARTA) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped UART-A peripheral driver. UARTA is a
 * simplified low-power UART block on small RA SKUs (different from
 * the SCI used on RA8D2). This driver exposes the FSP ``r_uarta``
 * open / write / read / close / baud-set surface so example projects
 * targeting other RA chips build unchanged on RA8D2.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. Bytes
 *          written go into a small internal ring buffer that loops
 *          back on read -- treat this as an API-compatibility shim.
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
 * @enum ra_uarta_clock_source_t
 * @brief Clock source selector (mirrors FSP ``uarta_clock_source_t``).
 */
typedef enum : uint8_t {
  k_ra_uarta_clk_sosc_loco = 0U, /**< SOSC/LOCO. */
  k_ra_uarta_clk_mosc      = 1U, /**< MOSC.       */
  k_ra_uarta_clk_hoco      = 2U, /**< HOCO.       */
  k_ra_uarta_clk_moco      = 3U, /**< MOCO.       */
  k_ra_uarta_clk_sosc      = 4U, /**< SOSC.       */
} ra_uarta_clock_source_t;

/**
 * @struct ra_uarta_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t                 channel;      /**< Channel index 0..1.        */
  uint32_t                baud_rate;    /**< Baud rate in bits/sec.     */
  uint8_t                 data_bits;    /**< 7 or 8.                    */
  uint8_t                 stop_bits;    /**< 1 or 2.                    */
  bool                    parity_en;    /**< True -> enable parity.     */
  bool                    parity_odd;   /**< True -> odd, false -> even.*/
  ra_uarta_clock_source_t clock_source; /**< Clock-source selector.     */
} ra_uarta_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_uarta_event_fn_t
 * @brief RX/TX event callback.
 * @param[in] ctx Caller context.
 * @param[in] channel Channel that fired.
 */
typedef void (*ra_uarta_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Open one UARTA channel.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->channel`` is 0 or 1.
 * @pre ``cfg->baud_rate`` is non-zero.
 * @post Channel state is open.
 * @post Internal ring buffer is empty.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_open(const ra_uarta_cfg_t* cfg);

/**
 * @brief Close one UARTA channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_close(uint8_t channel);

/**
 * @brief Write ``len`` bytes from ``data`` to ``channel``.
 * @param[in] channel Channel 0..1.
 * @param[in] data Source buffer.
 * @param[in] len Length in bytes.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_write(uint8_t channel, const uint8_t* data, uint16_t len);

/**
 * @brief Read up to ``len`` bytes into ``data`` from ``channel``.
 * @param[in] channel Channel 0..1.
 * @param[out] data Destination buffer.
 * @param[in] len Maximum bytes to read.
 * @param[out] out_read Bytes actually read.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_uarta_read(uint8_t channel, uint8_t* data, uint16_t len, uint16_t* out_read);

/**
 * @brief Update baud rate at runtime.
 * @param[in] channel Channel 0..1.
 * @param[in] baud_rate Baud in bits/sec, non-zero.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_set_baud(uint8_t channel, uint32_t baud_rate);

/**
 * @brief Read packed status word.
 * @param[in] channel Channel 0..1.
 * @param[out] out_status Bit 0 = open, bit 1 = bytes-buffered.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_get_status(uint8_t channel, uint8_t* out_status);

/**
 * @brief Attach a TX/RX event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_uarta_attach_handler(ra_uarta_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a TX/RX event.
 * @param[in] channel Channel that fired.
 * @since 0.1.0
 */
void ra_uarta_dispatch(uint8_t channel);

#ifdef __cplusplus
}
#endif
