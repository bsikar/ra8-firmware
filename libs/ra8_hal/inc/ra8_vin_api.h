/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_vin_api.h
 * @brief Video Input Module (VIN) driver function prototypes
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public function prototypes for the RA8D2 Video Input Module (VIN)
 * HAL driver: lifecycle (init / deinit / reset), capture arm/disarm,
 * geometry / pre-clip / scaling setters, the 10-to-8-bit LUT loader,
 * colour-space conversion coefficient setters, dithering / YUV-444 /
 * DMR options, CSI-2 input + framebuffer programming, status readers,
 * the interrupt path (enable mask, scanline compare, handler attach,
 * dispatch), power transitions, and the high-level dynamic capture
 * window + frame-end IRQ helpers. The configuration descriptors and
 * types these prototypes consume live in `ra8_vin_types.h`; both are
 * aggregated by the thin umbrella `ra8_vin.h`, which also documents
 * the full driver overview and state machine.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_vin_regs.h"
#include "ra8_vin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise VIN with the supplied descriptor.
 *
 * @details
 * Brings the MIPI CSI clock gate up via the MIPI-CSI MSTP bit
 * (VIN does not have its own MSTPCR bit on the RA8D2; the
 * peripheral's clocks are released as a side-effect of enabling
 * MIPI CSI -- see HUM Ch 11.2.8 "MSTPCRC" and section 67.1
 * "Block Diagram"). The function then writes a clean MC value
 * (ME = 0), programmes IS / MB1..3 / IE / SI / MC.IM / MC.CLP, and
 * clears all latched INTS bits.
 *
 * Pre-clip, scaling, LUT, and colour-space coefficients are left at
 * their reset defaults. Use the dedicated setters to programme them
 * before `ra8_vin_capture_start`.
 *
 * @par State Machine
 * @dot
 * digraph ra8_vin_api_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   uninitialized [label="uninitialized"];
 *   initialized [label="initialized"];
 *   capturing [label="capturing"];
 *
 *   __start -> uninitialized;
 *   uninitialized -> initialized [label="ra8_vin_init"];
 *   initialized -> capturing [label="ra8_vin_capture_start"];
 *   capturing -> initialized [label="ra8_vin_capture_stop"];
 *   initialized -> uninitialized [label="ra8_vin_deinit"];
 * }
 * @enddot
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_null_ptr cfg was NULL.
 * @retval k_ra8_err_invalid_arg cfg.image_stride_px was 0 or
 * cfg.interlace_mode / pixel_clip_mode
 * out of range.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre `cfg->framebuffer_addr_1` is 64-byte aligned.
 * @post MC reflects `cfg`, ME bit is cleared.
 * @post INTS is fully cleared.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_start
 * @see ra8_vin_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_init(const ra8_vin_config_t* cfg);

/**
 * @brief Tear down VIN: stop capture, mask interrupts, drop MSTP.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Driver has been initialized (otherwise this is a no-op write).
 * @post MC.ME and FC.CC are 0.
 * @post Registered callback (if any) is cleared.
 *
 * @note Not thread-safe.
 * @see ra8_vin_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_deinit(void);

/**
 * @brief Issue a soft reset by re-running the MC.ST settle sequence.
 *
 * @details
 * HUM Ch 67.3.1 p 3995 -- writes ME = 0, pulses MC.ST = 1, then
 * absorbs the >= 10 ICLK settling time by reading MC back the
 * configured number of times (`k_ra8_vin_st_settle_reads`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre `ra8_vin_init` succeeded.
 * @pre Caller has masked IRQs.
 * @post MC.ME = 0.
 * @post Internal pipeline state is back to its post-startup defaults.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_reset(void);

/* =============================================================================
 * Capture control
 * =============================================================================
 */

/**
 * @brief Start a capture operation.
 *
 * @details
 * Implements the procedure from HUM Ch 67.3.1 "Initialization
 * Procedure" p 3995:
 * 1. Ensure ME = 0 and CC = 0 (precondition).
 * 2. Pulse MC.ST = 1, then read MC back 10 times so the internal
 * state machine settles for >= 10 ICLK cycles.
 * 3. Set MC.ME = 1.
 * 4. If continuous mode, set FC.CC = 1.
 *
 * The MIPI CSI peripheral must be configured and started by the
 * caller separately -- VIN consumes data that arrives on the
 * shared CSI->VIN data path.
 *
 * @param[in] mode Single-shot, continuous, or continuous-with-field-
 * skip (`ra8_vin_capture_mode_t`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Capture armed and running.
 * @retval k_ra8_err_invalid_state ME or CC was already 1.
 * @retval k_ra8_err_invalid_arg Unknown `mode` value.
 *
 * @pre `ra8_vin_init` has been called.
 * @pre A valid frame buffer is programmed into MB1.
 * @post MC.ME = 1.
 * @post FC.CC reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_capture_arm(ra8_vin_capture_mode_t mode);

/**
 * @brief Stop the current capture and drain outstanding AXI traffic.
 *
 * @details
 * Implements HUM Ch 67.3.6 "Stopping capture operation" p 4006:
 * 1. Clear FC.CC so no new frames are queued.
 * 2. Clear MC.ME.
 * 3. Set MTCSTOP.STOPREQ to push outstanding writes to completion;
 * the driver polls MTCSTOP.STOPACK / OUTSTAND a bounded number
 * of times (`k_ra8_vin_stop_drain_max`) before returning
 * `k_ra8_err_hw_timeout`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Stop sequence drained successfully.
 * @retval k_ra8_err_invalid_state ME was already 0 (nothing to stop).
 * @retval k_ra8_err_hw_timeout OUTSTAND did not reach 0 in time.
 *
 * @pre `ra8_vin_init` has been called.
 * @pre Capture is currently active (MC.ME = 1).
 * @post MC.ME and FC.CC are both 0.
 * @post MTCSTOP.STOPREQ is asserted, outstanding AXI writes drained.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_capture_disarm(void);

/* =============================================================================
 * Geometry / pre-clip / scaling
 * =============================================================================
 */

/**
 * @brief Programme the pre-clip window (SLPRC / ELPRC / SPPRC / EPPRC).
 *
 * @details
 * HUM Ch 67.2.4..67.2.7 p 3980-3981. Defines the rectangle of the
 * input image that survives the pre-clip stage. End values must be
 * >= start values; both sides are clamped to 12 bits (4095 max).
 *
 * @param[in] window Non-NULL pre-clip descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Window programmed.
 * @retval k_ra8_err_null_ptr window was NULL.
 * @retval k_ra8_err_invalid_arg end < start or value > 4095.
 *
 * @pre Driver initialized, capture not running.
 * @pre `window != NULL`.
 * @post SLPRC/ELPRC/SPPRC/EPPRC reflect `window`.
 * @post Pre-clip-violation IRQs (PRCLIPH/PRCLIPV) become meaningful.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_clip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_preclip(const ra8_vin_preclip_t* window);

/**
 * @brief Programme the UDS scaling factor (UDS_SCALE).
 *
 * @details
 * HUM Ch 67.2.22 p 3997. Set the integer mantissa and 12-bit fraction
 * for both axes; the driver enables MC.SCLE only via
 * `ra8_vin_set_uds_ctrl(....multitap = ?,...)` plus
 * `ra8_vin_enable_scaling(true)`. UDS_SCALE itself is loaded by
 * `ra8_vin_capture_start` matching HUM Ch 67.3.5 p 4006.
 *
 * @param[in] scale Non-NULL scaling factor descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Scale programmed.
 * @retval k_ra8_err_null_ptr scale was NULL.
 * @retval k_ra8_err_invalid_arg Mantissa > 15 or fraction > 4095.
 *
 * @pre Driver initialized.
 * @pre `scale != NULL`.
 * @post UDS_SCALE reflects `scale`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_passband
 * @see ra8_vin_set_uds_clip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_uds_scale(const ra8_vin_uds_scale_t* scale);

/**
 * @brief Programme the UDS pass-band bandwidth (UDS_PASS_BWIDTH).
 *
 * @details
 * HUM Ch 67.2.23 p 3998. 7-bit V and H pass-band bandwidth values
 * applied by the multi-tap filter.
 *
 * @param[in] v_bwidth Vertical bandwidth (0..127).
 * @param[in] h_bwidth Horizontal bandwidth (0..127).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg Value > 127.
 *
 * @pre Driver initialized.
 * @pre v_bwidth + h_bwidth in legal 7-bit range.
 * @post UDS_PASS_BWIDTH reflects the new values.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_uds_passband(uint8_t v_bwidth, uint8_t h_bwidth);

/**
 * @brief Programme the UDS output clipping (UDS_CLIP_SIZE).
 *
 * @details
 * HUM Ch 67.2.24 p 3999. 12-bit output clipping size for V and H
 * after scaling.
 *
 * @param[in] v_size Vertical pixel count (0..4095).
 * @param[in] h_size Horizontal pixel count (0..4095).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg Value > 4095.
 *
 * @pre Driver initialized.
 * @pre Both sizes <= 4095.
 * @post UDS_CLIP_SIZE reflects the new values.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_uds_clip(uint16_t v_size, uint16_t h_size);

/**
 * @brief Programme UDS interpolation control (UDS_CTRL).
 *
 * @details
 * HUM Ch 67.2.21 p 3996. Picks bilinear vs nearest-neighbour per
 * channel, plus the multi-tap mode flag and two advanced-mode
 * tweaks. `ra8_vin_enable_scaling` toggles the MC.SCLE main enable.
 *
 * @param[in] ctrl Non-NULL UDS control descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_null_ptr ctrl was NULL.
 *
 * @pre Driver initialized.
 * @pre `ctrl != NULL`.
 * @post UDS_CTRL reflects `ctrl`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_uds_ctrl(const ra8_vin_uds_ctrl_t* ctrl);

/**
 * @brief Toggle the MC.SCLE main scaling-enable bit.
 *
 * @param[in] enable True -> SCLE = 1, false -> SCLE = 0.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Driver initialized.
 * @pre Capture not running (caller stops first if needed).
 * @post MC.SCLE matches `enable`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_enable_scaling(bool enable);

/* =============================================================================
 * LUT (10-to-8-bit conversion)
 * =============================================================================
 */

/**
 * @brief Programme all 256 entries of the Y / Cb / Cr conversion LUT.
 *
 * @details
 * HUM Ch 67.2.25 / 67.2.26 p 4000-4001. Each LUT byte is written by:
 * 1. Setting LUTP to the 10-bit (Y, Cb, Cr) pointer triple.
 * 2. Writing LUTD with the (Y, Cb, Cr) byte triple.
 *
 * The driver loops 256 iterations, advancing all three pointers in
 * lock-step. Pass NULL for `cb` or `cr` to reuse `y` for that
 * channel; passing NULL for `y` returns `k_ra8_err_null_ptr`.
 *
 * `enable` then drives MC.LUTE so the hardware actually consumes the
 * table on the next frame.
 *
 * @param[in] y_table Pointer to 256 8-bit Y values (must be non-NULL).
 * @param[in] cb_table Pointer to 256 8-bit Cb values (NULL -> use Y).
 * @param[in] cr_table Pointer to 256 8-bit Cr values (NULL -> use Y).
 * @param[in] enable Drive MC.LUTE after programming.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok LUT programmed.
 * @retval k_ra8_err_null_ptr y_table was NULL.
 *
 * @pre Driver initialized.
 * @pre Capture not running.
 * @post All 256 LUT entries reflect the supplied tables.
 * @post MC.LUTE matches `enable`.
 *
 * @note Not thread-safe; writes 256 LUTP/LUTD pairs.
 * @see ra8_vin_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_lut_program(const uint8_t* y_table,
                                            const uint8_t* cb_table,
                                            const uint8_t* cr_table,
                                            bool           enable);

/* =============================================================================
 * Colour-space conversion
 * =============================================================================
 */

/**
 * @brief Programme the YC -> RGB matrix coefficients (CSCE1..4).
 *
 * @details
 * HUM Ch 67.2.36..67.2.39 p 4011-4015. Loads the 14-bit Y multiplier,
 * the 12-bit Y / CbCr subtraction terms, and the 14-bit Cr/Cb
 * multiplier pairs.
 *
 * @param[in] coeffs Non-NULL coefficient descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Coefficients written.
 * @retval k_ra8_err_null_ptr coeffs was NULL.
 *
 * @pre Driver initialized, capture not running.
 * @pre `coeffs != NULL`.
 * @post CSCE1..4 reflect `coeffs`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_rgb_to_yc
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_yc_to_rgb(const ra8_vin_yc_to_rgb_t* coeffs);

/**
 * @brief Programme one row of the RGB -> YC matrix.
 *
 * @details
 * HUM Ch 67.2.27..67.2.35 p 4002-4011. The three rows correspond to
 * Y (channel 0), Cb (channel 1), and Cr (channel 2). Each row writes
 * three registers: setting 1 (R), setting 2 (G+B), setting 3 (LAP +
 * LHEN + LSFT).
 *
 * @param[in] channel Row index 0 (Y), 1 (Cb), 2 (Cr).
 * @param[in] row Non-NULL coefficient descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Row written.
 * @retval k_ra8_err_null_ptr row was NULL.
 * @retval k_ra8_err_invalid_arg channel > 2 or shift_down > 31.
 *
 * @pre Driver initialized, capture not running.
 * @pre `row != NULL` and channel < 3.
 * @post YCCRn / CBCCRn / CRCCRn reflect `row`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_yc_to_rgb
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_rgb_to_yc(uint8_t channel, const ra8_vin_rgb_to_yc_chan_t* row);

/* =============================================================================
 * Dithering / YUV-444 / DMR
 * =============================================================================
 */

/**
 * @brief Configure the dithering mode (MC.DC + MC.DC2).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. The DC field picks one of three patterns
 * (`ra8_vin_dc_value_t`); the DC2 bit reverses the direction of the
 * ordered patterns. Caller is responsible for keeping the input pixel
 * depth large enough for dithering to be meaningful.
 *
 * @param[in] mode Dither pattern (`ra8_vin_dc_value_t`).
 * @param[in] direction True -> DC2 = 1, false -> DC2 = 0.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg mode > 2.
 *
 * @pre Driver initialized, capture not running.
 * @pre Mode value is one of `k_ra8_vin_dc_*`.
 * @post MC.DC and MC.DC2 reflect the new values.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_yuv444_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_dithering(uint8_t mode, bool direction);

/**
 * @brief Configure the YUV-444 conversion mode (MC.YUV444).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. Picks data-extend vs interpolate when
 * promoting 4:2:2 input to 4:4:4 internally.
 *
 * @param[in] mode `ra8_vin_yuv444_value_t`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg mode > 1.
 *
 * @pre Driver initialized, capture not running.
 * @pre Mode is one of `k_ra8_vin_yuv444_*`.
 * @post MC.YUV444 matches `mode`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_dithering
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_yuv444_mode(uint8_t mode);

/**
 * @brief Set the interlace mode (MC.IM).
 *
 * @param[in] mode `ra8_vin_im_value_t`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg mode > 2.
 *
 * @pre Driver initialized, capture not running.
 * @pre Mode is one of `k_ra8_vin_im_*`.
 * @post MC.IM reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_interlace_mode(uint8_t mode);

/**
 * @brief Programme the DMR (Data Mode Register).
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Composes DTMD / ABIT / BPSM / EXRGB /
 * YC_THR / YMODE / A8BIT into a single 32-bit write.
 *
 * @param[in] mode Non-NULL DMR descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_null_ptr mode was NULL.
 * @retval k_ra8_err_invalid_arg conv_mode > 2 or y_mode > 3.
 *
 * @pre Driver initialized, capture not running.
 * @pre `mode != NULL`.
 * @post DMR reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_uv_offset
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_data_mode(const ra8_vin_data_mode_t* mode);

/* =============================================================================
 * CSI-2 / framebuffers
 * =============================================================================
 */

/**
 * @brief Programme CSI_IFMD (virtual channel + data type + extension).
 *
 * @param[in] input Non-NULL CSI input descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_null_ptr input was NULL.
 * @retval k_ra8_err_invalid_arg virtual_channel > 15 or data_type > 0x3F.
 *
 * @pre Driver initialized, capture not running.
 * @pre `input != NULL`.
 * @post CSI_IFMD reflects `input`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_field_detect
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_csi_input(const ra8_vin_csi_input_t* input);

/**
 * @brief Programme CSIFLD (field detection).
 *
 * @param[in] detect Non-NULL field-detect descriptor.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_null_ptr detect was NULL.
 * @retval k_ra8_err_invalid_arg even_field_num > 1.
 *
 * @pre Driver initialized.
 * @pre `detect != NULL`.
 * @post CSIFLD reflects `detect`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_csi_input
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_field_detect(const ra8_vin_field_detect_t* detect);

/**
 * @brief Re-programme MB1 / MB2 / MB3 (ping-pong framebuffers).
 *
 * @details
 * HUM Ch 67.2.10..67.2.12 p 3985-3987. Lower 7 address bits are
 * forced to zero (64-byte alignment); the function rejects unaligned
 * inputs with `k_ra8_err_invalid_arg`.
 *
 * @param[in] mb1 New MB1 base; ignored if zero.
 * @param[in] mb2 New MB2 base; ignored if zero.
 * @param[in] mb3 New MB3 base; ignored if zero.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg Any non-zero base unaligned.
 *
 * @pre Driver initialized.
 * @pre At least one of mb1/mb2/mb3 is non-zero.
 * @post Specified MBn registers reflect the new bases.
 *
 * @note Not thread-safe; safe to call mid-capture per HUM Ch 67.3.4
 * p 4005 ("Switching the buffer during operation").
 * @see ra8_vin_get_active_buffer
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_framebuffers(uint32_t mb1, uint32_t mb2, uint32_t mb3);

/**
 * @brief Programme UVAOF (UV plane address offset for YC-separated mode).
 *
 * @param[in] uv_addr 64-byte-aligned UV plane base; 0 disables.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg Address not 64-byte aligned.
 *
 * @pre Driver initialized.
 * @pre `uv_addr` is either zero or 64-byte aligned.
 * @post UVAOF reflects `uv_addr`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_data_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_uv_offset(uint32_t uv_addr);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Snapshot the INTS register.
 *
 * @param[out] out_mask Receives the current INTS value.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Snapshot taken.
 * @retval k_ra8_err_null_ptr out_mask was NULL.
 *
 * @pre `out_mask != NULL`.
 * @pre Driver is in any post-init state.
 * @post `*out_mask` contains the value of INTS.
 *
 * @note Re-entrant; just a register read.
 * @see ra8_vin_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_get_status(uint32_t* out_mask);

/**
 * @brief Clear write-1-to-clear bits in the INTS register.
 *
 * @details
 * Only the bits in `mask` that intersect `k_ra8_vin_int_w1c_mask`
 * are written back; all other bits are gated off so a careless
 * caller cannot toggle status bits the hardware owns.
 *
 * @param[in] mask Bits to clear (subset of `ra8_vin_int_mask_t`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Driver is in any post-init state.
 * @post Bits in `mask & k_ra8_vin_int_w1c_mask` read as 0 next access.
 *
 * @note Re-entrant; single masked register write.
 * @see ra8_vin_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_clear_status(uint32_t mask);

/**
 * @brief Decode the MS register into a typed snapshot.
 *
 * @param[out] out_status Non-NULL destination.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Snapshot taken.
 * @retval k_ra8_err_null_ptr out_status was NULL.
 *
 * @pre `out_status != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_status` decoded.
 *
 * @note Re-entrant.
 * @see ra8_vin_get_active_buffer
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_get_module_status(ra8_vin_module_status_t* out_status);

/**
 * @brief Read the LC (line count) register.
 *
 * @param[out] out_count Non-NULL destination.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Read OK.
 * @retval k_ra8_err_null_ptr out_count was NULL.
 *
 * @pre `out_count != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_count` carries 0..4095.
 *
 * @note Re-entrant.
 * @see ra8_vin_get_module_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_get_line_count(uint16_t* out_count);

/**
 * @brief Return the active framebuffer index from MS.FBS.
 *
 * @param[out] out_id Non-NULL destination (`ra8_vin_ms_fbs_t`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Read OK.
 * @retval k_ra8_err_null_ptr out_id was NULL.
 *
 * @pre `out_id != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_id` carries 0..3.
 *
 * @note Re-entrant.
 * @see ra8_vin_get_module_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_get_active_buffer(uint8_t* out_id);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Update the IE (Interrupt Enable) mask at runtime.
 *
 * @param[in] mask Bitwise OR of `ra8_vin_int_mask_t` values.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Driver initialized.
 * @pre Caller has masked the VIN IRQ if updating mid-capture.
 * @post IE register equals `mask`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_scanline_compare
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_interrupt_enable(uint32_t mask);

/**
 * @brief Programme the SI (Scanline Interrupt) compare register.
 *
 * @param[in] line 12-bit compare value, 0 disables.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Programmed.
 * @retval k_ra8_err_invalid_arg line > 4095.
 *
 * @pre Driver initialized.
 * @pre `line` <= 4095.
 * @post SI register equals `line`.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_interrupt_enable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_scanline_compare(uint16_t line);

/**
 * @brief Attach a VIN event callback (shared between status + error IRQs).
 *
 * @param[in] fn Callback invoked from `ra8_vin_dispatch`. May be NULL
 * to detach.
 * @param[in] ctx Context value forwarded to the callback.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Caller may not invoke this from within the callback.
 * @pre Single-writer context (init or IRQ-masked).
 * @post Subsequent `ra8_vin_dispatch` calls invoke `fn(ctx, ints)`.
 *
 * @note Not thread-safe; callback storage is plain static state.
 * @see ra8_vin_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_attach_handler(ra8_vin_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot INTS, fire callback, then W1C the latched bits.
 *
 * @details
 * Intended to be called from the VIN status / error ISR. Reads the
 * current INTS value, hands it to the registered callback (if any),
 * and writes back the W1C subset to acknowledge the event with the
 * hardware.
 *
 * @pre Called from a single CPU context.
 * @pre `ra8_vin_init` was successful.
 * @post W1C bits in INTS read as 0 after this returns.
 * @post Callback (if any) has run exactly once with the snapshot.
 *
 * @note Re-entrant only across distinct VIN instances (there is one).
 *       See HUM Ch 47 "Video Input Module (VIN)" pp 2455-2540.
 * @see ra8_vin_attach_handler
 * @since 0.1.0
 */
void ra8_vin_dispatch(void);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Quiesce VIN and disable its module-stop clock gate.
 *
 * @details
 * Stops capture (MC.ME = 0) and disables the MIPI CSI MSTP bit.
 * Note that this also gates the MIPI CSI receiver since the two
 * peripherals share `k_ra8_mstp_mipi_csi`.
 *
 * @return ra8_err_t error code from `ra8_mstp_disable`.
 *
 * @pre Caller has stopped any in-flight CSI traffic.
 * @pre Driver was previously initialized.
 * @post MC.ME = 0.
 * @post MIPI CSI MSTP gate is closed.
 *
 * @note Not thread-safe.
 * @see ra8_vin_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_enter_stop(void);

/**
 * @brief Re-enable the VIN clock gate after `ra8_vin_enter_stop`.
 *
 * @return ra8_err_t error code from `ra8_mstp_enable`.
 *
 * @pre `ra8_vin_enter_stop` was called.
 * @pre Caller will re-issue `ra8_vin_capture_start` afterwards.
 * @post MIPI CSI MSTP gate is open.
 * @post VIN registers are accessible again.
 *
 * @note Not thread-safe.
 * @see ra8_vin_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_exit_stop(void);

/* =============================================================================
 * Sweep 17: dynamic capture window + frame-end IRQ
 * =============================================================================
 */

/**
 * @brief Start a single-frame capture into `buf` with the supplied geometry.
 *
 * @details
 * High-level wrapper around `ra8_vin_capture_arm(k_ra8_vin_capture_single)`
 * that:
 *  1. Programmes MB1 with `(uint32_t)(uintptr_t)buf` (HUM Ch 67.2.10
 *     "MB1: Memory Base 1 Register" p 3985).
 *  2. Sets the IS register (HUM Ch 67.2.13 "IS: Image Stride Register"
 *     p 3984) and pre-clip end coordinates to define the capture window.
 *  3. Updates MC.INF (HUM Ch 67.2.1 "MC: Main Control Register" p 3975)
 *     so the input format matches.
 *  4. Caches `buf`/`w`/`h` so the frame-end handler (registered via
 *     `ra8_vin_attach_frame_handler`) sees the right buffer + length.
 *  5. Calls `ra8_vin_capture_arm(k_ra8_vin_capture_single)`.
 *
 * @param[in] buf Capture target (must be 64-byte aligned).
 * @param[in] w Width in pixels (1..4096).
 * @param[in] h Height in lines (1..4096).
 * @param[in] format Input format (`ra8_vin_input_fmt_t`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Capture armed.
 * @retval k_ra8_err_null_ptr buf was NULL.
 * @retval k_ra8_err_invalid_arg w/h out of range or buf misaligned.
 * @retval k_ra8_err_invalid_state ME or CC was already 1.
 *
 * @pre `ra8_vin_init` succeeded.
 * @pre `buf` is 64-byte aligned.
 * @post MB1 == (uint32_t)(uintptr_t)buf.
 * @post MC.ME == 1.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_stop
 * @see ra8_vin_attach_frame_handler
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_vin_capture_start(void* buf, uint16_t w, uint16_t h, ra8_vin_input_fmt_t format);

/**
 * @brief Stop the in-flight capture started via `ra8_vin_capture_start`.
 *
 * @details
 * Wraps `ra8_vin_capture_disarm` -- exists so the high-level start /
 * stop pair share matching names.
 *
 * @return ra8_err_t error code from `ra8_vin_capture_disarm`.
 *
 * @pre `ra8_vin_capture_start` was previously called.
 * @post MC.ME and FC.CC are both 0.
 *
 * @note Not thread-safe.
 * @see ra8_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_capture_stop(void);

/**
 * @brief Programme a region-of-interest pre-clip window.
 *
 * @details
 * Wraps `ra8_vin_set_preclip` with a (x, y, w, h) signature for callers
 * that prefer (x, y) over (start_line, start_pixel). HUM Ch 67.2.4..
 * 67.2.7 p 3980-3981.
 *
 * @param[in] x Top-left pixel index (0..4095).
 * @param[in] y Top-left line index (0..4095).
 * @param[in] w Window width in pixels (>=1).
 * @param[in] h Window height in lines (>=1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Window programmed.
 * @retval k_ra8_err_invalid_arg Arguments out of range.
 *
 * @pre Driver initialized, capture not running.
 * @pre `w >= 1`, `h >= 1`, `x + w - 1 <= 4095`, `y + h - 1 <= 4095`.
 * @post SLPRC/ELPRC/SPPRC/EPPRC reflect the requested window.
 *
 * @note Not thread-safe.
 * @see ra8_vin_set_preclip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Register the frame-end IRQ callback.
 *
 * @details
 * The driver's generic `ra8_vin_dispatch` fans every INTS bit through
 * a single mask-style callback. This frame-end variant is invoked
 * only when INTS.FIE is set after a `ra8_vin_capture_start` call, and
 * receives the cached buffer + length.
 *
 * @param[in] fn Callback (NULL detaches).
 * @param[in] ctx Forwarded to the callback.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre Single-writer context (init or IRQ-masked).
 * @post Subsequent `ra8_vin_dispatch` calls fire `fn` on FIE.
 *
 * @note Not thread-safe.
 * @see ra8_vin_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vin_attach_frame_handler(ra8_vin_frame_fn_t fn, void* ctx);

#ifdef __cplusplus
}
#endif
