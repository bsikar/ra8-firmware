/**
 * @file mdl_urlname.h
 * @brief Pure URL-to-name helpers shared by the CLI and the fetch orchestrator.
 *
 * @details
 * Three small, filesystem-free predicates that both `main.c` (series slug,
 * chapter folder name, page extension) and `mdl_fetch.c` (stable chapter
 * identifiers, page numbering) need, hoisted out of `main.c` so there is exactly
 * one implementation rather than a copy in each translation unit. Each derives a
 * name or a number from a scraped URL and is deliberately kept lexical (no
 * network, no filesystem) so it is unit-testable both directions.
 *
 * The last-segment helper runs its result through ::mdl_sanitize_segment, so a
 * chapter identifier or slug it returns can never be `..`, absolute, or contain
 * a path separator -- the same guarantee the download paths already rely on.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

/**
 * @brief Sanitised last non-empty path segment of a URL.
 *
 * @details
 * Strips any `?query`/`#fragment` and trailing slashes, takes the final path
 * segment, and passes it through ::mdl_sanitize_segment so the result is a
 * single filesystem-safe name. Used for the series slug, per-chapter folder
 * names, and the stable chapter identifier recorded in library state -- an
 * identifier derived from the URL path rather than the chapter's position in a
 * freshly scraped list, so it stays fixed as the site adds or reorders chapters.
 *
 * @param[in]  url Absolute URL to take the last segment of (never NULL).
 * @param[out] out Destination buffer for the sanitised segment (never NULL).
 * @param[in]  cap Capacity of @p out in bytes (must be >= 2).
 *
 * @return Nothing.
 *
 * @pre @p url and @p out are non-NULL; @p url is NUL-terminated.
 * @pre @p cap is at least 2 so a one-character name plus a NUL fits.
 * @post @p out is NUL-terminated and holds no `/`, `.`-only, or `..` result.
 * @post @p out is non-empty (a generated fallback when the segment was empty).
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @see mdl_sanitize_segment
 * @since 0.1.0
 */
void mdl_urlname_last_segment(const char* url, char* out, size_t cap);

/**
 * @brief Parse the last run of decimal digits in a URL as a chapter number.
 *
 * @details
 * Manga/manhwa chapter URLs end in a number (`.../chapter-137`); this returns
 * that number so chapters can be addressed and ordered by identity rather than
 * by list position, and so coverage gaps can be reported. The LAST digit run is
 * taken so a numeric host or path prefix does not mask the chapter number.
 *
 * @param[in] url URL to parse (never NULL).
 *
 * @return The parsed chapter number, or 0 when the URL holds no digits.
 * @retval 0 No decimal digit appears in @p url.
 *
 * @pre @p url is non-NULL and NUL-terminated.
 * @pre The caller treats 0 as "unnumbered", not "chapter zero" specifically.
 * @post @p url is not modified.
 *
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
long mdl_urlname_chapter_number(const char* url);

/**
 * @brief Choose a lower-case image file extension from a URL.
 *
 * @details
 * Reads the extension of the URL's last path segment, lower-cases it, and
 * accepts it only if it is a known raster type (`jpg`, `jpeg`, `png`, `gif`,
 * `webp`, `bmp`); anything else -- including no extension -- yields `jpg`. Used
 * to name a downloaded page file and to name the copy made when a byte-identical
 * page is reused from another chapter.
 *
 * @param[in]  url URL whose last segment carries the extension (never NULL).
 * @param[out] out Destination buffer for the NUL-terminated extension.
 * @param[in]  cap Capacity of @p out in bytes (must be >= 5 for "jpeg").
 *
 * @return Nothing.
 *
 * @pre @p url and @p out are non-NULL; @p url is NUL-terminated.
 * @pre @p cap is at least 5 so the longest accepted extension fits.
 * @post @p out is a NUL-terminated, lower-case, known extension (default `jpg`).
 * @post @p out contains no `?`/`#` bytes from a query or fragment.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
void mdl_urlname_ext(const char* url, char* out, size_t cap);
