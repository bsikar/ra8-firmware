/**
 * @file ra8_epaper.h
 * @brief IT8951 e-paper controller SPI driver -- public API
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written driver for the Waveshare / IT8951 e-paper timing
 * controller. The IT8951 wraps the actual e-paper panel and exposes
 * an SPI command/data interface to the host MCU. This driver is
 * intended for a future custom carrier board (the EK-RA8D2 v1
 * dev-kit ships with a parallel TFT, not e-paper); the EPD demo
 * apps that consume this driver compile but are gated off until the
 * board is built.
 *
 * The IT8951 SPI protocol is a "preamble" pattern documented in the
 * IT8951 datasheet (rev 0.2 chapter 3.4 "SPI Interface" and chapter
 * 4 "Application Note"; see also Waveshare's IT8951-AP user guide):
 *
 *   - Every SPI transaction begins with a 16-bit preamble that tells
 *     the controller whether the controller is about to send a command
 *     (`0x6000`), send pixel data (`0x0000`), or read pixel/status
 *     data (`0x1000`).
 *   - For command writes the host then clocks the 16-bit command
 *     code MSB-first.
 *   - For data writes / reads the host clocks 16-bit words MSB-first
 *     until the transfer ends.
 *   - Between every preamble and the data phase the host must wait
 *     for the controller's "HRDY" GPIO to assert.
 *
 * Public API surface:
 *
 *  - ``ra8_epaper_init``         -- reset + SYS_RUN + GET_DEV_INFO
 *                                  (0x0302) over the injected bus seam.
 *  - ``ra8_epaper_dev_info``     -- the decoded GET_DEV_INFO block,
 *                                  including the waveform-LUT version.
 *  - ``ra8_epaper_load_image``   -- transfer a packed 1/2/4/8 bpp
 *                                  greyscale buffer into the
 *                                  controller's frame buffer at a
 *                                  (x,y) location.
 *  - ``ra8_epaper_display_area`` -- kick off a panel refresh of the
 *                                  rectangle just loaded with the
 *                                  selected waveform mode.
 *  - ``ra8_epaper_get_vcom`` /
 *    ``ra8_epaper_set_vcom``     -- read / programme the panel's
 *                                  common-electrode bias (0x0039).
 *  - ``ra8_epaper_sleep``        -- send SLEEP (0x0003) so the
 *                                  panel drops into low-power.
 *
 * Two properties of this controller are **panel-specific and must not be
 * baked into firmware**:
 *
 *  - **VCOM** is per-unit calibration data printed on each panel's own
 *    flex cable. A wrong value costs contrast immediately and damages the
 *    film cumulatively. This driver can read and write it but takes no
 *    view on what it should be; ``libs/ra8_epd_cal`` owns resolving it
 *    from durable per-device storage and fails safe when it cannot.
 *  - **Waveform mode numbers** are a property of the controller's
 *    firmware LUT and differ between panel sizes. They are supplied per
 *    panel through ::ra8_epaper_waveform_cfg_t.
 *
 * Static allocation only -- the driver keeps one panel context
 * (``s_panel``) at file scope and rejects double-init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_spi_bus_ops.h"

/**
 * @enum ra8_epaper_waveform_t
 * @brief Symbolic IT8951 panel-refresh waveform selectors.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.4 "Display Update Modes" /
 * Waveshare IT8951 application note table 2. Each waveform trades
 * refresh latency for ghosting / quality.
 *
 * @warning These are **selectors, not wire values**. The number the
 *          controller actually wants in the DPY_AREA waveform argument
 *          is **firmware-LUT dependent and differs per panel size** --
 *          A2 is mode 4 on the ``M641`` LUT (6 inch and 6 inch HD) but
 *          mode 6 on the 7.8 / 9.7 / 10.3 inch panels. The wire numbers
 *          therefore live in ::ra8_epaper_waveform_cfg_t, are supplied
 *          per panel through ::ra8_epaper_cfg_t, and are resolved by
 *          ``ra8_epaper_display_area``. Never cast this enum onto the
 *          wire.
 *
 * @see ra8_epaper_waveform_cfg_t
 * @see ra8_epaper_waveform_cfg_for_lut
 */
typedef enum : uint8_t {
  k_ra8_epaper_wf_init = 0U, /**< INIT - flush to white, slowest.    */
  k_ra8_epaper_wf_du   = 1U, /**< DU   - direct update, 1 bpp, fast. */
  k_ra8_epaper_wf_gc16 = 2U, /**< GC16 - 16-grey full quality.       */
  k_ra8_epaper_wf_a2   = 3U, /**< A2   - black/white only, fastest.  */
} ra8_epaper_waveform_t;

/**
 * @struct ra8_epaper_waveform_cfg_t
 * @brief Per-panel map from ::ra8_epaper_waveform_t onto LUT mode numbers.
 *
 * @details
 * The IT8951 selects a refresh waveform by an integer index into the
 * waveform LUT its firmware loaded. Those indices are **not** fixed
 * across panels: Waveshare's own demo declares ``A2_Mode = 6`` and then
 * overrides it to ``4`` when ``GET_DEV_INFO`` reports LUT version
 * ``M641`` (the 6 inch and 6 inch HD panels). Baking one number into the
 * driver is therefore correct for exactly one panel family and silently
 * wrong -- wrong waveform, wrong optical result, needless panel stress --
 * for every other.
 *
 * Populate this from the LUT version string the controller reports at
 * init (``ra8_epaper_waveform_cfg_for_lut``), or state it directly in the
 * BSP descriptor when the board design fixes the panel.
 *
 * @invariant Every member is <= ::k_ra8_epaper_wf_mode_max.
 * @invariant ``du``, ``gc16`` and ``a2`` are non-zero (mode 0 is INIT), so
 *            a zero-initialised struct is rejected by ``ra8_epaper_init``
 *            rather than silently refreshing everything as INIT.
 *
 * @par Example:
 * @code
 * ra8_epaper_waveform_cfg_t wf = {};
 * (void)ra8_epaper_waveform_cfg_for_lut("M641", &wf); // -> wf.a2 == 4
 * cfg.waveform = wf;
 * @endcode
 *
 * @see ra8_epaper_waveform_t
 * @see ra8_epaper_waveform_cfg_for_lut
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t init; /**< LUT mode number for INIT (flush to white).  */
  uint8_t du;   /**< LUT mode number for DU (direct update).     */
  uint8_t gc16; /**< LUT mode number for GC16 (16-grey quality). */
  uint8_t a2;   /**< LUT mode number for A2 (bi-level, fastest). */
} ra8_epaper_waveform_cfg_t;

/**
 * @enum ra8_epaper_pixel_format_t
 * @brief Source pixel depth for a ``ra8_epaper_load_image`` transfer.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.5 "Image Buffer Endianness" and
 * the Waveshare IT8951 programming guide's ``IT8951_nBPP`` constants.
 * These enumerators are **driver-side selectors**; the two-bit code the
 * controller wants in the LD_IMG_AREA ``arg0`` bitfield uses a different
 * numbering (2 bpp = 0, 3 bpp = 1, 4 bpp = 2, 8 bpp = 3) and is applied
 * internally by ``ra8_epaper_load_image``.
 *
 * @note **4 bpp is the right default for greyscale content.** The panel
 *       renders 16 grey levels and the IT8951 keeps only the high nibble
 *       of an 8 bpp byte, so 4 bpp is visually identical to 8 bpp while
 *       halving the bytes on the wire -- the largest cheap throughput win
 *       available on either SPI or I80.
 *
 * @warning 1 bpp carries a hard geometry constraint that silently blanks
 *          the update when violated: see ::k_ra8_epaper_align_1bpp_px and
 *          ``ra8_epaper_align_area``.
 *
 * @see ra8_epaper_bits_per_pixel
 * @see ra8_epaper_image_bytes
 */
typedef enum : uint8_t {
  k_ra8_epaper_pf_1bpp = 0U, /**< 1 bpp bi-level, 8 px per byte (A2). */
  k_ra8_epaper_pf_2bpp = 1U, /**< 2 bpp, 4 px per byte.               */
  k_ra8_epaper_pf_4bpp = 2U, /**< 4 bpp, 2 px per byte -- preferred.  */
  k_ra8_epaper_pf_8bpp = 3U, /**< 8 bpp, one byte per pixel.          */
} ra8_epaper_pixel_format_t;

/**
 * @enum ra8_epaper_geom_limits_t
 * @brief Geometry / waveform constants the driver enforces.
 *
 * @details
 * ::k_ra8_epaper_align_1bpp_px encodes the vendor requirement published
 * on the Waveshare 6 inch HD wiki: "when we use 1bpp mode to update the
 * 6inch e-Paper and 6inch HD e-Paper, we should align the X (begin
 * point) and W (width) of the update area to four bytes (32bits),
 * otherwise, the image cannot be displayed". Four bytes of a 1 bpp
 * bitmap is 32 pixels, so both ``area->x`` and ``area->width`` must be
 * multiples of 32 in 1 bpp mode. This is not a quality hint -- a
 * misaligned A2 update does not render at all.
 *
 * @see ra8_epaper_area_is_aligned
 * @see ra8_epaper_align_area
 */
typedef enum : uint16_t {
  k_ra8_epaper_align_1bpp_px = 32U, /**< 4-byte (32 px) X/W grid for 1 bpp. */
  k_ra8_epaper_wf_mode_max   = 7U,  /**< Highest valid LUT mode number.     */
} ra8_epaper_geom_limits_t;

/**
 * @enum ra8_epaper_dev_info_limits_t
 * @brief Sizing of the ``GET_DEV_INFO`` (0x0302) response block.
 */
typedef enum : uint8_t {
  k_ra8_epaper_ver_chars = 16U, /**< Chars in a FW / LUT version string. */
} ra8_epaper_dev_info_limits_t;

/**
 * @struct ra8_epaper_dev_info_t
 * @brief Decoded ``GET_DEV_INFO`` (0x0302) response.
 *
 * @details
 * The controller answers GET_DEV_INFO with 20 16-bit words: panel width,
 * panel height, image-buffer base address low half, image-buffer base
 * address high half, then eight words of firmware-version string and
 * eight words of waveform-LUT-version string (IT8951 I80/SPI/I2C
 * programming guide, ``I80_GET_DEV_INFO``). The LUT version is the value
 * that decides waveform mode numbering -- feed it to
 * ``ra8_epaper_waveform_cfg_for_lut``.
 *
 * @invariant ``fw_version`` and ``lut_version`` are always NUL-terminated.
 * @invariant Populated once by ``ra8_epaper_init`` and read-only after.
 *
 * @par Example:
 * @code
 * ra8_epaper_dev_info_t info = {};
 * if (ra8_epaper_dev_info(&info) == k_ra8_ok) {
 *   ra8_epaper_waveform_cfg_t wf = {};
 *   (void)ra8_epaper_waveform_cfg_for_lut(info.lut_version, &wf);
 * }
 * @endcode
 *
 * @see ra8_epaper_dev_info
 * @see ra8_epaper_waveform_cfg_for_lut
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t panel_width;                              /**< Reported panel width (px).   */
  uint16_t panel_height;                             /**< Reported panel height (px).  */
  uint32_t image_buf_base;                           /**< Controller frame-RAM base.   */
  char     fw_version[k_ra8_epaper_ver_chars + 1U];  /**< Firmware version, NUL-term.  */
  char     lut_version[k_ra8_epaper_ver_chars + 1U]; /**< LUT version, NUL-terminated. */
} ra8_epaper_dev_info_t;

/**
 * @enum ra8_epaper_endian_t
 * @brief Source-buffer endianness flag for the LD_IMG_AREA transfer.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.5 -- the panel can swap byte
 * pairs on the fly so the host does not have to re-pack.
 */
typedef enum : uint8_t {
  k_ra8_epaper_endian_little = 0U, /**< Host buffer is little-endian. */
  k_ra8_epaper_endian_big    = 1U, /**< Host buffer is big-endian.    */
} ra8_epaper_endian_t;

/**
 * @struct ra8_epaper_cfg_t
 * @brief Configuration descriptor for ``ra8_epaper_init``.
 *
 * @details
 * The driver reaches the panel exclusively through the injected
 * ``bus`` seam (::ra8_spi_bus_ops_t), so different carrier boards may
 * pair the panel with either SPI implementation (SPI_B or SCI
 * Simple-SPI) -- the app initialises the chosen peripheral in mode 0
 * at up to the IT8951's 24 MHz ceiling and binds the seam, typically
 * via ``ra8_io_spi_bus_as_ops()``. The reset / busy GPIO pins are
 * encoded as ``ra8_port_pin_t`` packed (port<<8 | pin) values to avoid
 * a dependency on ra8_gpio_constants.h here.
 *
 * ``waveform`` carries the panel's LUT mode numbers and is **mandatory**:
 * a zero-initialised waveform map is rejected by ``ra8_epaper_init``, so
 * a board descriptor cannot accidentally inherit another panel's mode
 * numbering. Derive it from the controller-reported LUT version with
 * ``ra8_epaper_waveform_cfg_for_lut``.
 *
 * @invariant ``bus.xfer8`` is non-NULL and the panel dimensions are in
 *            ``1 .. 4096``.
 * @invariant ``waveform`` satisfies ::ra8_epaper_waveform_cfg_t's
 *            invariants.
 *
 * @par Example:
 * @code
 * ra8_epaper_cfg_t cfg = {};
 * cfg.bus = ra8_io_spi_bus_as_ops(&bus);
 * (void)ra8_epaper_waveform_cfg_for_lut("M641", &cfg.waveform);
 * cfg.panel_width  = 1448U;
 * cfg.panel_height = 1072U;
 * @endcode
 *
 * @see ra8_epaper_waveform_cfg_for_lut
 */
typedef struct {
  ra8_spi_bus_ops_t         bus;          /**< Injected SPI transfer seam (app-bound). */
  ra8_epaper_waveform_cfg_t waveform;     /**< Panel's LUT waveform mode numbers.      */
  uint16_t                  reset_pin;    /**< (port<<8)|pin -- panel /RESET line.     */
  uint16_t                  busy_pin;     /**< (port<<8)|pin -- panel HRDY input.      */
  uint16_t                  panel_width;  /**< Native panel width in pixels.           */
  uint16_t                  panel_height; /**< Native panel height in pixels.          */
} ra8_epaper_cfg_t;

/**
 * @struct ra8_epaper_area_t
 * @brief Rectangle descriptor (top-left + size) used by load + display.
 */
typedef struct {
  uint16_t x;      /**< Top-left X in panel coords (0 = left). */
  uint16_t y;      /**< Top-left Y in panel coords (0 = top).  */
  uint16_t width;  /**< Width in pixels.                       */
  uint16_t height; /**< Height in pixels.                      */
} ra8_epaper_area_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the IT8951 panel against the injected SPI bus seam.
 *
 * @details
 * Algorithm:
 *  1. Validate ``cfg`` (non-NULL ``bus.xfer8``, sane panel geometry).
 *  2. Pulse the panel /RESET line (10 ms low / 10 ms high) so the
 *     IT8951 resets into a known state.
 *  3. Wait for HRDY to assert.
 *  4. Send SYS_RUN (0x0001) to take the controller out of standby.
 *  5. Send GET_DEV_INFO (0x0302) and consume the 40-byte response so
 *     the device's panel size matches ``cfg``.
 *
 * The SPI bus itself is app-owned: initialise the peripheral behind
 * ``cfg->bus`` (mode 0, at most the IT8951's 24 MHz ceiling) before
 * calling this.
 *
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Panel responsive and identified.
 * @retval k_ra8_err_null_ptr       ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg    ``cfg`` field out of range or
 *                                 ``cfg->bus.xfer8`` NULL.
 * @retval k_ra8_err_invalid_state  Driver already initialized.
 * @retval k_ra8_err_hw_timeout     HRDY never asserted.
 *
 * @pre  The SPI peripheral behind ``cfg->bus`` is initialised (mode 0).
 * @pre  Reset / busy pins configured as GPIO output / input.
 *
 * @post On success, the driver state machine is in
 *       ``k_ra8_epaper_state_ready`` and accepts load / display calls.
 *
 * @note Not thread-safe; called once during single-threaded init.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_init(const ra8_epaper_cfg_t* cfg);

/**
 * @brief Bits per pixel carried by a ::ra8_epaper_pixel_format_t.
 *
 * @details
 * Pure lookup used to size transfers. Kept public because the PAL and the
 * host tests both need the packing arithmetic to agree with the driver's.
 *
 * @param[in] pf Pixel format selector.
 *
 * @return The number of bits each pixel occupies in the source buffer.
 * @retval 1 For ::k_ra8_epaper_pf_1bpp.
 * @retval 2 For ::k_ra8_epaper_pf_2bpp.
 * @retval 4 For ::k_ra8_epaper_pf_4bpp.
 * @retval 8 For ::k_ra8_epaper_pf_8bpp and any out-of-range input
 *           (8 bpp is the widest format, so an unknown selector can only
 *           over-estimate the buffer a caller must supply -- it can never
 *           under-run one).
 *
 * @pre  None.
 * @pre  None.
 * @post No state mutated.
 * @post Return depends solely on ``pf``.
 *
 * @note Pure function; thread-safe.
 *
 * @see ra8_epaper_image_bytes
 *
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_epaper_bits_per_pixel(ra8_epaper_pixel_format_t pf);

/**
 * @brief Bytes a packed source buffer needs for ``area`` at depth ``pf``.
 *
 * @details
 * Rows are packed independently and each starts on a byte boundary, so
 * the size is ``ceil(width * bpp / 8) * height``. This is the exact value
 * ``ra8_epaper_load_image`` requires in ``buf_len``.
 *
 * @param[in]  area      Rectangle to be transferred; non-NULL.
 * @param[in]  pf        Source pixel depth.
 * @param[out] out_bytes Receives the required byte count; non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               ``*out_bytes`` holds the size.
 * @retval k_ra8_err_null_ptr     ``area`` or ``out_bytes`` is NULL.
 * @retval k_ra8_err_invalid_size ``area`` has a zero dimension.
 *
 * @pre  ``area`` and ``out_bytes`` are readable / writable.
 * @pre  ``area`` describes a non-degenerate rectangle.
 * @post On success ``*out_bytes`` is non-zero.
 * @post On failure ``*out_bytes`` is untouched.
 *
 * @note Pure apart from the output write; thread-safe.
 *
 * @see ra8_epaper_bits_per_pixel
 * @see ra8_epaper_load_image
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_image_bytes(const ra8_epaper_area_t*  area,
                                               ra8_epaper_pixel_format_t pf,
                                               size_t*                   out_bytes);

/**
 * @brief Report whether ``area`` satisfies the depth's X/width alignment.
 *
 * @details
 * Only 1 bpp constrains geometry: the controller's bi-level LUT reads the
 * update window in 4-byte units, so ``x`` and ``width`` must both be
 * multiples of ::k_ra8_epaper_align_1bpp_px (32 pixels). Every other depth
 * is unconstrained and always reports aligned.
 *
 * @param[in] area Rectangle to test; non-NULL.
 * @param[in] pf   Pixel depth the rectangle will be transferred at.
 *
 * @return Whether the rectangle may be transferred as-is.
 * @retval true  ``pf`` is not 1 bpp, or both ``x`` and ``width`` sit on
 *               the 32-pixel grid.
 * @retval false ``pf`` is 1 bpp and ``x`` or ``width`` is misaligned.
 *
 * @pre  ``area`` is non-NULL and readable.
 * @pre  ``pf`` is a valid ::ra8_epaper_pixel_format_t.
 * @post No state mutated.
 * @post Return depends solely on the arguments.
 *
 * @note Pure function; thread-safe. A NULL ``area`` reports ``false``
 *       rather than trapping, so callers can use this as a guard.
 *
 * @see ra8_epaper_align_area
 *
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_epaper_area_is_aligned(const ra8_epaper_area_t*  area,
                                              ra8_epaper_pixel_format_t pf);

/**
 * @brief Grow ``area`` outward onto the alignment grid its depth requires.
 *
 * @details
 * Rounds ``x`` **down** and the right edge **up** to the next multiple of
 * ::k_ra8_epaper_align_1bpp_px, then clamps the right edge to
 * ``panel_width``. Growing outward (never inward) guarantees the caller's
 * requested rectangle stays wholly covered, so the refresh is a superset
 * of what was asked for rather than a silently cropped subset. A no-op
 * for depths other than 1 bpp.
 *
 * @param[in,out] area        Rectangle to align in place; non-NULL.
 * @param[in]     pf          Pixel depth the rectangle will be sent at.
 * @param[in]     panel_width Panel width in pixels used as the clamp;
 *                            must be non-zero and itself a multiple of
 *                            ::k_ra8_epaper_align_1bpp_px when ``pf`` is
 *                            1 bpp, otherwise the clamp could reintroduce
 *                            a misaligned right edge.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ``*area`` is aligned and within the panel.
 * @retval k_ra8_err_null_ptr    ``area`` is NULL.
 * @retval k_ra8_err_invalid_arg ``panel_width`` is zero, ``area`` steps
 *                               outside it, or the clamp cannot produce an
 *                               aligned width.
 *
 * @pre  ``area`` lies within ``panel_width``.
 * @pre  ``panel_width`` is the panel's true width.
 * @post On success ``ra8_epaper_area_is_aligned(area, pf)`` is ``true``.
 * @post On success the output rectangle contains the input rectangle.
 *
 * @note Not thread-safe with respect to ``*area``; the caller owns it.
 *
 * @par Example:
 * @code
 * ra8_epaper_area_t a = {.x = 17U, .y = 0U, .width = 3U, .height = 8U};
 * (void)ra8_epaper_align_area(&a, k_ra8_epaper_pf_1bpp, 1448U);
 * // a.x == 0, a.width == 32
 * @endcode
 *
 * @see ra8_epaper_area_is_aligned
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epaper_align_area(ra8_epaper_area_t* area, ra8_epaper_pixel_format_t pf, uint16_t panel_width);

/**
 * @brief Fill a waveform map from a controller-reported LUT version.
 *
 * @details
 * The waveform mode numbering is a property of the firmware LUT the
 * IT8951 loaded, which ``GET_DEV_INFO`` reports as a version string. The
 * ``M641`` LUT -- the 6 inch and 6 inch HD panels, including the
 * Waveshare 1448x1072 HAT -- places A2 at mode **4**; the vendor's
 * generic default for the larger 7.8 / 9.7 / 10.3 inch panels is mode
 * **6**. INIT (0), DU (1) and GC16 (2) are stable across both.
 *
 * The match is a prefix match on the leading ``M641`` token, because the
 * reported string carries a build suffix that varies between controller
 * firmware revisions.
 *
 * @param[in]  lut_version NUL-terminated LUT version string as reported
 *                         in ::ra8_epaper_dev_info_t; non-NULL.
 * @param[out] out_cfg     Receives the waveform map; non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            ``*out_cfg`` populated (``M641`` or generic).
 * @retval k_ra8_err_null_ptr  ``lut_version`` or ``out_cfg`` is NULL.
 *
 * @pre  ``lut_version`` is NUL-terminated.
 * @pre  ``out_cfg`` is writable.
 * @post On success every member of ``*out_cfg`` is <=
 *       ::k_ra8_epaper_wf_mode_max.
 * @post On failure ``*out_cfg`` is untouched.
 *
 * @note Pure apart from the output write; thread-safe.
 * @warning The generic fallback is the vendor's documented default, not a
 *          guess -- but it has only been verified against the panels
 *          Waveshare documents. A panel whose LUT is neither ``M641`` nor
 *          the generic numbering must state its map in the BSP descriptor.
 *
 * @see ra8_epaper_dev_info
 * @see ra8_epaper_waveform_cfg_t
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_waveform_cfg_for_lut(const char*                lut_version,
                                                        ra8_epaper_waveform_cfg_t* out_cfg);

/**
 * @brief Validate a panel descriptor without touching any hardware.
 *
 * @details
 * ``ra8_epaper_init`` calls this before it drives a single pin, so a bad
 * descriptor is rejected with no bus traffic and no reset pulse. It is
 * public because it is a total function over the descriptor: a board
 * bring-up can check its own ``ra8_epaper_cfg_t`` before a controller is
 * even attached.
 *
 * Beyond range-checking the geometry, this rejects a zero-initialised
 * waveform map. That check is load-bearing: mode 0 is INIT on every
 * documented LUT, so a descriptor that forgot to fill the map would
 * refresh *every* mode as a full INIT flash rather than fail, and a
 * missing A2 would look like a merely slow panel instead of a
 * misconfigured one.
 *
 * @param[in] cfg Descriptor to check; non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Every field is in range and usable.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg A field is out of range, the bus seam is
 *                               unbound, or the waveform map is unset.
 *
 * @pre  ``cfg`` is fully initialised by the caller.
 * @pre  ``cfg->waveform`` names this panel's LUT mode numbers.
 * @post No hardware is touched and no driver state is mutated.
 * @post On ``k_ra8_ok`` the descriptor is safe to pass to
 *       ``ra8_epaper_init``.
 *
 * @note Thread-safe: pure function over its argument.
 *
 * @see ra8_epaper_init
 * @see ra8_epaper_waveform_cfg_for_lut
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_validate_cfg(const ra8_epaper_cfg_t* cfg);

/**
 * @brief Report whether reported panel geometry matches the descriptor.
 *
 * @details
 * ``ra8_epaper_init`` cross-checks the geometry the controller reports in
 * its ``GET_DEV_INFO`` block against the descriptor it was handed, and
 * **warns rather than fails** on a mismatch. Both halves of that are
 * deliberate: a controller that has not finished loading its waveform
 * answers the first read with zeroes, and an application may legitimately
 * drive a sub-window of a larger panel, so a mismatch is information
 * rather than an error.
 *
 * Split out of ``ra8_epaper_init`` so the comparison is a pure function
 * that host tests can vary directly. Inside ``init`` it could only be
 * reached through a real ``GET_DEV_INFO`` exchange, whose reported
 * dimensions the test bus cannot steer to a value that is also a legal
 * descriptor size -- which left the "agrees" case untestable.
 *
 * @param[in] info Decoded device block; NULL is treated as disagreement.
 * @param[in] cfg  Descriptor to compare against; NULL likewise.
 *
 * @return ``true`` when both dimensions match, ``false`` otherwise.
 *
 * @pre  ``info`` was populated by ``ra8_epaper_decode_dev_info``.
 * @pre  ``cfg`` is the descriptor passed to ``ra8_epaper_init``.
 * @post No state is mutated and no bus traffic is issued.
 * @post A NULL argument yields ``false`` rather than a fault.
 *
 * @note Thread-safe: pure function over its arguments.
 *
 * @see ra8_epaper_init
 * @see ra8_epaper_decode_dev_info
 *
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_epaper_geometry_agrees(const ra8_epaper_dev_info_t* info,
                                              const ra8_epaper_cfg_t*      cfg);

/**
 * @brief Hand back the ``GET_DEV_INFO`` block captured during init.
 *
 * @details
 * ``ra8_epaper_init`` decodes the 40-byte response once and caches it, so
 * this is an O(1) copy with no bus traffic.
 *
 * @param[out] out_info Receives the decoded device block; non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Info copied.
 * @retval k_ra8_err_null_ptr      ``out_info`` is NULL.
 * @retval k_ra8_err_invalid_state Panel never initialized.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``out_info`` is writable.
 * @post ``*out_info`` mirrors the cached block.
 * @post No bus traffic and no driver state mutated.
 *
 * @note Thread-safe against other readers; not against ``ra8_epaper_init``.
 *
 * @see ra8_epaper_dev_info_t
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_dev_info(ra8_epaper_dev_info_t* out_info);

/**
 * @brief Decode a raw ``GET_DEV_INFO`` word block into a device-info struct.
 *
 * @details
 * The controller answers ``GET_DEV_INFO`` with 20 big-endian-assembled
 * words: panel width, panel height, the low and high halves of the
 * image-buffer base address, then two 16-character version strings packed
 * two ASCII chars per word. This is the pure decode half of that read --
 * ``ra8_epaper_init`` buffers the words off the bus and calls this.
 *
 * Version bytes outside printable ASCII are dropped to NUL, because the
 * strings are logged and a controller that has not finished loading its
 * waveform answers early reads with garbage.
 *
 * Split out so the layout can be pinned by host tests with no controller
 * attached: the decoded ``lut_version`` is what
 * ``ra8_epaper_waveform_cfg_for_lut`` maps to a waveform mode number, so a
 * mis-decode silently selects the wrong A2 mode on the panel.
 *
 * @param[in]  words    Response words, most significant byte first as
 *                      assembled by the bus layer; non-NULL.
 * @param[in]  count    Number of words available in ``words``; must be at
 *                      least 20.
 * @param[out] out_info Receives the decoded block; non-NULL. Fully
 *                      overwritten, so it need not be pre-zeroed.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Block decoded.
 * @retval k_ra8_err_null_ptr     ``words`` or ``out_info`` is NULL.
 * @retval k_ra8_err_invalid_arg  ``count`` is below the 20-word layout.
 *
 * @pre  ``words`` holds at least ``count`` readable words.
 * @pre  ``out_info`` is writable.
 * @post On success ``fw_version`` and ``lut_version`` are NUL-terminated.
 * @post No bus traffic and no driver state mutated.
 *
 * @note Thread-safe: pure function over its arguments.
 *
 * @par Example:
 * @code
 * ra8_epaper_dev_info_t info = {};
 * if (ra8_epaper_decode_dev_info(words, 20U, &info) == k_ra8_ok) {
 *   ra8_epaper_waveform_cfg_t wf = {};
 *   (void)ra8_epaper_waveform_cfg_for_lut(info.lut_version, &wf);
 * }
 * @endcode
 *
 * @see ra8_epaper_dev_info
 * @see ra8_epaper_waveform_cfg_for_lut
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epaper_decode_dev_info(const uint16_t* words, size_t count, ra8_epaper_dev_info_t* out_info);

/**
 * @brief Read the controller's current VCOM setting, in millivolts.
 *
 * @details
 * Issues the VCOM command (``0x0039``) with argument word ``0x0000``
 * ("get") and reads back one data word. The returned value is the
 * **magnitude** of the panel common-electrode bias in millivolts: VCOM is
 * always negative, so a panel labelled ``-1.53V`` reads back as ``1530``.
 * Confirmed against the Waveshare IT8951 reference driver's
 * ``EPD_IT8951_GetVCOM`` (``USDEF_I80_CMD_VCOM = 0x0039``).
 *
 * @param[out] out_mv Receives the VCOM magnitude in millivolts; non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Value read.
 * @retval k_ra8_err_null_ptr      ``out_mv`` is NULL.
 * @retval k_ra8_err_invalid_state Panel never initialized.
 * @retval k_ra8_err_hw_timeout    HRDY stuck low.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``out_mv`` is writable.
 * @post On success ``*out_mv`` holds the controller's VCOM magnitude.
 * @post No panel pixels are disturbed.
 *
 * @note Not thread-safe.
 * @warning The value the controller powers up with is whatever its own
 *          driver board persisted. Treat it as a *candidate* to be range
 *          checked, never as authoritative -- see ``ra8_epd_cal_resolve``.
 *
 * @see ra8_epaper_set_vcom
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_get_vcom(uint16_t* out_mv);

/**
 * @brief Programme the controller's VCOM, verify it, and grant the
 *        INV-VCOM-1 permit.
 *
 * @details
 * Issues the VCOM command (``0x0039``) with argument word ``0x0001``
 * ("set") followed by ``mv``, then immediately re-reads it with argument
 * ``0x0000`` and compares. Confirmed against the Waveshare IT8951
 * reference driver's ``EPD_IT8951_SetVCOM``.
 *
 * **The readback is not a diagnostic nicety, it is the invariant.** A
 * dead COPI line, a controller that quietly ignores the command, or a bus
 * returning zeroes all *accept* the write and change nothing -- a failure
 * mode that is indistinguishable from success without the compare, and
 * that would leave the film biased at whatever unknown value the
 * controller powered up with. Only a matching readback sets the permit
 * that ::ra8_epaper_display_area requires; any other outcome revokes it
 * and the panel stays dark.
 *
 * The permit is revoked on entry, so a failed call can never leave a
 * previous call's permit standing, and it does not survive
 * ::ra8_epaper_sleep.
 *
 * @param[in] mv VCOM **magnitude** in millivolts (the panel's label
 *               without its minus sign, e.g. ``1530`` for ``-1.53V``).
 *               Must be non-zero; the driver has no way to know the
 *               panel's legal window and does **not** range-check beyond
 *               that -- range checking against the panel's documented
 *               limits is ``ra8_epd_cal``'s job and must happen before
 *               this call.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                    VCOM programmed and read back equal;
 *                                     display commands are now permitted.
 * @retval k_ra8_err_invalid_arg       ``mv`` is zero.
 * @retval k_ra8_err_invalid_state     Panel never initialized.
 * @retval k_ra8_err_validation_failed The controller echoed a different
 *                                     value. The link or the controller is
 *                                     not to be trusted with the bias;
 *                                     the panel stays un-drivable.
 * @retval k_ra8_err_hw_timeout        HRDY stuck low.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``mv`` was validated against the panel's documented VCOM window.
 * @post On success the controller reports the new VCOM and
 *       ``ra8_epaper_vcom_verified()`` is true.
 * @post On any failure ``ra8_epaper_vcom_verified()`` is false and every
 *       subsequent ::ra8_epaper_display_area is refused.
 *
 * @note Not thread-safe.
 * @warning **Driving a panel at the wrong VCOM damages it.** The error is
 *          cumulative and not recoverable: a net DC bias across the
 *          electrophoretic film degrades contrast permanently, as a
 *          function of *time under bias* rather than refresh count. VCOM
 *          is per-unit calibration data printed on the panel's own flex
 *          cable; it must never be hardcoded in firmware. Resolve it
 *          through ``ra8_epd_cal_resolve`` and refuse to drive the panel
 *          when no trusted value exists.
 * @warning Persistence across a power cycle is **not established**. The
 *          vendor reference driver re-sends VCOM on every run, which
 *          implies this command programs a volatile setting rather than
 *          the driver board's stored configuration. Re-apply it at every
 *          init and do not rely on it sticking.
 *
 * @see ra8_epaper_get_vcom
 * @see ra8_epaper_vcom_verified
 * @see ra8_epaper_display_area
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_set_vcom(uint16_t mv);

/**
 * @brief Report whether the INV-VCOM-1 permit is currently held.
 *
 * @details
 * True only while the driver is initialized **and** the most recent
 * ::ra8_epaper_set_vcom read its value back unchanged. This is the exact
 * predicate ::ra8_epaper_display_area enforces, exposed so an application
 * can decide what to show (or, correctly, not show) without having to
 * provoke a refused refresh to find out.
 *
 * @return ``true`` when display commands are permitted, ``false`` otherwise.
 *
 * @pre  None; safe to call before ``ra8_epaper_init``.
 * @post No bus traffic and no driver state mutated.
 *
 * @note Not thread-safe against ``ra8_epaper_set_vcom`` / ``_init`` /
 *       ``_sleep``; safe against other readers.
 *
 * @see ra8_epaper_set_vcom
 * @see ra8_epaper_display_area
 *
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_epaper_vcom_verified(void);

/**
 * @brief Push a packed greyscale buffer into the controller's frame RAM.
 *
 * @details
 * Sends the LD_IMG_AREA (0x0021) command then streams ``buf`` over
 * SPI. Internally:
 *  1. Set target frame address via REG_LISAR (0x0208 / 0x020A).
 *  2. Send LD_IMG_AREA + 5-arg block: endianness, pf, rotate, area.
 *  3. Stream ``buf`` 16-bit words MSB-first.
 *  4. Send LD_IMG_END (0x0022).
 *
 * ``buf`` is packed at ``pf``'s depth, rows byte-aligned -- exactly
 * ``ra8_epaper_image_bytes(area, pf, ...)`` bytes. Prefer
 * ::k_ra8_epaper_pf_4bpp for greyscale: it matches the panel's 16 grey
 * levels, the controller discards the low nibble of 8 bpp data anyway,
 * and it halves the wire time.
 *
 * @param[in] area      Rectangle to update (panel coords); non-NULL. For
 *                      ::k_ra8_epaper_pf_1bpp, ``x`` and ``width`` must be
 *                      multiples of ::k_ra8_epaper_align_1bpp_px.
 * @param[in] buf       Packed source buffer; non-NULL.
 * @param[in] buf_len   Length of ``buf`` in bytes; must equal
 *                      ``ra8_epaper_image_bytes(area, pf, ...)``.
 * @param[in] pf        Source pixel depth.
 * @param[in] endian    Source endianness.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Buffer transferred.
 * @retval k_ra8_err_null_ptr        ``area`` or ``buf`` is NULL.
 * @retval k_ra8_err_invalid_state   Panel never initialized.
 * @retval k_ra8_err_invalid_size    ``buf_len`` mismatch or empty area.
 * @retval k_ra8_err_invalid_arg     1 bpp rectangle violates the 32-pixel
 *                                   X/width alignment.
 * @retval k_ra8_err_hw_timeout      HRDY stuck low.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``buf_len`` matches ``area`` at depth ``pf``.
 * @post Frame RAM holds the new pixel data; panel is NOT yet refreshed.
 * @post On any error path no partial area is marked for refresh.
 *
 * @note Not thread-safe.
 *
 * @see ra8_epaper_image_bytes
 * @see ra8_epaper_align_area
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_load_image(const ra8_epaper_area_t*  area,
                                              const uint8_t*            buf,
                                              size_t                    buf_len,
                                              ra8_epaper_pixel_format_t pf,
                                              ra8_epaper_endian_t       endian);

/**
 * @brief Refresh the indicated rectangle on the physical panel.
 *
 * @details
 * Resolves ``waveform`` through the panel's ::ra8_epaper_waveform_cfg_t
 * (supplied at init) into the LUT mode number this panel's controller
 * firmware expects, issues DPY_AREA (0x0034) with ``area`` and that mode
 * number, then polls REG_LUTAFSR (0x1224) until it reads zero (no LUTs
 * busy). IT8951 datasheet rev 0.2 chapter 4.2.4.
 *
 * **This call enforces INV-VCOM-1.** DPY_AREA is the command that puts a
 * bias across the electrophoretic film, so it is refused outright unless
 * ::ra8_epaper_set_vcom has programmed a VCOM *and read it back
 * unchanged*. There is no override and no "just this once" path: a wrong
 * or unknown bias damages the panel cumulatively with time under bias,
 * and a blank screen is recoverable where a degraded panel is not. Stage
 * pixels with ::ra8_epaper_load_image as much as you like beforehand --
 * loading frame RAM applies no bias and is deliberately ungated.
 *
 * @param[in] area     Rectangle to refresh (panel coords); non-NULL.
 * @param[in] waveform Symbolic waveform selector -- **not** a wire value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                    Panel idle and updated.
 * @retval k_ra8_err_null_ptr          ``area`` is NULL.
 * @retval k_ra8_err_invalid_state     Panel never initialized.
 * @retval k_ra8_err_validation_failed No verified VCOM -- INV-VCOM-1
 *                                     refusal. Resolve one through
 *                                     ``ra8_epd_cal_resolve`` and apply it;
 *                                     do not retry blind.
 * @retval k_ra8_err_invalid_arg       ``waveform`` is not a known selector.
 * @retval k_ra8_err_hw_timeout        LUT busy never cleared.
 *
 * @pre  ``ra8_epaper_load_image`` populated the area.
 * @pre  ``ra8_epaper_vcom_verified()`` is true.
 * @post The pixels of ``area`` match the most recent load.
 * @post The controller reports every LUT idle again.
 *
 * @note Not thread-safe.
 *
 * @see ra8_epaper_set_vcom
 * @see ra8_epaper_vcom_verified
 * @see ra8_epaper_waveform_cfg_for_lut
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_display_area(const ra8_epaper_area_t* area,
                                                ra8_epaper_waveform_t    waveform);

/**
 * @brief Drop the panel into deep-sleep (~15 uA per Waveshare AN).
 *
 * @details
 * Issues SLEEP (0x0003). Subsequent calls require a fresh
 * ``ra8_epaper_init`` because the controller forgets all register
 * state.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Panel asleep.
 * @retval k_ra8_err_invalid_state  Panel never initialized.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @post Driver state machine is back in ``uninitialized``; caller
 *       must re-init before any further load / display.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epaper_sleep(void);

#ifdef __cplusplus
}
#endif
