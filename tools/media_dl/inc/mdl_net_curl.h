/**
 * @file mdl_net_curl.h
 * @brief Factory for the concrete libcurl-backed ::mdl_net_iface_t backend.
 *
 * @details
 * This is the ONE header that names the libcurl backend, and only the
 * composition root (`main.c`) includes it. Every other layer -- extraction,
 * politeness, robots gating, the download loop -- includes only `mdl_net.h` and
 * sees the abstract ::mdl_net_iface_t vtable, so the concrete backend can be
 * swapped (for the future NetX/Mbed backend, or a scripted test fake) without
 * touching those layers. Keeping the factory out of `mdl_net.h` is what lets
 * that header stay backend-agnostic.
 *
 * @see mdl_net.h  The abstract seam and its dispatchers.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_net.h"

/** @brief Bytes reserved by the caller for one libcurl backend context. */
typedef enum : size_t {
  k_mdl_net_curl_storage_bytes = 1024U, /**< Private backend storage capacity. */
} mdl_net_curl_storage_size_t;

/**
 * @struct mdl_net_curl_storage
 * @brief Opaque, aligned, caller-owned storage for one host network backend
 * @details The concrete layout remains private to `mdl_net_curl.c`. Keeping
 * the bytes in the composition root removes all first-party heap allocation
 * from backend construction while preserving a replaceable network seam.
 * @invariant `bytes` is aligned for every private backend member.
 * @since 0.1.0
 */
typedef struct mdl_net_curl_storage {
  alignas(max_align_t) uint8_t bytes[k_mdl_net_curl_storage_bytes]; /**< Private context bytes. */
} mdl_net_curl_storage_t;

/**
 * @brief Initialise a libcurl-backed host network interface in caller storage.
 *
 * @details
 * The returned handle wires the libcurl backend's method table to a private
 * libcurl easy handle hardened per @p policy (scheme allowlist, SSRF and
 * cross-host redirect refusal, TLS verification, size and time bounds). One
 * easy handle is reused across requests so the connection pool and cookie jar
 * persist (unlike the Kotlin per-request client). This is the Dependency
 * Injection seam: production wires this factory; tests wire a fake with the
 * same ::mdl_net_iface_t shape.
 *
 * @param[out] net Caller-owned interface populated on success.
 * @param[in,out] storage Caller-owned private storage retained until destroy.
 * @param[in] policy Session security policy, or NULL for the safe defaults.
 *
 * @return Canonical initialisation status.
 * @retval k_ra8_ok A ready interface; release resources with ::mdl_net_destroy.
 * @retval k_ra8_err_invalid_arg A required object or credential byte view is
 * invalid, or a cookie row is unsafe.
 * @retval k_ra8_err_invalid_size One cookie row exceeds the bounded importer.
 * @retval k_ra8_err_not_supported This libcurl/TLS build cannot consume a
 * caller-owned CA blob.
 * @retval k_ra8_fail Global/easy init or option hardening failed.
 *
 * @pre libcurl is available at link time.
 * @pre `policy`, when non-NULL, describes the intended escape hatches.
 * @pre Credential bytes referenced by @p policy remain readable until
 * ::mdl_net_destroy because custom CA data is bound with `CURL_BLOB_NOCOPY`.
 * @post On success @p net owns a hardened libcurl easy handle while caller
 * storage remains valid.
 * @post On failure @p net and @p storage contain only zero bytes.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_net_curl_init(mdl_net_iface_t*        net,
                                          mdl_net_curl_storage_t* storage,
                                          const mdl_net_policy_t* policy);
