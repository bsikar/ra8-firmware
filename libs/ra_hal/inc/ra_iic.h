/**
 * @file ra_iic.h
 * @brief Full-featured I2C (IIC) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 3.2 full build-out of the RA8D2 IIC peripheral. Replaces
 * the Wave 0 polling-only ``ra_iic_controller_init /
 * ra_iic_write / ra_iic_read`` stub with a driver that ticks
 * every checkbox on the 14-checkbox template.
 *
 * API surface:
 *
 *  - ``ra_iic_init(channel, cfg)``      -- full config + MSTP enable
 *  - ``ra_iic_deinit(channel)``          -- release + MSTP disable
 *  - ``ra_iic_write / read``             -- polling transfers (legacy)
 *  - ``ra_iic_set_clock(channel, hz)``   -- runtime bit-rate change
 *  - ``ra_iic_get_errors / clear_errors``-- AL/NACK/TMO status
 *  - ``ra_iic_attach_transfer_handler``  -- interrupt-mode callback
 *  - ``ra_iic_enter_stop / exit_stop``   -- power transition
 *  - ``ra_iic_dispatch_txi / rxi / eri`` -- ISR entry points
 *
 * The legacy ``ra_iic_controller_init / write / read`` functions
 * from Wave 0 are kept so callers that already use them keep
 * compiling; the new descriptor-based ``ra_iic_init`` is the
 * preferred entry point.
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
 * @enum ra_iic_speed_t
 * @brief Common I2C bus speeds.
 */
typedef enum : uint32_t {
  k_ra_iic_speed_standard  = 100000U,  /**< 100 kHz standard mode. */
  k_ra_iic_speed_fast      = 400000U,  /**< 400 kHz fast mode.      */
  k_ra_iic_speed_fast_plus = 1000000U, /**< 1 MHz fast-plus.       */
} ra_iic_speed_t;

/**
 * @struct ra_iic_cfg_t
 * @brief Configuration descriptor for ``ra_iic_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_iic_init`` in
 * ``libs/ra_hal/src/iic.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t bus_hz;   /**< Target I2C clock rate (100/400/1000 kHz). */
  uint32_t pclkb_hz; /**< Current PCLKB frequency for ICBR calc.    */
} ra_iic_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_iic_err_mask_t
 * @brief Bit mask of IIC error flags.
 */
typedef enum : uint8_t {
  k_ra_iic_err_none     = 0x00U,
  k_ra_iic_err_arb_lost = 0x01U, /**< ICSR2.AL set.     */
  k_ra_iic_err_nack     = 0x02U, /**< ICSR2.NACKF set.  */
  k_ra_iic_err_timeout  = 0x04U, /**< ICSR2.TMOF set.   */
} ra_iic_err_mask_t;

/**
 * @typedef ra_iic_complete_fn_t
 * @brief Transfer-complete callback signature.
 *
 * @param[in] ctx      Caller-supplied context.
 * @param[in] err_mask OR of ``k_ra_iic_err_*`` bits; zero on success.
 */
typedef void (*ra_iic_complete_fn_t)(void* ctx, uint8_t err_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an IIC channel with a full config descriptor.
 *
 * @param[in] channel IIC channel (0..2).
 * @param[in] cfg     Configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, ICE is set and the channel is ready to
 *       write / read.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg);

/**
 * @brief Tear down an IIC channel.
 * @param[in] channel IIC channel number.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 * @post ICE is cleared; MSTP reference released.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API (kept for backward compat)
 * =============================================================================
 */

/**
 * @brief Legacy init: calls ra_iic_init with a placeholder config.
 * @param[in] channel IIC channel number (0..2).
 * @return `ra_err_t` error code.
 *
 * @note Pin routing + MSTP gating must be done before calling this.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_controller_init(uint8_t channel);

/**
 * @brief Blocking write of `len` bytes to a 7-bit target.
 *
 * @param[in] channel    IIC channel number.
 * @param[in] target_7b  7-bit slave address.
 * @param[in] data       Bytes to send.
 * @param[in] len        Number of bytes.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len);

/**
 * @brief Blocking read of `len` bytes from a 7-bit target.
 *
 * @param[in] channel    IIC channel number.
 * @param[in] target_7b  7-bit slave address.
 * @param[out] out       Receive buffer.
 * @param[in] len        Number of bytes to read.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the I2C bus clock without tearing down the channel.
 *
 * @param[in] channel  IIC channel.
 * @param[in] bus_hz   New bus clock in Hz.
 * @param[in] pclkb_hz Current PCLKB frequency.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel previously initialised.
 * @post ICBRL / ICBRH reflect the new divider.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz);

/* =============================================================================
 * Error status
 * =============================================================================
 */

/**
 * @brief Read the ICSR2 error bits (AL, NACKF, TMOF).
 * @param[in]  channel  IIC channel.
 * @param[out] out_mask Bit OR of ``k_ra_iic_err_*``.
 * @return ``k_ra_ok`` / ``k_ra_err_null_ptr`` / ``k_ra_err_invalid_arg``.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear the ICSR2 error flags.
 * @param[in] channel IIC channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_clear_errors(uint8_t channel);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a transfer-complete callback for a channel.
 *
 * @param[in] channel IIC channel.
 * @param[in] fn      Callback fired on transfer end / error.
 *                     NULL to detach.
 * @param[in] ctx     Context passed to the callback.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel previously initialised.
 * @post ICIER bits reflect the attach state.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t
ra_iic_attach_transfer_handler(uint8_t channel, ra_iic_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put the channel into MSTP-gated stop state.
 * @param[in] channel IIC channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop state.
 * @param[in] channel IIC channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iic_exit_stop(uint8_t channel);

/* =============================================================================
 * DMA TX / RX (Wave 3.7b)
 * =============================================================================
 */

/**
 * @brief Kick off a DMA-backed TX transfer on an IIC channel.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from
 * ``data[]`` into the channel's ICDRT register as byte elements.
 * On completion, ``on_complete`` fires from DMAC ISR context.
 *
 * @param[in]  channel         IIC channel 0..2.
 * @param[in]  data            Source buffer. Must outlive the transfer.
 * @param[in]  len             Number of bytes; non-zero.
 * @param[in]  on_complete     Completion callback. May be NULL.
 * @param[in]  ctx             Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Transfer armed.
 * @retval k_ra_err_null_ptr       ``data`` / ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg    Channel or ``len`` invalid.
 * @retval k_ra_err_no_mem         All DMAC channels in use.
 * @retval k_ra_err_hw_error       ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised via ``ra_iic_init``.
 * @pre ``ra_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_iic_read_dma
 * @since 0.3.0
 */
[[nodiscard]] ra_err_t ra_iic_write_dma(uint8_t              channel,
                                        const uint8_t*       data,
                                        uint16_t             len,
                                        ra_dma_complete_fn_t on_complete,
                                        void*                ctx,
                                        uint8_t*             out_dma_channel);

/**
 * @brief Kick off a DMA-backed RX transfer on an IIC channel.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from the
 * channel's ICDRR register into ``out_buf[]`` as byte elements.
 *
 * @param[in]  channel         IIC channel 0..2.
 * @param[out] out_buf         Destination buffer. Must outlive transfer.
 * @param[in]  len             Number of bytes; non-zero.
 * @param[in]  on_complete     Completion callback. May be NULL.
 * @param[in]  ctx             Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Transfer armed.
 * @retval k_ra_err_null_ptr       ``out_buf`` / ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg    Channel or ``len`` invalid.
 * @retval k_ra_err_no_mem         All DMAC channels in use.
 * @retval k_ra_err_hw_error       ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised via ``ra_iic_init``.
 * @pre ``ra_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_iic_write_dma
 * @since 0.3.0
 */
[[nodiscard]] ra_err_t ra_iic_read_dma(uint8_t              channel,
                                       uint8_t*             out_buf,
                                       uint16_t             len,
                                       ra_dma_complete_fn_t on_complete,
                                       void*                ctx,
                                       uint8_t*             out_dma_channel);

/* =============================================================================
 * ISR dispatch (test-callable)
 * =============================================================================
 */

/**
 * @brief Dispatch TXI -- advance TX transfer state machine.
 * @param[in] channel IIC channel.
 * @since 0.2.0
 */
void ra_iic_dispatch_txi(uint8_t channel);

/**
 * @brief Dispatch RXI -- advance RX transfer state machine.
 * @param[in] channel IIC channel.
 * @since 0.2.0
 */
void ra_iic_dispatch_rxi(uint8_t channel);

/**
 * @brief Dispatch ERI -- collect + clear error flags, fire callback.
 * @param[in] channel IIC channel.
 * @since 0.2.0
 */
void ra_iic_dispatch_eri(uint8_t channel);

#ifdef __cplusplus
}
#endif
