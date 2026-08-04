/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_export_internal.h
 * @brief Module-private page-list limits and the JOF exporter entry point.
 *
 * @details
 * The chapter exporter is one logical module split across two translation
 * units: the container writers and the format dispatch (mdl_export.c), and the
 * JOF tile-atlas exporter (mdl_export_jof.c). The JOF arm is the only one that
 * pulls in the firmware's `ra8_jof` producer -- a whole decode/encode
 * stack the archive writers never touch -- so it owns its own translation unit
 * and its own includes rather than widening the dependency surface of every
 * other format.
 *
 * The page-list limits live here because both units size the same fixed
 * `names[][]` table with them: the dispatcher declares the storage, the JOF
 * exporter walks it, and a second private copy of either bound in one unit
 * would let the two disagree silently.
 *
 * Nothing in this header is part of the exporter-facing API in mdl_export.h.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum mdl_export_limits_t
 * @brief Fixed bounds on the per-chapter page list.
 *
 * @details
 * The exporter walks a chapter directory into a statically sized table rather
 * than allocating per entry, so both bounds are hard caps: a chapter with more
 * than ::k_max_pages images is truncated at the listing step, and an entry name
 * longer than ::k_name_max - 1 bytes is not a page this tool packages.
 *
 * @invariant Every `names[][]` table passed between the two exporter units is
 *            declared `char[k_max_pages][k_name_max]`.
 *
 * @par Example:
 * @code
 * static char s_names[k_max_pages][k_name_max];
 * const size_t count = list_pages(dir, s_names, (size_t)k_max_pages);
 * @endcode
 *
 * @see mdl_export_jof()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_max_pages = 2048, /**< Max page entries per chapter. */
  k_name_max  = 256,  /**< Max entry-name bytes.         */
} mdl_export_limits_t;

/**
 * @brief Convert every page in a chapter directory to a sibling `.jof` atlas.
 *
 * @details
 * Transcodes each listed page in place: `page.jpg` becomes `page.jof` beside
 * it, leaving the source file untouched. Each page is produced through the
 * firmware's own `ra8_jof` producer, so a container this tool writes is
 * byte-identical to one the board would produce from the same source, and the
 * full-width band geometry is the shape `ra8_longstrip` scrolls.
 *
 * Stops at the first page that fails and reports that page's error, so a
 * partially converted directory is always explained by a non-ok return rather
 * than discovered later; pages already written before the failure remain.
 *
 * @param[in] dir   Chapter directory holding the page files.
 * @param[in] names Page file names, `count` entries of at most ::k_name_max
 *                  bytes each, in the order they should be converted.
 * @param[in] count Number of valid entries in @p names.
 *
 * @return Result code.
 * @retval k_ra8_ok                Every page was transcoded.
 * @retval k_ra8_err_not_supported A page is in no format the producer decodes.
 * @retval k_ra8_err_invalid_size  A page's geometry admits no work arena.
 * @retval k_ra8_err_no_mem        The host allocator refused an arena.
 * @retval k_ra8_fail              A page could not be read, or its output
 *                                 could not be opened or written.
 *
 * @pre @p dir names an existing, readable directory.
 * @pre @p names holds @p count readable entries.
 * @post On ::k_ra8_ok a `.jof` sibling exists for every entry in @p names.
 * @post Any output file whose produce step failed is removed, never left torn.
 *
 * @note Not thread-safe: installs a process-wide `ra8_log` byte sink.
 * @warning Redirects `ra8_log` output to stderr for the rest of the process.
 *
 * @see mdl_export_chapter()
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t mdl_export_jof(const char* dir, const char names[][k_name_max], size_t count);
