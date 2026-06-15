/**
 * @file arnopro_latin1.h
 * @brief Declaration of the Latin-1 ArnoPro font baked into internal flash.
 *
 * @par Tag
 * [Ring 4 / Fonts] {World: NS}
 *
 * @details
 * One header per baked font (named after the font), declaring the `extern`
 * symbols that `scripts/utils/font_to_c.py` defines from
 * `libs/fonts/arnopro_latin1.otf`. The generator emits a build-only `.c`
 * (`const unsigned char g_ra_font_arnopro_latin1[]` + `_len`) that `#include`s
 * this header; apps that consume the font include it too. The hex array is
 * generated at build time and is **not committed**, so the large byte literals
 * never reach the QC gates.
 *
 * A baked font is a valid TTF/OTF blob accepted by `ra_reflow_init`'s
 * `font_data`, and is the natural provisioning blob for @ref ra_sdfont_load:
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
 * @var g_ra_font_arnopro_latin1
 * @brief Baked Latin-1 subset of ArnoPro-Regular (OpenType/CFF), in flash.
 *
 * @details A ~56 KB Latin-1 + common-typographic subset of
 * `libs/fonts/ArnoPro-Regular.otf`, checked in at
 * `libs/fonts/arnopro_latin1.otf` and turned into a `.rodata` array at build
 * time by `scripts/utils/font_to_c.py`. A valid OTF blob for `ra_reflow_init`.
 * @note Generated, build-only definition; read-only.
 * @since 0.1.0
 */
extern const unsigned char g_ra_font_arnopro_latin1[];

/**
 * @var g_ra_font_arnopro_latin1_len
 * @brief Length of ::g_ra_font_arnopro_latin1 in bytes.
 * @since 0.1.0
 */
extern const unsigned int g_ra_font_arnopro_latin1_len;
