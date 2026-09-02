/**
 * @file media_download_image_internal.h
 * @brief Private encoded-image download and RABOOK publication composition.
 * @details Declares the application seam that downloads one encoded image into
 *          caller-owned RAM, formats it as a real RBKC/RABOOK1 artifact, and
 *          publishes it through the already-bound strict storage transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_err.h"

/**
 * @brief Download one encoded image and publish it as a strict `.rabook`.
 * @param[in,out] link Open, exclusively owned C6 link.
 * @param[in] url NUL-terminated HTTPS source-image URL.
 * @param[in] destination Canonical final VFS destination.
 * @param[in] transfer Bound strict VFS storage, SHA, and chunk policy.
 * @return Download, format, validation, or publication status.
 * @retval k_ra8_ok A generated RBKC/RABOOK1 artifact was published.
 * @retval k_ra8_err_null_ptr A required pointer or callback was null.
 * @retval k_ra8_err_not_supported The configured output is not RABOOK.
 * @retval other C6, raster, builder, compression, hash, VFS, and validation
 *               errors propagate unchanged.
 * @pre @p link is open and no other caller uses it or the supplied contexts.
 * @pre @p transfer owns a strict format validator and unused transaction.
 * @post Success leaves one validated final artifact and no private stage.
 * @post Failure leaves no newly published artifact and aborts an opened stage.
 * @note All source, codec, book, and container bytes use fixed caller storage.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_media_download_image_run(ra8_c6link_t*                    link,
                              const char*                      url,
                              const char*                      destination,
                              const ra8_mdl_transfer_config_t* transfer);
