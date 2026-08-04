/**
 * @file ra8_ceu_types.h
 * @brief Capture Engine Unit (CEU) configuration descriptors and types
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Type definitions for the RA8D2 Capture Engine Unit (CEU) HAL
 * driver: the typed enums, register-bundle structs, the full
 * `ra8_ceu_config_t` open-time descriptor, the live-status snapshot
 * struct, the address-bundle struct, and the event-callback typedef.
 * The function prototypes that consume these types live in
 * `ra8_ceu_api.h`; both are aggregated by the thin umbrella
 * `ra8_ceu.h`. See that umbrella for the full driver overview and
 * state machine.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_ceu_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Configuration descriptors
 * =============================================================================
 */

/**
 * @enum ra8_ceu_capture_format_t
 * @brief CAMCR.JPG capture-format select.
 *
 * @details
 * Mirrors FSP `ceu_capture_format_t` and HUM Ch 60.2.3 "CAMCR"
 * (p 3636-3641) JPG[1:0] field. Determines which Y/C separator and
 * which fetch state-machine the CEU runs:
 *  - `image_capture`    -- YCbCr 4:2:2 input split into separate
 *                          luminance / chrominance buffers (CDAYR /
 *                          CDACR).
 *  - `data_synchronous` -- the sensor lines are sampled directly
 *                          on HD/VD timing into a single linear
 *                          buffer (CDAYR only).
 *  - `data_enable`      -- byte-stream capture gated by an external
 *                          DEN-style signal (JPEG payload mode);
 *                          firewall and CDSSR are active.
 */
typedef enum : uint8_t {
  k_ra8_ceu_fmt_image_capture    = 0U, /**< YCbCr image capture mode.   */
  k_ra8_ceu_fmt_data_synchronous = 1U, /**< Raw synchronous data fetch. */
  k_ra8_ceu_fmt_data_enable      = 2U, /**< JPEG / data-enable fetch.   */
} ra8_ceu_capture_format_t;

/**
 * @enum ra8_ceu_capture_mode_t
 * @brief CAPCR.CTNCP single-shot vs continuous capture.
 *
 * @details
 * HUM Ch 60.2.2 "CAPCR" p 3634-3635 -- continuous mode is only
 * legal in image-capture format.
 */
typedef enum : uint8_t {
  k_ra8_ceu_capture_single     = 0U, /**< CE auto-clears after CPE.        */
  k_ra8_ceu_capture_continuous = 1U, /**< CE stays asserted across frames. */
} ra8_ceu_capture_mode_t;

/**
 * @enum ra8_ceu_data_bus_t
 * @brief CAMCR.DTIF data-bus width select.
 */
typedef enum : uint8_t {
  k_ra8_ceu_bus_8_bit  = 0U, /**< VIO_D[7:0]  used. */
  k_ra8_ceu_bus_16_bit = 1U, /**< VIO_D[15:0] used. */
} ra8_ceu_data_bus_t;

/**
 * @enum ra8_ceu_polarity_t
 * @brief CAMCR.HDPOL / VDPOL / FLDPOL polarity select.
 */
typedef enum : uint8_t {
  k_ra8_ceu_pol_high_active = 0U, /**< Sync signal is active high. */
  k_ra8_ceu_pol_low_active  = 1U, /**< Sync signal is active low.  */
} ra8_ceu_polarity_t;

/**
 * @enum ra8_ceu_edge_t
 * @brief CAMCR.DSEL / HDSEL / VDSEL / FLDSEL latch-edge select.
 */
typedef enum : uint8_t {
  k_ra8_ceu_edge_rising  = 0U, /**< Sample on rising VIO_CLK edge.  */
  k_ra8_ceu_edge_falling = 1U, /**< Sample on falling VIO_CLK edge. */
} ra8_ceu_edge_t;

/**
 * @enum ra8_ceu_input_order_t
 * @brief CAMCR.DTARY image-capture input-byte order.
 *
 * @details
 * HUM Ch 60.2.3 p 3636-3641. Only meaningful when capture_format is
 * image_capture. Selects which 4-byte rotation of the YCbCr 4:2:2
 * stream the CEU expects on the parallel bus.
 */
typedef enum : uint8_t {
  k_ra8_ceu_input_cb0_y0_cr0_y1 = 0U, /**< Cb0 Y0 Cr0 Y1. */
  k_ra8_ceu_input_cr0_y0_cb0_y1 = 1U, /**< Cr0 Y0 Cb0 Y1. */
  k_ra8_ceu_input_y0_cb0_y1_cr0 = 2U, /**< Y0 Cb0 Y1 Cr0. */
  k_ra8_ceu_input_y0_cr0_y1_cb0 = 3U, /**< Y0 Cr0 Y1 Cb0. */
} ra8_ceu_input_order_t;

/**
 * @enum ra8_ceu_output_format_t
 * @brief CDOCR.CDS output-format select.
 *
 * @details
 * HUM Ch 60.2.20 p 3662-3663. Only meaningful in image-capture
 * mode; data-fetch modes must use 4:2:2 (CDS = 1).
 */
typedef enum : uint8_t {
  k_ra8_ceu_output_ycbcr_420 = 0U, /**< 4:2:2 in -> 4:2:0 in memory.  */
  k_ra8_ceu_output_ycbcr_422 = 1U, /**< Pass-through (no conversion). */
} ra8_ceu_output_format_t;

/**
 * @enum ra8_ceu_burst_mode_t
 * @brief CAPCR.MTCM[1:0] bus-burst transfer size.
 *
 * @details
 * HUM Ch 60.2.2 "CAPCR" p 3634-3635. Larger bursts give higher
 * peak throughput but block the bus longer; pick to match the
 * downstream RAM contention budget.
 */
typedef enum : uint8_t {
  k_ra8_ceu_burst_32  = 0U, /**< 32-byte bus bursts.  */
  k_ra8_ceu_burst_64  = 1U, /**< 64-byte bus bursts.  */
  k_ra8_ceu_burst_128 = 2U, /**< 128-byte bus bursts. */
  k_ra8_ceu_burst_256 = 3U, /**< 256-byte bus bursts. */
} ra8_ceu_burst_mode_t;

/**
 * @enum ra8_ceu_field_select_t
 * @brief CAIFR.FCI -- first captured field for interlace input.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647.
 */
typedef enum : uint8_t {
  k_ra8_ceu_field_immediate = 0U, /**< Capture starts at next VD. */
  k_ra8_ceu_field_top       = 1U, /**< Wait for top field.        */
  k_ra8_ceu_field_bottom    = 2U, /**< Wait for bottom field.     */
} ra8_ceu_field_select_t;

/**
 * @enum ra8_ceu_interlace_t
 * @brief CAIFR.IFS -- progressive vs interlace input.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647.
 */
typedef enum : uint8_t {
  k_ra8_ceu_progressive = 0U, /**< Single-field-per-frame stream. */
  k_ra8_ceu_interlace   = 1U, /**< Top + bottom field stream.     */
} ra8_ceu_interlace_t;

/**
 * @enum ra8_ceu_fields_capture_t
 * @brief CAIFR.CIM -- both fields vs single field of an interlace pair.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647. Only meaningful when IFS = 1.
 */
typedef enum : uint8_t {
  k_ra8_ceu_fields_both = 0U, /**< Capture top + bottom fields.         */
  k_ra8_ceu_fields_one  = 1U, /**< Capture only the FCI-selected field. */
} ra8_ceu_fields_capture_t;

/**
 * @enum ra8_ceu_plane_t
 * @brief CRCNTR plane-A / plane-B selector.
 *
 * @details
 * HUM Ch 60.2.8 "CRCNTR" p 3649. Plane B is the shadow programming
 * window; setting CRCNTR.RVS makes the CEU swap planes on the next
 * VD edge so a new geometry / address set takes effect cleanly.
 */
typedef enum : uint8_t {
  k_ra8_ceu_plane_a = 0U, /**< Use Plane A registers (default). */
  k_ra8_ceu_plane_b = 1U, /**< Use Plane B (shadow) registers.  */
} ra8_ceu_plane_t;

/**
 * @struct ra8_ceu_byte_swap_t
 * @brief CDOCR.COBS / COWS / COLS byte-swap control bundle.
 *
 * @details
 * HUM Ch 60.2.20 p 3662-3663. The three swaps compose: COBS swaps
 * adjacent bytes in 16-bit words, COWS swaps adjacent 16-bit words
 * in 32-bit dwords, COLS swaps adjacent 32-bit dwords in 64-bit
 * qwords. Set every flag your downstream consumer needs in one
 * shot via `ra8_ceu_byte_swap_set`.
 */
typedef struct {
  bool swap_8_bit;  /**< COBS: byte-swap inside 16-bit words.   */
  bool swap_16_bit; /**< COWS: word-swap inside 32-bit dwords.  */
  bool swap_32_bit; /**< COLS: dword-swap inside 64-bit qwords. */
} ra8_ceu_byte_swap_t;

/**
 * @struct ra8_ceu_edge_info_t
 * @brief CAMCR.DSEL / HDSEL / VDSEL / FLDSEL latch-edge bundle.
 *
 * @details
 * HUM Ch 60.2.3 p 3636-3641. Each member maps directly to one
 * CAMCR bit (24, 26, 27, 25 respectively).
 */
typedef struct {
  ra8_ceu_edge_t data;  /**< CAMCR.DSEL   (data lines).    */
  ra8_ceu_edge_t hsync; /**< CAMCR.HDSEL  (HD).            */
  ra8_ceu_edge_t vsync; /**< CAMCR.VDSEL  (VD).            */
  ra8_ceu_edge_t field; /**< CAMCR.FLDSEL (FLD interlace). */
} ra8_ceu_edge_info_t;

/**
 * @struct ra8_ceu_scale_t
 * @brief CFLCR scale-down + CFSZR clip-size programming bundle.
 *
 * @details
 * HUM Ch 60.2.10 "CFLCR" p 3650 + Ch 60.2.11 "CFSZR" p 3651. The
 * scale-down factor is encoded as a `mantissa.fraction` fixed-point
 * value (4-bit mantissa, 12-bit fraction) per axis. A value of 0
 * disables that axis; the CEU treats the size-clip register as the
 * raw filter input dimensions in that case.
 */
typedef struct {
  uint16_t h_mantissa;    /**< CFLCR.HMANT[3:0].  */
  uint16_t h_fraction;    /**< CFLCR.HFRAC[11:0]. */
  uint16_t v_mantissa;    /**< CFLCR.VMANT[3:0].  */
  uint16_t v_fraction;    /**< CFLCR.VFRAC[11:0]. */
  uint16_t h_output_clip; /**< CFSZR.HFCLP[11:0]. */
  uint16_t v_output_clip; /**< CFSZR.VFCLP[11:0]. */
} ra8_ceu_scale_t;

/**
 * @struct ra8_ceu_config_t
 * @brief Full configuration descriptor for `ra8_ceu_init`.
 *
 * @details
 * Every CEU register that has to be programmed at open time is
 * driven from this single struct. Optional features are switched
 * on/off by a flag instead of needing a separate setter call:
 *  - Set `interlace` to enable interlace capture (CAIFR.IFS=1).
 *  - Set `bundle_write` to enable bundle-write mode (CDOCR.CBE=1).
 *  - Set `low_pass_filter` to enable the input LPF (CLFCR.LPF=1).
 *  - Set `scale.h_*` or `scale.v_*` non-zero to enable scale-down.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in `ra8_ceu_init` in
 * `libs/ra8_hal/src/ra8_ceu.c`.
 */
typedef struct {
  uint16_t                 width_px;        /**< Captured image width (CMCYR.HCYL).  */
  uint16_t                 height_px;       /**< Captured image height (CMCYR.VCYL). */
  uint16_t                 x_start_px;      /**< CAMOR.HOFST start offset.           */
  uint16_t                 y_start_px;      /**< CAMOR.VOFST start offset.           */
  uint16_t                 x_capture_px;    /**< CAPWR.HWDTH capture width cycles.   */
  uint16_t                 y_capture_lines; /**< CAPWR.VWDTH capture line count.     */
  uint16_t                 dst_stride;      /**< Destination stride bytes (CDWDR).   */
  uint8_t                  frame_drop;      /**< CAPCR.FDRP[7:0].                    */
  uint8_t                  bytes_per_pixel; /**< Used to derive scaled stride.       */
  uint32_t                 interrupts;      /**< CEIER bitmask (k_ra8_ceu_evt_*).    */
  ra8_ceu_capture_format_t capture_format;  /**< CAMCR.JPG.                          */
  ra8_ceu_capture_mode_t   capture_mode;    /**< CAPCR.CTNCP single/continuous.      */
  ra8_ceu_data_bus_t       data_bus;        /**< CAMCR.DTIF.                         */
  ra8_ceu_polarity_t       hsync_polarity;  /**< CAMCR.HDPOL.                        */
  ra8_ceu_polarity_t       vsync_polarity;  /**< CAMCR.VDPOL.                        */
  ra8_ceu_polarity_t       field_polarity;  /**< CAMCR.FLDPOL.                       */
  ra8_ceu_input_order_t    input_order;     /**< CAMCR.DTARY (image mode).           */
  ra8_ceu_output_format_t  output_format;   /**< CDOCR.CDS YCbCr 422/420.            */
  ra8_ceu_burst_mode_t     burst_mode;      /**< CAPCR.MTCM bus burst size.          */
  ra8_ceu_field_select_t   first_field;     /**< CAIFR.FCI.                          */
  ra8_ceu_edge_info_t      edge;            /**< CAMCR.DSEL/HDSEL/VDSEL/FLDSEL.      */
  ra8_ceu_byte_swap_t      byte_swap;       /**< CDOCR.COBS/COWS/COLS.               */
  ra8_ceu_scale_t          scale;           /**< CFLCR + CFSZR scale-down.           */
  bool                     interlace;       /**< CAIFR.IFS = 1 if true.              */
  bool                     one_field_only;  /**< CAIFR.CIM = 1 if true.              */
  bool                     bundle_write;    /**< CDOCR.CBE = 1 if true.              */
  bool                     low_pass_filter; /**< CLFCR.LPF = 1 if true.              */
  uint32_t                 image_area_size; /**< For data-enable firewall window.    */
} ra8_ceu_config_t;

/**
 * @struct ra8_ceu_status_t
 * @brief Snapshot of the CEU live-status registers.
 *
 * @details
 * Returned by `ra8_ceu_status_snapshot`. Callers that only need the
 * raw event-flag word can use `ra8_ceu_get_status` instead.
 */
typedef struct {
  uint32_t        events;          /**< CETCR raw value (event flags). */
  uint32_t        data_size;       /**< CDSSR last-write byte count.   */
  bool            capturing;       /**< CSTSR.CPTON.                   */
  bool            reset_in_flight; /**< CAPSR.CPKIL.                   */
  ra8_ceu_plane_t active_plane;    /**< CSTSR.CRST register plane.     */
  bool            top_field;       /**< CSTSR.CPFLD == 1.              */
} ra8_ceu_status_t;

/**
 * @struct ra8_ceu_buffers_t
 * @brief Address bundle programmed into the CDAYR / CDACR family.
 *
 * @details
 * Every pointer must be 8-byte aligned (HUM Ch 60.2.13-60.2.16
 * p 3656-3658). The bottom-field and bundle-2 pointers are only
 * required for interlace + bundle-write modes; pass NULL to leave
 * the matching register at zero.
 */
typedef struct {
  uint8_t* y_top;             /**< CDAYR  -- Y / top-field address.       */
  uint8_t* c_top;             /**< CDACR  -- C / top-field C address.     */
  uint8_t* y_bottom;          /**< CDBYR  -- Y bottom-field address.      */
  uint8_t* c_bottom;          /**< CDBCR  -- C bottom-field C address.    */
  uint8_t* y_top_2;           /**< CDAYR2 -- bundle-2 Y top.              */
  uint8_t* c_top_2;           /**< CDACR2 -- bundle-2 C top.              */
  uint8_t* y_bottom_2;        /**< CDBYR2 -- bundle-2 Y bottom.           */
  uint8_t* c_bottom_2;        /**< CDBCR2 -- bundle-2 C bottom.           */
  uint32_t bundle_size_bytes; /**< CBDSR bundle write size (bytes/lines). */
} ra8_ceu_buffers_t;

/**
 * @typedef ra8_ceu_event_fn_t
 * @brief Callback fired by `ra8_ceu_dispatch` for every CETCR event.
 *
 * @param[in] ctx        Caller-supplied context pointer.
 * @param[in] event_mask Snapshot of CETCR at dispatch time, masked
 *                       by the currently enabled CEIER bits.
 */
typedef void (*ra8_ceu_event_fn_t)(void* ctx, uint32_t event_mask);
