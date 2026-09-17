/**
 * @file mdl_url_guard.h
 * @brief Pure URL / address safety predicates for the libcurl backend.
 *
 * @details
 * `mdl` fetches attacker-influenced URLs by design: the first URL comes
 * from `argv` or a config, and every subsequent image URL is extracted from
 * hostile HTML. The libcurl backend (mdl_net_curl.c) must therefore refuse a
 * `file://` read, refuse a redirect that resolves to loopback / private /
 * link-local address space (the SSRF sharp edge), and bound a response's size.
 *
 * The decisions themselves are pure functions with no libcurl dependency so
 * they are unit-testable host-side without a network seam: the backend calls
 * them, and the tests call them directly. Address classification uses the
 * host's `inet_pton` rather than a hand-rolled parser -- this is host tooling,
 * not firmware, so the platform's battle-tested parser is the right choice.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum mdl_addr_class_t
 * @brief Reachability class of a resolved peer address.
 *
 * @details
 * The SSRF guard maps every resolved IP into one of these buckets and then
 * asks ::mdl_addr_is_fetchable whether the active policy permits it. Only
 * ::k_mdl_addr_public is fetchable by default; the rest name the address
 * spaces a hostile page would use to turn the downloader into a fetch
 * primitive against the local host or network.
 *
 * @invariant An address that `inet_pton` cannot parse is ::k_mdl_addr_unknown,
 *            which is never fetchable under any policy.
 *
 * @see mdl_classify_ip()
 * @see mdl_addr_is_fetchable()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mdl_addr_public    = 0, /**< Routable public unicast address.       */
  k_mdl_addr_loopback  = 1, /**< 127.0.0.0/8 or ::1.                    */
  k_mdl_addr_private   = 2, /**< RFC1918, RFC6598 (CGNAT), or fc00::/7. */
  k_mdl_addr_linklocal = 3, /**< 169.254.0.0/16 or fe80::/10.           */
  k_mdl_addr_unknown   = 4, /**< Unparseable, unspecified, or reserved. */
} mdl_addr_class_t;

/**
 * @brief True when `url` uses an allowlisted scheme (`http://` or `https://`).
 *
 * @details
 * The scheme test is case-insensitive and anchored at the start of the string,
 * so `file://`, `ftp://`, `gopher://`, `data:` and every other scheme are
 * rejected. It is a belt-and-suspenders complement to libcurl's
 * `CURLOPT_PROTOCOLS_STR`: the option pins the transport, this refuses the URL
 * before it is ever handed to libcurl.
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
bool mdl_url_scheme_allowed(const char* url);

/**
 * @brief Classify a resolved peer IP string into a ::mdl_addr_class_t.
 *
 * @details
 * Accepts either dotted-quad IPv4 or an IPv6 literal (including the
 * IPv4-mapped `::ffff:a.b.c.d` form, which is unwrapped and classified as
 * IPv4). Anything `inet_pton` rejects, plus the unspecified and
 * multicast/reserved ranges, becomes ::k_mdl_addr_unknown so the caller
 * fails closed.
 *
 * @param[in] ip Resolved address literal (e.g. libcurl's
 *               `CURLINFO_PRIMARY_IP`), or NULL.
 *
 * @return The reachability class of `ip`.
 * @retval k_mdl_addr_public    Routable public unicast.
 * @retval k_mdl_addr_loopback  127.0.0.0/8 or ::1.
 * @retval k_mdl_addr_private   RFC1918 / RFC6598 / fc00::/7.
 * @retval k_mdl_addr_linklocal 169.254.0.0/16 or fe80::/10.
 * @retval k_mdl_addr_unknown   NULL, unparseable, unspecified, or reserved.
 *
 * @pre `ip`, when non-NULL, is a NUL-terminated string.
 * @pre The caller maps ::k_mdl_addr_unknown to a refusal.
 * @post `ip` is not modified.
 * @post No allocation or I/O is performed.
 *
 * @note Thread-safe: `inet_pton` writes only caller-local storage.
 * @since 0.1.0
 */
mdl_addr_class_t mdl_classify_ip(const char* ip);

/**
 * @brief Decide whether an address class is fetchable under the active policy.
 *
 * @details
 * ::k_mdl_addr_public is always fetchable. Loopback, private and link-local
 * addresses are fetchable only when `allow_private` is set (the explicit
 * `--allow-private` opt-in). ::k_mdl_addr_unknown is never fetchable, so an
 * address the parser could not classify can never be reached even with the
 * opt-in.
 *
 * @param[in] cls           Address class from ::mdl_classify_ip.
 * @param[in] allow_private Whether the private-space opt-in is active.
 *
 * @return Whether a fetch to an address of this class is permitted.
 * @retval true  The class is public, or private-space and the opt-in is set.
 * @retval false The class is unknown, or private-space without the opt-in.
 *
 * @pre `cls` is a value produced by ::mdl_classify_ip.
 * @pre `allow_private` reflects the caller's `--allow-private` flag.
 * @post No state is modified.
 * @post The return is a pure function of the two arguments.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
bool mdl_addr_is_fetchable(mdl_addr_class_t cls, bool allow_private);

/**
 * @brief True when appending `add` bytes to `have` would exceed `cap`.
 *
 * @details
 * Overflow-safe running-total check for the response-size cap enforced in the
 * write callbacks. A `cap` of zero means "no cap" and always returns false,
 * matching libcurl's own convention for unlimited transfer options.
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

 * @post Documented outputs and the return value describe the same outcome.
 */
bool mdl_size_exceeds(uint64_t have, uint64_t add, uint64_t cap);

/**
 * @brief Extract the authority (host and optional port) from an http(s) URL.
 *
 * @details
 * Copies the `host[:port]` of `url` into `out`, lower-cased, dropping any
 * `scheme://`, `user:pass@` userinfo, and trailing path. The port is kept: a
 * robots.txt policy and a same-host redirect check are both scoped per
 * (scheme, host, port) origin, so `:8080` is part of the identity. A URL with
 * no recognisable authority yields an empty string and a `false` return.
 *
 * @param[in]  url URL to inspect (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the NUL-terminated host.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether a host was extracted and fit in `out`.
 * @retval true  A non-empty host was written to `out`.
 * @retval false `url`/`out` was NULL, `cap` was 0, no authority was found, or
 *               the host did not fit.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre `url`, when non-NULL, is a NUL-terminated string.
 * @post On `false`, `out[0]` is set to `'\0'` when `cap > 0`.
 * @post `url` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
bool mdl_url_host(const char* url, char* out, size_t cap);

/**
 * @brief Extract the path (with leading `/`, without query/fragment) from a URL.
 *
 * @details
 * Copies the path component of an http(s) URL into `out`, defaulting to `"/"`
 * when the authority is followed by no path. The query string and fragment are
 * dropped. Used to test a target against parsed robots.txt rules, which match
 * on the path only.
 *
 * @param[in]  url URL to inspect (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the NUL-terminated path.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether a path was written (including the `"/"` default).
 * @retval true  A path (possibly `"/"`) was written to `out`.
 * @retval false `url`/`out` was NULL, `cap` was 0, no authority was found, or
 *               the path did not fit.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre `url`, when non-NULL, is a NUL-terminated string.
 * @post On `false` with `cap > 0`, `out[0]` is `'\0'`.
 * @post `url` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
bool mdl_url_path(const char* url, char* out, size_t cap);
