/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sci_spi.h
 * @brief SCI Simple-SPI controller-mode driver (polling, full-duplex)
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Drives an RA8D2 SCI_B channel in *Simple SPI* mode
 * (``CCR3.MOD = 011b``) as a controller, with an internal clock and
 * the SCKn pin as the clock output. This is the transport the
 * EK-RA8D2 Pmod2 (J25) microSD uses: HUM Table 20.13 routes P601
 * (SCK0_B) / P602 (CIPO0_B) / P603 (COPI0_B) / P604 (SS0_B) to
 * **SCI0 Simple SPI** at ``PSEL = 00100b`` -- the RSPI/SPI_B
 * peripheral has no function on those pins. Use the chip-select as a
 * GPIO so it can be held low across a multi-frame SD transaction.
 *
 * Bit-rate (HUM Table 38.7, clock-synchronous / simple-SPI row,
 * ``BGDM = 0``): ``B = TCLK / (4 * 4^n * (N + 1))`` with ``n =
 * CCR2.CKS`` and ``N = CCR2.BRR``. The driver picks the smallest
 * ``n`` that keeps ``N <= 255``.
 *
 * SPI mode maps to CCR3.CPOL/CPHA directly (mode 0 = CPOL 0, CPHA 0),
 * which is the SD-over-SPI default.
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_spi.h"

/**
 * @brief Configuration descriptor for ::ra8_sci_spi_init.
 *
 * @details ``pclk_hz`` is the SCI baud-clock source (PCLKA on the
 * RA8D2). ``mode`` reuses the public ::ra8_spi_mode_t encoding.
 */
typedef struct {
  uint32_t       baud_hz;   /**< Desired SCK bit-rate (Hz).         */
  uint32_t       pclk_hz;   /**< SCI baud-clock source (PCLKA, Hz). */
  ra8_spi_mode_t mode;      /**< Clock polarity/phase (mode 0..3).  */
  bool           lsb_first; /**< ``true`` shifts LSB first.         */
} ra8_sci_spi_cfg_t;

/**
 * @brief Bring an SCI channel up as a Simple-SPI controller.
 *
 * @param[in] channel SCI channel index (0..9).
 * @param[in] cfg     Configuration; must be non-NULL.
 *
 * @retval k_ra8_ok Channel is enabled (TE+RE) and ready to transfer.
 * @retval k_ra8_err_null_ptr ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range or baud 0.
 *
 * @pre The SCKn / CIPOn / COPIn pins are routed to the SCI function
 *      (``PSEL = 00100b``) and the chip-select is owned as a GPIO.
 * @post The SCI MSTP gate is enabled and the channel is in Simple-SPI
 *       controller mode at the requested bit-rate.
 * @note Not thread-safe; serialize access to a channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_spi_init(uint8_t channel, const ra8_sci_spi_cfg_t* cfg);

/**
 * @brief Disable the channel and release its MSTP gate.
 *
 * @param[in] channel SCI channel index (0..9).
 * @retval k_ra8_ok Channel disabled.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 * @pre ::ra8_sci_spi_init succeeded for this channel.
 * @post TE/RE are cleared and the MSTP gate is released.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_spi_deinit(uint8_t channel);

/**
 * @brief Re-program the bit-rate without otherwise reconfiguring.
 *
 * @param[in] channel SCI channel index (0..9).
 * @param[in] baud_hz Desired SCK bit-rate (Hz), non-zero.
 * @param[in] pclk_hz SCI baud-clock source (PCLKA, Hz).
 * @retval k_ra8_ok Bit-rate updated.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range or baud 0.
 * @pre ::ra8_sci_spi_init succeeded for this channel.
 * @post CCR2.CKS/BRR reflect the new rate; the channel stays enabled.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclk_hz);

/**
 * @brief Full-duplex single-frame transfer.
 *
 * @param[in]  channel SCI channel index (0..9).
 * @param[in]  tx      Byte shifted out on COPI.
 * @param[out] rx      Byte shifted in on CIPO; may be NULL to discard.
 * @retval k_ra8_ok Frame completed.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 * @retval k_ra8_err_hw_timeout The TX-empty or RX-full poll expired.
 * @pre ::ra8_sci_spi_init succeeded for this channel.
 * @post One frame was clocked; ``*rx`` holds the received byte.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

/**
 * @brief Full-duplex multi-byte transfer.
 *
 * @details Either buffer may be NULL: a NULL ``tx`` shifts idle
 * (0xFF) bytes (SD/flash read convention); a NULL ``rx`` discards the
 * received bytes. ``len == 0`` is a no-op success.
 *
 * @param[in]  channel SCI channel index (0..9).
 * @param[in]  tx      Bytes to shift out, or NULL for idle fill.
 * @param[out] rx      Buffer for received bytes, or NULL to discard.
 * @param[in]  len     Number of bytes to transfer.
 * @retval k_ra8_ok All bytes transferred.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 * @retval k_ra8_err_hw_timeout A per-frame poll expired.
 * @pre ::ra8_sci_spi_init succeeded for this channel.
 * @post ``len`` frames were clocked.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sci_spi_xfer(uint8_t channel, const uint8_t* tx, uint8_t* rx, uint32_t len);

#ifdef __cplusplus
}
#endif
