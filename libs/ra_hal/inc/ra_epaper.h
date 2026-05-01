/**
 * @file ra_epaper.h
 * @brief IT8951 e-paper controller SPI driver -- public API
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
 *     the controller whether the master is about to send a command
 *     (`0x6000`), send pixel data (`0x0000`), or read pixel/status
 *     data (`0x1000`).
 *   - For command writes the host then clocks the 16-bit command
 *     code MSB-first.
 *   - For data writes / reads the host clocks 16-bit words MSB-first
 *     until the transfer ends.
 *   - Between every preamble and the data phase the host must wait
 *     for the controller's "HRDY" GPIO to assert.
 *
 * Public API surface (matches the four entry points the ereader app
 * calls):
 *
 *  - ``ra_epaper_init``         -- bring up SPI + reset + run
 *                                  GET_DEV_INFO (0x0302).
 *  - ``ra_epaper_load_image``   -- transfer an 8 bpp greyscale buffer
 *                                  into the controller's frame
 *                                  buffer at a (x,y) location.
 *  - ``ra_epaper_display_area`` -- kick off a panel refresh of the
 *                                  rectangle just loaded with the
 *                                  selected waveform mode.
 *  - ``ra_epaper_sleep``        -- send SLEEP (0x0003) so the
 *                                  panel drops into low-power.
 *
 * Static allocation only -- the driver keeps one panel context
 * (``s_panel``) at file scope and rejects double-init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_epaper_waveform_t
 * @brief IT8951 panel-refresh waveform modes.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.4 "Display Update Modes" /
 * Waveshare IT8951 application note table 2. Each waveform trades
 * refresh latency for ghosting / quality. Only the four modes that
 * the ereader page-turn flow exercises are exposed; users that need
 * the full 16-mode set can extend this enum.
 */
typedef enum : uint8_t {
  k_ra_epaper_wf_init = 0U, /**< INIT - flush to white, slowest.       */
  k_ra_epaper_wf_du   = 1U, /**< DU   - direct update, 1 bpp, fast.    */
  k_ra_epaper_wf_gc16 = 2U, /**< GC16 - 16-grey full quality.          */
  k_ra_epaper_wf_a2   = 4U, /**< A2   - black/white only, fastest.     */
} ra_epaper_waveform_t;

/**
 * @enum ra_epaper_pixel_format_t
 * @brief Pixel-format encoding written into LISAR before a load.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.5 "Image Buffer Endianness".
 * The driver only loads 8 bpp greyscale (one byte per pixel) since
 * that is what ra_reflow / ra_gfx produce when targeting an EPD.
 */
typedef enum : uint8_t {
  k_ra_epaper_pf_8bpp = 0U, /**< 8 bits per pixel, packed greyscale.   */
} ra_epaper_pixel_format_t;

/**
 * @enum ra_epaper_endian_t
 * @brief Source-buffer endianness flag for the LD_IMG_AREA transfer.
 *
 * @details
 * IT8951 datasheet rev 0.2 chapter 4.1.5 -- the panel can swap byte
 * pairs on the fly so the host does not have to re-pack.
 */
typedef enum : uint8_t {
  k_ra_epaper_endian_little = 0U, /**< Host buffer is little-endian.   */
  k_ra_epaper_endian_big    = 1U, /**< Host buffer is big-endian.      */
} ra_epaper_endian_t;

/**
 * @struct ra_epaper_cfg_t
 * @brief Configuration descriptor for ``ra_epaper_init``.
 *
 * @details
 * The driver lets the caller pick the SPI channel (0 or 1) and the
 * SPI baud, since different carrier boards may pair the panel with
 * different SPI lines. The reset / busy GPIO pins are encoded as
 * ``ra_port_pin_t`` packed (port<<8 | pin) values to avoid a
 * dependency on ra_gpio_constants.h here.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  spi_channel;  /**< SPI channel: 0 or 1.                   */
  uint32_t spi_baud_hz;  /**< Target SPI clock; 12 MHz typical.      */
  uint32_t pclka_hz;     /**< Current PCLKA in Hz (for SPBR calc).   */
  uint16_t reset_pin;    /**< (port<<8)|pin -- panel /RESET line.    */
  uint16_t busy_pin;     /**< (port<<8)|pin -- panel HRDY input.     */
  uint16_t panel_width;  /**< Native panel width in pixels.          */
  uint16_t panel_height; /**< Native panel height in pixels.         */
} ra_epaper_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_epaper_area_t
 * @brief Rectangle descriptor (top-left + size) used by load + display.
 */
typedef struct {
  uint16_t x;      /**< Top-left X in panel coords (0 = left).   */
  uint16_t y;      /**< Top-left Y in panel coords (0 = top).    */
  uint16_t width;  /**< Width in pixels.                         */
  uint16_t height; /**< Height in pixels.                        */
} ra_epaper_area_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the IT8951 panel against the configured SPI bus.
 *
 * @details
 * Algorithm:
 *  1. Validate ``cfg``.
 *  2. Initialise the SPI channel via ``ra_spi_init`` in mode 0.
 *  3. Pulse the panel /RESET line (10 ms low / 10 ms high) so the
 *     IT8951 resets into a known state.
 *  4. Wait for HRDY to assert.
 *  5. Send SYS_RUN (0x0001) to take the controller out of standby.
 *  6. Send GET_DEV_INFO (0x0302) and consume the 40-byte response so
 *     the device's panel size matches ``cfg``.
 *
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Panel responsive and identified.
 * @retval k_ra_err_null_ptr       ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg    ``cfg`` field out of range.
 * @retval k_ra_err_invalid_state  Driver already initialised.
 * @retval k_ra_err_hw_timeout     HRDY never asserted.
 * @retval k_ra_err_hw_init_failed SPI init failed.
 *
 * @pre  ``ra_mstp_init`` and ``ra_pfs_route_peripheral`` for SPI pins done.
 * @pre  Reset / busy pins configured as GPIO output / input.
 *
 * @post On success, the driver state machine is in
 *       ``k_ra_epaper_state_ready`` and accepts load / display calls.
 *
 * @note Not thread-safe; called once during single-threaded init.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epaper_init(const ra_epaper_cfg_t* cfg);

/**
 * @brief Push an 8 bpp greyscale buffer into the controller's frame RAM.
 *
 * @details
 * Sends the LD_IMG_AREA (0x0021) command then streams ``buf`` over
 * SPI. Internally:
 *  1. Set target frame address via REG_LISAR (0x1000 / 0x1004).
 *  2. Send LD_IMG_AREA + 4-arg block: endianness, pf, rotate, area.
 *  3. Stream ``buf`` 16-bit words MSB-first.
 *  4. Send LD_IMG_END (0x0022).
 *
 * @param[in] area      Rectangle to update (panel coords).
 * @param[in] buf       Source buffer; ``area->width * area->height``
 *                      bytes, 8 bpp greyscale.
 * @param[in] buf_len   Length of ``buf`` in bytes; must equal
 *                      ``area->width * area->height``.
 * @param[in] endian    Source endianness.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Buffer transferred.
 * @retval k_ra_err_null_ptr        ``area`` or ``buf`` is NULL.
 * @retval k_ra_err_invalid_state   Panel never initialised.
 * @retval k_ra_err_invalid_size    ``buf_len`` mismatch.
 * @retval k_ra_err_hw_timeout      HRDY stuck low.
 *
 * @pre  ``ra_epaper_init`` succeeded.
 * @post Frame RAM holds the new pixel data; panel is NOT yet refreshed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epaper_load_image(const ra_epaper_area_t* area,
                                            const uint8_t*          buf,
                                            size_t                  buf_len,
                                            ra_epaper_endian_t      endian);

/**
 * @brief Refresh the indicated rectangle on the physical panel.
 *
 * @details
 * Issues DPY_AREA (0x0034) with ``area`` and ``waveform``, then
 * polls REG_LUTAFSR (0x1224) until it reads zero (no LUTs busy).
 * IT8951 datasheet rev 0.2 chapter 4.2.4.
 *
 * @param[in] area     Rectangle to refresh (panel coords).
 * @param[in] waveform Waveform mode.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Panel idle and updated.
 * @retval k_ra_err_null_ptr        ``area`` is NULL.
 * @retval k_ra_err_invalid_state   Panel never initialised.
 * @retval k_ra_err_hw_timeout      LUT busy never cleared.
 *
 * @pre  ``ra_epaper_load_image`` populated the area.
 * @post The pixels of ``area`` match the most recent load.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epaper_display_area(const ra_epaper_area_t* area,
                                              ra_epaper_waveform_t    waveform);

/**
 * @brief Drop the panel into deep-sleep (~15 uA per Waveshare AN).
 *
 * @details
 * Issues SLEEP (0x0003). Subsequent calls require a fresh
 * ``ra_epaper_init`` because the controller forgets all register
 * state.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Panel asleep.
 * @retval k_ra_err_invalid_state  Panel never initialised.
 *
 * @pre  ``ra_epaper_init`` succeeded.
 * @post Driver state machine is back in ``uninitialised``; caller
 *       must re-init before any further load / display.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epaper_sleep(void);

#ifdef __cplusplus
}
#endif
