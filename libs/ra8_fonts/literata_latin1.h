/**
 * @file literata_latin1.h
 * @brief Declaration of the Latin-1 Literata font baked into internal flash.
 *
 * @par Tag
 * [Ring 4 / Fonts] {World: NS}
 *
 * @details
 * One header per baked font (named after the font), declaring the `extern`
 * symbols that `scripts/gen/font_to_c.py` defines from
 * `libs/ra8_fonts/literata_latin1.ttf`. The generator emits a build-only `.c`
 * (`const unsigned char g_ra8_font_literata_latin1[]` + `_len`) that `#include`s
 * this header; apps that consume the font include it too. The hex array is
 * generated at build time and is **not committed**, so the large byte literals
 * never reach the QC gates.
 *
 * Literata is Google's open (SIL OFL 1.1) serif family, drawn specifically for
 * long-form on-screen reading in Play Books -- the reading-body face for the
 * e-reader. A baked font is a valid TTF/OTF blob accepted by `ra8_reflow_init`'s
 * `font_data`, and is the natural provisioning blob for @ref ra8_sdfont_load:
 * when a microSD carries no `FONT.OTF`, the baked font is written to the card
 * and read back, so a blank card renders real proportional text with no
 * host-side image prep.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

/**
 * @var g_ra8_font_literata_latin1
 * @brief Baked Latin-1 subset of Literata Regular (TrueType/glyf), in flash.
 *
 * @details A ~37 KB Latin-1 + common-typographic subset of
 * `libs/ra8_fonts/Literata-Regular.ttf` (SIL OFL 1.1), checked in at
 * `libs/ra8_fonts/literata_latin1.ttf` and turned into a `.rodata` array at build
 * time by `scripts/gen/font_to_c.py`. A valid TTF blob for `ra8_reflow_init`.
 * @note Generated, build-only definition; read-only.
 * @since 0.1.0
 */
extern const unsigned char g_ra8_font_literata_latin1[];

/**
 * @var g_ra8_font_literata_latin1_len
 * @brief Length of ::g_ra8_font_literata_latin1 in bytes.
 * @since 0.1.0
 */
extern const unsigned int g_ra8_font_literata_latin1_len;
