/**
 * @file ra8_mipi_csi_ctrl.h
 * @brief MIPI CSI-2 receiver HAL driver -- control, status, and IRQ-enable API
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Lifecycle, reception control, module-level status, data-type filter,
 * ECC / scrambling / frame-error knobs, EPD / LRTE tuning, and the
 * per-data-lane / per-VC / power-management status + IRQ-enable
 * prototypes for the RA8D2 MIPI CSI-2 receiver HAL. Split out of
 * ``ra8_mipi_csi.h`` to keep each header within the per-file line
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
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the MIPI CSI receiver.
 *
 * @details
 * Sequence:
 *
 *  1. Validate ``cfg`` and the lane count.
 *  2. Ungate the peripheral via ``ra8_mstp_enable(k_ra8_mstp_mipi_csi)``.
 *  3. Clear MCT3.RXEN (no reception during configuration).
 *  4. Spin until RTST.VSRSTS reads 0 (no reset in progress).
 *  5. Programme MCT0 (lane count + GRMD + ECCV13 + LFSREN +
 *     ZLMD + EDMD + RVMD).
 *  6. Programme MCT2 (FRRCLK + FRRSKW).
 *  7. Programme EPCT (EPD enable, option, spacers).
 *  8. Programme EMCT (VLSIEN + EOTPEN).
 *  9. Programme DTEL / DTEH (data-type filter).
 * 10. Programme GSCT (short-packet threshold + store flag).
 * 11. Programme RXIE / DLIE0 / DLIE1 / VCIE0..15 / PMIE / GSIE.
 * 12. Leave RXEN clear -- caller arms via ``start_receive``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Receiver configured, RXEN still 0.
 * @retval k_ra8_err_null_ptr      ``cfg`` was nullptr.
 * @retval k_ra8_err_invalid_arg   Lane count not in ``{1, 2}`` or VLSIEN
 *                                out of range.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 * @retval k_ra8_err_hw_timeout    VSRSTS did not clear within budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 *
 * @post On success, the peripheral is clocked and configured.
 * @post MCT3.RXEN is 0 (no incoming data accepted).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_mipi_csi_start_receive
 * @see ra8_mipi_csi_deinit
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_init(const ra8_mipi_csi_config_t* cfg);

/**
 * @brief Tear down the MIPI CSI receiver.
 *
 * @details
 * Clears MCT3.RXEN, pulses VSRST to flush any in-flight state,
 * clears every IRQ-enable register, drops cached callbacks, and
 * gates the MSTP bit. Safe to call after a partial init.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Receiver gated cleanly.
 * @retval k_ra8_err_invalid_state ra8_mstp ref-count was already 0.
 *
 * @pre IRQs masked or single-threaded teardown context.
 * @pre Caller has stopped any upstream camera transmission.
 * @post MCT3.RXEN is 0.
 * @post MSTP bit for MIPI CSI is set (peripheral gated).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_deinit(void);

/**
 * @brief Issue a software reset (RTCT.VSRST) and wait for completion.
 *
 * @details
 * Pulses RTCT.VSRST then spins on RTST.VSRSTS until it clears. Used
 * after stop_receive to drain the video-pixel pipeline before the
 * next start.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Reset completed.
 * @retval k_ra8_err_hw_timeout VSRSTS did not clear in time.
 *
 * @pre Peripheral is ungated.
 * @pre Caller is in single-threaded context.
 * @post RTST.VSRSTS reads 0.
 * @post Video-pixel pipeline is empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_reset(void);

/* =============================================================================
 * Reception control
 * =============================================================================
 */

/**
 * @brief Enable MIPI CSI reception (set MCT3.RXEN).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                MCT3.RXEN set.
 * @retval k_ra8_err_invalid_state RXEN was already set.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Upstream MIPI PHY is up and clocking.
 * @post MCT3.RXEN reads 1.
 * @post Subsequent CSI-2 packets land in RXST / VCSTn / GSHT.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_mipi_csi_stop_receive
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_start_receive(void);

/**
 * @brief Disable MIPI CSI reception (clear MCT3.RXEN, pulse VSRST).
 *
 * @details
 * The HUM (and FSP) require the application to wait for a full
 * frame after stopping the upstream camera before calling this --
 * we do not poll for that condition because the driver has no way
 * to know the frame interval. Caller is responsible.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Upstream camera has stopped sending data.
 * @post MCT3.RXEN reads 0.
 * @post RTCT.VSRST has been written 1 (reset pulse issued).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_stop_receive(void);

/* =============================================================================
 * Module-level status
 * =============================================================================
 */

/**
 * @brief Read the RXST receive-status register.
 *
 * @param[out] out_mask On success, the live RXST value (RACT,
 *                      RACTDET, FRMn[15:0]).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Status returned in ``*out_mask``.
 * @retval k_ra8_err_null_ptr ``out_mask`` was nullptr.
 *
 * @pre ``out_mask`` is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post ``*out_mask`` holds the RXST value.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_get_status(uint32_t* out_mask);

/**
 * @brief Clear the sticky receive-status bits selected by ``mask``.
 *
 * @details
 * Only RACTDET (bit 17) is currently writable in RXSC; other bits
 * in ``mask`` are silently dropped by hardware.
 *
 * @param[in] mask Bit mask to write to RXSC.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller knows which sticky bits are write-1-to-clear.
 * @post RXSC has been written.
 * @post Hardware will eventually deassert the targeted RXST bits.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_clear_status(uint32_t mask);

/**
 * @brief Programme RXIE (receive interrupt enable mask).
 * @param[in] mask  Bit mask to programme into RXIE.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post RXIE = mask.
 * @post Subsequent matching events trigger the receive IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_rx_irq_enable(uint32_t mask);

/**
 * @brief Read MIST (Module Interrupt Status, RO summary).
 * @param[out] out_mask On success, the live MIST value.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Status returned.
 * @retval k_ra8_err_null_ptr out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post *out_mask holds MIST.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_get_module_irq_status(uint32_t* out_mask);

/**
 * @brief Read MCG (Module Configuration, RO IP info) and decode.
 *
 * @param[out] out On success, the decoded MCG snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Info returned in ``*out``.
 * @retval k_ra8_err_null_ptr ``out`` was nullptr.
 *
 * @pre out is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post *out holds the decoded MCG snapshot.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_get_module_info(ra8_mipi_csi_module_info_t* out);

/* =============================================================================
 * Data-type filter
 * =============================================================================
 */

/**
 * @brief Programme the receive data-type filter (DTEL + DTEH).
 *
 * @details
 * A bit set in ``low_mask`` enables the corresponding data type in
 * the 0x00-0x1F range; a bit set in ``high_mask`` enables one in
 * 0x20-0x3F. Bits documented as reserved (DTEL[3:0] read 1, [7:5]
 * read 0, [29:16] read 0; DTEH bits other than 4 and 10) are
 * preserved by the hardware.
 *
 * @param[in] low_mask  Mask written to DTEL.
 * @param[in] high_mask Mask written to DTEH.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded context.
 * @post DTEL = low_mask, DTEH = high_mask.
 * @post Receiver accepts only the selected data types.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_data_type_filter(uint32_t low_mask, uint32_t high_mask);

/* =============================================================================
 * ECC / scrambling / frame-error knobs
 * =============================================================================
 */

/**
 * @brief Update MCT0 ECC bits (ECCV13, LFSREN) atomically.
 *
 * @param[in] eccv13 1 = ECC v1.3 (24-bit) mode, 0 = legacy ECC.
 * @param[in] lfsren 1 = enable LFSR descrambling.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bits updated.
 * @retval k_ra8_err_invalid_state RXEN was set (can only update while gated).
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post MCT0.ECCV13 and MCT0.LFSREN reflect the requested state.
 * @post Other MCT0 bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_ecc_mode(bool eccv13, bool lfsren);

/**
 * @brief Update MCT0 frame-error notification bits atomically.
 *
 * @param[in] zlmd 1 = treat zero-length LP as ErrFrameSync.
 * @param[in] edmd 1 = forward ErrFrameData errors to ISR.
 * @param[in] rvmd 1 = receive Reserved-DT packets normally.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bits updated.
 * @retval k_ra8_err_invalid_state RXEN was set.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post MCT0.ZLMD / EDMD / RVMD reflect the requested state.
 * @post Other MCT0 bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_frame_error_mode(bool zlmd, bool edmd, bool rvmd);

/* =============================================================================
 * EPD / LRTE tuning
 * =============================================================================
 */

/**
 * @brief Programme EPCT (long-packet spacer + Option-2 EPD).
 *
 * @param[in] enable      1 = EPCT.EPDEN -- enable EPD operation.
 * @param[in] option_2    1 = EPCT.EPDOP -- select Option-2 EPD.
 * @param[in] long_spacer EPCT.SLP[14:0] -- long-packet spacer count.
 * @param[in] short_spacer EPCT.SSP[14:0] -- short-packet spacer count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Updated.
 * @retval k_ra8_err_invalid_arg   spacer values exceed 15 bits.
 * @retval k_ra8_err_invalid_state RXEN was set.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post EPCT contains the requested fields.
 * @post Subsequent transmissions honour the new spacer values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mipi_csi_set_epd(bool enable, bool option_2, uint16_t long_spacer, uint16_t short_spacer);

/**
 * @brief Programme EMCT (LRTE / EOTP options).
 *
 * @param[in] vlsien Variable-length spacer mode (EMCT.VLSIEN[5:4]).
 * @param[in] eotp_enable 1 = EMCT.EOTPEN -- expect EoT packet.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Updated.
 * @retval k_ra8_err_invalid_arg   vlsien out of enum range.
 * @retval k_ra8_err_invalid_state RXEN was set.
 *
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post EMCT.VLSIEN and EMCT.EOTPEN reflect the requested state.
 * @post Other EMCT bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_set_lrte(ra8_mipi_csi_vlsien_t vlsien, bool eotp_enable);

/* =============================================================================
 * Per-data-lane status / IRQ
 * =============================================================================
 */

/**
 * @brief Read DLST(N) (Data Lane N Status).
 * @param[in]  lane     Data lane index (0..1).
 * @param[out] out_mask Live DLST(N) value.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Status returned.
 * @retval k_ra8_err_invalid_arg lane > 1.
 * @retval k_ra8_err_null_ptr  out_mask was nullptr.
 * @pre lane <= 1.
 * @pre out_mask is non-NULL.
 * @post *out_mask holds the DLST(N) value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_dl_get_status(uint8_t lane, uint32_t* out_mask);

/**
 * @brief W1C-clear bits in DLST(N) via DLSC(N).
 * @param[in] lane Data lane index (0..1).
 * @param[in] mask Bits to clear.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Cleared.
 * @retval k_ra8_err_invalid_arg lane > 1.
 * @pre lane <= 1.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post DLSC(N) has been written.
 * @post Targeted DLST(N) bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_dl_clear_status(uint8_t lane, uint32_t mask);

/**
 * @brief Programme DLIE(N) (Data Lane N IRQ enable).
 * @param[in] lane Data lane index (0..1).
 * @param[in] mask Bits to set in DLIE(N).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Updated.
 * @retval k_ra8_err_invalid_arg lane > 1.
 * @pre lane <= 1.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post DLIE(N) = mask.
 * @post Subsequent matching DL events trigger the DL IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_dl_set_irq_enable(uint8_t lane, uint32_t mask);

/* =============================================================================
 * Per-virtual-channel status / IRQ
 * =============================================================================
 */

/**
 * @brief Read VCST(M) (Virtual Channel M Status).
 * @param[in]  vc       Virtual channel index (0..15).
 * @param[out] out_mask Live VCST(M) value.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Status returned.
 * @retval k_ra8_err_invalid_arg vc > 15.
 * @retval k_ra8_err_null_ptr   out_mask was nullptr.
 * @pre vc <= 15.
 * @pre out_mask is non-NULL.
 * @post *out_mask holds the VCST(M) value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_vc_get_status(uint8_t vc, uint32_t* out_mask);

/**
 * @brief W1C-clear bits in VCST(M) via VCSC(M).
 * @param[in] vc   Virtual channel index (0..15).
 * @param[in] mask Bits to clear.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Cleared.
 * @retval k_ra8_err_invalid_arg vc > 15.
 * @pre vc <= 15.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post VCSC(M) has been written.
 * @post Targeted VCST(M) bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_vc_clear_status(uint8_t vc, uint32_t mask);

/**
 * @brief Programme VCIE(M) (Virtual Channel M IRQ enable).
 * @param[in] vc   Virtual channel index (0..15).
 * @param[in] mask Bits to set in VCIE(M).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Updated.
 * @retval k_ra8_err_invalid_arg vc > 15.
 * @pre vc <= 15.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post VCIE(M) = mask.
 * @post Subsequent matching VC events trigger the VC IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_vc_set_irq_enable(uint8_t vc, uint32_t mask);

/* =============================================================================
 * Power-management status / IRQ
 * =============================================================================
 */

/**
 * @brief Read PMST (Power Management Status) live value.
 * @param[out] out_mask Live PMST value.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Status returned.
 * @retval k_ra8_err_null_ptr  out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @post *out_mask holds the PMST value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_pm_get_status(uint32_t* out_mask);

/**
 * @brief W1C-clear bits in PMST via PMSC.
 * @param[in] mask Bits to clear (only [7:0] are clearable).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller knows which PM bits are W1C.
 * @post PMSC has been written.
 * @post Targeted PMST bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_pm_clear_status(uint32_t mask);

/**
 * @brief Programme PMIE (Power Management IRQ enable).
 * @param[in] mask Bits to set in PMIE.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 * @pre ``ra8_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post PMIE = mask.
 * @post Subsequent matching PM events trigger the PM IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_csi_pm_set_irq_enable(uint32_t mask);

#ifdef __cplusplus
}
#endif
