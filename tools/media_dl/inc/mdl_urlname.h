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

/**
 * @brief Sniff true image extension and MIME type from magic bytes and/or HTTP Content-Type.
 *
 * @details
 * Inspects HTTP `Content-Type` header and magic bytes of buffer data:
 *   - JPEG: `FF D8 FF` -> `.jpg` / `image/jpeg`
 *   - PNG: `89 50 4E 47` -> `.png` / `image/png`
 *   - WebP: `RIFF....WEBP` -> `.webp` / `image/webp`
 *   - GIF: `GIF87a` / `GIF89a` -> `.gif` / `image/gif`
 *
 * @param[in]  buf          Data buffer holding raw magic bytes (may be NULL if buf_len == 0).
 * @param[in]  buf_len      Length of @p buf in bytes.
 * @param[in]  content_type HTTP Content-Type header string (may be NULL or empty).
 * @param[out] out_ext      Destination buffer for lower-case extension (e.g. "jpg"). May be NULL.
 * @param[in]  ext_cap      Capacity of @p out_ext in bytes.
 * @param[out] out_mime     Destination buffer for exact MIME type (e.g. "image/jpeg"). May be NULL.
 * @param[in]  mime_cap     Capacity of @p out_mime in bytes.
 *
 * @return true if an image type was recognized from magic bytes or Content-Type header, false otherwise.
 */
bool mdl_urlname_sniff_image_type(const void* buf,
                                  size_t buf_len,
                                  const char* content_type,
                                  char* out_ext,
                                  size_t ext_cap,
                                  char* out_mime,
                                  size_t mime_cap);

/**
 * @brief Sniff true image extension and MIME type from a file on disk.
 *
 * @param[in]  file_path    Absolute or relative path to the image file on disk.
 * @param[in]  content_type HTTP Content-Type header string (may be NULL or empty).
 * @param[out] out_ext      Destination buffer for lower-case extension (e.g. "jpg"). May be NULL.
 * @param[in]  ext_cap      Capacity of @p out_ext in bytes.
 * @param[out] out_mime     Destination buffer for exact MIME type (e.g. "image/jpeg"). May be NULL.
 * @param[in]  mime_cap     Capacity of @p out_mime in bytes.
 *
 * @return true if an image type was recognized from magic bytes or Content-Type header, false otherwise.
 */
bool mdl_urlname_sniff_file(const char* file_path,
                            const char* content_type,
                            char* out_ext,
                            size_t ext_cap,
                            char* out_mime,
                            size_t mime_cap);
