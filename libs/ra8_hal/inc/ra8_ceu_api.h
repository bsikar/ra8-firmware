/**
 * @file ra8_ceu_api.h
 * @brief Capture Engine Unit (CEU) driver function prototypes
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public function prototypes for the RA8D2 Capture Engine Unit (CEU)
 * HAL driver: lifecycle (init / deinit / reset), status + IRQ
 * dispatch, power transitions, capture arm/disarm, the live
 * plane / firewall / byte-swap reconfiguration setters, and the
 * DMA-coupling helpers. The configuration descriptors and types
 * these prototypes consume live in `ra8_ceu_types.h`; both are
 * aggregated by the thin umbrella `ra8_ceu.h`, which also documents
 * the full driver overview and state machine.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_ceu_types.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the CEU and write the full configuration.
 *
 * @details
 * Steps performed:
 *  1. `ra8_mstp_enable(k_ra8_mstp_ceu)` to release MSTPC16.
 *  2. Wait for any in-flight software reset (CSTSR.CPTON==0,
 *     CAPSR.CPKIL==0) with a bounded spin.
 *  3. Program CFLCR (scale-down), CAIFR (interlace), CAPCR
 *     (continuous + burst + frame-drop), CAMCR (sync polarities,
 *     latch edges, capture format, input order, FLDPOL, DTIF),
 *     CMCYR (image dimensions), CAMOR (start offsets), CAPWR
 *     (capture cycles), CFSZR (filter clip), CDWDR (destination
 *     stride), CLFCR (LPF enable), CDOCR (output format, byte
 *     swap, bundle-write enable), CFWCR (firewall off; armed in
 *     capture_start).
 *  4. Clear CETCR and write CEIER from `cfg->interrupts`.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Capture engine powered + configured.
 * @retval k_ra8_err_null_ptr      `cfg` was NULL.
 * @retval k_ra8_err_invalid_arg   `cfg->capture_mode` is continuous
 *                                in a non-image-capture format.
 * @retval k_ra8_err_hw_timeout    MSTP enable failed or the
 *                                reset-clear spin overran.
 *
 * @pre `cfg` is non-NULL.
 * @pre `ra8_mstp_init` has been called.
 * @post CEU module-stop bit is cleared.
 * @post CEU is in idle (CSTSR.CPTON == 0) and ready for
 *       `ra8_ceu_capture_start`.
 *
 * @note Not thread-safe -- single-threaded init context only.
 *
 * @see ra8_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_init(const ra8_ceu_config_t* cfg);

/**
 * @brief Stop the CEU and drop its module-stop bit.
 *
 * @details
 * Issues CAPSR.CPKIL=1 to abort any active capture, masks every
 * interrupt source by writing 0 to CEIER, clears the registered
 * callback, and finally calls `ra8_mstp_disable(k_ra8_mstp_ceu)`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok             Driver torn down, MSTP released.
 * @retval k_ra8_err_hw_timeout MSTP disable read-back failed.
 *
 * @pre `ra8_ceu_init` was previously called successfully.
 * @pre Caller does not race a CEU IRQ on another core.
 * @post CEU is fully powered down (MSTPC16 set).
 * @post Registered callback is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_deinit(void);

/**
 * @brief Issue a software reset of the capture state machine.
 *
 * @details
 * Sets CAPSR.CPKIL and spins on CSTSR.CPTON / CAPSR.CPKIL until
 * both clear or the budget expires. Used to recover from a stuck
 * capture (e.g. after a sensor stopped emitting VD edges).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok             Reset completed cleanly.
 * @retval k_ra8_err_hw_timeout Status bits did not clear in budget.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre Caller has the CEU IRQ masked or is in CEU IRQ context.
 * @post CSTSR.CPTON == 0.
 * @post CAPSR.CPKIL == 0.
 *
 * @note Bounded spin of `k_ra8_ceu_reset_spin` iterations.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_reset(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Snapshot the CEU event-status register (CETCR).
 *
 * @param[out] out_mask Receives the raw CETCR value.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok           Snapshot taken.
 * @retval k_ra8_err_null_ptr `out_mask` was NULL.
 *
 * @pre `out_mask` is non-NULL.
 * @pre `ra8_ceu_init` previously called (CEU MSTP cleared).
 * @post `*out_mask` reflects the value of CETCR at call time.
 * @post No status flag is cleared.
 *
 * @note Thread-safe with respect to its own register read.
 *
 * @see ra8_ceu_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_get_status(uint32_t* out_mask);

/**
 * @brief Clear the CETCR event flags identified by `mask`.
 *
 * @details
 * CETCR is "write-0-to-clear" per HUM Ch 60.2.22 -- writing the
 * complement of `mask` clears exactly those bits. The driver does
 * a read-modify-write so other in-flight flags survive.
 *
 * @param[in] mask Bitmask of `k_ra8_ceu_evt_*` values to clear.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always; mask of zero is a no-op.
 *
 * @pre CEU is powered (MSTPC16 cleared).
 * @pre `mask` only contains bits defined in `ra8_ceu_cetcr_mask_t`.
 * @post Bits in `mask` are deasserted in CETCR.
 * @post Bits not in `mask` retain their previous state.
 *
 * @note Not thread-safe.
 *
 * @see ra8_ceu_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_clear_status(uint32_t mask);

/**
 * @brief Snapshot CSTSR + CDSSR + the active plane into one struct.
 *
 * @param[out] out Non-NULL status struct to populate.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok           Snapshot taken.
 * @retval k_ra8_err_null_ptr `out` was NULL.
 *
 * @pre `out` is non-NULL.
 * @pre `ra8_ceu_init` previously called.
 * @post `out->capturing` mirrors CSTSR.CPTON.
 * @post `out->data_size` mirrors CDSSR.
 *
 * @note Thread-safe (read-only).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_status_snapshot(ra8_ceu_status_t* out);

/**
 * @brief Return the byte count written by the last data-enable capture.
 *
 * @details
 * Reads CDSSR per HUM Ch 60.2.24 p 3674. Only meaningful after a
 * data-enable-fetch capture completes; in image / data-sync modes
 * this register stays at zero.
 *
 * @param[out] out_bytes Non-NULL byte-count receiver.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok           Snapshot taken.
 * @retval k_ra8_err_null_ptr `out_bytes` was NULL.
 *
 * @pre `out_bytes` is non-NULL.
 * @pre `ra8_ceu_init` previously called.
 * @post `*out_bytes` == CDSSR.
 *
 * @note Thread-safe (read-only).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_data_size_get(uint32_t* out_bytes);

/**
 * @brief Update the CEIER interrupt-enable bitmask.
 *
 * @param[in] mask Bitwise-OR of `k_ra8_ceu_evt_*` values.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre `mask` only contains bits defined in `ra8_ceu_cetcr_mask_t`.
 * @post CEIER == `mask`.
 * @post Future `ra8_ceu_dispatch` calls gate the callback against
 *       the new mask.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_interrupts_set(uint32_t mask);

/**
 * @brief Register a callback invoked by `ra8_ceu_dispatch`.
 *
 * @param[in] fn  Callback function, or NULL to clear.
 * @param[in] ctx Opaque context forwarded as the first callback arg.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed (or cleared).
 *
 * @pre Caller is in single-threaded init or has IRQs masked.
 * @pre `fn` is either NULL or points to a callable function.
 * @post Subsequent `ra8_ceu_dispatch` calls invoke `fn(ctx, mask)`.
 * @post Previously-registered callback is overwritten.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_attach_handler(ra8_ceu_event_fn_t fn, void* ctx);

/**
 * @brief Drain CEU interrupt events and fire the registered callback.
 *
 * @details
 * Intended to be called from the CEU ISR (or from a soft-IRQ
 * context fed by the ISR). Snapshots CETCR, clears the observed
 * bits via write-0, and invokes the registered callback with the
 * masked event set. The mask passed to the callback is gated by
 * the currently-enabled CEIER bits.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre Caller is the CEU IRQ context (or has IRQs masked).
 * @post All bits that were observed in CETCR are cleared.
 * @post Registered callback (if any) was invoked exactly once.
 *
 * @note Returns silently if no callback is registered.
 * @since 0.1.0
 *
 */
void ra8_ceu_dispatch(void);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Stop active captures and drop the CEU module-stop bit.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok             CEU stopped + MSTP released.
 * @retval k_ra8_err_hw_timeout MSTP disable read-back failed.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre Caller does not race the CEU ISR.
 * @post CEU is in MSTP-gated stop.
 * @post No further CEU IRQs will fire until `ra8_ceu_exit_stop`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_enter_stop(void);

/**
 * @brief Exit the MSTP-gated stop entered by `ra8_ceu_enter_stop`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok             MSTP re-enabled, CEU window accessible.
 * @retval k_ra8_err_hw_timeout MSTP enable read-back failed.
 *
 * @pre `ra8_ceu_enter_stop` was previously called.
 * @pre Caller does not race the CEU ISR.
 * @post CEU window is accessible (MSTPC16 cleared).
 * @post Caller must reprogram any registers that lost state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_exit_stop(void);

/* =============================================================================
 * Capture operation
 * =============================================================================
 */

/**
 * @brief Arm a capture into the supplied buffer.
 *
 * @details
 * Writes the buffer base into CDAYR (and CDACR for image-capture
 * mode), clears any pending CETCR flags, and finally sets
 * CAPSR.CE=1. Capture begins on the next VD edge. The CETCR.CPE
 * flag fires when the buffer is filled (single-shot) or at every
 * frame boundary (continuous).
 *
 * In data-enable-fetch mode the firewall (CFWCR) is automatically
 * armed to `image_area_size - 1` from the buffer base.
 *
 * @param[in] buffer Capture target. Must be non-NULL and 8-byte aligned.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Capture armed; CE bit set.
 * @retval k_ra8_err_null_ptr      `buffer` was NULL.
 * @retval k_ra8_err_invalid_arg   `buffer` is not 8-byte aligned.
 * @retval k_ra8_err_busy          A capture is already in progress
 *                                (CSTSR.CPTON == 1) or a software
 *                                reset is in flight (CAPSR.CPKIL == 1).
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre CEU is idle (CSTSR.CPTON == 0).
 * @pre `buffer` is non-NULL and 8-byte aligned.
 * @post CDAYR == `(uintptr_t)buffer`.
 * @post CAPSR.CE == 1 (capture armed).
 *
 * @note Not thread-safe.
 *
 * @see ra8_ceu_dispatch  Listens for CETCR.CPE on capture completion.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_arm(uint8_t* buffer);

/**
 * @brief Arm a capture using a full address bundle.
 *
 * @details
 * Like `ra8_ceu_capture_start` but lets the caller specify the C
 * (chrominance) base, the bottom-field addresses (interlace), and
 * the bundle-2 pair (ping-pong). Pointers left NULL leave the
 * matching register at its previous value.
 *
 * @param[in] bufs Non-NULL buffer descriptor.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                 Capture armed; CE set.
 * @retval k_ra8_err_null_ptr       `bufs` was NULL or `bufs->y_top` NULL.
 * @retval k_ra8_err_invalid_arg    Any non-NULL pointer fails 8-byte
 *                                 alignment.
 * @retval k_ra8_err_busy           CSTSR.CPTON==1 or CAPSR.CPKIL==1.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre CEU is idle (CSTSR.CPTON == 0).
 * @pre `bufs->y_top` is non-NULL and 8-byte aligned.
 * @post CDAYR == `(uintptr_t)bufs->y_top`.
 * @post CAPSR.CE == 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_start_ex(const ra8_ceu_buffers_t* bufs);

/**
 * @brief Stop the running capture by clearing CAPSR.CE.
 *
 * @details
 * Distinct from `ra8_ceu_reset` -- this only clears the CE bit so
 * the next VD does not start a new frame. Continuous-mode captures
 * use this to break out cleanly without a CPKIL software reset.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre Caller has CEU IRQ masked or is in IRQ context.
 * @post CAPSR.CE == 0.
 * @post CSTSR.CPTON will go to 0 once the in-flight frame ends.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_disarm(void);

/* =============================================================================
 * Plane / firewall / byte-swap setters (live reconfig)
 * =============================================================================
 */

/**
 * @brief Mirror the active configuration into Plane B and arm a swap.
 *
 * @details
 * Writes the current Plane A image of every 3-plane register into
 * Plane B (offset +0x1000) and sets CRCNTR.RVS so the CEU swaps
 * planes on the next VD edge. Used to apply a new geometry / new
 * capture buffer without dropping a frame.
 *
 * @param[in] bufs Buffer bundle to install on Plane B; pass NULL
 *                 to copy the existing Plane A addresses unchanged.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok               Plane B armed.
 * @retval k_ra8_err_invalid_arg  Any non-NULL pointer is misaligned.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre Caller does not race the CEU ISR.
 * @post Plane B mirrors Plane A (with bufs overrides applied).
 * @post CRCNTR.RVS == 1 (swap armed for next VD).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_plane_b_program(const ra8_ceu_buffers_t* bufs);

/**
 * @brief Force an immediate plane swap (CRCMPR.RA).
 *
 * @details
 * HUM Ch 60.2.9 "CRCMPR" p 3649. Bypasses the VD-edge sync the
 * normal Plane-B path uses; use only when a frame must be dropped.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CRCMPR.RA == 1 momentarily; the bit self-clears.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_plane_swap_force(void);

/**
 * @brief Configure the CFWCR firewall window.
 *
 * @details
 * HUM Ch 60.2.18 "CFWCR" p 3661. The firewall fires CETCR.FWF if
 * the data-enable-fetch engine attempts a write past the supplied
 * upper-bound address. Disabled if `enable` is false or the
 * upper-bound is zero.
 *
 * @param[in] enable    True to assert CFWCR.FWE.
 * @param[in] upper_bound Upper-bound address (lower 5 bits forced
 *                         to 0x1F by the hardware).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CFWCR == (`enable`?FWE:0) | (`upper_bound` & FWV mask).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_firewall_set(bool enable, uint32_t upper_bound);

/**
 * @brief Replace CDOCR byte-swap configuration in-place.
 *
 * @param[in] swap Non-NULL byte-swap descriptor.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok           Updated.
 * @retval k_ra8_err_null_ptr `swap` was NULL.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre `swap` is non-NULL.
 * @post CDOCR.COBS == `swap->swap_8_bit`.
 * @post CDOCR.COWS == `swap->swap_16_bit`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_byte_swap_set(const ra8_ceu_byte_swap_t* swap);

/**
 * @brief Configure the bundle-write size (CBDSR).
 *
 * @details
 * HUM Ch 60.2.17 p 3660. The size is in bytes for data-enable
 * mode or in lines for image / data-sync mode. The lower 3 bits
 * are masked off by the hardware to enforce the 8-line/32-byte
 * bundle alignment.
 *
 * @param[in] size_bytes Bundle size; aligned-down to 8 by the helper.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CBDSR == `size_bytes & ~7`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_bundle_size_set(uint32_t size_bytes);

/**
 * @brief Enable or disable the input low-pass filter (CLFCR.LPF).
 *
 * @param[in] enable true => CLFCR.LPF = 1.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CLFCR.LPF == `enable`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_low_pass_set(bool enable);

/**
 * @brief Enable or disable continuous-capture mode (CAPCR.CTNCP).
 *
 * @param[in] mode Single-shot or continuous.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CAPCR.CTNCP reflects `mode`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_mode_set(ra8_ceu_capture_mode_t mode);

/**
 * @brief Update the frame-drop counter (CAPCR.FDRP).
 *
 * @param[in] count Number of frames to skip between captures (0..255).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_ceu_init` previously called.
 * @post CAPCR.FDRP == count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_frame_drop_set(uint8_t count);

/* =============================================================================
 * DMA coupling helpers
 * =============================================================================
 */

/**
 * @brief Pump a finished CEU bundle into a downstream DMAC channel.
 *
 * @details
 * The CEU writes pixels straight to SRAM/SDRAM via its own bus
 * initiator, so it does not need the DMAC for the camera pixel path.
 * This helper exists for the application-level pump that copies a
 * completed CEU frame into a second buffer (e.g. ping-ponging
 * frames into the GLCDC layer-2 framebuffer). It packages the
 * existing `ra8_dmac_start` call with the CEU's preferred 32-bit /
 * source+dest-incrementing layout.
 *
 * @param[in] channel  DMAC channel index.
 * @param[in] src      Source buffer (typically the CDAYR-pointed window).
 * @param[in] dst      Destination buffer.
 * @param[in] bytes    Bytes to copy (multiple of 4).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                DMAC channel armed.
 * @retval k_ra8_err_null_ptr      `src` or `dst` was NULL.
 * @retval k_ra8_err_invalid_arg   `bytes` not a multiple of 4 or 0.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre `ra8_dmac_init` (or equivalent) previously called.
 * @post DMAC channel `channel` armed for a one-shot block transfer.
 *
 * @note Wraps `ra8_dmac_start` -- the channel becomes unavailable
 *       to other consumers until the transfer completes.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ceu_dma_pump(uint8_t channel, const uint8_t* src, uint8_t* dst, uint32_t bytes);

/* =============================================================================
 * Sweep 17: DMA-driven framebuffer + multi-frame capture
 * =============================================================================
 */

/**
 * @brief Cache a DMAC-target framebuffer for the next `ra8_ceu_capture_start`
 *        call.
 *
 * @details
 * The CEU writes pixel data straight to memory via its own bus initiator,
 * so the "DMA buffer" here is just the next CDAYR target the driver
 * will arm when `ra8_ceu_capture_start(num_frames)` is called. The
 * length is cached so the frame-end callback can hand the whole
 * buffer back to the consumer.
 *
 * @param[in] buf Capture target (must be 8-byte aligned per HUM
 *                Ch 60.2.13 p 3656).
 * @param[in] len Buffer length in bytes (>0).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Buffer cached.
 * @retval k_ra8_err_null_ptr      buf was NULL.
 * @retval k_ra8_err_invalid_arg   len was 0 or buf misaligned.
 *
 * @pre `ra8_ceu_init` previously called.
 * @pre `buf` is non-NULL and 8-byte aligned, `len > 0`.
 * @post The driver remembers (buf, len) for the next capture_start.
 *
 * @note Not thread-safe.
 * @see ra8_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_set_dma_buffer(uint8_t* buf, uint32_t len);

/**
 * @brief Start a single or continuous capture into the cached DMA buffer.
 *
 * @details
 * Wraps `ra8_ceu_capture_arm` with the buffer that was cached via
 * `ra8_ceu_set_dma_buffer`. `num_frames` selects the capture mode:
 *  - `num_frames == 1` -> single-shot (CAPCR.CTNCP = 0).
 *  - `num_frames == 0` -> continuous capture (CAPCR.CTNCP = 1).
 *  - Otherwise -> single-shot; the driver tracks the remaining count
 *    through the dispatcher (out of scope for this entry-point, see
 *    @par Note below).
 *
 * @param[in] num_frames 0 = continuous, otherwise number of frames.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Capture armed.
 * @retval k_ra8_err_invalid_state  No buffer cached via
 *                                 `ra8_ceu_set_dma_buffer`.
 * @retval k_ra8_err_busy           CEU is already capturing.
 *
 * @pre `ra8_ceu_init` and `ra8_ceu_set_dma_buffer` were called.
 * @post CAPCR.CTNCP reflects continuous vs single.
 * @post CAPSR.CE == 1.
 *
 * @note Not thread-safe.
 * @see ra8_ceu_capture_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_start(uint32_t num_frames);

/**
 * @brief Stop the in-flight capture started via `ra8_ceu_capture_start`.
 *
 * @details
 * Wraps `ra8_ceu_capture_disarm` so the high-level start/stop pair
 * share matching names.
 *
 * @return ra8_err_t error code from `ra8_ceu_capture_disarm`.
 *
 * @pre `ra8_ceu_capture_start` was previously called.
 * @post CAPSR.CE == 0.
 *
 * @note Not thread-safe.
 * @see ra8_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ceu_capture_stop(void);

#ifdef __cplusplus
}
#endif
