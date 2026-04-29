/**
 * @file ra_spi.h
 * @brief SPI_B master driver (RA8D2 Type-B SPI peripheral)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for the SPI_B master driver. Implementation lives in
 * ``libs/ra_hal/src/ra_spi_b.c`` and mirrors FSP ``r_spi_b`` in
 * polling-mode master flow. The earlier register layout (legacy
 * 8/16-bit SPI block) was replaced wholesale on RA8D2 -- see
 * ``ra8d2_spi_regs.h`` for the SPI_B register file.
 *
 * API surface:
 *
 * - ``ra_spi_init(channel, cfg)`` -- full config + MSTP enable
 * - ``ra_spi_deinit(channel)`` -- SPE clear + MSTP release
 * - ``ra_spi_master_init`` -- defaults init (mode 0, PCLKA = 125 MHz)
 * - ``ra_spi_xfer8`` -- single-byte full-duplex polling xfer
 * - ``ra_spi_set_clock`` -- runtime SPBR change
 * - ``ra_spi_get_errors / clear_errors``-- overrun/mode/parity/underrun
 * - ``ra_spi_attach_transfer_handler`` -- IRQ callback
 * - ``ra_spi_enter_stop / exit_stop`` -- power transition
 * - ``ra_spi_dispatch_spti / spri / spei`` -- ISR entry points
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_dma.h"
#include "ra_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra_spi_mode_t
 * @brief CPOL / CPHA combinations.
 */
typedef enum : uint8_t {
  k_ra_spi_mode_0 = 0U, /**< CPOL=0, CPHA=0. */
  k_ra_spi_mode_1 = 1U, /**< CPOL=0, CPHA=1. */
  k_ra_spi_mode_2 = 2U, /**< CPOL=1, CPHA=0. */
  k_ra_spi_mode_3 = 3U, /**< CPOL=1, CPHA=1. */
} ra_spi_mode_t;

/**
 * @enum ra_spi_bit_width_t
 * @brief Per-frame bit width for multi-byte transfers.
 *
 * @details
 * Mirrors the subset of FSP ``spi_bit_width_t`` that this driver
 * supports. The enum value is the raw SPCMDn.SPB encoding written
 * into the SPI_B command register (HUM Ch 43.2.7 "SPCMDm" p 2893):
 * SPB[4:0] = N - 1 where N is the frame width in bits, so 8-bit
 * frames program SPB = 0b00111, 16-bit frames program SPB = 0b01111,
 * and 32-bit frames program SPB = 0b11111. Matching the FSP enum
 * value lets the value flow straight into SPCMDn.SPB without a
 * lookup table.
 *
 * @see r_spi_b_bit_width_config in FSP ``r_spi_b.c``.
 */
typedef enum : uint8_t {
  k_ra_spi_width_8  = 7U,  /**<  8-bit frame -> SPCMDn.SPB = 0b00111. */
  k_ra_spi_width_16 = 15U, /**< 16-bit frame -> SPCMDn.SPB = 0b01111. */
  k_ra_spi_width_32 = 31U, /**< 32-bit frame -> SPCMDn.SPB = 0b11111. */
} ra_spi_bit_width_t;

/**
 * @struct ra_spi_cfg_t
 * @brief Configuration descriptor for ``ra_spi_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_spi_init`` in
 * ``libs/ra_hal/src/ra_spi_b.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t      baud_hz;   /**< Target SPI clock in Hz. */
  uint32_t      pclka_hz;  /**< Current PCLKA in Hz (for SPBR calc). */
  ra_spi_mode_t mode;      /**< CPOL / CPHA combination. */
  bool          lsb_first; /**< True for LSB-first transfers. */
} ra_spi_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_spi_err_mask_t
 * @brief Bit mask of SPI error flags.
 */
typedef enum : uint8_t {
  k_ra_spi_err_none     = 0x00U,
  k_ra_spi_err_overrun  = 0x01U, /**< SPSR.OVRF set. */
  k_ra_spi_err_mode     = 0x02U, /**< SPSR.MODERF set. */
  k_ra_spi_err_parity   = 0x04U, /**< SPSR.PERF set. */
  k_ra_spi_err_underrun = 0x08U, /**< SPSR.UDRF set. */
} ra_spi_err_mask_t;

/**
 * @typedef ra_spi_complete_fn_t
 * @brief Transfer-complete callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] err_mask OR of ``k_ra_spi_err_*`` bits; zero on success.
 */
typedef void (*ra_spi_complete_fn_t)(void* ctx, uint8_t err_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an SPI channel with a full config descriptor.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] cfg Configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, SPE is set and the channel is ready to xfer.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg);

/**
 * @brief Tear down a channel.
 * @param[in] channel SPI channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 * @post SPE is cleared; MSTP reference released.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Legacy init (1.9 MHz at PCLKA = 125 MHz, mode 0).
 * @param[in] channel SPI channel (0 or 1).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_master_init(uint8_t channel);

/**
 * @brief Full-duplex 8-bit exchange.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] tx Byte to transmit.
 * @param[out] rx Pointer to receive the shifted-in byte (may be NULL).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

/* =============================================================================
 * Multi-byte / multi-width polling transfers (FSP r_spi_b parity)
 * =============================================================================
 */

/**
 * @brief Multi-frame TX-only polling transfer.
 *
 * @details
 * Mirrors FSP ``R_SPI_B_Write`` (delegates to
 * ``r_spi_b_write_read_common`` with ``p_dest = NULL``). For each of
 * ``len`` units the driver:
 *  1. Waits for SPSR.SPTEF (TX buffer empty).
 *  2. Writes one unit (1, 2, or 4 bytes from ``tx``) to SPDR.
 *  3. Waits for SPSR.SPRF (RX buffer full).
 *  4. Reads SPDR and discards the result.
 *
 * Polling is bounded so the driver cannot spin forever on a stuck
 * bus (NASA P10 Rule 2). Default timeout budget tracks
 * ``k_ra_timeout_default_ms``.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] tx Source buffer (``len`` units of ``bit_width``); must be non-NULL.
 * @param[in] len Number of units to transfer; ``0`` is a no-op success.
 * @param[in] bit_width Per-frame width: 8, 16, or 32 bits.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer completed.
 * @retval k_ra_err_null_ptr ``tx`` is NULL with ``len > 0``.
 * @retval k_ra_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra_err_hw_timeout SPSR.SPTEF / SPSR.SPRF never asserted.
 *
 * @pre Channel previously initialised via ``ra_spi_init``.
 * @pre Caller-supplied buffer alignment matches ``bit_width`` (16 / 32 bit
 *      access requires properly aligned pointers).
 * @post On success, every requested unit has shifted out on COPI and
 *       SPSR.SPRF / SPSR.SPTEF have been cleared via SPSRC.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra_spi_read
 * @see ra_spi_write_read
 */
[[nodiscard]] ra_err_t
ra_spi_write(uint8_t channel, const void* tx, uint32_t len, ra_spi_bit_width_t bit_width);

/**
 * @brief Multi-frame RX-only polling transfer.
 *
 * @details
 * Mirrors FSP ``R_SPI_B_Read`` (delegates to
 * ``r_spi_b_write_read_common`` with ``p_src = NULL``). For each of
 * ``len`` units the driver writes a dummy ``0xFF`` / ``0xFFFF`` /
 * ``0xFFFFFFFF`` to SPDR (matching common SD-card / SPI-flash idle
 * patterns) and then captures one unit into ``rx``.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[out] rx Destination buffer (``len`` units of ``bit_width``); non-NULL.
 * @param[in] len Number of units to receive; ``0`` is a no-op success.
 * @param[in] bit_width Per-frame width: 8, 16, or 32 bits.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer completed.
 * @retval k_ra_err_null_ptr ``rx`` is NULL with ``len > 0``.
 * @retval k_ra_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra_err_hw_timeout Polling SPSR flag never asserted.
 *
 * @pre Channel previously initialised via ``ra_spi_init``.
 * @pre Caller-supplied buffer alignment matches ``bit_width``.
 * @post On success, ``rx[0..len-1]`` contains the bytes / words shifted
 *       in on CIPO during ``len`` dummy clock cycles.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra_spi_write
 * @see ra_spi_write_read
 */
[[nodiscard]] ra_err_t
ra_spi_read(uint8_t channel, void* rx, uint32_t len, ra_spi_bit_width_t bit_width);

/**
 * @brief Multi-frame full-duplex polling transfer.
 *
 * @details
 * Mirrors FSP ``R_SPI_B_WriteRead`` (delegates to
 * ``r_spi_b_write_read_common`` with both buffers non-NULL). Per
 * unit: wait SPTEF -> push ``tx[i]`` into SPDR -> wait SPRF ->
 * read SPDR into ``rx[i]``. ``tx`` and ``rx`` may be the same
 * buffer (in-place exchange).
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] tx Source buffer (``len`` units of ``bit_width``); non-NULL.
 * @param[out] rx Destination buffer (``len`` units of ``bit_width``); non-NULL.
 * @param[in] len Number of units to exchange; ``0`` is a no-op success.
 * @param[in] bit_width Per-frame width: 8, 16, or 32 bits.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer completed.
 * @retval k_ra_err_null_ptr ``tx`` or ``rx`` is NULL with ``len > 0``.
 * @retval k_ra_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra_err_hw_timeout Polling SPSR flag never asserted.
 *
 * @pre Channel previously initialised via ``ra_spi_init``.
 * @post On success, every unit has been exchanged in both directions.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra_spi_write
 * @see ra_spi_read
 */
[[nodiscard]] ra_err_t ra_spi_write_read(uint8_t            channel,
                                         const void*        tx,
                                         void*              rx,
                                         uint32_t           len,
                                         ra_spi_bit_width_t bit_width);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the SPI clock without tearing down the channel.
 *
 * @param[in] channel SPI channel.
 * @param[in] baud_hz Target bit-rate in Hz.
 * @param[in] pclka_hz Current PCLKA frequency.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclka_hz);

/* =============================================================================
 * Error status
 * =============================================================================
 */

/**
 * @brief Read the SPSR error bits (OVRF, MODERF, PERF, UDRF).
 *
 * @param[in] channel SPI channel.
 * @param[out] out_mask OR of ``k_ra_spi_err_*``.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear the SPSR error flags.
 * @param[in] channel SPI channel.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_clear_errors(uint8_t channel);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a transfer-complete callback for a channel.
 *
 * @param[in] channel SPI channel.
 * @param[in] fn Callback fired on transfer end / error.
 * @param[in] ctx Context passed to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_spi_attach_transfer_handler(uint8_t channel, ra_spi_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put the channel into MSTP-gated stop state.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop state.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_exit_stop(uint8_t channel);

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @brief Kick off a DMA-backed TX transfer on an SPI channel.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from
 * ``data[]`` into the channel's SPDR register. The SPI block must
 * be configured for 8-bit frames via the cfg passed to
 * ``ra_spi_init``; wider-frame DMA streaming is a future wave.
 *
 * @param[in] channel SPI channel.
 * @param[in] data Source buffer. Must outlive transfer.
 * @param[in] len Number of bytes; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer armed.
 * @retval k_ra_err_null_ptr ``data`` / ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg Channel or ``len`` invalid.
 * @retval k_ra_err_no_mem All DMAC channels in use.
 * @retval k_ra_err_hw_error ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised with 8-bit frames.
 * @pre ``ra_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_write_dma(uint8_t              channel,
                                        const uint8_t*       data,
                                        uint16_t             len,
                                        ra_dma_complete_fn_t on_complete,
                                        void*                ctx,
                                        uint8_t*             out_dma_channel);

/**
 * @brief Kick off a DMA-backed RX transfer on an SPI channel.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from the
 * channel's SPDR register into ``out_buf[]``.
 *
 * @param[in] channel SPI channel.
 * @param[out] out_buf Destination buffer. Must outlive transfer.
 * @param[in] len Number of bytes; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer armed.
 * @retval k_ra_err_null_ptr ``out_buf`` / ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg Channel or ``len`` invalid.
 * @retval k_ra_err_no_mem All DMAC channels in use.
 * @retval k_ra_err_hw_error ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised with 8-bit frames.
 * @pre ``ra_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_spi_read_dma(uint8_t              channel,
                                       uint8_t*             out_buf,
                                       uint16_t             len,
                                       ra_dma_complete_fn_t on_complete,
                                       void*                ctx,
                                       uint8_t*             out_dma_channel);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch SPTI -- advance TX state.
 * @param[in] channel SPI channel.
 * @since 0.1.0
 */
void ra_spi_dispatch_spti(uint8_t channel);

/**
 * @brief Dispatch SPRI -- advance RX state.
 * @param[in] channel SPI channel.
 * @since 0.1.0
 */
void ra_spi_dispatch_spri(uint8_t channel);

/**
 * @brief Dispatch SPEI -- collect + clear errors, fire callback.
 * @param[in] channel SPI channel.
 * @since 0.1.0
 */
void ra_spi_dispatch_spei(uint8_t channel);

#ifdef __cplusplus
}
#endif
