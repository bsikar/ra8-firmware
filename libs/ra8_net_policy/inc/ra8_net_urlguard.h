/**
 * @file ra8_net_urlguard.h
 * @brief URL and peer-address safety policy for any consumer that fetches.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Anything in the tree that issues a request against an attacker-influenced
 * URL needs the same four answers before it dispatches: is the scheme one we
 * speak, is the resolved peer somewhere we are willing to reach, does the
 * response still fit the cap, and what is the origin this request belongs to.
 * Until now the only implementation of that policy lived inside a downloader
 * application, so an OTA client or a companion-radio media path had no
 * supported way to ask.
 *
 * The four answers, in the order a caller uses them:
 *
 * 1. ::ra8_net_urlguard_scheme_allowed refuses the URL before any transport
 *    sees it, so `file:`, `gopher:` and `data:` never reach a backend.
 * 2. ::ra8_net_urlguard_classify_ip buckets the resolved peer, and
 *    ::ra8_net_urlguard_addr_fetchable applies the private-space policy to
 *    that bucket. Together they are the SSRF guard: without them a hostile
 *    page turns a fetcher into a probe against the local host and network.
 * 3. ::ra8_net_urlguard_size_exceeds is the overflow-safe running-total test
 *    a receive callback applies to its response cap.
 * 4. ::ra8_net_urlguard_host and ::ra8_net_urlguard_path split a URL into the
 *    authority a policy is keyed on and the path a rule set matches against.
 *
 * Every entry point is a pure function over caller-owned storage: no
 * allocation, no network call, no name resolution, no global state. Address
 * classification parses the literal in-tree rather than calling the host's
 * `inet_pton`, so this policy is available on the target and not only on a
 * POSIX host.
 *
 * @code
 * if (!ra8_net_urlguard_scheme_allowed(url)) {
 *   return k_ra8_err_access_denied;
 * }
 *
 * char origin[k_ra8_net_urlguard_host_cap];
 * if (ra8_net_urlguard_host(url, origin, sizeof(origin)) != k_ra8_ok) {
 *   return k_ra8_err_access_denied;
 * }
 *
 * // ... once the transport reports the address it actually connected to:
 * const ra8_net_addr_class_t cls = ra8_net_urlguard_classify_ip(peer_ip);
 * if (!ra8_net_urlguard_addr_fetchable(cls, allow_private)) {
 *   return k_ra8_err_access_denied;
 * }
 * @endcode
 *
 * @note These are lexical and numeric predicates only. They do not resolve a
 *       name, so a caller defeating DNS rebinding must classify the address
 *       the transport actually connected to, not one it resolved earlier.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_net_urlguard_limits_t
 * @brief Buffer sizes a caller of this policy sizes its storage against.
 *
 * @details
 * ::k_ra8_net_urlguard_host_cap is large enough for the longest authority a
 * DNS name can carry (253 bytes) plus a `:65535` port and the NUL, so a
 * caller that sizes a host buffer with it never loses a legal origin to
 * ::k_ra8_err_no_mem.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_net_urlguard_host_cap = 262U, /**< Authority buffer: name, port, NUL. */
} ra8_net_urlguard_limits_t;

/**
 * @enum ra8_net_addr_class_t
 * @brief Reachability class of a resolved peer address.
 *
 * @details
 * The SSRF guard maps every resolved IP into one of these buckets and then
 * asks ::ra8_net_urlguard_addr_fetchable whether the active policy permits
 * it. Only ::k_ra8_net_addr_public is fetchable by default; the rest name the
 * address spaces a hostile page would use to turn a fetcher into a probe
 * against the local host or network.
 *
 * @invariant An address literal the parser cannot read is
 *            ::k_ra8_net_addr_unknown, which is never fetchable under any
 *            policy.
 *
 * @see ra8_net_urlguard_classify_ip()
 * @see ra8_net_urlguard_addr_fetchable()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_net_addr_public    = 0, /**< Routable public unicast address.       */
  k_ra8_net_addr_loopback  = 1, /**< 127.0.0.0/8 or ::1.                    */
  k_ra8_net_addr_private   = 2, /**< RFC1918, RFC6598 (CGNAT), or fc00::/7. */
  k_ra8_net_addr_linklocal = 3, /**< 169.254.0.0/16 or fe80::/10.           */
  k_ra8_net_addr_unknown   = 4, /**< Unparseable, unspecified, or reserved. */
} ra8_net_addr_class_t;

/**
 * @brief True when `url` uses an allowlisted scheme (`http://` or `https://`).
 *
 * @details
 * The scheme test is case-insensitive and anchored at the start of the
 * string, so `file://`, `ftp://`, `gopher://`, `data:` and every other scheme
 * are rejected. It is the belt to a transport's own protocol pin: the pin
 * bounds what the backend will speak, this refuses the URL before the backend
 * is ever handed it.
 *
 * @param[in] url Candidate URL, or NULL.
 *
 * @return Whether the scheme is on the allowlist.
 * @retval true  `url` begins with `http://` or `https://`.
 * @retval false `url` is NULL, empty, or uses any other scheme.
 *
 * @pre `url`, when non-NULL, is a NUL-terminated string.
 * @pre The caller treats `false` as a hard refusal, not a warning.
 * @post `url` is not modified.
 * @post No allocation or I/O is performed.
 *
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_net_urlguard_scheme_allowed(const char* url);

/**
 * @brief Classify a peer address literal into a ::ra8_net_addr_class_t.
 *
 * @details
 * Accepts dotted-quad IPv4 and an IPv6 literal, including the IPv4-mapped
 * `::ffff:a.b.c.d` form, which is unwrapped and classified as IPv4. The
 * literal is parsed in-tree, so this is available on the target as well as on
 * a host. Anything the parser rejects, plus the unspecified and
 * multicast/reserved ranges, becomes ::k_ra8_net_addr_unknown so the caller
 * fails closed.
 *
 * A zone identifier (`fe80::1%eth0`) is rejected rather than stripped: a
 * scoped literal is not an address this policy can reason about on its own.
 *
 * @param[in] ip Peer address literal, or NULL.
 *
 * @return The reachability class of `ip`.
 * @retval k_ra8_net_addr_public    Routable public unicast.
 * @retval k_ra8_net_addr_loopback  127.0.0.0/8 or ::1.
 * @retval k_ra8_net_addr_private   RFC1918 / RFC6598 / fc00::/7.
 * @retval k_ra8_net_addr_linklocal 169.254.0.0/16 or fe80::/10.
 * @retval k_ra8_net_addr_unknown   NULL, unparseable, unspecified, reserved.
 *
 * @pre `ip`, when non-NULL, is a NUL-terminated string.
 * @pre The caller maps ::k_ra8_net_addr_unknown to a refusal.
 * @post `ip` is not modified.
 * @post No allocation or I/O is performed.
 *
 * @note Thread-safe: writes only call-local storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_net_addr_class_t ra8_net_urlguard_classify_ip(const char* ip);

/**
 * @brief Decide whether an address class is fetchable under the active policy.
 *
 * @details
 * ::k_ra8_net_addr_public is always fetchable. Loopback, private and
 * link-local addresses are fetchable only when `allow_private` is set, the
 * explicit opt-in a caller exposes as a flag. ::k_ra8_net_addr_unknown is
 * never fetchable, so an address the parser could not classify can never be
 * reached even with the opt-in.
 *
 * @param[in] cls           Address class from ::ra8_net_urlguard_classify_ip.
 * @param[in] allow_private Whether the private-space opt-in is active.
 *
 * @return Whether a fetch to an address of this class is permitted.
 * @retval true  The class is public, or private-space with the opt-in set.
 * @retval false The class is unknown, or private-space without the opt-in.
 *
 * @pre `cls` is a value produced by ::ra8_net_urlguard_classify_ip.
 * @pre `allow_private` reflects the caller's private-space opt-in.
 * @post No state is modified.
 * @post The return is a pure function of the two arguments.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_net_urlguard_addr_fetchable(ra8_net_addr_class_t cls, bool allow_private);

/**
 * @brief True when appending `add` bytes to `have` would exceed `cap`.
 *
 * @details
 * Overflow-safe running-total check for the response-size cap a receive
 * callback enforces. A `cap` of zero means "no cap" and always returns false,
 * matching the convention transports use for an unlimited transfer.
 *
 * @param[in] have Bytes already accepted.
 * @param[in] add  Bytes about to be appended.
 * @param[in] cap  Maximum permitted total, or 0 for unlimited.
 *
 * @return Whether accepting `add` would breach `cap`.
 * @retval true  `cap` is non-zero and `have + add > cap`.
 * @retval false `cap` is zero, or the total still fits.
 *
 * @pre `have <= cap` or `cap == 0` (the running total never starts over cap).
 * @pre The caller aborts the transfer when the result is true.
 * @post No state is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_net_urlguard_size_exceeds(uint64_t have, uint64_t add, uint64_t cap);

/**
 * @brief Extract the authority (host and optional port) from an http(s) URL.
 *
 * @details
 * Copies the `host[:port]` of `url` into `out`, lower-cased, dropping any
 * `scheme://`, `user:pass@` userinfo, and trailing path. The port is kept: a
 * politeness policy and a same-origin redirect check are both scoped per
 * (scheme, host, port), so `:8080` is part of the identity.
 *
 * The three refusals are distinguished, which is the lift over the
 * application-local predicate this replaces: a malformed call, a URL with no
 * authority, and an authority that did not fit are three different facts a
 * caller may want to report differently.
 *
 * @param[in]  url URL to inspect (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the NUL-terminated authority.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether an authority was extracted.
 * @retval k_ra8_ok               A non-empty authority was written to `out`.
 * @retval k_ra8_err_invalid_arg  `url` or `out` was NULL, or `cap` was 0.
 * @retval k_ra8_err_not_found    `url` carries no `://` authority.
 * @retval k_ra8_err_no_mem       The authority did not fit in `cap`.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre `url`, when non-NULL, is a NUL-terminated string.
 * @post On every non-ok return with `cap > 0`, `out[0]` is `'\0'`.
 * @post `url` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_urlguard_host(const char* url, char* out, size_t cap);

/**
 * @brief Extract the path (leading `/`, no query or fragment) from a URL.
 *
 * @details
 * Copies the path component of an http(s) URL into `out`, defaulting to `"/"`
 * when the authority is followed by no path. The query string and fragment
 * are dropped, because a rule set that matches a request matches on the path
 * only.
 *
 * @param[in]  url URL to inspect (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the NUL-terminated path.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether a path was written.
 * @retval k_ra8_ok               A path, possibly `"/"`, was written to `out`.
 * @retval k_ra8_err_invalid_arg  `url` or `out` was NULL, or `cap` was 0.
 * @retval k_ra8_err_not_found    `url` carries no `://` authority.
 * @retval k_ra8_err_no_mem       The path did not fit in `cap`.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre `url`, when non-NULL, is a NUL-terminated string.
 * @post On every non-ok return with `cap > 0`, `out[0]` is `'\0'`.
 * @post `url` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_urlguard_path(const char* url, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
