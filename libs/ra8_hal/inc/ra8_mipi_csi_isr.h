/**
 * @file ra8_mipi_csi_isr.h
 * @brief MIPI CSI-2 receiver HAL driver -- FIFO, ISR dispatch, framing, power
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Generic short-packet FIFO control, the ISR-side dispatchers, the
 * per-VC framing helpers + error-reporting attach, and the low-power
 * MSTP transition helpers for the RA8D2 MIPI CSI-2 receiver HAL. Split
 * out of ``ra8_mipi_csi.h`` to keep each header within the per-file line
 * budget; consumers continue to include ``ra8_mipi_csi.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_csi_regs.h"
#include "ra8_mipi_csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Generic short-packet FIFO
 * =============================================================================
 */

/**
 * @brief Programme GSCT (short-packet FIFO threshold + store flag).
 *
 * @param[in] threshold     FIFO depth-1 reported in GSST.PNUM that
 *                          asserts the GTH IRQ. Range 0..15.
 * @param[in] store_enable  1 = store incoming short packets in FIFO.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Updated.
 * @retval k_ra8_err_invalid_arg threshold > 15.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre threshold <= 15.
 * @post GSCT.SHTH = threshold.
 * @post GSCT.GFIF = store_enable.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_configure(uint8_t threshold, bool store_enable);

/**
 * @brief Programme GSIE (short-packet IRQ enable).
 * @param[in] mask Bits to set in GSIE.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post GSIE = mask.
 * @post Subsequent matching GST events trigger the GST IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_set_irq_enable(uint32_t mask);

/**
 * @brief Read GSST (short-packet status).
 * @param[out] out_mask Live GSST value.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Status returned.
 * @retval k_ra8_err_null_ptr out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post *out_mask holds the GSST value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_get_status(uint32_t* out_mask);

/**
 * @brief W1C-clear GSST.GOV (FIFO overflow) via GSSC.
 * @param[in] mask Bits to clear (only GOV bit is W1C).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller knows which GSST bits are W1C.
 * @post GSSC has been written.
 * @post GSST.GOV will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_clear_status(uint32_t mask);

/**
 * @brief Drain one stored short-packet header from the FIFO.
 *
 * @details
 * Reads GSST.PNUM first; if non-zero, sets GSIU.FINC (advance read
 * pointer) then reads GSHT to retrieve the next header. The
 * returned ``raw`` field is the unprocessed GSHT snapshot and the
 * other fields are decoded for caller convenience.
 *
 * @param[out] out On success, the decoded short-packet header.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Header drained into ``*out``.
 * @retval k_ra8_err_null_ptr out was nullptr.
 * @retval k_ra8_err_empty    FIFO had no stored packets.
 *
 * @pre out is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post On success, GSIU.FINC was pulsed and *out holds a valid header.
 * @post GSST.PNUM is decremented by 1 on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_read_short_packet(ra8_mipi_csi_short_packet_t* out);

/**
 * @brief Request a clear of the entire short-packet FIFO (GSIU.GFCLR).
 *
 * @details
 * Writes 1 to GSIU.GFCLR, then waits for GSST.GCD to read 1 (clear
 * complete) and writes 0 to GSIU.GFCLR to release the request line.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Cleared.
 * @retval k_ra8_err_hw_timeout GCD did not assert within budget.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded context.
 * @post FIFO is empty (GSST.PNUM = 0).
 * @post GSIU.GFCLR is back to 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_clear_fifo(void);

/**
 * @brief Re-enable storing into the FIFO after a GOV overflow (GSIU.GFEN).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller has cleared the GOV flag via clear_status.
 * @post GSIU.GFEN was pulsed (W1).
 * @post Hardware will resume queuing packets.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_short_packet_re_enable_store(void);

/* =============================================================================
 * Interrupt path (handlers + dispatchers)
 * =============================================================================
 */

/**
 * @brief Attach the receive-status callback (MIPI CSI RX vector).
 *
 * @param[in] fn  Callback fired by ``ra8_mipi_csi_dispatch``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always (passing ``fn = nullptr`` is the way to detach).
 *
 * @pre Caller is in single-threaded init context.
 * @pre ``fn`` is either nullptr (detach) or a valid function pointer.
 * @post The dispatcher will invoke ``fn(ctx, rxst)`` until detached.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_handler(ra8_mipi_csi_event_fn_t fn, void* ctx);

/**
 * @brief Attach the per-data-lane callback (MIPI CSI DL vector).
 * @param[in] fn  Callback fired by ``ra8_mipi_csi_dispatch_dl``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra8_mipi_csi_dispatch_dl`` will invoke ``fn`` for matching lanes.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_dl_handler(ra8_mipi_csi_dl_event_fn_t fn, void* ctx);

/**
 * @brief Attach the per-VC callback (MIPI CSI VC vector).
 * @param[in] fn  Callback fired by ``ra8_mipi_csi_dispatch_vc``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra8_mipi_csi_dispatch_vc`` will invoke ``fn`` per VC + once for generic.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_vc_handler(ra8_mipi_csi_vc_event_fn_t fn, void* ctx);

/**
 * @brief Attach the power-management callback (MIPI CSI PM vector).
 * @param[in] fn  Callback fired by ``ra8_mipi_csi_dispatch_pm``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra8_mipi_csi_dispatch_pm`` will invoke ``fn`` until detached.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_pm_handler(ra8_mipi_csi_pm_event_fn_t fn, void* ctx);

/**
 * @brief Attach the short-packet FIFO callback (MIPI CSI GST vector).
 * @param[in] fn  Callback fired by ``ra8_mipi_csi_dispatch_short_packet``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra8_mipi_csi_dispatch_short_packet`` will invoke ``fn`` until detached.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_short_packet_handler(ra8_mipi_csi_short_event_fn_t fn,
                                                                 void*                         ctx);

/**
 * @brief ISR-side dispatcher for the receive vector.
 *
 * @details
 * Designed to be called from the wired-up MIPI CSI receive ISR
 * (``mipi_csi_rx_isr`` in the FSP layout). Reads RXST once, writes
 * RACTDETC to RXSC to clear the sticky detect bit, then forwards the
 * RXST snapshot to the registered callback.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI RX vector.
 * @post RXSC.RACTDETC has been written 1.
 * @post If a handler is attached, it has run synchronously.
 *
 * @note Thread safety: ISR-only; do not call from threads.
 * @since 0.1.0
 */
void ra8_mipi_csi_dispatch(void);

/**
 * @brief ISR-side dispatcher for the data-lane vector.
 *
 * @details
 * Reads MIST + DLST0 + DLST1, W1C-clears DLSC0/DLSC1 (preserving the
 * RO ULP bit), and invokes the data-lane callback once per lane that
 * showed up in MIST.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI DL vector.
 * @post DLSC0 / DLSC1 have been W1C-cleared.
 * @post Handler ran once per lane that flagged in MIST.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra8_mipi_csi_dispatch_dl(void);

/**
 * @brief ISR-side dispatcher for the per-VC vector.
 *
 * @details
 * Reads MIST, then for every VC bit that is set walks the
 * per-channel registers, clears each VCSC(M), and invokes the
 * per-VC callback. Generic errors (MLF + ECD) are accumulated and
 * delivered as a final callback with vc = 0xFF.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI VC vector.
 * @post Every flagged VCSC(M) has been written.
 * @post Handler ran once per VC plus once for the generic-error summary.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra8_mipi_csi_dispatch_vc(void);

/**
 * @brief ISR-side dispatcher for the power-management vector.
 *
 * @details
 * Reads PMST, W1C-clears the lower 8 bits via PMSC, then invokes
 * the PM callback.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI PM vector.
 * @post PMSC has been written with the W1C-clearable bits.
 * @post Handler ran once with the PMST snapshot.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra8_mipi_csi_dispatch_pm(void);

/**
 * @brief ISR-side dispatcher for the generic-short-packet vector.
 *
 * @details
 * Reads GSST, W1C-clears GOV via GSSC, then invokes the GST
 * callback. The handler is expected to drain the FIFO via
 * ``ra8_mipi_csi_read_short_packet``.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI GST vector.
 * @post GSSC has been written with the W1C-clearable bits.
 * @post Handler ran once with the GSST snapshot.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra8_mipi_csi_dispatch_short_packet(void);

/* =============================================================================
 * Sweep 6 -- per-VC framing helpers + error reporting
 * =============================================================================
 */

/**
 * @brief Filter which CSI-2 virtual channels the receiver will surface.
 *
 * @details
 * Each bit i of ``vc_mask`` enables VCi (i = 0..15). The driver
 * disables VCIE on every channel whose bit is clear (so spurious
 * traffic does not raise IRQs) and restores the previous IRQ mask on
 * channels that are re-enabled. HUM Ch 66.3.20 "VCIE(M)" p 3952.
 *
 * @param[in] vc_mask Bitmap of accepted virtual channels (bit 0 = VC0).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               VCIE updated for every channel.
 * @retval k_ra8_err_invalid_arg  ``vc_mask == 0`` (would silence the receiver).
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init / reconfigure context.
 * @post VCIE for every selected VC is non-zero (default mask restored).
 * @post VCIE for every unselected VC is zero.
 *
 * @note Not thread-safe.
 * @see ra8_mipi_csi_set_data_format
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_virtual_channels(uint16_t vc_mask);

/**
 * @brief Select the CSI-2 payload format the receiver should accept on a
 *        specific virtual channel.
 *
 * @details
 * The hardware filter (DTEL/DTEH) is global across virtual channels --
 * selecting a format on one VC therefore enables the matching DT bit
 * for the entire receiver. The driver records the per-VC selection in
 * a software shadow so a subsequent call with
 * ``k_ra8_mipi_csi_format_off`` can re-compute the union and clear the
 * DT bit if no other VC still wants it. HUM Ch 66.3.10/66.3.11
 * pp 3943-3944.
 *
 * @param[in] vc     Virtual channel index (0..15).
 * @param[in] format Payload format to accept (or ``_off`` to disable).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Filter updated.
 * @retval k_ra8_err_invalid_arg   ``vc > 15`` or unknown format value.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre ``vc`` is in 0..15.
 * @post DTEL/DTEH reflect the union of all per-VC selections.
 * @post Future calls to ::ra8_mipi_csi_set_data_type_filter override
 *       these contributions with explicit masks.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_data_format(uint8_t vc, ra8_mipi_csi_data_format_t format);

/**
 * @brief Register a callback invoked on ECC / CRC error reports.
 *
 * @details
 * The CSI VC dispatcher already surfaces VCST contents. This helper
 * filters those events down to the ECC + CRC error subset and
 * decodes them into a ::ra8_mipi_csi_error_report_t for the caller.
 * Pass ``fn = nullptr`` to detach.
 *
 * @param[in] fn  Callback to invoke (or NULL to detach).
 * @param[in] ctx Caller-owned context forwarded to ``fn``.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller's ``fn`` is ISR-safe.
 * @pre Caller owns the ``ctx`` lifetime for as long as ``fn`` is attached.
 * @post Subsequent ``ra8_mipi_csi_dispatch_vc`` calls forward ECC/CRC
 *       errors to ``fn`` in addition to the VC callback.
 * @post Detaching with NULL silently swallows further errors.
 *
 * @note ISR-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_attach_error_handler(ra8_mipi_csi_error_fn_t fn, void* ctx);
/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Gate the peripheral via MSTP (low-power entry helper).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Peripheral gated.
 * @retval k_ra8_err_invalid_state ra8_mstp ref-count was already 0.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded at least once.
 * @pre Reception has been stopped via ``ra8_mipi_csi_stop_receive``.
 * @post MSTP bit for MIPI CSI is set.
 * @post Register access is undefined until ``exit_stop``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_enter_stop(void);

/**
 * @brief Ungate the peripheral via MSTP (low-power exit helper).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Peripheral clocked.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 *
 * @pre ``ra8_mipi_csi_enter_stop`` was called previously.
 * @pre Caller is in single-threaded resume context.
 * @post MSTP bit for MIPI CSI is cleared.
 * @post Register access is valid again.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_exit_stop(void);

#ifdef __cplusplus
}
#endif
