/**
 * @file ra_ceu.h
 * @brief Capture Engine Unit (CEU) camera-capture driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage HAL driver for the RA8D2 Capture Engine Unit. The
 * CEU is the parallel camera-input path that DMAs frames from an
 * external CMOS sensor (the EK-RA8D2's on-board OV5640) into
 * SRAM/SDRAM. This driver implements every operating mode and
 * register field documented in HUM Ch 60 (p 3626-3682):
 *
 *  - **Image-capture mode** (CAMCR.JPG = 00) -- YCbCr 4:2:2 input,
 *    optional hardware conversion to YCbCr 4:2:0, optional bilinear
 *    scale-down, optional input-low-pass pre-filter.
 *  - **Data-synchronous-fetch mode** (CAMCR.JPG = 01) -- raw
 *    sensor pixels at HD/VD timing, no Y/C separation.
 *  - **Data-enable-fetch mode** (CAMCR.JPG = 10) -- gated capture
 *    of byte-stream data (e.g. JPEG payloads); CFWCR firewall is
 *    armed automatically here.
 *  - **Single-shot vs continuous capture** (CAPCR.CTNCP).
 *  - **Bundle write mode** (CDOCR.CBE) with 4-address ping-pong
 *    across CDAYR / CDAYR2 / CDBYR / CDBYR2.
 *  - **Interlace** capture with both-field or one-field clipping
 *    (CAIFR.IFS / CIM, CAMCR.FLDPOL/FLDSEL, CDBYR/CDBCR programming).
 *  - **Plane B** shadow programming for in-flight reconfiguration
 *    across a VD edge plus the forced-swap (CRCMPR.RA) escape hatch.
 *  - **Byte swap** at 8/16/32-bit granularity (CDOCR.COBS / COWS /
 *    COLS).
 *  - **TrustZone-style firewall** (CFWCR) capping the legal write
 *    window during data-enable fetch.
 *  - **Frame-drop** counter (CAPCR.FDRP) and **burst-mode** size
 *    (CAPCR.MTCM, 32/64/128/256-byte transfers).
 *  - **Edge select** for the data lines, HD, VD, FLD inputs
 *    (CAMCR.DSEL / HDSEL / VDSEL / FLDSEL).
 *  - **Every IRQ source** documented in CETCR -- frame end, field
 *    end, illegal register write, HD, VD, four bundle-end events,
 *    CRAM overflow, HD/VD mismatch, VD error, firewall fault, HD
 *    missing, VD missing -- with a registered callback dispatch.
 *  - **Lifecycle**: init / deinit / reset / enter_stop / exit_stop
 *    plus a dedicated software-reset path bounded by a CSTSR.CPTON
 *    spin with `k_ra_err_hw_timeout` on overrun.
 *  - **Status**: CSTSR snapshot (CPTON, CPFLD field, CRST plane),
 *    CDSSR byte-count read after data-enable capture.
 *
 * ## Driver state machine
 *
 * @par State Machine:
 * @startuml
 * [*] --> Closed
 * Closed   --> Idle      : ra_ceu_init()
 * Idle     --> Capturing : ra_ceu_capture_start()
 * Capturing --> Idle     : CETCR.CPE (single-shot)
 * Capturing --> Capturing: CETCR.CPE (continuous, CTNCP=1)
 * Capturing --> Idle     : ra_ceu_capture_stop() / reset()
 * Idle     --> Stopped   : ra_ceu_enter_stop()
 * Stopped  --> Idle      : ra_ceu_exit_stop()
 * Idle     --> Closed    : ra_ceu_deinit()
 * @enduml
 *
 * | From      | Trigger                | To        |
 * |-----------|------------------------|-----------|
 * | Closed    | `ra_ceu_init`          | Idle      |
 * | Idle      | `ra_ceu_capture_start` | Capturing |
 * | Capturing | CPE (single-shot)      | Idle      |
 * | Capturing | `ra_ceu_capture_stop`  | Idle      |
 * | Capturing | `ra_ceu_reset`         | Idle      |
 * | Idle      | `ra_ceu_enter_stop`    | Stopped   |
 * | Stopped   | `ra_ceu_exit_stop`     | Idle      |
 * | Idle      | `ra_ceu_deinit`        | Closed    |
 *
 * The driver does not maintain explicit state -- the state is
 * derived from CSTSR.CPTON, CAPSR.CE / CPKIL and the CEIER mask.
 * Every public API checks the live register state before issuing a
 * mode change so the table above is observable, not enforced via a
 * private state variable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ceu_regs.h"
#include "ra_err.h"

/* =============================================================================
 * Configuration descriptors
 * =============================================================================
 */

/**
 * @enum ra_ceu_capture_format_t
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
  k_ra_ceu_fmt_image_capture    = 0U, /**< YCbCr image capture mode.   */
  k_ra_ceu_fmt_data_synchronous = 1U, /**< Raw synchronous data fetch. */
  k_ra_ceu_fmt_data_enable      = 2U, /**< JPEG / data-enable fetch.   */
} ra_ceu_capture_format_t;

/**
 * @enum ra_ceu_capture_mode_t
 * @brief CAPCR.CTNCP single-shot vs continuous capture.
 *
 * @details
 * HUM Ch 60.2.2 "CAPCR" p 3634-3635 -- continuous mode is only
 * legal in image-capture format.
 */
typedef enum : uint8_t {
  k_ra_ceu_capture_single     = 0U, /**< CE auto-clears after CPE.       */
  k_ra_ceu_capture_continuous = 1U, /**< CE stays asserted across frames.*/
} ra_ceu_capture_mode_t;

/**
 * @enum ra_ceu_data_bus_t
 * @brief CAMCR.DTIF data-bus width select.
 */
typedef enum : uint8_t {
  k_ra_ceu_bus_8_bit  = 0U, /**< VIO_D[7:0]  used.  */
  k_ra_ceu_bus_16_bit = 1U, /**< VIO_D[15:0] used.  */
} ra_ceu_data_bus_t;

/**
 * @enum ra_ceu_polarity_t
 * @brief CAMCR.HDPOL / VDPOL / FLDPOL polarity select.
 */
typedef enum : uint8_t {
  k_ra_ceu_pol_high_active = 0U, /**< Sync signal is active high. */
  k_ra_ceu_pol_low_active  = 1U, /**< Sync signal is active low.  */
} ra_ceu_polarity_t;

/**
 * @enum ra_ceu_edge_t
 * @brief CAMCR.DSEL / HDSEL / VDSEL / FLDSEL latch-edge select.
 */
typedef enum : uint8_t {
  k_ra_ceu_edge_rising  = 0U, /**< Sample on rising VIO_CLK edge.  */
  k_ra_ceu_edge_falling = 1U, /**< Sample on falling VIO_CLK edge. */
} ra_ceu_edge_t;

/**
 * @enum ra_ceu_input_order_t
 * @brief CAMCR.DTARY image-capture input-byte order.
 *
 * @details
 * HUM Ch 60.2.3 p 3636-3641. Only meaningful when capture_format is
 * image_capture. Selects which 4-byte rotation of the YCbCr 4:2:2
 * stream the CEU expects on the parallel bus.
 */
typedef enum : uint8_t {
  k_ra_ceu_input_cb0_y0_cr0_y1 = 0U, /**< Cb0 Y0 Cr0 Y1.  */
  k_ra_ceu_input_cr0_y0_cb0_y1 = 1U, /**< Cr0 Y0 Cb0 Y1.  */
  k_ra_ceu_input_y0_cb0_y1_cr0 = 2U, /**< Y0 Cb0 Y1 Cr0.  */
  k_ra_ceu_input_y0_cr0_y1_cb0 = 3U, /**< Y0 Cr0 Y1 Cb0.  */
} ra_ceu_input_order_t;

/**
 * @enum ra_ceu_output_format_t
 * @brief CDOCR.CDS output-format select.
 *
 * @details
 * HUM Ch 60.2.20 p 3662-3663. Only meaningful in image-capture
 * mode; data-fetch modes must use 4:2:2 (CDS = 1).
 */
typedef enum : uint8_t {
  k_ra_ceu_output_ycbcr_420 = 0U, /**< 4:2:2 in -> 4:2:0 in memory. */
  k_ra_ceu_output_ycbcr_422 = 1U, /**< Pass-through (no conversion). */
} ra_ceu_output_format_t;

/**
 * @enum ra_ceu_burst_mode_t
 * @brief CAPCR.MTCM[1:0] bus-burst transfer size.
 *
 * @details
 * HUM Ch 60.2.2 "CAPCR" p 3634-3635. Larger bursts give higher
 * peak throughput but block the bus longer; pick to match the
 * downstream RAM contention budget.
 */
typedef enum : uint8_t {
  k_ra_ceu_burst_32  = 0U, /**< 32-byte bus bursts.  */
  k_ra_ceu_burst_64  = 1U, /**< 64-byte bus bursts.  */
  k_ra_ceu_burst_128 = 2U, /**< 128-byte bus bursts. */
  k_ra_ceu_burst_256 = 3U, /**< 256-byte bus bursts. */
} ra_ceu_burst_mode_t;

/**
 * @enum ra_ceu_field_select_t
 * @brief CAIFR.FCI -- first captured field for interlace input.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647.
 */
typedef enum : uint8_t {
  k_ra_ceu_field_immediate = 0U, /**< Capture starts at next VD.    */
  k_ra_ceu_field_top       = 1U, /**< Wait for top field.           */
  k_ra_ceu_field_bottom    = 2U, /**< Wait for bottom field.        */
} ra_ceu_field_select_t;

/**
 * @enum ra_ceu_interlace_t
 * @brief CAIFR.IFS -- progressive vs interlace input.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647.
 */
typedef enum : uint8_t {
  k_ra_ceu_progressive = 0U, /**< Single-field-per-frame stream. */
  k_ra_ceu_interlace   = 1U, /**< Top + bottom field stream.     */
} ra_ceu_interlace_t;

/**
 * @enum ra_ceu_fields_capture_t
 * @brief CAIFR.CIM -- both fields vs single field of an interlace pair.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR" p 3647. Only meaningful when IFS = 1.
 */
typedef enum : uint8_t {
  k_ra_ceu_fields_both = 0U, /**< Capture top + bottom fields.    */
  k_ra_ceu_fields_one  = 1U, /**< Capture only the FCI-selected field.*/
} ra_ceu_fields_capture_t;

/**
 * @enum ra_ceu_plane_t
 * @brief CRCNTR plane-A / plane-B selector.
 *
 * @details
 * HUM Ch 60.2.8 "CRCNTR" p 3649. Plane B is the shadow programming
 * window; setting CRCNTR.RVS makes the CEU swap planes on the next
 * VD edge so a new geometry / address set takes effect cleanly.
 */
typedef enum : uint8_t {
  k_ra_ceu_plane_a = 0U, /**< Use Plane A registers (default).  */
  k_ra_ceu_plane_b = 1U, /**< Use Plane B (shadow) registers.   */
} ra_ceu_plane_t;

/**
 * @struct ra_ceu_byte_swap_t
 * @brief CDOCR.COBS / COWS / COLS byte-swap control bundle.
 *
 * @details
 * HUM Ch 60.2.20 p 3662-3663. The three swaps compose: COBS swaps
 * adjacent bytes in 16-bit words, COWS swaps adjacent 16-bit words
 * in 32-bit dwords, COLS swaps adjacent 32-bit dwords in 64-bit
 * qwords. Set every flag your downstream consumer needs in one
 * shot via `ra_ceu_byte_swap_set`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool swap_8_bit;  /**< COBS: byte-swap inside 16-bit words.    */
  bool swap_16_bit; /**< COWS: word-swap inside 32-bit dwords.   */
  bool swap_32_bit; /**< COLS: dword-swap inside 64-bit qwords.  */
} ra_ceu_byte_swap_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ceu_edge_info_t
 * @brief CAMCR.DSEL / HDSEL / VDSEL / FLDSEL latch-edge bundle.
 *
 * @details
 * HUM Ch 60.2.3 p 3636-3641. Each member maps directly to one
 * CAMCR bit (24, 26, 27, 25 respectively).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_ceu_edge_t data;  /**< CAMCR.DSEL   (data lines).      */
  ra_ceu_edge_t hsync; /**< CAMCR.HDSEL  (HD).              */
  ra_ceu_edge_t vsync; /**< CAMCR.VDSEL  (VD).              */
  ra_ceu_edge_t field; /**< CAMCR.FLDSEL (FLD interlace).   */
} ra_ceu_edge_info_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ceu_scale_t
 * @brief CFLCR scale-down + CFSZR clip-size programming bundle.
 *
 * @details
 * HUM Ch 60.2.10 "CFLCR" p 3650 + Ch 60.2.11 "CFSZR" p 3651. The
 * scale-down factor is encoded as a `mantissa.fraction` fixed-point
 * value (4-bit mantissa, 12-bit fraction) per axis. A value of 0
 * disables that axis; the CEU treats the size-clip register as the
 * raw filter input dimensions in that case.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t h_mantissa;    /**< CFLCR.HMANT[3:0].   */
  uint16_t h_fraction;    /**< CFLCR.HFRAC[11:0].  */
  uint16_t v_mantissa;    /**< CFLCR.VMANT[3:0].   */
  uint16_t v_fraction;    /**< CFLCR.VFRAC[11:0].  */
  uint16_t h_output_clip; /**< CFSZR.HFCLP[11:0].  */
  uint16_t v_output_clip; /**< CFSZR.VFCLP[11:0].  */
} ra_ceu_scale_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ceu_config_t
 * @brief Full configuration descriptor for `ra_ceu_init`.
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
 * each member is read in `ra_ceu_init` in
 * `libs/ra_hal/src/ra_ceu.c`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t                width_px;        /**< Captured image width (CMCYR.HCYL).  */
  uint16_t                height_px;       /**< Captured image height (CMCYR.VCYL). */
  uint16_t                x_start_px;      /**< CAMOR.HOFST start offset.           */
  uint16_t                y_start_px;      /**< CAMOR.VOFST start offset.           */
  uint16_t                x_capture_px;    /**< CAPWR.HWDTH capture width cycles.   */
  uint16_t                y_capture_lines; /**< CAPWR.VWDTH capture line count.     */
  uint16_t                dst_stride;      /**< Destination stride bytes (CDWDR).   */
  uint8_t                 frame_drop;      /**< CAPCR.FDRP[7:0].                    */
  uint8_t                 bytes_per_pixel; /**< Used to derive scaled stride.       */
  uint32_t                interrupts;      /**< CEIER bitmask (k_ra_ceu_evt_*).     */
  ra_ceu_capture_format_t capture_format;  /**< CAMCR.JPG.                          */
  ra_ceu_capture_mode_t   capture_mode;    /**< CAPCR.CTNCP single/continuous.      */
  ra_ceu_data_bus_t       data_bus;        /**< CAMCR.DTIF.                         */
  ra_ceu_polarity_t       hsync_polarity;  /**< CAMCR.HDPOL.                        */
  ra_ceu_polarity_t       vsync_polarity;  /**< CAMCR.VDPOL.                        */
  ra_ceu_polarity_t       field_polarity;  /**< CAMCR.FLDPOL.                       */
  ra_ceu_input_order_t    input_order;     /**< CAMCR.DTARY (image mode).           */
  ra_ceu_output_format_t  output_format;   /**< CDOCR.CDS YCbCr 422/420.            */
  ra_ceu_burst_mode_t     burst_mode;      /**< CAPCR.MTCM bus burst size.          */
  ra_ceu_field_select_t   first_field;     /**< CAIFR.FCI.                          */
  ra_ceu_edge_info_t      edge;            /**< CAMCR.DSEL/HDSEL/VDSEL/FLDSEL.      */
  ra_ceu_byte_swap_t      byte_swap;       /**< CDOCR.COBS/COWS/COLS.               */
  ra_ceu_scale_t          scale;           /**< CFLCR + CFSZR scale-down.           */
  bool                    interlace;       /**< CAIFR.IFS = 1 if true.              */
  bool                    one_field_only;  /**< CAIFR.CIM = 1 if true.              */
  bool                    bundle_write;    /**< CDOCR.CBE = 1 if true.              */
  bool                    low_pass_filter; /**< CLFCR.LPF = 1 if true.              */
  uint32_t                image_area_size; /**< For data-enable firewall window.    */
} ra_ceu_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ceu_status_t
 * @brief Snapshot of the CEU live-status registers.
 *
 * @details
 * Returned by `ra_ceu_status_snapshot`. Callers that only need the
 * raw event-flag word can use `ra_ceu_get_status` instead.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t       events;          /**< CETCR raw value (event flags).  */
  uint32_t       data_size;       /**< CDSSR last-write byte count.    */
  bool           capturing;       /**< CSTSR.CPTON.                    */
  bool           reset_in_flight; /**< CAPSR.CPKIL.                 */
  ra_ceu_plane_t active_plane;    /**< CSTSR.CRST register plane.   */
  bool           top_field;       /**< CSTSR.CPFLD == 1.            */
} ra_ceu_status_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ceu_buffers_t
 * @brief Address bundle programmed into the CDAYR / CDACR family.
 *
 * @details
 * Every pointer must be 8-byte aligned (HUM Ch 60.2.13-60.2.16
 * p 3656-3658). The bottom-field and bundle-2 pointers are only
 * required for interlace + bundle-write modes; pass NULL to leave
 * the matching register at zero.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t* y_top;             /**< CDAYR  -- Y / top-field address.      */
  uint8_t* c_top;             /**< CDACR  -- C / top-field C address.    */
  uint8_t* y_bottom;          /**< CDBYR  -- Y bottom-field address.     */
  uint8_t* c_bottom;          /**< CDBCR  -- C bottom-field C address.   */
  uint8_t* y_top_2;           /**< CDAYR2 -- bundle-2 Y top.             */
  uint8_t* c_top_2;           /**< CDACR2 -- bundle-2 C top.             */
  uint8_t* y_bottom_2;        /**< CDBYR2 -- bundle-2 Y bottom.          */
  uint8_t* c_bottom_2;        /**< CDBCR2 -- bundle-2 C bottom.          */
  uint32_t bundle_size_bytes; /**< CBDSR bundle write size (bytes/lines).*/
} ra_ceu_buffers_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_ceu_event_fn_t
 * @brief Callback fired by `ra_ceu_dispatch` for every CETCR event.
 *
 * @param[in] ctx        Caller-supplied context pointer.
 * @param[in] event_mask Snapshot of CETCR at dispatch time, masked
 *                       by the currently enabled CEIER bits.
 */
typedef void (*ra_ceu_event_fn_t)(void* ctx, uint32_t event_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the CEU and write the full configuration.
 *
 * @details
 * Steps performed:
 *  1. `ra_mstp_enable(k_ra_mstp_ceu)` to release MSTPC16.
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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Capture engine powered + configured.
 * @retval k_ra_err_null_ptr      `cfg` was NULL.
 * @retval k_ra_err_invalid_arg   `cfg->capture_mode` is continuous
 *                                in a non-image-capture format.
 * @retval k_ra_err_hw_timeout    MSTP enable failed or the
 *                                reset-clear spin overran.
 *
 * @pre `cfg` is non-NULL.
 * @pre `ra_mstp_init` has been called.
 * @post CEU module-stop bit is cleared.
 * @post CEU is in idle (CSTSR.CPTON == 0) and ready for
 *       `ra_ceu_capture_start`.
 *
 * @note Not thread-safe -- single-threaded init context only.
 *
 * @see ra_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_init(const ra_ceu_config_t* cfg);

/**
 * @brief Stop the CEU and drop its module-stop bit.
 *
 * @details
 * Issues CAPSR.CPKIL=1 to abort any active capture, masks every
 * interrupt source by writing 0 to CEIER, clears the registered
 * callback, and finally calls `ra_mstp_disable(k_ra_mstp_ceu)`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok             Driver torn down, MSTP released.
 * @retval k_ra_err_hw_timeout MSTP disable read-back failed.
 *
 * @pre `ra_ceu_init` was previously called successfully.
 * @pre Caller does not race a CEU IRQ on another core.
 * @post CEU is fully powered down (MSTPC16 set).
 * @post Registered callback is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_deinit(void);

/**
 * @brief Issue a software reset of the capture state machine.
 *
 * @details
 * Sets CAPSR.CPKIL and spins on CSTSR.CPTON / CAPSR.CPKIL until
 * both clear or the budget expires. Used to recover from a stuck
 * capture (e.g. after a sensor stopped emitting VD edges).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok             Reset completed cleanly.
 * @retval k_ra_err_hw_timeout Status bits did not clear in budget.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre Caller has the CEU IRQ masked or is in CEU IRQ context.
 * @post CSTSR.CPTON == 0.
 * @post CAPSR.CPKIL == 0.
 *
 * @note Bounded spin of `k_ra_ceu_reset_spin` iterations.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_reset(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Snapshot the CEU event-status register (CETCR).
 *
 * @param[out] out_mask Receives the raw CETCR value.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok           Snapshot taken.
 * @retval k_ra_err_null_ptr `out_mask` was NULL.
 *
 * @pre `out_mask` is non-NULL.
 * @pre `ra_ceu_init` previously called (CEU MSTP cleared).
 * @post `*out_mask` reflects the value of CETCR at call time.
 * @post No status flag is cleared.
 *
 * @note Thread-safe with respect to its own register read.
 *
 * @see ra_ceu_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_get_status(uint32_t* out_mask);

/**
 * @brief Clear the CETCR event flags identified by `mask`.
 *
 * @details
 * CETCR is "write-0-to-clear" per HUM Ch 60.2.22 -- writing the
 * complement of `mask` clears exactly those bits. The driver does
 * a read-modify-write so other in-flight flags survive.
 *
 * @param[in] mask Bitmask of `k_ra_ceu_evt_*` values to clear.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always; mask of zero is a no-op.
 *
 * @pre CEU is powered (MSTPC16 cleared).
 * @pre `mask` only contains bits defined in `ra_ceu_cetcr_mask_t`.
 * @post Bits in `mask` are deasserted in CETCR.
 * @post Bits not in `mask` retain their previous state.
 *
 * @note Not thread-safe.
 *
 * @see ra_ceu_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_clear_status(uint32_t mask);

/**
 * @brief Snapshot CSTSR + CDSSR + the active plane into one struct.
 *
 * @param[out] out Non-NULL status struct to populate.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok           Snapshot taken.
 * @retval k_ra_err_null_ptr `out` was NULL.
 *
 * @pre `out` is non-NULL.
 * @pre `ra_ceu_init` previously called.
 * @post `out->capturing` mirrors CSTSR.CPTON.
 * @post `out->data_size` mirrors CDSSR.
 *
 * @note Thread-safe (read-only).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_status_snapshot(ra_ceu_status_t* out);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok           Snapshot taken.
 * @retval k_ra_err_null_ptr `out_bytes` was NULL.
 *
 * @pre `out_bytes` is non-NULL.
 * @pre `ra_ceu_init` previously called.
 * @post `*out_bytes` == CDSSR.
 *
 * @note Thread-safe (read-only).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_data_size_get(uint32_t* out_bytes);

/**
 * @brief Update the CEIER interrupt-enable bitmask.
 *
 * @param[in] mask Bitwise-OR of `k_ra_ceu_evt_*` values.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre `mask` only contains bits defined in `ra_ceu_cetcr_mask_t`.
 * @post CEIER == `mask`.
 * @post Future `ra_ceu_dispatch` calls gate the callback against
 *       the new mask.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_interrupts_set(uint32_t mask);

/**
 * @brief Register a callback invoked by `ra_ceu_dispatch`.
 *
 * @param[in] fn  Callback function, or NULL to clear.
 * @param[in] ctx Opaque context forwarded as the first callback arg.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Callback installed (or cleared).
 *
 * @pre Caller is in single-threaded init or has IRQs masked.
 * @pre `fn` is either NULL or points to a callable function.
 * @post Subsequent `ra_ceu_dispatch` calls invoke `fn(ctx, mask)`.
 * @post Previously-registered callback is overwritten.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_attach_handler(ra_ceu_event_fn_t fn, void* ctx);

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
 * @pre `ra_ceu_init` previously called.
 * @pre Caller is the CEU IRQ context (or has IRQs masked).
 * @post All bits that were observed in CETCR are cleared.
 * @post Registered callback (if any) was invoked exactly once.
 *
 * @note Returns silently if no callback is registered.
 * @since 0.1.0
 *
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 */
void ra_ceu_dispatch(void);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Stop active captures and drop the CEU module-stop bit.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok             CEU stopped + MSTP released.
 * @retval k_ra_err_hw_timeout MSTP disable read-back failed.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre Caller does not race the CEU ISR.
 * @post CEU is in MSTP-gated stop.
 * @post No further CEU IRQs will fire until `ra_ceu_exit_stop`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_enter_stop(void);

/**
 * @brief Exit the MSTP-gated stop entered by `ra_ceu_enter_stop`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok             MSTP re-enabled, CEU window accessible.
 * @retval k_ra_err_hw_timeout MSTP enable read-back failed.
 *
 * @pre `ra_ceu_enter_stop` was previously called.
 * @pre Caller does not race the CEU ISR.
 * @post CEU window is accessible (MSTPC16 cleared).
 * @post Caller must reprogram any registers that lost state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_exit_stop(void);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Capture armed; CE bit set.
 * @retval k_ra_err_null_ptr      `buffer` was NULL.
 * @retval k_ra_err_invalid_arg   `buffer` is not 8-byte aligned.
 * @retval k_ra_err_busy          A capture is already in progress
 *                                (CSTSR.CPTON == 1) or a software
 *                                reset is in flight (CAPSR.CPKIL == 1).
 *
 * @pre `ra_ceu_init` previously called.
 * @pre CEU is idle (CSTSR.CPTON == 0).
 * @pre `buffer` is non-NULL and 8-byte aligned.
 * @post CDAYR == `(uintptr_t)buffer`.
 * @post CAPSR.CE == 1 (capture armed).
 *
 * @note Not thread-safe.
 *
 * @see ra_ceu_dispatch  Listens for CETCR.CPE on capture completion.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_arm(uint8_t* buffer);

/**
 * @brief Arm a capture using a full address bundle.
 *
 * @details
 * Like `ra_ceu_capture_start` but lets the caller specify the C
 * (chrominance) base, the bottom-field addresses (interlace), and
 * the bundle-2 pair (ping-pong). Pointers left NULL leave the
 * matching register at its previous value.
 *
 * @param[in] bufs Non-NULL buffer descriptor.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                 Capture armed; CE set.
 * @retval k_ra_err_null_ptr       `bufs` was NULL or `bufs->y_top` NULL.
 * @retval k_ra_err_invalid_arg    Any non-NULL pointer fails 8-byte
 *                                 alignment.
 * @retval k_ra_err_busy           CSTSR.CPTON==1 or CAPSR.CPKIL==1.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre CEU is idle (CSTSR.CPTON == 0).
 * @pre `bufs->y_top` is non-NULL and 8-byte aligned.
 * @post CDAYR == `(uintptr_t)bufs->y_top`.
 * @post CAPSR.CE == 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_start_ex(const ra_ceu_buffers_t* bufs);

/**
 * @brief Stop the running capture by clearing CAPSR.CE.
 *
 * @details
 * Distinct from `ra_ceu_reset` -- this only clears the CE bit so
 * the next VD does not start a new frame. Continuous-mode captures
 * use this to break out cleanly without a CPKIL software reset.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre Caller has CEU IRQ masked or is in IRQ context.
 * @post CAPSR.CE == 0.
 * @post CSTSR.CPTON will go to 0 once the in-flight frame ends.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_disarm(void);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok               Plane B armed.
 * @retval k_ra_err_invalid_arg  Any non-NULL pointer is misaligned.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre Caller does not race the CEU ISR.
 * @post Plane B mirrors Plane A (with bufs overrides applied).
 * @post CRCNTR.RVS == 1 (swap armed for next VD).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_plane_b_program(const ra_ceu_buffers_t* bufs);

/**
 * @brief Force an immediate plane swap (CRCMPR.RA).
 *
 * @details
 * HUM Ch 60.2.9 "CRCMPR" p 3649. Bypasses the VD-edge sync the
 * normal Plane-B path uses; use only when a frame must be dropped.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CRCMPR.RA == 1 momentarily; the bit self-clears.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_plane_swap_force(void);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CFWCR == (`enable`?FWE:0) | (`upper_bound` & FWV mask).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_firewall_set(bool enable, uint32_t upper_bound);

/**
 * @brief Replace CDOCR byte-swap configuration in-place.
 *
 * @param[in] swap Non-NULL byte-swap descriptor.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok           Updated.
 * @retval k_ra_err_null_ptr `swap` was NULL.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre `swap` is non-NULL.
 * @post CDOCR.COBS == `swap->swap_8_bit`.
 * @post CDOCR.COWS == `swap->swap_16_bit`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_byte_swap_set(const ra_ceu_byte_swap_t* swap);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CBDSR == `size_bytes & ~7`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_bundle_size_set(uint32_t size_bytes);

/**
 * @brief Enable or disable the input low-pass filter (CLFCR.LPF).
 *
 * @param[in] enable true => CLFCR.LPF = 1.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CLFCR.LPF == `enable`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_low_pass_set(bool enable);

/**
 * @brief Enable or disable continuous-capture mode (CAPCR.CTNCP).
 *
 * @param[in] mode Single-shot or continuous.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CAPCR.CTNCP reflects `mode`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_mode_set(ra_ceu_capture_mode_t mode);

/**
 * @brief Update the frame-drop counter (CAPCR.FDRP).
 *
 * @param[in] count Number of frames to skip between captures (0..255).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Always.
 *
 * @pre `ra_ceu_init` previously called.
 * @post CAPCR.FDRP == count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_frame_drop_set(uint8_t count);

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
 * existing `ra_dmac_start` call with the CEU's preferred 32-bit /
 * source+dest-incrementing layout.
 *
 * @param[in] channel  DMAC channel index.
 * @param[in] src      Source buffer (typically the CDAYR-pointed window).
 * @param[in] dst      Destination buffer.
 * @param[in] bytes    Bytes to copy (multiple of 4).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                DMAC channel armed.
 * @retval k_ra_err_null_ptr      `src` or `dst` was NULL.
 * @retval k_ra_err_invalid_arg   `bytes` not a multiple of 4 or 0.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre `ra_dmac_init` (or equivalent) previously called.
 * @post DMAC channel `channel` armed for a one-shot block transfer.
 *
 * @note Wraps `ra_dmac_start` -- the channel becomes unavailable
 *       to other consumers until the transfer completes.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ceu_dma_pump(uint8_t channel, const uint8_t* src, uint8_t* dst, uint32_t bytes);

/* =============================================================================
 * Sweep 17: DMA-driven framebuffer + multi-frame capture
 * =============================================================================
 */

/**
 * @brief Cache a DMAC-target framebuffer for the next `ra_ceu_capture_start`
 *        call.
 *
 * @details
 * The CEU writes pixel data straight to memory via its own bus initiator,
 * so the "DMA buffer" here is just the next CDAYR target the driver
 * will arm when `ra_ceu_capture_start(num_frames)` is called. The
 * length is cached so the frame-end callback can hand the whole
 * buffer back to the consumer.
 *
 * @param[in] buf Capture target (must be 8-byte aligned per HUM
 *                Ch 60.2.13 p 3656).
 * @param[in] len Buffer length in bytes (>0).
 *
 * @return ra_err_t
 * @retval k_ra_ok                Buffer cached.
 * @retval k_ra_err_null_ptr      buf was NULL.
 * @retval k_ra_err_invalid_arg   len was 0 or buf misaligned.
 *
 * @pre `ra_ceu_init` previously called.
 * @pre `buf` is non-NULL and 8-byte aligned, `len > 0`.
 * @post The driver remembers (buf, len) for the next capture_start.
 *
 * @note Not thread-safe.
 * @see ra_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_set_dma_buffer(uint8_t* buf, uint32_t len);

/**
 * @brief Start a single or continuous capture into the cached DMA buffer.
 *
 * @details
 * Wraps `ra_ceu_capture_arm` with the buffer that was cached via
 * `ra_ceu_set_dma_buffer`. `num_frames` selects the capture mode:
 *  - `num_frames == 1` -> single-shot (CAPCR.CTNCP = 0).
 *  - `num_frames == 0` -> continuous capture (CAPCR.CTNCP = 1).
 *  - Otherwise -> single-shot; the driver tracks the remaining count
 *    through the dispatcher (out of scope for this entry-point, see
 *    @par Note below).
 *
 * @param[in] num_frames 0 = continuous, otherwise number of frames.
 *
 * @return ra_err_t
 * @retval k_ra_ok                 Capture armed.
 * @retval k_ra_err_invalid_state  No buffer cached via
 *                                 `ra_ceu_set_dma_buffer`.
 * @retval k_ra_err_busy           CEU is already capturing.
 *
 * @pre `ra_ceu_init` and `ra_ceu_set_dma_buffer` were called.
 * @post CAPCR.CTNCP reflects continuous vs single.
 * @post CAPSR.CE == 1.
 *
 * @note Not thread-safe.
 * @see ra_ceu_capture_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_start(uint32_t num_frames);

/**
 * @brief Stop the in-flight capture started via `ra_ceu_capture_start`.
 *
 * @details
 * Wraps `ra_ceu_capture_disarm` so the high-level start/stop pair
 * share matching names.
 *
 * @return ra_err_t error code from `ra_ceu_capture_disarm`.
 *
 * @pre `ra_ceu_capture_start` was previously called.
 * @post CAPSR.CE == 0.
 *
 * @note Not thread-safe.
 * @see ra_ceu_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ceu_capture_stop(void);

#ifdef __cplusplus
}
#endif
