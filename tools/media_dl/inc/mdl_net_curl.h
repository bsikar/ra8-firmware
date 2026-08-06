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

#include "mdl_net.h"
#include "ra8_attributes.h"

/**
 * @brief Create the libcurl-backed host network interface.
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
 * @param[in] policy Session security policy, or NULL for the safe defaults
 *                   (no private hosts, same-host redirects only, no size cap).
 *
 * @return Owned handle, or NULL on allocation/init failure, or if any
 *         security-relevant libcurl option could not be applied.
 * @retval non-NULL A ready interface; release it with ::mdl_net_destroy.
 * @retval NULL     Global init, allocation, or option-hardening failed.
 *
 * @pre libcurl is available at link time.
 * @pre `policy`, when non-NULL, describes the intended escape hatches.
 * @post On success the returned handle owns a hardened libcurl easy handle.
 * @post On failure no resources are leaked.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
RA8_DI_SLOT("net_iface")
#include "ra8_arena.h"

mdl_net_iface_t* mdl_net_curl_create(ra8_arena_t* arena, const mdl_net_policy_t* policy);
