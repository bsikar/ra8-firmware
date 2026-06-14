/**
 * @file examples/ek_ra8d2/hw_validated/manual/ereader_ui/baked_font.h
 * @brief Latin-1 subset font baked into internal flash for no-card reflow.
 *
 * @details
 * The Reading body reflows real proportional text through `libs/ra_reflow`.
 * Normally it loads `FONT.OTF` off the microSD; with no card it used to fall
 * back to the bundled 8x16 bitmap font. This blob -- a ~56 KB Latin-1 + common
 * typographic subset of ArnoPro-Regular, checked in at
 * `libs/fonts/arnopro_latin1.otf` -- is compiled into `.rodata` so ra_reflow can
 * render from flash with no SD card at all (the array is generated at build time
 * by `scripts/utils/font_to_c.py`; see that script for the exact pyftsubset
 * command and why the generated `.c` is not committed). Reading from flash also
 * sidesteps the slow emulated-SPI font read in board_sim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

/**
 * @var g_ra_font_arnopro_latin1
 * @brief Baked Latin-1 subset of ArnoPro-Regular (OpenType/CFF), in flash.
 * @details A valid TTF/OTF blob accepted by `ra_reflow_init`'s `font_data`.
 *          Generated from `libs/fonts/arnopro_latin1.otf`.
 * @since 0.1.0
 */
extern const unsigned char g_ra_font_arnopro_latin1[];

/**
 * @var g_ra_font_arnopro_latin1_len
 * @brief Length of ::g_ra_font_arnopro_latin1 in bytes.
 * @since 0.1.0
 */
extern const unsigned int g_ra_font_arnopro_latin1_len;
