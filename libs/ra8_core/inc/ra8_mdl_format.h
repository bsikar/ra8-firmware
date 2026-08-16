/**
 * @file ra8_mdl_format.h
 * @brief Shared media artifact format contract for download and export paths.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * This type names the artifact the RA8 application will actually publish. It
 * is deliberately separate from the C6 raw-HTTPS transport protocol: selecting
 * RABOOK means the RA8-side exporter must build, validate, and transactionally
 * commit a `.rabook`; it never means that a transport backend may merely relabel
 * downloaded bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum ra8_mdl_format_t
 * @brief Artifact format selected by a media-download composition root.
 * @details Values cover every current host exporter so the same selection can
 * be carried into firmware composition without depending on CLI-only headers.
 * Reserved values remain explicit and cannot be mistaken for implemented
 * writers.
 * @invariant ::k_ra8_mdl_format_rabook selects the strict RBKC/RABOOK1 writer.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_mdl_format_loose   = 0U,   /**< Leave loose page images.                 */
  k_ra8_mdl_format_cbz     = 1U,   /**< ZIP of images (`.cbz`).                  */
  k_ra8_mdl_format_cbt     = 2U,   /**< Tar of images (`.cbt`).                  */
  k_ra8_mdl_format_cbr     = 3U,   /**< Reserved RAR output.                     */
  k_ra8_mdl_format_cbt_xz  = 4U,   /**< Reserved xz-compressed tar output.       */
  k_ra8_mdl_format_cbt_gz  = 5U,   /**< Gzip-compressed tar (`.cbt.gz`).         */
  k_ra8_mdl_format_epub    = 6U,   /**< Fixed-layout EPUB (`.epub`).             */
  k_ra8_mdl_format_jof     = 7U,   /**< Native JOF tile atlas per page (`.jof`). */
  k_ra8_mdl_format_rabook  = 8U,   /**< Chunked reader-native book (`.rabook`).  */
  k_ra8_mdl_format_invalid = 255U, /**< Unrecognized or unsupported selection.   */
} ra8_mdl_format_t;
