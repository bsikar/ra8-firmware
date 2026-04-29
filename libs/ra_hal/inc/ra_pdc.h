/**
 * @file ra_pdc.h
 * @brief Parallel Data Capture (PDC) camera-bridge driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the parallel-data camera bridge that mirrors FSP
 * ``r_pdc``. PDC accepts a parallel-bus pixel stream
 * (PIXCLK + HSYNC + VSYNC + 8-bit data lines) from a CMOS sensor
 * and DMA-fills a contiguous framebuffer through its 22-stage
 * 32-bit FIFO (PCDR). Distinct from CEU which has its own Y/Cb/Cr
 * separator + bilinear scaler. Use PDC for raw-byte capture (e.g.
 * monochrome, RAW Bayer, bypassing the CEU image pipeline).
 *
 * API surface (all `[[nodiscard]]`, `@since 0.1.0`):
 *
 * - ``ra_pdc_open``           -- programme PCCR0 (clock divider,
 *                                endian, sync polarities, IRQ
 *                                enables) + VCR / HCR capture
 *                                window.
 * - ``ra_pdc_close``          -- disable capture, mask interrupts,
 *                                clear callback.
 * - ``ra_pdc_capture_start``  -- record destination buffer pointer
 *                                + assert PCCR1.PCE for a
 *                                single-shot DMA-driven capture.
 * - ``ra_pdc_capture_stop``   -- clear PCCR1.PCE.
 * - ``ra_pdc_get_status``     -- read PCSR (FBSY/FEMPF/FEF/OVRF/
 *                                UDRF/VERF/HERF).
 * - ``ra_pdc_clear_status``   -- write-1-clear PCSR sticky bits.
 * - ``ra_pdc_attach_handler`` -- install the frame-end + error
 *                                callback dispatched from the
 *                                PDC IRQ.
 *
 * ## Capture flow
 *
 * @par State Machine:
 * @startuml
 * [*] --> Closed
 * Closed   --> Idle      : ra_pdc_open()
 * Idle     --> Capturing : ra_pdc_capture_start(buf)
 * Capturing --> Idle     : PCSR.FEF (single-shot)
 * Capturing --> Idle     : ra_pdc_capture_stop()
 * Idle     --> Closed    : ra_pdc_close()
 * @enduml
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
 * @enum ra_pdc_clock_div_t
 * @brief PCCR0.PCKDIV PCKO output clock divider.
 *
 * @details
 * Mirrors FSP ``pdc_clock_division_t``. PCKO is the optional
 * camera reference clock that the MCU outputs back to the sensor;
 * the divider drives it from the PDC peripheral clock.
 */
typedef enum : uint8_t {
  k_ra_pdc_div_2  = 0U, /**< CLK / 2.  */
  k_ra_pdc_div_4  = 1U, /**< CLK / 4.  */
  k_ra_pdc_div_6  = 2U, /**< CLK / 6.  */
  k_ra_pdc_div_8  = 3U, /**< CLK / 8.  */
  k_ra_pdc_div_10 = 4U, /**< CLK / 10. */
  k_ra_pdc_div_12 = 5U, /**< CLK / 12. */
  k_ra_pdc_div_14 = 6U, /**< CLK / 14. */
  k_ra_pdc_div_16 = 7U, /**< CLK / 16. */
} ra_pdc_clock_div_t;

/**
 * @enum ra_pdc_endian_t
 * @brief PCCR0.EDS captured-data endian.
 */
typedef enum : uint8_t {
  k_ra_pdc_endian_little = 0U, /**< Little-endian byte ordering. */
  k_ra_pdc_endian_big    = 1U, /**< Big-endian byte ordering.    */
} ra_pdc_endian_t;

/**
 * @enum ra_pdc_polarity_t
 * @brief PCCR0.HPS / PCCR0.VPS sync polarity.
 */
typedef enum : uint8_t {
  k_ra_pdc_pol_active_high = 0U, /**< Active-high sync.   */
  k_ra_pdc_pol_active_low  = 1U, /**< Active-low sync.    */
} ra_pdc_polarity_t;

/**
 * @struct ra_pdc_config_t
 * @brief PDC open-time configuration.
 *
 * @details
 * Mirrors the union of FSP ``capture_cfg_t`` + ``pdc_extended_cfg_t``
 * fields the driver actually consumes. ``y_capture_start_pixel``
 * + ``y_capture_pixels`` and ``x_capture_start_byte`` +
 * ``x_capture_bytes`` programme the VCR / HCR window. Each pair
 * must satisfy ``start + size <= 4095``.
 *
 * @invariant ``x_capture_start_byte + x_capture_bytes <= 4095``
 * @invariant ``y_capture_start_pixel + y_capture_pixels <= 4095``
 */
typedef struct {
  ra_pdc_clock_div_t clock_div;             /**< PCKO frequency divider.    */
  ra_pdc_endian_t    endian;                /**< Captured-data endian.      */
  ra_pdc_polarity_t  hsync_polarity;        /**< HSYNC active level.        */
  ra_pdc_polarity_t  vsync_polarity;        /**< VSYNC active level.        */
  bool               pcko_output_enable;    /**< Enable PCKO pin output.    */
  bool               enable_frame_end_irq;  /**< Set PCCR0.FEIE.            */
  bool               enable_error_irq;      /**< Set OVIE/UDRIE/VERIE/HERIE.*/
  uint16_t           x_capture_start_byte;  /**< HCR.HST (0..4095).         */
  uint16_t           x_capture_bytes;       /**< HCR.HSZ (1..4095-HST).     */
  uint16_t           y_capture_start_pixel; /**< VCR.VST (0..4095).         */
  uint16_t           y_capture_pixels;      /**< VCR.VSZ (1..4095-VST).     */
} ra_pdc_config_t;

/**
 * @typedef ra_pdc_event_fn_t
 * @brief PDC frame-end + error callback prototype.
 *
 * @param[in] ctx         Caller context registered via
 *                        ::ra_pdc_attach_handler.
 * @param[in] status_mask PCSR snapshot at IRQ entry.
 * @param[in] buffer      Destination buffer registered via
 *                        ::ra_pdc_capture_start, or `nullptr` if
 *                        no capture was active.
 */
typedef void (*ra_pdc_event_fn_t)(void* ctx, uint32_t status_mask, uint8_t* buffer);

/**
 * @brief Open the PDC peripheral.
 *
 * @details
 * Programmes PCCR0 (clock divider, endian, sync polarities, IRQ
 * enables, optional PCKO output) and the VCR / HCR capture
 * window. PCCR1.PCE is left clear; the actual capture is armed
 * by ::ra_pdc_capture_start. PCSR W1C bits are cleared before
 * return so the first capture sees a clean status word.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok PDC opened, awaiting ::ra_pdc_capture_start.
 * @retval k_ra_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra_err_invalid_arg A field overflowed the 12-bit
 *                              VCR/HCR window or zero-sized.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Capture-window pairs satisfy `start + size <= 4095`.
 *
 * @post On success, PCCR0 holds the requested control word.
 * @post On success, PCSR sticky bits are cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_open(const ra_pdc_config_t* cfg);

/**
 * @brief Close the PDC peripheral.
 *
 * @details
 * Clears PCCR1.PCE, masks every IRQ enable in PCCR0, and
 * forgets any installed callback. The destination-buffer
 * pointer is forgotten so a subsequent ::ra_pdc_capture_start
 * is required before any capture activity can resume.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre None.
 * @post PCCR1.PCE = 0.
 * @post Internal callback / buffer pointers are NULL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_close(void);

/**
 * @brief Start a single-shot capture into ``buf``.
 *
 * @details
 * Records ``buf`` as the destination so the IRQ dispatcher can
 * forward it to the registered callback, clears PCSR sticky
 * bits, then asserts PCCR1.PCE which arms the FIFO + DMA
 * pipeline. The actual byte-by-byte fill is driven by the
 * lower-level transfer engine (DMAC/DTC); the FIFO drains
 * whenever PCDR has 32 bytes, which on FSP is hooked via the
 * data-ready interrupt.
 *
 * @param[in] buf Destination framebuffer (non-NULL).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Capture armed.
 * @retval k_ra_err_null_ptr ``buf`` was NULL.
 *
 * @pre ::ra_pdc_open has been called.
 * @pre ``buf`` points to a writable buffer large enough for the
 *      configured VCR/HCR window.
 *
 * @post On success, PCCR1.PCE = 1.
 * @post On success, PCSR sticky bits are cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_capture_start(uint8_t* buf);

/**
 * @brief Stop an in-flight capture.
 *
 * @details
 * Clears PCCR1.PCE so the FIFO state machine releases the bus
 * at the next byte boundary. Safe to call when no capture is
 * active -- the write is idempotent.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre None.
 * @post PCCR1.PCE = 0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_capture_stop(void);

/**
 * @brief Snapshot the PCSR status word.
 *
 * @param[out] out_mask Receives the live PCSR value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok ``*out_mask`` populated.
 * @retval k_ra_err_null_ptr ``out_mask`` was NULL.
 *
 * @pre ``out_mask`` is non-NULL.
 * @post Hardware state unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_get_status(uint32_t* out_mask);

/**
 * @brief Write-1-clear sticky PCSR bits.
 *
 * @details
 * Only the W1C bits (FEF, OVRF, UDRF, VERF, HERF) honour the
 * write; FBSY and FEMPF are read-only and silently ignored.
 *
 * @param[in] mask Bits to clear (same layout as PCSR).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre None.
 * @post PCSR W1C bits requested by ``mask`` are cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_clear_status(uint32_t mask);

/**
 * @brief Install the frame-end + error callback.
 *
 * @param[in] fn  Callback (NULL detaches).
 * @param[in] ctx Opaque context handed back to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Callback registered.
 *
 * @pre None.
 * @post Subsequent ::ra_pdc_dispatch calls invoke ``fn``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdc_attach_handler(ra_pdc_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch one PDC IRQ -- snapshot PCSR, fire callback,
 *        clear sticky bits.
 *
 * @details
 * Wired into the PDC interrupt vector. Reads PCSR, hands the
 * mask + buffer pointer to the registered callback, then writes
 * the W1C bits back so the next event re-arms cleanly.
 *
 * @since 0.1.0
 */
void ra_pdc_dispatch(void);

#ifdef __cplusplus
}
#endif
