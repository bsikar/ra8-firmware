/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_vin_types.h
 * @brief Video Input Module (VIN) configuration descriptors and types
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Type definitions for the RA8D2 Video Input Module (VIN) HAL driver:
 * the typed enums (input format, capture mode), the configuration
 * descriptor `ra8_vin_config_t`, the pre-clip / UDS-scale / UDS-ctrl
 * register-bundle structs, the colour-space coefficient structs, the
 * decoded data-mode / CSI-input / field-detect / module-status views,
 * and the event / frame-end callback typedefs. The function prototypes
 * that consume these types live in `ra8_vin_api.h`; both are aggregated
 * by the thin umbrella `ra8_vin.h`. See that umbrella for the full
 * driver overview and state machine.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_vin_regs.h"

/**
 * @enum ra8_vin_input_fmt_t
 * @brief Camera input format for the MC.INF[2:0] field.
 *
 * @details
 * Values map directly to MC.INF as defined in HUM Ch 67.2.1 p 3975
 * Table 67.3 "Captured data formats" p 3977. The driver only routes
 * the value into the MC register; pixel-depth-specific colour-space
 * conversion is the caller's responsibility.
 */
typedef enum : uint8_t {
  k_ra8_vin_input_ycbcr422_8  = 1U, /**< 8-bit YCbCr-422.        */
  k_ra8_vin_input_ycbcr422_10 = 3U, /**< 10-bit YCbCr-422.       */
  k_ra8_vin_input_raw8        = 4U, /**< RAW8 user-defined data. */
  k_ra8_vin_input_rgb888      = 6U, /**< 24-bit RGB888.          */
} ra8_vin_input_fmt_t;

/**
 * @enum ra8_vin_capture_mode_t
 * @brief Single frame vs. continuous capture for `ra8_vin_capture_start`.
 *
 * @details
 * `single` arms one frame into MB1; `continuous` rolls MB1 -> MB2 ->
 * MB3 -> MB1 ->...; `interlaced_field_skip` honours MC.IM (set via
 * `ra8_vin_set_interlace_mode`) and only writes the requested field.
 */
typedef enum : uint8_t {
  k_ra8_vin_capture_single                = 0U, /**< Capture one frame into MB1. */
  k_ra8_vin_capture_continuous            = 1U, /**< Roll MB1 -> MB2 -> MB3 ->.. */
  k_ra8_vin_capture_continuous_field_skip = 2U, /**< Continuous, honour IM mode. */
} ra8_vin_capture_mode_t;

/**
 * @struct ra8_vin_config_t
 * @brief Initial configuration for `ra8_vin_init`.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in `ra8_vin_init` in
 * `libs/ra8_hal/src/ra8_vin.c`.
 */
typedef struct {
  ra8_vin_input_fmt_t input_fmt;          /**< MC.INF: input format.             */
  bool                bypass_csc;         /**< MC.BPS: skip colour conversion.   */
  bool                big_endian;         /**< MC.EN: pack big-endian to memory. */
  uint8_t             interlace_mode;     /**< MC.IM (`ra8_vin_im_value_t`).     */
  uint8_t             pixel_clip_mode;    /**< MC.CLP (`ra8_vin_clp_value_t`).   */
  uint16_t            image_stride_px;    /**< IS register, in pixels.           */
  uint32_t            framebuffer_addr_1; /**< MB1 (must be 64-byte aligned).    */
  uint32_t            framebuffer_addr_2; /**< MB2 (continuous mode only).       */
  uint32_t            framebuffer_addr_3; /**< MB3 (continuous mode only).       */
  uint32_t            interrupt_enable;   /**< IE register mask.                 */
  uint16_t            scanline_compare;   /**< SI register; 0 disables.          */
} ra8_vin_config_t;

/**
 * @struct ra8_vin_preclip_t
 * @brief Pre-clip window (SLPRC / ELPRC / SPPRC / EPPRC).
 *
 * @details
 * HUM Ch 67.2.4..67.2.7 p 3980-3981. Lines and pixels are inclusive
 * 12-bit indices; setting end < start triggers `k_ra8_err_invalid_arg`
 * from the setter.
 */
typedef struct {
  uint16_t line_start;  /**< SLPRC: first line that survives the clip. */
  uint16_t line_end;    /**< ELPRC: last line that survives the clip.  */
  uint16_t pixel_start; /**< SPPRC: first pixel within each line.      */
  uint16_t pixel_end;   /**< EPPRC: last pixel within each line.       */
} ra8_vin_preclip_t;

/**
 * @struct ra8_vin_uds_scale_t
 * @brief UDS scaling factors for the V and H axes.
 *
 * @details
 * HUM Ch 67.2.22 p 3997. Each axis carries a 4-bit integer mantissa
 * and a 12-bit fractional part. Set both halves of an axis to zero
 * to disable scaling on that axis (UDS_SCALE = 0).
 */
typedef struct {
  uint8_t  v_mantissa; /**< VMANT[3:0].  */
  uint16_t v_fraction; /**< VFRAC[11:0]. */
  uint8_t  h_mantissa; /**< HMANT[3:0].  */
  uint16_t h_fraction; /**< HFRAC[11:0]. */
} ra8_vin_uds_scale_t;

/**
 * @struct ra8_vin_uds_ctrl_t
 * @brief UDS interpolation control bits.
 *
 * @details
 * HUM Ch 67.2.21 p 3996. `multitap` selects multi-tap vs bilinear/
 * nearest; the per-channel `*_nearest` flags only matter when
 * `multitap` is false.
 */
typedef struct {
  bool b_cb_nearest; /**< NE_BCB: 1 = nearest-neighbour for B/Cb.  */
  bool g_y_nearest;  /**< NE_GY: 1 = nearest-neighbour for G/Y.    */
  bool r_cr_nearest; /**< NE_RCR: 1 = nearest-neighbour for R/Cr.  */
  bool multitap;     /**< BC: 1 = multi-tap, 0 = bilinear/nearest. */
  bool advanced_bl;  /**< BLADV: advanced bilinear-mode tweak.     */
  bool advanced_amd; /**< AMD: advanced pixel-count formula.       */
} ra8_vin_uds_ctrl_t;

/**
 * @struct ra8_vin_yc_to_rgb_t
 * @brief Coefficients for the YC -> RGB matrix (CSCEn).
 *
 * @details
 * HUM Ch 67.2.36..67.2.39 p 4011-4015. Wraps the four CSCE registers.
 * - CSCE1: Y multiplier (14-bit) + ROUND.
 * - CSCE2: Y subtract / CbCr subtract (each 12-bit).
 * - CSCE3: low/high 14-bit multipliers used by Cr.
 * - CSCE4: low/high 14-bit multipliers used by Cb.
 */
typedef struct {
  uint16_t y_multiplier; /**< YMUL2[13:0].       */
  bool     enable_round; /**< ROUND[16].         */
  uint16_t y_sub;        /**< YSUB2[27:16].      */
  uint16_t cbcr_sub;     /**< CSUB2[11:0].       */
  uint16_t cr_mul_lo;    /**< CSCE3 low [13:0].  */
  uint16_t cr_mul_hi;    /**< CSCE3 high[29:16]. */
  uint16_t cb_mul_lo;    /**< CSCE4 low [13:0].  */
  uint16_t cb_mul_hi;    /**< CSCE4 high[29:16]. */
} ra8_vin_yc_to_rgb_t;

/**
 * @struct ra8_vin_rgb_to_yc_chan_t
 * @brief One RGB-to-YC matrix row (Y, Cb, or Cr).
 *
 * @details
 * HUM Ch 67.2.27..67.2.35 p 4002-4011. The R/G/B coefficients are
 * 13/12/12-bit signed values; the additive offset is 12-bit and the
 * shift / round controls live in setting 3.
 */
typedef struct {
  uint16_t r_coeff;    /**< Setting 1 R coefficient.                   */
  uint16_t g_coeff;    /**< Setting 2 G coefficient.                   */
  uint16_t b_coeff;    /**< Setting 2 B coefficient.                   */
  uint16_t add_offset; /**< Setting 3 additive offset (LAP).           */
  bool     enable_hen; /**< Setting 3 round-off enable (LHEN).         */
  uint8_t  shift_down; /**< Setting 3 shift-down volume (LSFT, 5-bit). */
} ra8_vin_rgb_to_yc_chan_t;

/**
 * @struct ra8_vin_data_mode_t
 * @brief Decoded view of the DMR (Data Mode) register.
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Wraps DTMD / ABIT / BPSM / EXRGB / YC_THR /
 * YMODE / A8BIT in one C struct so the driver can build the full
 * 32-bit register from a typed view.
 */
typedef struct {
  uint8_t conv_mode;  /**< DTMD (`ra8_vin_dtmd_value_t`).   */
  bool    alpha_bit;  /**< ABIT for ARGB-1555 alpha.        */
  bool    byte_swap;  /**< BPSM swap output bytes.          */
  bool    extend_rgb; /**< EXRGB pad RGB to 32 bits.        */
  bool    yc_through; /**< YC_THR YC pass-through mode.     */
  uint8_t y_mode;     /**< YMODE (`ra8_vin_ymode_value_t`). */
  uint8_t alpha_byte; /**< A8BIT ARGB-8888 alpha byte.      */
} ra8_vin_data_mode_t;

/**
 * @struct ra8_vin_csi_input_t
 * @brief CSI_IFMD parameter view.
 *
 * @details
 * HUM Ch 67.2.8 p 3982. Selects virtual channel, MIPI CSI-2 data
 * type, and whether to sign-extend (false) or zero-extend (true)
 * the data lane samples.
 */
typedef struct {
  uint8_t virtual_channel; /**< VC_SEL (0..15).                         */
  uint8_t data_type;       /**< DT (`ra8_vin_csi_dt_value_t`).          */
  bool    zero_extend;     /**< DES0: 1 = zero-extend, 0 = sign-extend. */
} ra8_vin_csi_input_t;

/**
 * @struct ra8_vin_field_detect_t
 * @brief CSIFLD parameter view.
 *
 * @details
 * HUM Ch 67.2.9 p 3983.
 */
typedef struct {
  bool    enable;         /**< FLD_EN.                               */
  bool    even_field_sel; /**< FLD_SEL (bit-4 of FLD_SEL[5:4]).      */
  uint8_t even_field_num; /**< FLD_NUM (`ra8_vin_field_polarity_t`). */
} ra8_vin_field_detect_t;

/**
 * @struct ra8_vin_module_status_t
 * @brief Decoded snapshot of the MS (Module Status) register.
 *
 * @details
 * HUM Ch 67.2.2 p 3978. `frame_buffer_id` / `latest_frame_buffer`
 * carry `ra8_vin_ms_fbs_t` values.
 */
typedef struct {
  bool    capture_active;      /**< MS.CA.             */
  bool    active_video;        /**< MS.AV.             */
  bool    even_field;          /**< MS.FS (1 -> even). */
  uint8_t frame_buffer_id;     /**< MS.FBS.            */
  bool    memory_active;       /**< MS.MA.             */
  uint8_t latest_frame_buffer; /**< MS.FMS.            */
} ra8_vin_module_status_t;

/**
 * @typedef ra8_vin_event_fn_t
 * @brief VIN event callback (frame complete / overflow / VSYNC).
 * @param[in] ctx Caller-supplied context.
 * @param[in] status_mask Snapshot of the INTS register at the time
 * of dispatch.
 */
typedef void (*ra8_vin_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @typedef ra8_vin_frame_fn_t
 * @brief Frame-end callback registered via `ra8_vin_attach_frame_handler`.
 *
 * @details
 * Distinct from the generic INTS-mask callback registered through
 * `ra8_vin_attach_handler` -- this one fires from `ra8_vin_dispatch`
 * only when the FIE (frame-end) bit is set, and receives the buffer
 * base + byte length of the just-completed frame as decoded from the
 * driver's most recent `ra8_vin_capture_start` call.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] buf Buffer base whose frame just completed.
 * @param[in] len Frame length in bytes (= width * height * bytes_per_pixel).
 *
 * @since 0.1.0
 */
typedef void (*ra8_vin_frame_fn_t)(void* ctx, void* buf, uint32_t len);
