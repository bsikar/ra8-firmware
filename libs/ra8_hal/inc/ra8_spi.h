/**
 * @file ra8_spi.h
 * @brief SPI_B controller driver (RA8D2 Type-B SPI peripheral)
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for the SPI_B controller driver. Implementation lives in
 * ``libs/ra8_hal/src/ra8_spi_b.c`` and mirrors FSP ``r_spi_b`` in
 * polling-mode controller flow. The earlier register layout (legacy
 * 8/16-bit SPI block) was replaced wholesale on RA8D2 -- see
 * ``ra8_spi_regs.h`` for the SPI_B register file.
 *
 * API surface:
 *
 * - ``ra8_spi_init(channel, cfg)`` -- full config + MSTP enable
 * - ``ra8_spi_deinit(channel)`` -- SPE clear + MSTP release
 * - ``ra8_spi_controller_init`` -- defaults init (mode 0, PCLKA = 125 MHz)
 * - ``ra8_spi_xfer8`` -- single-byte full-duplex polling xfer
 * - ``ra8_spi_set_clock`` -- runtime SPBR change
 * - ``ra8_spi_get_errors / clear_errors``-- overrun/mode/parity/underrun
 * - ``ra8_spi_attach_transfer_handler`` -- IRQ callback
 * - ``ra8_spi_enter_stop / exit_stop`` -- power transition
 * - ``ra8_spi_dispatch_spti / spri / spei`` -- ISR entry points
 *
 * @note On RA8D2 the SPI_B block is the *only* SPI peripheral instance:
 *       there is no separate legacy ``ra8_spi`` driver to demonstrate. This
 *       header is the public API and ``ra8_spi_b.c`` is its implementation;
 *       ``test_ra8_spi.c`` and ``test_ra8_spi_b.c`` both exercise it. The
 *       on-silicon example/HIL is ``examples/.../hil/spi_loopback`` (it
 *       includes this header, runs the internal-loopback path, and prints
 *       ``spi: pass``). A separate ``spi_b_loopback`` example would link the
 *       same ``.c`` and duplicate ``spi_loopback`` verbatim, so none exists.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_spi_mode_t
 * @brief CPOL / CPHA combinations.
 */
typedef enum : uint8_t {
  k_ra8_spi_mode_0 = 0U, /**< CPOL=0, CPHA=0. */
  k_ra8_spi_mode_1 = 1U, /**< CPOL=0, CPHA=1. */
  k_ra8_spi_mode_2 = 2U, /**< CPOL=1, CPHA=0. */
  k_ra8_spi_mode_3 = 3U, /**< CPOL=1, CPHA=1. */
} ra8_spi_mode_t;

/**
 * @enum ra8_spi_bit_width_t
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
  k_ra8_spi_width_8  = 7U,  /**< 8-bit frame -> SPCMDn.SPB = 0b00111.  */
  k_ra8_spi_width_16 = 15U, /**< 16-bit frame -> SPCMDn.SPB = 0b01111. */
  k_ra8_spi_width_32 = 31U, /**< 32-bit frame -> SPCMDn.SPB = 0b11111. */
} ra8_spi_bit_width_t;

/**
 * @struct ra8_spi_cfg_t
 * @brief Configuration descriptor for ``ra8_spi_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_spi_init`` in
 * ``libs/ra8_hal/src/ra8_spi_b.c``.
 */
typedef struct {
  uint32_t       baud_hz;   /**< Target SPI clock in Hz.                          */
  uint32_t       pclka_hz;  /**< Current PCLKA in Hz (for SPBR calc).             */
  ra8_spi_mode_t mode;      /**< CPOL / CPHA combination.                         */
  bool           lsb_first; /**< True for LSB-first transfers.                    */
  bool           loopback;  /**< SPCR2.SPLP=1: internal COPI->CIPO tie (no pins). */
} ra8_spi_cfg_t;

/**
 * @enum ra8_spi_err_mask_t
 * @brief Bit mask of SPI error flags.
 */
typedef enum : uint8_t {
  k_ra8_spi_err_none     = 0x00U, /**< RA8 SPI error none. */
  k_ra8_spi_err_overrun  = 0x01U, /**< SPSR.OVRF set.      */
  k_ra8_spi_err_mode     = 0x02U, /**< SPSR.MODERF set.    */
  k_ra8_spi_err_parity   = 0x04U, /**< SPSR.PERF set.      */
  k_ra8_spi_err_underrun = 0x08U, /**< SPSR.UDRF set.      */
} ra8_spi_err_mask_t;

/**
 * @typedef ra8_spi_complete_fn_t
 * @brief Transfer-complete callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] err_mask OR of ``k_ra8_spi_err_*`` bits; zero on success.
 */
typedef void (*ra8_spi_complete_fn_t)(void* ctx, uint8_t err_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an SPI channel with a full config descriptor.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] cfg Configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success, SPE is set and the channel is ready to xfer.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_init(uint8_t channel, const ra8_spi_cfg_t* cfg);

/**
 * @brief Tear down a channel.
 * @param[in] channel SPI channel.
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 * @post SPE is cleared; MSTP reference released.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Legacy init (1.9 MHz at PCLKA = 125 MHz, mode 0).
 * @param[in] channel SPI channel (0 or 1).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_controller_init(uint8_t channel);

/**
 * @brief Full-duplex 8-bit exchange.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] tx Byte to transmit.
 * @param[out] rx Pointer to receive the shifted-in byte (may be NULL).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx);

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
 * ``k_ra8_timeout_default_ms``.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] tx Source buffer (``len`` units of ``bit_width``); must be non-NULL.
 * @param[in] len Number of units to transfer; ``0`` is a no-op success.
 * @param[in] bit_width Per-frame width: 8, 16, or 32 bits.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer completed.
 * @retval k_ra8_err_null_ptr ``tx`` is NULL with ``len > 0``.
 * @retval k_ra8_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra8_err_hw_timeout SPSR.SPTEF / SPSR.SPRF never asserted.
 *
 * @pre Channel previously initialized via ``ra8_spi_init``.
 * @pre Caller-supplied buffer alignment matches ``bit_width`` (16 / 32 bit
 *      access requires properly aligned pointers).
 * @post On success, every requested unit has shifted out on COPI and
 *       SPSR.SPRF / SPSR.SPTEF have been cleared via SPSRC.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra8_spi_read
 * @see ra8_spi_write_read
 */
[[nodiscard]] ra8_err_t
ra8_spi_write(uint8_t channel, const void* tx, uint32_t len, ra8_spi_bit_width_t bit_width);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer completed.
 * @retval k_ra8_err_null_ptr ``rx`` is NULL with ``len > 0``.
 * @retval k_ra8_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra8_err_hw_timeout Polling SPSR flag never asserted.
 *
 * @pre Channel previously initialized via ``ra8_spi_init``.
 * @pre Caller-supplied buffer alignment matches ``bit_width``.
 * @post On success, ``rx[0..len-1]`` contains the bytes / words shifted
 *       in on CIPO during ``len`` dummy clock cycles.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra8_spi_write
 * @see ra8_spi_write_read
 */
[[nodiscard]] ra8_err_t
ra8_spi_read(uint8_t channel, void* rx, uint32_t len, ra8_spi_bit_width_t bit_width);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer completed.
 * @retval k_ra8_err_null_ptr ``tx`` or ``rx`` is NULL with ``len > 0``.
 * @retval k_ra8_err_invalid_arg Channel or ``bit_width`` invalid.
 * @retval k_ra8_err_hw_timeout Polling SPSR flag never asserted.
 *
 * @pre Channel previously initialized via ``ra8_spi_init``.
 * @post On success, every unit has been exchanged in both directions.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @see ra8_spi_write
 * @see ra8_spi_read
 */
[[nodiscard]] ra8_err_t ra8_spi_write_read(uint8_t             channel,
                                           const void*         tx,
                                           void*               rx,
                                           uint32_t            len,
                                           ra8_spi_bit_width_t bit_width);

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
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclka_hz);

/* =============================================================================
 * Error status
 * =============================================================================
 */

/**
 * @brief Read the SPSR error bits (OVRF, MODERF, PERF, UDRF).
 *
 * @param[in] channel SPI channel.
 * @param[out] out_mask OR of ``k_ra8_spi_err_*``.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear the SPSR error flags.
 * @param[in] channel SPI channel.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_clear_errors(uint8_t channel);

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
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_spi_attach_transfer_handler(uint8_t channel, ra8_spi_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put the channel into MSTP-gated stop state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_exit_stop(uint8_t channel);

/* =============================================================================
 * Target (peripheral) mode -- SPI_B as an SPI peripheral
 * =============================================================================
 */

/**
 * @brief Initialise an SPI_B channel in target (peripheral) mode.
 *
 * @details
 * Configures the SPI_B peripheral to act as a target device: the external
 * controller drives RSPCK and asserts SSL (CS); the MCU responds on CIPO
 * and receives on COPI. Implementation lives in
 * ``libs/ra8_hal/src/ra8_spi_b_target.c``.
 *
 * Register-level effect (HUM Ch 43.2.4 "SPCR" p 2884):
 *  - SPCR is written with MSTR=0 (peripheral/target), MODFEN=1, SPE=1.
 *  - SCKASE is NOT set -- it is valid only in controller mode.
 *  - SPBR (SPCR3) is set to zero because the clock is external.
 *  - SPCMD0 is programmed from ``cfg`` (CPHA, CPOL, LSBF) for 8-bit frames.
 *
 * The ``baud_hz`` and ``pclka_hz`` fields of ``cfg`` are accepted but
 * ignored -- the bit-rate is determined by the external controller.
 *
 * @param[in] channel SPI channel (0 or 1).
 * @param[in] cfg     Configuration descriptor (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel configured in target mode; awaiting
 *                              external controller frames.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` is out of range.
 * @retval k_ra8_err_hw_error    ``ra8_mstp_enable`` failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success, SPCR.SPE=1 and SPCR.MSTR=0 (target mode is active).
 * @post SPDCR, SPDCR2, SPCR2, and SPCR3 are cleared; SPSR flags are clear.
 *
 * @note Thread safety: not thread-safe.
 * @note To deinitialise a channel opened in target mode, call
 *       ``ra8_spi_deinit(channel)`` from ``ra8_spi.h``.
 *
 * @see ra8_spi_b_target_xfer  Exchange one byte after init.
 * @see ra8_spi_init            Initialise the same channel in controller mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_b_target_init(uint8_t channel, const ra8_spi_cfg_t* cfg);

/**
 * @brief Exchange one byte as the SPI target in a polled blocking call.
 *
 * @details
 * Performs a full-duplex single-byte exchange in target (peripheral) role:
 *
 *  1. Waits for SPSR.SPTEF (TX buffer empty) -- confirms it is safe to
 *     pre-load ``tx`` without overwriting a pending outgoing byte.
 *  2. Writes ``tx`` to SPDR (CIPO data -- what this peripheral sends to
 *     the external controller on the next controller-initiated frame).
 *  3. Waits for SPSR.SPRF (RX buffer full) -- asserted when the external
 *     controller has completed clocking one SPI frame.
 *  4. Reads SPDR into ``*rx`` (the COPI byte received from the controller).
 *
 * Under ``RA8_OFF_TARGET`` the SPSR wait reduces to a single-shot
 * register read: pre-seed the register as shown below.
 *
 * @par Example:
 * @code
 * // Off-target (or pre-staged hardware test):
 * ra8_spi(0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
 * uint8_t rx = 0U;
 * ra8_spi_b_target_xfer(0, 0xA5U, &rx);
 * @endcode
 *
 * @param[in]  channel SPI channel (0 or 1).
 * @param[in]  tx      Byte to send on CIPO (Controller In Peripheral Out).
 * @param[out] rx      Pointer to receive the COPI byte; may be NULL if the
 *                     received byte is not needed.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Frame exchange completed.
 * @retval k_ra8_err_null_ptr   ``channel`` is out of range (``ra8_spi()`` returned NULL).
 * @retval k_ra8_err_hw_timeout SPSR.SPTEF or SPSR.SPRF never asserted within
 *                             ``k_spi_b_target_poll_limit`` iterations.
 *
 * @pre Channel previously initialised via ``ra8_spi_b_target_init``.
 * @pre SPCR.SPE is set.
 * @post On ``k_ra8_ok``, ``tx`` was loaded into SPDR and (if ``rx != NULL``)
 *       ``*rx`` holds the byte received on COPI from the controller.
 * @post SPSR.SPTEF and SPSR.SPRF have been cleared via SPSRC.
 *
 * @note Thread safety: not thread-safe.
 * @note The SPI_B SPDR register backs TX and RX through separate FIFO
 *       queues on hardware; they share a single address in the fake's
 *       plain-RAM model, so the fake echoes ``tx`` back as ``*rx``.
 *
 * @see ra8_spi_b_target_init  Initialise the channel first.
 * @see ra8_spi_xfer8           Controller-mode equivalent.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_b_target_xfer(uint8_t channel, uint8_t tx, uint8_t* rx);

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @brief Kick off a DMA-backed TX transfer on an SPI channel.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``len`` bytes from
 * ``data[]`` into the channel's SPDR register. The SPI block must
 * be configured for 8-bit frames via the cfg passed to
 * ``ra8_spi_init``; wider-frame DMA streaming is a future wave.
 *
 * @param[in] channel SPI channel.
 * @param[in] data Source buffer. Must outlive transfer.
 * @param[in] len Number of bytes; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``data`` / ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg Channel or ``len`` invalid.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized with 8-bit frames.
 * @pre ``ra8_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_write_dma(uint8_t               channel,
                                          const uint8_t*        data,
                                          uint16_t              len,
                                          ra8_dma_complete_fn_t on_complete,
                                          void*                 ctx,
                                          uint8_t*              out_dma_channel);

/**
 * @brief Kick off a DMA-backed RX transfer on an SPI channel.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``len`` bytes from the
 * channel's SPDR register into ``out_buf[]``.
 *
 * @param[in] channel SPI channel.
 * @param[out] out_buf Destination buffer. Must outlive transfer.
 * @param[in] len Number of bytes; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``out_buf`` / ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg Channel or ``len`` invalid.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized with 8-bit frames.
 * @pre ``ra8_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_spi_read_dma(uint8_t               channel,
                                         uint8_t*              out_buf,
                                         uint16_t              len,
                                         ra8_dma_complete_fn_t on_complete,
                                         void*                 ctx,
                                         uint8_t*              out_dma_channel);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch SPTI -- advance TX state.
 *
 * @details
 * Invoked from the SPTIn IRQ trampoline when the transmit FIFO/holding
 * register is ready for the next word. Loads the next byte from the
 * caller-supplied TX buffer into ``SPDR`` (HUM Ch 36.2.7 "SPDR : SPI
 * Data Register", p 1532) and decrements the remaining count. Silently
 * returns if the channel is out of range or no transfer is in progress.
 *
 * @param[in] channel SPI channel index (0..1).
 *
 * @pre ``ra8_spi_init(channel, ...)`` succeeded.
 * @pre Called from ISR context (or unit-test driver).
 * @post Either one TX word was written or the transfer is complete.
 * @post No state change if channel is out of range.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
void ra8_spi_dispatch_spti(uint8_t channel);

/**
 * @brief Dispatch SPRI -- advance RX state.
 *
 * @details
 * Invoked from the SPRIn IRQ trampoline when the receive FIFO/holding
 * register has new data. Reads ``SPDR`` (HUM Ch 36.2.7, p 1532) into
 * the caller-supplied RX buffer and, when the byte count reaches zero,
 * invokes the registered completion callback.
 *
 * @param[in] channel SPI channel index (0..1).
 *
 * @pre ``ra8_spi_init(channel, ...)`` succeeded.
 * @pre Called from ISR context (or unit-test driver).
 * @post Either one RX word was consumed or the completion callback ran.
 * @post No state change if channel is out of range.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
void ra8_spi_dispatch_spri(uint8_t channel);

/**
 * @brief Dispatch SPEI -- collect + clear errors, fire callback.
 *
 * @details
 * Invoked from the SPEIn IRQ trampoline. Reads and clears the error
 * status bits in ``SPSR`` (HUM Ch 36.2.5 "SPSR : SPI Status Register",
 * p 1530) -- mode-fault (MODF), overrun (OVRF), parity (PERF) -- then
 * invokes the registered error callback with the collected mask.
 *
 * @param[in] channel SPI channel index (0..1).
 *
 * @pre ``ra8_spi_init(channel, ...)`` succeeded.
 * @pre Called from ISR context (or unit-test driver).
 * @post All error bits in ``SPSR`` are cleared for ``channel``.
 * @post Error callback invoked exactly once with the captured mask.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
void ra8_spi_dispatch_spei(uint8_t channel);

#ifdef __cplusplus
}
#endif
