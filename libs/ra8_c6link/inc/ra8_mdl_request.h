/**
 * @file ra8_mdl_request.h
 * @brief Complete typed media request accepted by the C6 RPC endpoint.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Binds the transport-neutral HTTP policy in `ra8_mdl_http.h` to the
 * one thing that is not transport-neutral about a media request: the artifact
 * identity the caller demands back, spelled ::mdl_format_t. That type is
 * declared in the downloader's own headers, so this record stays beside the
 * protocol that carries it rather than moving to `libs/ra8_mdl` with the
 * policy and response records (issue #746).
 *
 * @see ra8_mdl_http.h      The transport-neutral policy and response contract.
 * @see ra8_mdl_protocol.h  The bounded framing this request is packed into.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include "mdl_format.h"
#include "ra8_mdl_http.h"
#include "ra8_mdl_protocol.h"

/**
 * @struct ra8_mdl_request_t
 * @brief Complete typed HTTPS request accepted by protocol version 3.
 * @invariant `url` is a nonempty HTTPS URL shorter than ::k_ra8_mdl_url_max.
 * @invariant `format` is one concrete ::mdl_format_t value through RABOOK.
 * @since 0.1.0
 */
typedef struct {
  const char*           url;    /**< Absolute HTTPS source URL.        */
  mdl_format_t          format; /**< Exact returned artifact identity. */
  ra8_mdl_http_policy_t http;   /**< Forwarded request policy.         */
} ra8_mdl_request_t;
