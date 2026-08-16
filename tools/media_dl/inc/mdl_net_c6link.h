/**
 * @file mdl_net_c6link.h
 * @brief Media network backend over the ESP32-C6 raw HTTPS transport.
 *
 * @details
 * Binds the downloader's portable ::mdl_net_iface_t to an already-open
 * ::ra8_c6link_t. The backend verifies the C6 terminal digest independently
 * through caller-supplied SHA-256 callbacks before exposing bytes to the
 * downloader. Artifact selection remains RA8-side policy through
 * ::ra8_mdl_format_t; the transport never relabels raw bytes as a container.
 *
 * Protocol version 1 cannot carry User-Agent, Referer, conditional headers, or
 * a caller timeout. Requests using those fields fail with
 * ::k_ra8_err_not_supported rather than silently dropping policy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "mdl_net.h"
#include "ra8_c6link_mdl_transfer.h"

/**
 * @struct mdl_net_c6link_t
 * @brief Caller-owned state for one serialized C6 media network backend.
 * @invariant `link` is open and exclusively owned while a request executes.
 * @invariant `chunk_bytes` and `max_chunks` bound every remote transfer.
 * @since 0.1.0
 */
typedef struct {
  ra8_c6link_t*          link;        /**< Borrowed open C6 link.             */
  ra8_mdl_sha256_iface_t sha256;      /**< Caller-owned independent digest.   */
  uint16_t               chunk_bytes; /**< Requested bytes per remote pull.   */
  uint32_t               max_chunks;  /**< Hard pull bound for one response.  */
} mdl_net_c6link_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind an open C6 link as a portable media network backend.
 * @param[out] net Interface returned to downloader/session code.
 * @param[out] backend Caller-owned backend state retained by @p net.
 * @param[in,out] link Already-open exclusively owned C6 link.
 * @param[in] sha256 Complete caller-owned streaming SHA-256 seam.
 * @param[in] chunk_bytes Requested bytes per pull.
 * @param[in] max_chunks Hard maximum pulls per response.
 * @return Binding status.
 * @retval k_ra8_ok The interface is ready for unconditional HTTPS requests.
 * @retval k_ra8_err_null_ptr A required pointer or SHA callback is null.
 * @retval k_ra8_err_invalid_size A transfer bound is zero or excessive.
 * @pre No request is active on @p link or @p backend.
 * @pre SHA context storage remains live until the interface is destroyed.
 * @post Success initializes both caller-owned output objects.
 * @post Failure leaves @p net and @p backend zeroed after pointer validation.
 * @note The binding owns neither the link nor SHA context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_net_c6link_init(mdl_net_iface_t*              net,
                                            mdl_net_c6link_t*             backend,
                                            ra8_c6link_t*                 link,
                                            const ra8_mdl_sha256_iface_t* sha256,
                                            uint16_t                      chunk_bytes,
                                            uint32_t                      max_chunks);

#ifdef __cplusplus
}
#endif
