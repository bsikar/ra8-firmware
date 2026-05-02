/**
 * @file ra_vin.h
 * @brief Video Input Module (VIN) driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * / Round-3 driver for the RA8D2 Video Input Module (VIN).
 * The block sits between the MIPI CSI-2 receiver and external SDRAM
 * and writes captured frames into one of three memory-base buffers
 * (MB1..MB3). It optionally pre-clips, scales (UDS), look-up-table
 * remaps, and colour-space converts the data en-route.
 *
 * The public API mirrors the register surface documented in HUM
 * Ch 67 "Video Input Module" p 3973-4031:
 *
 * - lifecycle: `ra_vin_init` / `ra_vin_deinit`
 * - capture: `ra_vin_capture_start` (single / continuous /
 * field-skip / interlaced), `ra_vin_capture_stop`
 * - pre-clip: `ra_vin_set_preclip`
 * - scaling: `ra_vin_set_uds_scale` /
 * `ra_vin_set_uds_passband` /
 * `ra_vin_set_uds_clip` /
 * `ra_vin_set_uds_ctrl`
 * - LUT: `ra_vin_lut_program` (256 entries Y/Cb/Cr)
 * - colour conv.: `ra_vin_set_yc_to_rgb` / `ra_vin_set_rgb_to_yc`
 * - dithering: `ra_vin_set_dithering`
 * - YUV-444 mode: `ra_vin_set_yuv444_mode`
 * - DMR options: `ra_vin_set_data_mode`
 * - CSI-2 input: `ra_vin_set_csi_input` /
 * `ra_vin_set_field_detect`
 * - framebuffers: `ra_vin_set_framebuffers` /
 * `ra_vin_set_uv_offset` (YC-separated)
 * - status: `ra_vin_get_status` / `ra_vin_clear_status` /
 * `ra_vin_get_module_status` /
 * `ra_vin_get_line_count` /
 * `ra_vin_get_active_buffer`
 * - IRQ glue: `ra_vin_attach_handler` + `ra_vin_dispatch` +
 * `ra_vin_set_interrupt_enable` +
 * `ra_vin_set_scanline_compare`
 * - power: `ra_vin_enter_stop` / `ra_vin_exit_stop` /
 * `ra_vin_reset`
 *
 * The driver enforces NASA Power-of-10 rules: every loop has a fixed
 * upper bound, every public function returns an `ra_err_t`, and pre/
 * post-condition checks run in both release and unit-test builds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_vin_regs.h"
#include "ra_err.h"

/**
 * @enum ra_vin_input_fmt_t
 * @brief Camera input format for the MC.INF[2:0] field.
 *
 * @details
 * Values map directly to MC.INF as defined in HUM Ch 67.2.1 p 3975
 * Table 67.3 "Captured data formats" p 3977. The driver only routes
 * the value into the MC register; pixel-depth-specific colour-space
 * conversion is the caller's responsibility.
 */
typedef enum : uint8_t {
  k_ra_vin_input_ycbcr422_8  = 1U, /**< 8-bit YCbCr-422. */
  k_ra_vin_input_ycbcr422_10 = 3U, /**< 10-bit YCbCr-422. */
  k_ra_vin_input_raw8        = 4U, /**< RAW8 user-defined data. */
  k_ra_vin_input_rgb888      = 6U, /**< 24-bit RGB888. */
} ra_vin_input_fmt_t;

/**
 * @enum ra_vin_capture_mode_t
 * @brief Single frame vs. continuous capture for `ra_vin_capture_start`.
 *
 * @details
 * `single` arms one frame into MB1; `continuous` rolls MB1 -> MB2 ->
 * MB3 -> MB1 ->...; `interlaced_field_skip` honours MC.IM (set via
 * `ra_vin_set_interlace_mode`) and only writes the requested field.
 */
typedef enum : uint8_t {
  k_ra_vin_capture_single                = 0U, /**< Capture one frame into MB1. */
  k_ra_vin_capture_continuous            = 1U, /**< Roll MB1 -> MB2 -> MB3 ->.. */
  k_ra_vin_capture_continuous_field_skip = 2U, /**< Continuous, honour IM mode. */
} ra_vin_capture_mode_t;

/**
 * @struct ra_vin_config_t
 * @brief Initial configuration for `ra_vin_init`.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in `ra_vin_init` in
 * `libs/ra_hal/src/ra_vin.c`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_vin_input_fmt_t input_fmt;          /**< MC.INF: input format. */
  bool               bypass_csc;         /**< MC.BPS: skip colour conversion. */
  bool               big_endian;         /**< MC.EN: pack big-endian to memory. */
  uint8_t            interlace_mode;     /**< MC.IM (`ra_vin_im_value_t`). */
  uint8_t            pixel_clip_mode;    /**< MC.CLP (`ra_vin_clp_value_t`). */
  uint16_t           image_stride_px;    /**< IS register, in pixels. */
  uint32_t           framebuffer_addr_1; /**< MB1 (must be 64-byte aligned). */
  uint32_t           framebuffer_addr_2; /**< MB2 (continuous mode only). */
  uint32_t           framebuffer_addr_3; /**< MB3 (continuous mode only). */
  uint32_t           interrupt_enable;   /**< IE register mask. */
  uint16_t           scanline_compare;   /**< SI register; 0 disables. */
} ra_vin_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_preclip_t
 * @brief Pre-clip window (SLPRC / ELPRC / SPPRC / EPPRC).
 *
 * @details
 * HUM Ch 67.2.4..67.2.7 p 3980-3981. Lines and pixels are inclusive
 * 12-bit indices; setting end < start triggers `k_ra_err_invalid_arg`
 * from the setter.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t line_start;  /**< SLPRC: first line that survives the clip. */
  uint16_t line_end;    /**< ELPRC: last line that survives the clip. */
  uint16_t pixel_start; /**< SPPRC: first pixel within each line. */
  uint16_t pixel_end;   /**< EPPRC: last pixel within each line. */
} ra_vin_preclip_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_uds_scale_t
 * @brief UDS scaling factors for the V and H axes.
 *
 * @details
 * HUM Ch 67.2.22 p 3997. Each axis carries a 4-bit integer mantissa
 * and a 12-bit fractional part. Set both halves of an axis to zero
 * to disable scaling on that axis (UDS_SCALE = 0).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  v_mantissa; /**< VMANT[3:0]. */
  uint16_t v_fraction; /**< VFRAC[11:0]. */
  uint8_t  h_mantissa; /**< HMANT[3:0]. */
  uint16_t h_fraction; /**< HFRAC[11:0]. */
} ra_vin_uds_scale_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_uds_ctrl_t
 * @brief UDS interpolation control bits.
 *
 * @details
 * HUM Ch 67.2.21 p 3996. `multitap` selects multi-tap vs bilinear/
 * nearest; the per-channel `*_nearest` flags only matter when
 * `multitap` is false.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool b_cb_nearest; /**< NE_BCB: 1 = nearest-neighbour for B/Cb. */
  bool g_y_nearest;  /**< NE_GY: 1 = nearest-neighbour for G/Y. */
  bool r_cr_nearest; /**< NE_RCR: 1 = nearest-neighbour for R/Cr. */
  bool multitap;     /**< BC: 1 = multi-tap, 0 = bilinear/nearest. */
  bool advanced_bl;  /**< BLADV: advanced bilinear-mode tweak. */
  bool advanced_amd; /**< AMD: advanced pixel-count formula. */
} ra_vin_uds_ctrl_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_yc_to_rgb_t
 * @brief Coefficients for the YC -> RGB matrix (CSCEn).
 *
 * @details
 * HUM Ch 67.2.36..67.2.39 p 4011-4015. Wraps the four CSCE registers.
 * - CSCE1: Y multiplier (14-bit) + ROUND.
 * - CSCE2: Y subtract / CbCr subtract (each 12-bit).
 * - CSCE3: low/high 14-bit multipliers used by Cr.
 * - CSCE4: low/high 14-bit multipliers used by Cb.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t y_multiplier; /**< YMUL2[13:0]. */
  bool     enable_round; /**< ROUND[16]. */
  uint16_t y_sub;        /**< YSUB2[27:16]. */
  uint16_t cbcr_sub;     /**< CSUB2[11:0]. */
  uint16_t cr_mul_lo;    /**< CSCE3 low [13:0]. */
  uint16_t cr_mul_hi;    /**< CSCE3 high[29:16]. */
  uint16_t cb_mul_lo;    /**< CSCE4 low [13:0]. */
  uint16_t cb_mul_hi;    /**< CSCE4 high[29:16]. */
} ra_vin_yc_to_rgb_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_rgb_to_yc_chan_t
 * @brief One RGB-to-YC matrix row (Y, Cb, or Cr).
 *
 * @details
 * HUM Ch 67.2.27..67.2.35 p 4002-4011. The R/G/B coefficients are
 * 13/12/12-bit signed values; the additive offset is 12-bit and the
 * shift / round controls live in setting 3.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t r_coeff;    /**< Setting 1 R coefficient. */
  uint16_t g_coeff;    /**< Setting 2 G coefficient. */
  uint16_t b_coeff;    /**< Setting 2 B coefficient. */
  uint16_t add_offset; /**< Setting 3 additive offset (LAP). */
  bool     enable_hen; /**< Setting 3 round-off enable (LHEN). */
  uint8_t  shift_down; /**< Setting 3 shift-down volume (LSFT, 5-bit). */
} ra_vin_rgb_to_yc_chan_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_data_mode_t
 * @brief Decoded view of the DMR (Data Mode) register.
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Wraps DTMD / ABIT / BPSM / EXRGB / YC_THR /
 * YMODE / A8BIT in one C struct so the driver can build the full
 * 32-bit register from a typed view.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t conv_mode;  /**< DTMD (`ra_vin_dtmd_value_t`). */
  bool    alpha_bit;  /**< ABIT for ARGB-1555 alpha. */
  bool    byte_swap;  /**< BPSM swap output bytes. */
  bool    extend_rgb; /**< EXRGB pad RGB to 32 bits. */
  bool    yc_through; /**< YC_THR YC pass-through mode. */
  uint8_t y_mode;     /**< YMODE (`ra_vin_ymode_value_t`). */
  uint8_t alpha_byte; /**< A8BIT ARGB-8888 alpha byte. */
} ra_vin_data_mode_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_csi_input_t
 * @brief CSI_IFMD parameter view.
 *
 * @details
 * HUM Ch 67.2.8 p 3982. Selects virtual channel, MIPI CSI-2 data
 * type, and whether to sign-extend (false) or zero-extend (true)
 * the data lane samples.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t virtual_channel; /**< VC_SEL (0..15). */
  uint8_t data_type;       /**< DT (`ra_vin_csi_dt_value_t`). */
  bool    zero_extend;     /**< DES0: 1 = zero-extend, 0 = sign-extend. */
} ra_vin_csi_input_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_field_detect_t
 * @brief CSIFLD parameter view.
 *
 * @details
 * HUM Ch 67.2.9 p 3983.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool    enable;         /**< FLD_EN. */
  bool    even_field_sel; /**< FLD_SEL (bit-4 of FLD_SEL[5:4]). */
  uint8_t even_field_num; /**< FLD_NUM (`ra_vin_field_polarity_t`). */
} ra_vin_field_detect_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_vin_module_status_t
 * @brief Decoded snapshot of the MS (Module Status) register.
 *
 * @details
 * HUM Ch 67.2.2 p 3978. `frame_buffer_id` / `latest_frame_buffer`
 * carry `ra_vin_ms_fbs_t` values.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool    capture_active;      /**< MS.CA. */
  bool    active_video;        /**< MS.AV. */
  bool    even_field;          /**< MS.FS (1 -> even). */
  uint8_t frame_buffer_id;     /**< MS.FBS. */
  bool    memory_active;       /**< MS.MA. */
  uint8_t latest_frame_buffer; /**< MS.FMS. */
} ra_vin_module_status_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_vin_event_fn_t
 * @brief VIN event callback (frame complete / overflow / VSYNC).
 * @param[in] ctx Caller-supplied context.
 * @param[in] status_mask Snapshot of the INTS register at the time
 * of dispatch.
 */
typedef void (*ra_vin_event_fn_t)(void* ctx, uint32_t status_mask);

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
 * before `ra_vin_capture_start`.
 *
 * @par State Machine
 * @startuml
 * [*] --> uninitialised
 * uninitialised --> initialised: ra_vin_init
 * initialised --> capturing: ra_vin_capture_start
 * capturing --> initialised: ra_vin_capture_stop
 * initialised --> uninitialised: ra_vin_deinit
 * @enduml
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Success.
 * @retval k_ra_err_null_ptr cfg was NULL.
 * @retval k_ra_err_invalid_arg cfg.image_stride_px was 0 or
 * cfg.interlace_mode / pixel_clip_mode
 * out of range.
 * @retval k_ra_err_hw_init_failed MSTP enable failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre `cfg->framebuffer_addr_1` is 64-byte aligned.
 * @post MC reflects `cfg`, ME bit is cleared.
 * @post INTS is fully cleared.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_start
 * @see ra_vin_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_init(const ra_vin_config_t* cfg);

/**
 * @brief Tear down VIN: stop capture, mask interrupts, drop MSTP.
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Driver has been initialised (otherwise this is a no-op write).
 * @post MC.ME and FC.CC are 0.
 * @post Registered callback (if any) is cleared.
 *
 * @note Not thread-safe.
 * @see ra_vin_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_deinit(void);

/**
 * @brief Issue a soft reset by re-running the MC.ST settle sequence.
 *
 * @details
 * HUM Ch 67.3.1 p 3995 -- writes ME = 0, pulses MC.ST = 1, then
 * absorbs the >= 10 ICLK settling time by reading MC back the
 * configured number of times (`k_ra_vin_st_settle_reads`).
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre `ra_vin_init` succeeded.
 * @pre Caller has masked IRQs.
 * @post MC.ME = 0.
 * @post Internal pipeline state is back to its post-startup defaults.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_reset(void);

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
 * skip (`ra_vin_capture_mode_t`).
 *
 * @return ra_err_t
 * @retval k_ra_ok Capture armed and running.
 * @retval k_ra_err_invalid_state ME or CC was already 1.
 * @retval k_ra_err_invalid_arg Unknown `mode` value.
 *
 * @pre `ra_vin_init` has been called.
 * @pre A valid frame buffer is programmed into MB1.
 * @post MC.ME = 1.
 * @post FC.CC reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_capture_arm(ra_vin_capture_mode_t mode);

/**
 * @brief Stop the current capture and drain outstanding AXI traffic.
 *
 * @details
 * Implements HUM Ch 67.3.6 "Stopping capture operation" p 4006:
 * 1. Clear FC.CC so no new frames are queued.
 * 2. Clear MC.ME.
 * 3. Set MTCSTOP.STOPREQ to push outstanding writes to completion;
 * the driver polls MTCSTOP.STOPACK / OUTSTAND a bounded number
 * of times (`k_ra_vin_stop_drain_max`) before returning
 * `k_ra_err_hw_timeout`.
 *
 * @return ra_err_t
 * @retval k_ra_ok Stop sequence drained successfully.
 * @retval k_ra_err_invalid_state ME was already 0 (nothing to stop).
 * @retval k_ra_err_hw_timeout OUTSTAND did not reach 0 in time.
 *
 * @pre `ra_vin_init` has been called.
 * @pre Capture is currently active (MC.ME = 1).
 * @post MC.ME and FC.CC are both 0.
 * @post MTCSTOP.STOPREQ is asserted, outstanding AXI writes drained.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_capture_disarm(void);

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
 * @return ra_err_t
 * @retval k_ra_ok Window programmed.
 * @retval k_ra_err_null_ptr window was NULL.
 * @retval k_ra_err_invalid_arg end < start or value > 4095.
 *
 * @pre Driver initialised, capture not running.
 * @pre `window != NULL`.
 * @post SLPRC/ELPRC/SPPRC/EPPRC reflect `window`.
 * @post Pre-clip-violation IRQs (PRCLIPH/PRCLIPV) become meaningful.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_clip
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_preclip(const ra_vin_preclip_t* window);

/**
 * @brief Programme the UDS scaling factor (UDS_SCALE).
 *
 * @details
 * HUM Ch 67.2.22 p 3997. Set the integer mantissa and 12-bit fraction
 * for both axes; the driver enables MC.SCLE only via
 * `ra_vin_set_uds_ctrl(....multitap = ?,...)` plus
 * `ra_vin_enable_scaling(true)`. UDS_SCALE itself is loaded by
 * `ra_vin_capture_start` matching HUM Ch 67.3.5 p 4006.
 *
 * @param[in] scale Non-NULL scaling factor descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Scale programmed.
 * @retval k_ra_err_null_ptr scale was NULL.
 * @retval k_ra_err_invalid_arg Mantissa > 15 or fraction > 4095.
 *
 * @pre Driver initialised.
 * @pre `scale != NULL`.
 * @post UDS_SCALE reflects `scale`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_passband
 * @see ra_vin_set_uds_clip
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_uds_scale(const ra_vin_uds_scale_t* scale);

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
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg Value > 127.
 *
 * @pre Driver initialised.
 * @pre v_bwidth + h_bwidth in legal 7-bit range.
 * @post UDS_PASS_BWIDTH reflects the new values.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_uds_passband(uint8_t v_bwidth, uint8_t h_bwidth);

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
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg Value > 4095.
 *
 * @pre Driver initialised.
 * @pre Both sizes <= 4095.
 * @post UDS_CLIP_SIZE reflects the new values.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_uds_clip(uint16_t v_size, uint16_t h_size);

/**
 * @brief Programme UDS interpolation control (UDS_CTRL).
 *
 * @details
 * HUM Ch 67.2.21 p 3996. Picks bilinear vs nearest-neighbour per
 * channel, plus the multi-tap mode flag and two advanced-mode
 * tweaks. `ra_vin_enable_scaling` toggles the MC.SCLE master enable.
 *
 * @param[in] ctrl Non-NULL UDS control descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_null_ptr ctrl was NULL.
 *
 * @pre Driver initialised.
 * @pre `ctrl != NULL`.
 * @post UDS_CTRL reflects `ctrl`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_uds_ctrl(const ra_vin_uds_ctrl_t* ctrl);

/**
 * @brief Toggle the MC.SCLE master scaling-enable bit.
 *
 * @param[in] enable True -> SCLE = 1, false -> SCLE = 0.
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Driver initialised.
 * @pre Capture not running (caller stops first if needed).
 * @post MC.SCLE matches `enable`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uds_scale
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_enable_scaling(bool enable);

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
 * channel; passing NULL for `y` returns `k_ra_err_null_ptr`.
 *
 * `enable` then drives MC.LUTE so the hardware actually consumes the
 * table on the next frame.
 *
 * @param[in] y_table Pointer to 256 8-bit Y values (must be non-NULL).
 * @param[in] cb_table Pointer to 256 8-bit Cb values (NULL -> use Y).
 * @param[in] cr_table Pointer to 256 8-bit Cr values (NULL -> use Y).
 * @param[in] enable Drive MC.LUTE after programming.
 *
 * @return ra_err_t
 * @retval k_ra_ok LUT programmed.
 * @retval k_ra_err_null_ptr y_table was NULL.
 *
 * @pre Driver initialised.
 * @pre Capture not running.
 * @post All 256 LUT entries reflect the supplied tables.
 * @post MC.LUTE matches `enable`.
 *
 * @note Not thread-safe; writes 256 LUTP/LUTD pairs.
 * @see ra_vin_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_lut_program(const uint8_t* y_table,
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
 * @return ra_err_t
 * @retval k_ra_ok Coefficients written.
 * @retval k_ra_err_null_ptr coeffs was NULL.
 *
 * @pre Driver initialised, capture not running.
 * @pre `coeffs != NULL`.
 * @post CSCE1..4 reflect `coeffs`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_rgb_to_yc
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_yc_to_rgb(const ra_vin_yc_to_rgb_t* coeffs);

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
 * @return ra_err_t
 * @retval k_ra_ok Row written.
 * @retval k_ra_err_null_ptr row was NULL.
 * @retval k_ra_err_invalid_arg channel > 2 or shift_down > 31.
 *
 * @pre Driver initialised, capture not running.
 * @pre `row != NULL` and channel < 3.
 * @post YCCRn / CBCCRn / CRCCRn reflect `row`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_yc_to_rgb
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_rgb_to_yc(uint8_t channel, const ra_vin_rgb_to_yc_chan_t* row);

/* =============================================================================
 * Dithering / YUV-444 / DMR
 * =============================================================================
 */

/**
 * @brief Configure the dithering mode (MC.DC + MC.DC2).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. The DC field picks one of three patterns
 * (`ra_vin_dc_value_t`); the DC2 bit reverses the direction of the
 * ordered patterns. Caller is responsible for keeping the input pixel
 * depth large enough for dithering to be meaningful.
 *
 * @param[in] mode Dither pattern (`ra_vin_dc_value_t`).
 * @param[in] direction True -> DC2 = 1, false -> DC2 = 0.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg mode > 2.
 *
 * @pre Driver initialised, capture not running.
 * @pre Mode value is one of `k_ra_vin_dc_*`.
 * @post MC.DC and MC.DC2 reflect the new values.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_yuv444_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_dithering(uint8_t mode, bool direction);

/**
 * @brief Configure the YUV-444 conversion mode (MC.YUV444).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. Picks data-extend vs interpolate when
 * promoting 4:2:2 input to 4:4:4 internally.
 *
 * @param[in] mode `ra_vin_yuv444_value_t`.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg mode > 1.
 *
 * @pre Driver initialised, capture not running.
 * @pre Mode is one of `k_ra_vin_yuv444_*`.
 * @post MC.YUV444 matches `mode`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_dithering
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_yuv444_mode(uint8_t mode);

/**
 * @brief Set the interlace mode (MC.IM).
 *
 * @param[in] mode `ra_vin_im_value_t`.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg mode > 2.
 *
 * @pre Driver initialised, capture not running.
 * @pre Mode is one of `k_ra_vin_im_*`.
 * @post MC.IM reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_interlace_mode(uint8_t mode);

/**
 * @brief Programme the DMR (Data Mode Register).
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Composes DTMD / ABIT / BPSM / EXRGB /
 * YC_THR / YMODE / A8BIT into a single 32-bit write.
 *
 * @param[in] mode Non-NULL DMR descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_null_ptr mode was NULL.
 * @retval k_ra_err_invalid_arg conv_mode > 2 or y_mode > 3.
 *
 * @pre Driver initialised, capture not running.
 * @pre `mode != NULL`.
 * @post DMR reflects `mode`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_uv_offset
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_data_mode(const ra_vin_data_mode_t* mode);

/* =============================================================================
 * CSI-2 / framebuffers
 * =============================================================================
 */

/**
 * @brief Programme CSI_IFMD (virtual channel + data type + extension).
 *
 * @param[in] input Non-NULL CSI input descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_null_ptr input was NULL.
 * @retval k_ra_err_invalid_arg virtual_channel > 15 or data_type > 0x3F.
 *
 * @pre Driver initialised, capture not running.
 * @pre `input != NULL`.
 * @post CSI_IFMD reflects `input`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_field_detect
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_csi_input(const ra_vin_csi_input_t* input);

/**
 * @brief Programme CSIFLD (field detection).
 *
 * @param[in] detect Non-NULL field-detect descriptor.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_null_ptr detect was NULL.
 * @retval k_ra_err_invalid_arg even_field_num > 1.
 *
 * @pre Driver initialised.
 * @pre `detect != NULL`.
 * @post CSIFLD reflects `detect`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_csi_input
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_field_detect(const ra_vin_field_detect_t* detect);

/**
 * @brief Re-programme MB1 / MB2 / MB3 (ping-pong framebuffers).
 *
 * @details
 * HUM Ch 67.2.10..67.2.12 p 3985-3987. Lower 7 address bits are
 * forced to zero (64-byte alignment); the function rejects unaligned
 * inputs with `k_ra_err_invalid_arg`.
 *
 * @param[in] mb1 New MB1 base; ignored if zero.
 * @param[in] mb2 New MB2 base; ignored if zero.
 * @param[in] mb3 New MB3 base; ignored if zero.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg Any non-zero base unaligned.
 *
 * @pre Driver initialised.
 * @pre At least one of mb1/mb2/mb3 is non-zero.
 * @post Specified MBn registers reflect the new bases.
 *
 * @note Not thread-safe; safe to call mid-capture per HUM Ch 67.3.4
 * p 4005 ("Switching the buffer during operation").
 * @see ra_vin_get_active_buffer
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_framebuffers(uint32_t mb1, uint32_t mb2, uint32_t mb3);

/**
 * @brief Programme UVAOF (UV plane address offset for YC-separated mode).
 *
 * @param[in] uv_addr 64-byte-aligned UV plane base; 0 disables.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg Address not 64-byte aligned.
 *
 * @pre Driver initialised.
 * @pre `uv_addr` is either zero or 64-byte aligned.
 * @post UVAOF reflects `uv_addr`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_data_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_uv_offset(uint32_t uv_addr);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Snapshot the INTS register.
 *
 * @param[out] out_mask Receives the current INTS value.
 *
 * @return ra_err_t
 * @retval k_ra_ok Snapshot taken.
 * @retval k_ra_err_null_ptr out_mask was NULL.
 *
 * @pre `out_mask != NULL`.
 * @pre Driver is in any post-init state.
 * @post `*out_mask` contains the value of INTS.
 *
 * @note Re-entrant; just a register read.
 * @see ra_vin_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_get_status(uint32_t* out_mask);

/**
 * @brief Clear write-1-to-clear bits in the INTS register.
 *
 * @details
 * Only the bits in `mask` that intersect `k_ra_vin_int_w1c_mask`
 * are written back; all other bits are gated off so a careless
 * caller cannot toggle status bits the hardware owns.
 *
 * @param[in] mask Bits to clear (subset of `ra_vin_int_mask_t`).
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Driver is in any post-init state.
 * @post Bits in `mask & k_ra_vin_int_w1c_mask` read as 0 next access.
 *
 * @note Re-entrant; single masked register write.
 * @see ra_vin_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_clear_status(uint32_t mask);

/**
 * @brief Decode the MS register into a typed snapshot.
 *
 * @param[out] out_status Non-NULL destination.
 *
 * @return ra_err_t
 * @retval k_ra_ok Snapshot taken.
 * @retval k_ra_err_null_ptr out_status was NULL.
 *
 * @pre `out_status != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_status` decoded.
 *
 * @note Re-entrant.
 * @see ra_vin_get_active_buffer
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_get_module_status(ra_vin_module_status_t* out_status);

/**
 * @brief Read the LC (line count) register.
 *
 * @param[out] out_count Non-NULL destination.
 *
 * @return ra_err_t
 * @retval k_ra_ok Read OK.
 * @retval k_ra_err_null_ptr out_count was NULL.
 *
 * @pre `out_count != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_count` carries 0..4095.
 *
 * @note Re-entrant.
 * @see ra_vin_get_module_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_get_line_count(uint16_t* out_count);

/**
 * @brief Return the active framebuffer index from MS.FBS.
 *
 * @param[out] out_id Non-NULL destination (`ra_vin_ms_fbs_t`).
 *
 * @return ra_err_t
 * @retval k_ra_ok Read OK.
 * @retval k_ra_err_null_ptr out_id was NULL.
 *
 * @pre `out_id != NULL`.
 * @pre Driver in any post-init state.
 * @post `*out_id` carries 0..3.
 *
 * @note Re-entrant.
 * @see ra_vin_get_module_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_get_active_buffer(uint8_t* out_id);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Update the IE (Interrupt Enable) mask at runtime.
 *
 * @param[in] mask Bitwise OR of `ra_vin_int_mask_t` values.
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Driver initialised.
 * @pre Caller has masked the VIN IRQ if updating mid-capture.
 * @post IE register equals `mask`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_scanline_compare
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_interrupt_enable(uint32_t mask);

/**
 * @brief Programme the SI (Scanline Interrupt) compare register.
 *
 * @param[in] line 12-bit compare value, 0 disables.
 *
 * @return ra_err_t
 * @retval k_ra_ok Programmed.
 * @retval k_ra_err_invalid_arg line > 4095.
 *
 * @pre Driver initialised.
 * @pre `line` <= 4095.
 * @post SI register equals `line`.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_interrupt_enable
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_scanline_compare(uint16_t line);

/**
 * @brief Attach a VIN event callback (shared between status + error IRQs).
 *
 * @param[in] fn Callback invoked from `ra_vin_dispatch`. May be NULL
 * to detach.
 * @param[in] ctx Context value forwarded to the callback.
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Caller may not invoke this from within the callback.
 * @pre Single-writer context (init or IRQ-masked).
 * @post Subsequent `ra_vin_dispatch` calls invoke `fn(ctx, ints)`.
 *
 * @note Not thread-safe; callback storage is plain static state.
 * @see ra_vin_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_attach_handler(ra_vin_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot INTS, fire callback, then W1C the latched bits.
 *
 * @details
 * Intended to be called from the VIN status / error ISR. Reads the
 * current INTS value, hands it to the registered callback (if any),
 * and writes back the W1C subset to acknowledge the event with the
 * hardware.
 *
 * @return None.
 * @retval None
 *
 * @pre Called from a single CPU context.
 * @pre `ra_vin_init` was successful.
 * @post W1C bits in INTS read as 0 after this returns.
 * @post Callback (if any) has run exactly once with the snapshot.
 *
 * @note Re-entrant only across distinct VIN instances (there is one).
 *       See HUM Ch 47 "Video Input Module (VIN)" pp 2455-2540.
 * @see ra_vin_attach_handler
 * @since 0.1.0
 */
void ra_vin_dispatch(void);

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
 * peripherals share `k_ra_mstp_mipi_csi`.
 *
 * @return ra_err_t error code from `ra_mstp_disable`.
 *
 * @pre Caller has stopped any in-flight CSI traffic.
 * @pre Driver was previously initialised.
 * @post MC.ME = 0.
 * @post MIPI CSI MSTP gate is closed.
 *
 * @note Not thread-safe.
 * @see ra_vin_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_enter_stop(void);

/**
 * @brief Re-enable the VIN clock gate after `ra_vin_enter_stop`.
 *
 * @return ra_err_t error code from `ra_mstp_enable`.
 *
 * @pre `ra_vin_enter_stop` was called.
 * @pre Caller will re-issue `ra_vin_capture_start` afterwards.
 * @post MIPI CSI MSTP gate is open.
 * @post VIN registers are accessible again.
 *
 * @note Not thread-safe.
 * @see ra_vin_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_exit_stop(void);

/* =============================================================================
 * Sweep 17: dynamic capture window + frame-end IRQ
 * =============================================================================
 */

/**
 * @typedef ra_vin_frame_fn_t
 * @brief Frame-end callback registered via `ra_vin_attach_frame_handler`.
 *
 * @details
 * Distinct from the generic INTS-mask callback registered through
 * `ra_vin_attach_handler` -- this one fires from `ra_vin_dispatch`
 * only when the FIE (frame-end) bit is set, and receives the buffer
 * base + byte length of the just-completed frame as decoded from the
 * driver's most recent `ra_vin_capture_start` call.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] buf Buffer base whose frame just completed.
 * @param[in] len Frame length in bytes (= width * height * bytes_per_pixel).
 *
 * @since 0.1.0
 */
typedef void (*ra_vin_frame_fn_t)(void* ctx, void* buf, uint32_t len);

/**
 * @brief Start a single-frame capture into `buf` with the supplied geometry.
 *
 * @details
 * High-level wrapper around `ra_vin_capture_arm(k_ra_vin_capture_single)`
 * that:
 *  1. Programmes MB1 with `(uint32_t)(uintptr_t)buf` (HUM Ch 67.2.10
 *     "MB1: Memory Base 1 Register" p 3985).
 *  2. Sets the IS register (HUM Ch 67.2.13 "IS: Image Stride Register"
 *     p 3984) and pre-clip end coordinates to define the capture window.
 *  3. Updates MC.INF (HUM Ch 67.2.1 "MC: Main Control Register" p 3975)
 *     so the input format matches.
 *  4. Caches `buf`/`w`/`h` so the frame-end handler (registered via
 *     `ra_vin_attach_frame_handler`) sees the right buffer + length.
 *  5. Calls `ra_vin_capture_arm(k_ra_vin_capture_single)`.
 *
 * @param[in] buf Capture target (must be 64-byte aligned).
 * @param[in] w Width in pixels (1..4096).
 * @param[in] h Height in lines (1..4096).
 * @param[in] format Input format (`ra_vin_input_fmt_t`).
 *
 * @return ra_err_t
 * @retval k_ra_ok Capture armed.
 * @retval k_ra_err_null_ptr buf was NULL.
 * @retval k_ra_err_invalid_arg w/h out of range or buf misaligned.
 * @retval k_ra_err_invalid_state ME or CC was already 1.
 *
 * @pre `ra_vin_init` succeeded.
 * @pre `buf` is 64-byte aligned.
 * @post MB1 == (uint32_t)(uintptr_t)buf.
 * @post MC.ME == 1.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_stop
 * @see ra_vin_attach_frame_handler
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_vin_capture_start(void* buf, uint16_t w, uint16_t h, ra_vin_input_fmt_t format);

/**
 * @brief Stop the in-flight capture started via `ra_vin_capture_start`.
 *
 * @details
 * Wraps `ra_vin_capture_disarm` -- exists so the high-level start /
 * stop pair share matching names.
 *
 * @return ra_err_t error code from `ra_vin_capture_disarm`.
 *
 * @pre `ra_vin_capture_start` was previously called.
 * @post MC.ME and FC.CC are both 0.
 *
 * @note Not thread-safe.
 * @see ra_vin_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_capture_stop(void);

/**
 * @brief Programme a region-of-interest pre-clip window.
 *
 * @details
 * Wraps `ra_vin_set_preclip` with a (x, y, w, h) signature for callers
 * that prefer (x, y) over (start_line, start_pixel). HUM Ch 67.2.4..
 * 67.2.7 p 3980-3981.
 *
 * @param[in] x Top-left pixel index (0..4095).
 * @param[in] y Top-left line index (0..4095).
 * @param[in] w Window width in pixels (>=1).
 * @param[in] h Window height in lines (>=1).
 *
 * @return ra_err_t
 * @retval k_ra_ok Window programmed.
 * @retval k_ra_err_invalid_arg Arguments out of range.
 *
 * @pre Driver initialised, capture not running.
 * @pre `w >= 1`, `h >= 1`, `x + w - 1 <= 4095`, `y + h - 1 <= 4095`.
 * @post SLPRC/ELPRC/SPPRC/EPPRC reflect the requested window.
 *
 * @note Not thread-safe.
 * @see ra_vin_set_preclip
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Register the frame-end IRQ callback.
 *
 * @details
 * The driver's generic `ra_vin_dispatch` fans every INTS bit through
 * a single mask-style callback. This frame-end variant is invoked
 * only when INTS.FIE is set after a `ra_vin_capture_start` call, and
 * receives the cached buffer + length.
 *
 * @param[in] fn Callback (NULL detaches).
 * @param[in] ctx Forwarded to the callback.
 *
 * @return ra_err_t
 * @retval k_ra_ok Always.
 *
 * @pre Single-writer context (init or IRQ-masked).
 * @post Subsequent `ra_vin_dispatch` calls fire `fn` on FIE.
 *
 * @note Not thread-safe.
 * @see ra_vin_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_vin_attach_frame_handler(ra_vin_frame_fn_t fn, void* ctx);

#ifdef __cplusplus
}
#endif
