/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sdfont.h
 * @brief Load a TTF/OTF font off a Pmod SD card, self-provisioning if absent.
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 4 / SDFont] {World: NS}
 *
 * @details
 * One reusable helper for the e-reader font-storage path shared by the
 * `sd_font_render` and `ereader_ui` example apps. It brings up an SD card on
 * an SCI Simple-SPI bus (Pmod2 / J25 on the EK-RA8D2), mounts the FAT volume
 * through `ra8_fs`, and loads a named font file into a caller-owned buffer.
 *
 * The distinguishing feature is **self-provisioning**: when the named file is
 * absent from the card, the helper writes a caller-supplied fallback blob
 * (a font baked into flash) to the card, then reads it back -- so a freshly
 * formatted "random" card boots straight into a working render with no
 * host-side image prep. Provisioning is opt-in: pass `provision_blob == NULL`
 * to keep the load strictly read-only (the historical `ereader_ui` behaviour).
 *
 * The helper is transport-thin: it owns the `ra8_sdmmc_spi` <-> `ra8_sci_spi`
 * chip-select / clock / transfer shim internally, so callers only describe the
 * bus (channel + four pins + PCLKA rate) instead of re-implementing those three
 * callbacks in every app.
 *
 * @par Example:
 * @code
 * const ra8_sdfont_cfg_t cfg = {
 *   .spi_channel    = 0U,                                   // Pmod2 = SCI0
 *   .sck            = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck,
 *   .cipo           = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo,
 *   .copi           = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi,
 *   .cs             = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs,
 *   .pclka_hz       = pclka_hz,
 *   .filename       = "FONT.OTF",
 *   .provision_blob = g_ra8_font_literata_latin1,            // baked fallback
 *   .provision_len  = g_ra8_font_literata_latin1_len,
 * };
 * uint32_t           len = 0U;
 * ra8_sdfont_source_t src = k_ra8_sdfont_source_card;
 * if (ra8_sdfont_load(&cfg, font_buf, sizeof(font_buf), &len, &src) == k_ra8_ok) {
 *   ra8_reflow_init(w, h, font_buf, len, px, ink, link, &engine);
 * }
 * @endcode
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"

/**
 * @enum ra8_sdfont_source_t
 * @brief Provenance of the font bytes returned by ::ra8_sdfont_load.
 *
 * @details Lets the caller distinguish a card that already carried the font
 * from one that was empty and got provisioned on this boot -- useful for
 * diagnostics, HIL banners, and deciding whether to surface a "first run"
 * message.
 *
 * @see ra8_sdfont_load
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_sdfont_source_card        = 0, /**< File already on the card; read as-is. */
  k_ra8_sdfont_source_provisioned = 1, /**< File was absent; written from the blob, then
                                       *   read back from the card.                     */
} ra8_sdfont_source_t;

/**
 * @struct ra8_sdfont_cfg_t
 * @brief SD-card font-store bus description + provisioning policy.
 *
 * @details Describes the SCI Simple-SPI bus the SD card sits on and the
 * fallback font used for self-provisioning. The chip-select pin is driven as a
 * plain GPIO output because SD SPI-mode holds CS asserted across a whole
 * command + data trailer, which the SCI hardware chip-select controller (which
 * pulses per word) cannot do.
 *
 * @invariant @ref sck, @ref cipo, @ref copi, @ref cs are four distinct pins.
 * @invariant When @ref provision_blob is non-NULL, @ref provision_len > 0.
 *
 * @see ra8_sdfont_load
 * @since 0.1.0
 */
typedef struct {
  uint8_t        spi_channel;    /**< SCI channel in Simple-SPI mode (Pmod2 = SCI0 = 0). */
  ra8_port_pin_t sck;            /**< SPI clock pin.                                     */
  ra8_port_pin_t cipo;           /**< Controller-In / Peripheral-Out pin.                */
  ra8_port_pin_t copi;           /**< Controller-Out / Peripheral-In pin.                */
  ra8_port_pin_t cs;             /**< Chip-select pin, driven as a GPIO output.          */
  uint32_t       pclka_hz;       /**< PCLKA rate (Hz) feeding the SCI baud divider.      */
  const char*    filename;       /**< 8.3 file to load; NULL selects "FONT.OTF".         */
  const uint8_t* provision_blob; /**< Fallback font written if the file is absent;
                                  *   NULL disables provisioning (read-only load).       */
  uint32_t       provision_len;  /**< Length of @ref provision_blob in bytes. */
} ra8_sdfont_cfg_t;

/**
 * @brief Mount the Pmod SD card and load a font, provisioning it if absent.
 *
 * @details
 * Routes the configured SPI pins to SCI Simple-SPI, claims chip-select as a
 * GPIO output, brings up the card through `ra8_sdmmc_spi`, mounts the FAT volume
 * through `ra8_fs`, and opens @ref ra8_sdfont_cfg_t::filename for reading. If the
 * file is absent and @ref ra8_sdfont_cfg_t::provision_blob is non-NULL, the blob
 * is written to the card with `ra8_fs_write_file`, then re-opened and read back
 * -- so the returned bytes are always the on-card copy, byte-for-byte identical
 * to what a later boot will read. The font is copied into @p buf and the FAT
 * volume is unmounted before return.
 *
 * @param[in]  cfg        Non-NULL bus + provisioning descriptor.
 * @param[out] buf        Non-NULL destination buffer for the font bytes.
 * @param[in]  cap        Capacity of @p buf in bytes; must be > 0.
 * @param[out] out_len    Non-NULL; receives the number of bytes loaded.
 * @param[out] out_source Optional (may be NULL); receives whether the font came
 *                        from the card or was provisioned this boot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Font loaded; @p *out_len holds its length.
 * @retval k_ra8_err_null_ptr    @p cfg, @p buf, or @p out_len is NULL.
 * @retval k_ra8_err_invalid_arg @p cap is 0.
 * @retval k_ra8_err_not_found   File absent and provisioning disabled (NULL blob).
 * @retval k_ra8_err_no_data     File present but shorter than a usable font header.
 * @retval other                Propagated from `ra8_sci_spi`, `ra8_sdmmc_spi`, or
 *                              `ra8_fs` (no card, mount failure, write failure...).
 *
 * @pre `ra8_cgc_init` has run and @ref ra8_sdfont_cfg_t::pclka_hz is the live PCLKA rate.
 * @pre @p buf points to at least @p cap writable bytes (SDRAM-backed for big fonts).
 * @post On success @p *out_len is in `[16, cap]` and @p buf holds the font.
 * @post On any return the FAT volume opened here is unmounted.
 *
 * @note Not thread-safe and not ISR-safe: blocking, polled SPI; single-shot
 *       module state backs the transport shim. Call once from init context.
 *
 * @see ra8_sdfont_source_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdfont_load(const ra8_sdfont_cfg_t* cfg,
                                        uint8_t*                buf,
                                        uint32_t                cap,
                                        uint32_t*               out_len,
                                        ra8_sdfont_source_t*    out_source);
