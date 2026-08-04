/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_stbtt_guard.h
 * @brief sfnt (TrueType/OpenType) table-directory bounds guard for stb_truetype.
 * @ingroup grp_ereader
 *
 * @details
 * The vendored stb_truetype (`libs/third_party/stb/stb_truetype.h`) parses the
 * sfnt table directory using the `numTables` count and the per-table
 * offset/length fields read straight out of the font file, with no check that
 * those values stay inside the supplied buffer. `stbtt_InitFont()` ->
 * `stbtt__find_table()` therefore walks past the end of a crafted font and
 * performs an out-of-bounds read (SEGV under AddressSanitizer). stb_truetype
 * never takes a buffer length, so it cannot self-defend.
 *
 * The e-reader feeds `stbtt_InitFont()` fully attacker-controlled bytes: the
 * `@font-face` fonts embedded in an EPUB (via `ra8_reflow_register_face()` /
 * `ra8_reflow_bind_font()`) and the book font attached with
 * `ra8_epub_set_font()`. An attacker-supplied EPUB can thus reach the unguarded
 * parse on-device.
 *
 * ::ra8_stbtt_sfnt_dir_in_bounds validates the full offset table + table
 * directory against the buffer length *before* `stbtt_InitFont()` is invoked,
 * so the firmware rejects a malformed font cleanly instead of dereferencing
 * past its buffer. It is a pure, heap-free byte validator: it includes no
 * stb_truetype internals and touches no hardware.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Verify that a font's sfnt table directory lies within its buffer.
 *
 * @details
 * Mirrors exactly the reads `stbtt_InitFont()` / `stbtt__find_table()` make
 * while walking the sfnt offset table, table directory, and the table-internal
 * fields InitFont reads, and confirms each one stays inside `[0, len)`:
 *  1. The 12-byte offset table (sfnt version, `numTables`, and the three
 *     binary-search hint fields) must fit: `fontstart + 12 <= len`.
 *  2. The whole table directory must fit:
 *     `fontstart + 12 + numTables * 16 <= len`.
 *  3. Every table record's declared extent must fit: for each record,
 *     `tableOffset + tableLength <= len`.
 *  4. The fields `stbtt_InitFont()` reads *inside* cmap / head / maxp must fit.
 *     Most importantly, InitFont strides the cmap encoding-record sub-directory
 *     as `cmap + 4 + 8 * numTables` using a `numTables` it trusts from inside
 *     the cmap table -- a value the table's declared length does not bound, so
 *     a crafted count (up to 65535) drives a ~512 KiB out-of-bounds read. This
 *     check requires `cmap + 4 + 8 * numTables <= len`, plus the fixed
 *     `head + 52 <= len` and `maxp + 6 <= len` extents InitFont's other
 *     table-internal reads need.
 *
 * All arithmetic is performed in `uint64_t` so that a hostile `fontstart`
 * (up to `UINT32_MAX`, e.g. a crafted TrueType-collection sub-font offset) or
 * a hostile per-table offset/length cannot overflow the bound computation.
 * The function bounds every read `stbtt_InitFont()` itself issues. It does NOT
 * bound the deeper reads the glyph-lookup / rasterisation path makes after init
 * (`stbtt_FindGlyphIndex` following an attacker-controlled cmap subtable offset,
 * then the loca / glyf outline walk); stb_truetype does not bound those against
 * the buffer either, so a crafted font can still drive an out-of-bounds read
 * there. Hardening that deeper path is tracked separately as parser hardening
 * (issue #179); it is a much larger change than this directory pre-check.
 *
 * @param[in] data      Pointer to the first byte of the font buffer.
 * @param[in] len       Length of the font buffer, bytes.
 * @param[in] fontstart Byte offset of this font's sfnt offset table within
 *                      @p data (0 for a bare TTF/OTF; the sub-font offset for
 *                      a TrueType collection). This is the same value passed
 *                      as the `offset` argument to `stbtt_InitFont()`.
 *
 * @return `true` if the offset table, every table record, and every cmap /
 *         head / maxp field InitFont reads lie fully within the buffer;
 *         `false` otherwise.
 * @retval true  The directory is in bounds; `stbtt_InitFont()` may safely walk it.
 * @retval false @p data is `nullptr`, or the offset table / directory / any
 *               table record / a cmap-head-maxp internal read extends past
 *               @p len -- reject the font.
 *
 * @pre @p data references at least @p len readable bytes (or is `nullptr`).
 * @pre @p len is the true byte length of the buffer @p data points to.
 * @post @p data and the buffer it points to are unmodified (pure read).
 * @post No read `stbtt_InitFont()` issues lands outside `[data, data + len)`.
 *
 * @note Thread-safe and re-entrant: no shared or static state, no allocation.
 *
 * @par Example:
 * @code
 * const int32_t off = stbtt_GetFontOffsetForIndex(blob, 0);
 * if (off >= 0 && !ra8_stbtt_sfnt_dir_in_bounds(blob, blob_len, (uint32_t)off)) {
 *   return k_ra8_err_not_supported; // malformed directory -- reject
 * }
 * if (off < 0 || stbtt_InitFont(&info, blob, off) == 0) {
 *   return k_ra8_err_not_supported;
 * }
 * @endcode
 *
 * @since 0.1.0
 */
bool ra8_stbtt_sfnt_dir_in_bounds(const uint8_t* data, size_t len, uint32_t fontstart);
