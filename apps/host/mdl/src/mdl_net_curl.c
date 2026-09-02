/**
 * @file mdl_net_curl.c
 * @brief libcurl host backend registered through the mdl_net vtable seam.
 *
 * @details
 * Host-only implementation of the ::mdl_net_iface_t interface from @ref
 * mdl_net.h. It builds a `{ vtable, ctx }` handle whose four methods forward to
 * one reused libcurl easy handle; callers reach them only through the
 * dispatchers, never by name. libcurl handles TLS, redirects, gzip and the
 * connection pool -- exactly the parts we do not want to hand-roll before the
 * on-device NetX/Mbed TLS stack exists.
 *
 * The URL that reaches `curl_easy_perform` is attacker-influenced by design, so
 * the handle is hardened rather than left on libcurl's inherited defaults: the
 * transport is pinned to http/https, redirects are refused when they change
 * host or resolve to non-public address space (the SSRF guard, checked on the
 * resolved peer via `CURLOPT_PREREQFUNCTION`), TLS verification is asserted
 * explicitly, `.netrc` and proxy-env are disabled, and every response is size-
 * and time-bounded. Every `curl_easy_setopt` of a security-relevant option is
 * checked; a failure fails handle creation rather than proceeding unhardened.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_net_curl.h"

#include <curl/curl.h>
#include <limits.h>
#include <string.h>

#include "mdl_net.h"
#include "mdl_net_curl_internal.h"
#include "mdl_net_internal.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief Backend tunables. */
typedef enum : uint32_t {
  k_curl_max_redirects   = 5,     /**< Redirect hops to follow.                  */
  k_http_not_modified    = 304,   /**< Conditional GET reused the held entity.   */
  k_http_client_err_min  = 400,   /**< First HTTP status treated as an error.    */
  k_http_not_found       = 404,   /**< Absent resource (client error).           */
  k_http_too_many_req    = 429,   /**< Too Many Requests (throttle).             */
  k_http_server_err_min  = 500,   /**< First HTTP status that is a server error. */
  k_http_unavailable     = 503,   /**< Service Unavailable (throttle).           */
  k_connect_timeout_ms   = 15000, /**< TCP/TLS connect budget, ms.               */
  k_low_speed_bytes      = 64,    /**< Below this many B/s...                    */
  k_low_speed_secs       = 30,    /**< ...for this long, abort a stalled xfer.   */
  k_origin_host_max      = 256,   /**< Stored origin-host buffer bytes.          */
  k_request_header_max   = 256,   /**< Bytes in one conditional-request header.  */
  k_request_header_count = 2,     /**< ETag and Last-Modified header slots.      */
} mdl_curl_limits_t;

/** @brief Fixed, stable request-header list retained by the backend context. */
typedef struct {
  struct curl_slist nodes[k_request_header_count]; /**< libcurl list nodes. */
  /** Stable conditional-request header text. */
  char               values[k_request_header_count][k_request_header_max];
  struct curl_slist* head;  /**< First active node. */
  size_t             count; /**< Active node count. */
} mdl_req_headers_t;

/** @brief Private state of the libcurl backend (the vtable's `ctx`). */
typedef struct {
  CURL*           curl;             /**< Reused easy handle.          */
  bool            allow_private;    /**< SSRF opt-in (private peers). */
  bool            allow_cross_host; /**< Cross-host redirect opt-in.  */
  uint64_t        max_bytes;        /**< Per-response cap (0 = none). */
  const char*     proxy;            /**< HTTP/HTTPS proxy URL.        */
  const char*     socks5;           /**< SOCKS5 proxy URL.            */
  mdl_net_bytes_t cookies;          /**< Imported cookie-file bytes.  */
  mdl_net_bytes_t ca_pem;           /**< Caller-owned CA PEM bytes.   */
#if LIBCURL_VERSION_NUM >= 0x074D00
  struct curl_blob ca_blob; /**< Stable NOCOPY blob descriptor. */
#endif
  char              origin_host[k_origin_host_max]; /**< Host of the current request. */
  mdl_req_headers_t request_headers;                /**< Stable conditional headers.  */
} mdl_curl_ctx_t;

static_assert(sizeof(mdl_curl_ctx_t) <= k_mdl_net_curl_storage_bytes,
              "mdl_net_curl_storage_t is too small for the private context");
static_assert(alignof(mdl_net_curl_storage_t) >= alignof(mdl_curl_ctx_t),
              "mdl_net_curl_storage_t is insufficiently aligned");

/** @brief Response-header capture sink: keeps the final response's Retry-After. */
typedef struct {
  char retry_after[k_mdl_retry_after_max];   /**< Raw value, "" when absent.         */
  char etag[k_mdl_etag_max];                 /**< Raw ETag, "" when absent.          */
  char last_modified[k_mdl_last_mod_max];    /**< Raw Last-Modified, "" when absent. */
  char content_type[k_mdl_content_type_max]; /**< Raw Content-Type, "" when absent.  */
} hdr_sink_t;

/**
 * @brief Lower-case an ASCII byte (locale-independent).
 * @details Maps uppercase ASCII letters and preserves all other bytes.
 * @param[in] c Character to map.
 * @return Lower-case equivalent.
 * @retval other Mapped or unchanged character.
 * @pre @p c is representable as `char`.
 * @pre Locale-specific folding is not required.
 * @post No state is modified.
 * @post Non-uppercase input is unchanged.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static char internal_ascii_lower(char c)
{
  if ((c >= 'A') && (c <= 'Z')) {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

/**
 * @brief True when `line` begins with `prefix`, ASCII case-insensitively.
 * @details Compares a bounded header line against a lower-case literal prefix.
 * @param[in] line Header bytes, not necessarily NUL-terminated.
 * @param[in] line_len Readable line length.
 * @param[in] prefix NUL-terminated lower-case prefix.
 * @return Whether the complete prefix matched.
 * @retval true The bounded line starts with @p prefix.
 * @retval false A byte differs or the line ends first.
 * @pre @p line is readable for @p line_len bytes and @p prefix is non-NULL.
 * @pre @p prefix is lower-case ASCII.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads only arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_header_is(const char* line, size_t line_len, const char* prefix)
{
  size_t i = 0U;
  for (; (i < line_len) && (prefix[i] != '\0'); ++i) {
    if (internal_ascii_lower(line[i]) != prefix[i]) {
      return false;
    }
  }
  return prefix[i] == '\0';
}

/**
 * @brief Copy a header value (after the colon), trimmed of CR/LF/space, bounded.
 * @details Finds the first colon, trims surrounding HTTP whitespace, and copies with NUL termination.
 * @param[in] line Header bytes.
 * @param[in] line_len Readable byte count.
 * @param[out] out Destination string.
 * @param[in] cap Destination capacity.
 * @return Nothing.
 * @pre @p line and @p out are non-NULL; @p cap is non-zero.
 * @pre @p line is readable for @p line_len bytes.
 * @post @p out is NUL-terminated and contains at most `cap - 1` bytes.
 * @post @p line is unchanged.
 * @note Longer values are deliberately truncated.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_header_value(const char* line, size_t line_len, char* out, size_t cap)
{
  size_t start = 0U;
  while ((start < line_len) && (line[start] != ':')) {
    ++start;
  }
  if (start < line_len) {
    ++start; /* skip the colon */
  }
  while ((start < line_len) && ((line[start] == ' ') || (line[start] == '\t'))) {
    ++start;
  }
  size_t end = line_len;
  while ((end > start) && ((line[end - 1U] == '\r') || (line[end - 1U] == '\n') ||
                           (line[end - 1U] == ' ') || (line[end - 1U] == '\t'))) {
    --end;
  }
  size_t n = end - start;
  if (n >= cap) {
    n = cap - 1U;
  }
  for (size_t i = 0U; i < n; ++i) {
    out[i] = line[start + i];
  }
  out[n] = '\0';
}

/**
 * @brief Capture selected headers from the final libcurl response.
 * @details Clears redirect-hop metadata on a status line and latches bounded validator fields.
 * @param[in] buffer Header bytes supplied by libcurl.
 * @param[in] size Element size.
 * @param[in] nitems Element count.
 * @param[in,out] user Optional ::hdr_sink_t callback state.
 * @return Number of elements consumed, or zero on multiplication overflow.
 * @retval 0 The byte count overflowed.
 * @retval other @p nitems when accepted.
 * @pre @p buffer is readable for `size * nitems` bytes when representable.
 * @pre @p user is NULL or points to writable header sink storage.
 * @post Recognised headers update only their bounded sink field.
 * @post A new status line clears metadata from the preceding redirect hop.
 * @note Signature is fixed by libcurl's callback ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_on_header(char* buffer, size_t size, size_t nitems, void* user)
{
  if ((nitems != 0U) && (size > (SIZE_MAX / nitems))) {
    return 0U;
  }
  const size_t len  = size * nitems;
  hdr_sink_t*  sink = (hdr_sink_t*)user;
  if (sink == nullptr) {
    return len;
  }
  /* A new status line starts a fresh response (redirect chain): drop stale values. */
  if (internal_header_is(buffer, len, "http/")) {
    sink->retry_after[0]   = '\0';
    sink->etag[0]          = '\0';
    sink->last_modified[0] = '\0';
    sink->content_type[0]  = '\0';
  } else if (internal_header_is(buffer, len, "retry-after:")) {
    internal_header_value(buffer, len, sink->retry_after, sizeof(sink->retry_after));
  } else if (internal_header_is(buffer, len, "etag:")) {
    internal_header_value(buffer, len, sink->etag, sizeof(sink->etag));
  } else if (internal_header_is(buffer, len, "last-modified:")) {
    internal_header_value(buffer, len, sink->last_modified, sizeof(sink->last_modified));
  } else if (internal_header_is(buffer, len, "content-type:")) {
    internal_header_value(buffer, len, sink->content_type, sizeof(sink->content_type));
  }
  return len;
}

RA8_PRIV size_t priv_mdl_net_curl_buf_write(char* data, size_t size, size_t nmemb, void* user)
{
  buf_sink_t* sink = (buf_sink_t*)user;
  if ((nmemb != 0U) && (size > (SIZE_MAX / nmemb))) {
    return 0U;
  }
  const size_t bytes = size * nmemb;
  if (sink == nullptr) {
    return 0U;
  }
  if ((sink->len > sink->cap) || (bytes > (sink->cap - sink->len))) {
    sink->overflow = true;
    return 0U; /* Signals libcurl to abort the transfer. */
  }
  memcpy(sink->buf + sink->len, data, bytes);
  sink->len += bytes;
  return nmemb;
}

RA8_PRIV size_t priv_mdl_net_curl_body_write(char* data, size_t size, size_t nmemb, void* user)
{
  mdl_net_curl_body_state_t* sink = (mdl_net_curl_body_state_t*)user;
  if ((sink == nullptr) || (sink->sink == nullptr) || (sink->sink->write == nullptr) ||
      (sink->sink->ctx == nullptr)) {
    return 0U;
  }
  if ((nmemb != 0U) && (size > (SIZE_MAX / nmemb))) {
    sink->overflow = true;
    return 0U;
  }
  const size_t bytes = size * nmemb;
  if ((bytes > (size_t)UINT32_MAX) || mdl_size_exceeds(sink->written, (uint64_t)bytes, sink->cap)) {
    sink->overflow = true;
    return 0U;
  }
  uint32_t        written = 0U;
  const ra8_err_t error =
    sink->sink->write(sink->sink->ctx, (const uint8_t*)data, (uint32_t)bytes, &written);
  if ((error != k_ra8_ok) || (written != (uint32_t)bytes)) {
    sink->sink_error = (error != k_ra8_ok) ? error : k_ra8_err_invalid_state;
    return 0U;
  }
  sink->written += (uint64_t)written;
  return bytes;
}

/**
 * @brief Verify that libcurl's effective redirect host is permitted.
 * @details Allows configured cross-host redirects; otherwise compares the effective URL host to origin.
 * @param[in] net Initialised curl backend state.
 * @return Whether the effective host satisfies redirect policy.
 * @retval true Cross-host policy allows it or the host matches.
 * @retval false The effective URL cannot be classified or changes host.
 * @pre @p net and its easy handle are non-NULL.
 * @pre `origin_host` was populated for the request when policy requires it.
 * @post Backend state is unchanged.
 * @post No network operation is initiated.
 * @note Failure to classify is fail-closed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_redirect_host_ok(mdl_curl_ctx_t* net)
{
  if (net->allow_cross_host || (net->origin_host[0] == '\0')) {
    return true;
  }
  char* eff = nullptr;
  if ((curl_easy_getinfo(net->curl, CURLINFO_EFFECTIVE_URL, &eff) != CURLE_OK) ||
      (eff == nullptr)) {
    return false; /* Security decision cannot be made: fail closed. */
  }
  char host[k_origin_host_max];
  if (!mdl_url_host(eff, host, sizeof(host))) {
    return false;
  }
  return strcmp(host, net->origin_host) == 0;
}

/* The libcurl CURLOPT_PREREQFUNCTION ABI fixes these parameter types as
 * non-const `char*`; conn_local_ip is unused here but cannot be re-qualified. */
/**
 * @brief Refuse non-public peers and forbidden cross-host redirects before transfer.
 * @details Classifies libcurl's resolved primary address and then applies redirect-host policy.
 * @param[in] clientp Initialised ::mdl_curl_ctx_t callback state.
 * @param[in] conn_primary_ip NUL-terminated resolved peer address.
 * @param[in] conn_local_ip Local address supplied by libcurl but unused.
 * @param[in] conn_primary_port Resolved peer port supplied by libcurl.
 * @param[in] conn_local_port Local port supplied by libcurl.
 * @return libcurl prerequisite decision.
 * @retval CURL_PREREQFUNC_OK The peer and redirect host are permitted.
 * @retval CURL_PREREQFUNC_ABORT State or policy validation failed.
 * @pre Address pointers follow libcurl's prerequisite callback contract.
 * @pre @p clientp is NULL or points to backend state.
 * @post No connection data is modified.
 * @post A decision that cannot be made is denied.
 * @note Parameter types are fixed by libcurl's ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_on_prereq(
  void* clientp,
  char* conn_primary_ip,
  char*
    conn_local_ip, // NOLINT(readability-non-const-parameter) -- libcurl prereq callback ABI fixes char*.
  int conn_primary_port,
  int conn_local_port)
{
  (void)conn_local_ip;
  (void)conn_primary_port;
  (void)conn_local_port;
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)clientp;
  if (net == nullptr) {
    return CURL_PREREQFUNC_ABORT;
  }
  const mdl_addr_class_t cls = mdl_classify_ip(conn_primary_ip);
  if (!mdl_addr_is_fetchable(cls, net->allow_private)) {
    return CURL_PREREQFUNC_ABORT;
  }
  if (!internal_redirect_host_ok(net)) {
    return CURL_PREREQFUNC_ABORT;
  }
  return CURL_PREREQFUNC_OK;
}

/**
 * @brief True if `code` is CURLE_OK (setopt success).
 * @details Normalises libcurl option results for checked boolean chains.
 * @param[in] code libcurl result code.
 * @return Whether the operation succeeded.
 * @retval true @p code equals `CURLE_OK`.
 * @retval false Any libcurl error was reported.
 * @pre @p code came from a libcurl operation.
 * @pre Exact success classification is intended.
 * @post No state is modified.
 * @post Input is unchanged.
 * @note Thread-safe: pure comparison.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_ok_code(CURLcode code)
{
  return code == CURLE_OK;
}

/**
 * @brief Apply the security-critical, life-of-handle options (all checked).
 * @details Configures proxy policy, TLS verification, protocol limits, redirects, and peer checks.
 * @param[in,out] curl Easy handle being hardened.
 * @param[in] net Backend security policy and callback state.
 * @return Canonical policy or option status.
 * @retval k_ra8_ok Every required option succeeded.
 * @retval k_ra8_err_not_supported Custom CA bytes cannot be bound by this
 * libcurl build.
 * @retval other Proxy policy is unsafe, custom input is invalid, or an option
 * failed.
 * @pre @p curl and @p net are non-NULL.
 * @pre Policy strings, when present, are NUL-terminated.
 * @post On true, the handle enforces the documented transport policy.
 * @post On false, the caller destroys the partial handle.
 * @note A proxy without the private-address escape hatch is rejected fail-closed.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_apply_security_opts(CURL* curl, mdl_curl_ctx_t* net)
{
  const bool using_proxy = ((net->socks5 != nullptr) && (net->socks5[0] != '\0')) ||
                           ((net->proxy != nullptr) && (net->proxy[0] != '\0'));
  /* CURLOPT_PREREQFUNCTION sees the proxy peer, not the proxy's resolution of
   * the target. Requiring the explicit private-host escape hatch avoids
   * presenting the default policy as an SSRF guarantee it cannot provide. */
  if (using_proxy && !net->allow_private) {
    return k_ra8_fail;
  }
  bool proxy_ok = true;
  if ((net->socks5 != nullptr) && (net->socks5[0] != '\0')) {
    proxy_ok =
      internal_ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, net->socks5)) &&
      internal_ok_code(curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME));
  } else if ((net->proxy != nullptr) && (net->proxy[0] != '\0')) {
    proxy_ok = internal_ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, net->proxy));
  } else {
    proxy_ok = internal_ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, ""));
  }

#if LIBCURL_VERSION_NUM >= 0x074D00
  struct curl_blob* ca_blob = &net->ca_blob;
#else
  struct curl_blob* ca_blob = nullptr;
#endif
  const ra8_err_t ca_error = priv_mdl_net_curl_apply_ca_blob(curl, &net->ca_pem, ca_blob);
  if (ca_error != k_ra8_ok) {
    return ca_error;
  }
  const bool options_ok =
    proxy_ok && internal_ok_code(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https")) &&
    /* Defaults are correct today; assert them so the guarantee is in code. */
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)k_curl_max_redirects)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, internal_on_prereq)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_PREREQDATA, net));
  return options_ok ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Apply the behavioural, life-of-handle options (all checked).
 *
 * @details Enables transparent content decoding, imports validated caller-owned
 * cookie bytes, installs the response-header callback, and enforces connect and
 * low-speed time bounds. Any failed libcurl option rejects the handle.
 *
 * @param[in,out] curl Easy handle being configured.
 * @param[in] net Backend policy containing the optional cookie bytes.
 * @return Canonical validation or libcurl option status.
 * @retval k_ra8_ok All options and cookies were applied.
 * @retval other Cookie validation or a libcurl option failed.
 * @pre @p curl is a valid easy handle.
 * @pre @p net is NULL or remains readable for this call.
 * @post On true, the handle has the complete required behavioural policy.
 * @post On false, the caller will destroy rather than use the partial handle.
 * @note The security-critical transport options are applied separately.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_apply_behavior_opts(CURL* curl, const mdl_curl_ctx_t* net)
{
  const bool options_ok =
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "")) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, internal_on_header)) &&
    internal_ok_code(
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)k_connect_timeout_ms)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)k_low_speed_bytes)) &&
    internal_ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)k_low_speed_secs));
  return options_ok ? priv_mdl_net_curl_apply_cookies(curl, &net->cookies) : k_ra8_fail;
}

/**
 * @brief Append one bounded header to caller-owned stable list storage.
 *
 * @details Formats `name: value` into the next fixed buffer, then links the
 * corresponding embedded curl-list node. No allocation or ownership transfer
 * occurs, so the storage remains valid through `curl_easy_perform`.
 *
 * @param[in,out] headers Fixed header buffers and embedded list nodes.
 * @param[in] name NUL-terminated HTTP header name.
 * @param[in] value NUL-terminated HTTP header value.
 * @return Whether the complete header was appended.
 * @retval true  A node was linked and the count advanced.
 * @retval false The table was full or the formatted header did not fit.
 * @pre All arguments are non-NULL and the header state was initialised.
 * @pre @p headers remains alive until the request header is detached.
 * @post On true, `headers->count` increases by one and the list remains terminated.
 * @post On false, `headers->count` and the linked prefix are unchanged.
 * @note Header syntax and value policy are supplied by the internal caller.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_append_req_header(mdl_req_headers_t* headers, const char* name, const char* value)
{
  if (headers->count >= (size_t)k_request_header_count) {
    return false;
  }
  const size_t i = headers->count;
  const int    n =
    __builtin_snprintf(headers->values[i], sizeof(headers->values[i]), "%s: %s", name, value);
  if ((n < 0) || ((size_t)n >= sizeof(headers->values[i]))) {
    return false;
  }
  headers->nodes[i].data = headers->values[i];
  headers->nodes[i].next = nullptr;
  if (i == 0U) {
    headers->head = &headers->nodes[i];
  } else {
    headers->nodes[i - 1U].next = &headers->nodes[i];
  }
  headers->count += 1U;
  return true;
}

/**
 * @brief Build conditional request headers (If-None-Match, If-Modified-Since).
 *
 * @details Resets @p headers, then appends each non-empty validator supplied by
 * @p req. A NULL request produces an empty, valid header list.
 *
 * @param[in] req Optional request carrying entity validators.
 * @param[out] headers Fixed storage receiving the embedded curl list.
 * @return Whether every requested validator fit.
 * @retval true  The complete conditional-header list was built.
 * @retval false A requested header exceeded a fixed bound.
 * @pre @p headers points to writable ::mdl_req_headers_t storage.
 * @pre Validator strings in @p req, when present, are NUL-terminated.
 * @post @p headers is initialised even when no validator is requested.
 * @post On true, the list order is ETag followed by modification date.
 * @note The returned list borrows only storage embedded in @p headers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_build_req_headers(const mdl_net_req_t* req,
                                                    mdl_req_headers_t*   headers)
{
  *headers = (mdl_req_headers_t){};
  if ((req != nullptr) && (req->if_none_match != nullptr) && (req->if_none_match[0] != '\0')) {
    if (!internal_append_req_header(headers, "If-None-Match", req->if_none_match)) {
      return false;
    }
  }
  if ((req != nullptr) && (req->if_modified_since != nullptr) &&
      (req->if_modified_since[0] != '\0')) {
    if (!internal_append_req_header(headers, "If-Modified-Since", req->if_modified_since)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Apply the per-request options shared by both fetch paths.
 * @details Validates and stores the origin host, then sets URL, timeout, referer, and User-Agent.
 * @param[in,out] net Initialised backend state.
 * @param[in] url Absolute request URL.
 * @param[in] req Request metadata and timeout.
 * @return Whether validation and all required options succeeded.
 * @retval true The handle is ready for this request.
 * @retval false Host extraction or a libcurl option failed.
 * @pre @p net, @p url, and @p req are non-NULL.
 * @pre Request strings, when present, are NUL-terminated.
 * @post On true, `origin_host` identifies @p url for redirect enforcement.
 * @post On host failure, `origin_host` is empty.
 * @note The caller separately attaches response sinks and conditional headers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_apply_req(mdl_curl_ctx_t* net, const char* url, const mdl_net_req_t* req)
{
  if (!mdl_url_host(url, net->origin_host, sizeof(net->origin_host))) {
    net->origin_host[0] = '\0';
    return false;
  }
  CURL* curl = net->curl;
  bool  ok = internal_ok_code(curl_easy_setopt(curl, CURLOPT_URL, url)) &&
             internal_ok_code(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms)) &&
             /* CURLOPT_REFERER with NULL clears any prior value -- what we want. */
             internal_ok_code(curl_easy_setopt(curl, CURLOPT_REFERER, req->referer));
  if (ok && (req->user_agent != nullptr)) {
    ok = internal_ok_code(curl_easy_setopt(curl, CURLOPT_USERAGENT, req->user_agent));
  }
  return ok;
}

/**
 * @brief Detach request headers; their backing remains valid in backend state.
 *
 * @details Clears `CURLOPT_HTTPHEADER` only when an embedded list was attached.
 * The fixed nodes and strings are caller-owned and therefore are not freed.
 *
 * @param[in,out] curl Easy handle from which to detach the request list.
 * @param[in] headers Fixed list state used for the completed request.
 * @return Nothing.
 * @pre @p curl and @p headers are non-NULL.
 * @pre The request using @p headers has completed or failed before transfer.
 * @post A non-empty list is no longer associated with @p curl.
 * @post @p headers and all embedded backing bytes remain unchanged.
 * @note A libcurl detach error is intentionally ignored during cleanup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_release_req_headers(CURL* curl, const mdl_req_headers_t* headers)
{
  if (headers->head == nullptr) {
    return;
  }
  struct curl_slist* const no_headers = nullptr;
  (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, no_headers);
}

RA8_PRIV ra8_err_t priv_mdl_net_curl_classify(CURLcode code, bool overflow, long status)
{
  if (overflow) {
    return k_ra8_err_no_mem;
  }
  if (code == CURLE_OPERATION_TIMEDOUT) {
    return k_ra8_err_timeout;
  }
  if (code != CURLE_OK) {
    return k_ra8_fail;
  }
  return priv_mdl_net_classify_http(status);
}

/**
 * @brief Read status and captured headers, fill @p resp, and classify the result.
 * @details Retrieves the HTTP response code, copies bounded metadata, and maps transport status.
 * @param[in] curl Easy handle for the completed request.
 * @param[in] code Result from `curl_easy_perform`.
 * @param[in] overflow Whether the body sink exceeded its cap.
 * @param[in] hdr Captured final-response headers.
 * @param[out] resp Optional public response metadata.
 * @return Canonical network result.
 * @retval k_ra8_ok Transfer and HTTP classification succeeded.
 * @retval other Canonical transport, size, throttle, or HTTP error.
 * @pre @p curl and @p hdr are non-NULL and the transfer has completed.
 * @pre @p resp is NULL or points to writable response storage.
 * @post A non-NULL @p resp contains the final status and bounded headers.
 * @post Curl and captured header state remain owned by the caller.
 * @note Thread safety follows ownership of the easy handle.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_finish_transfer(CURL*             curl,
                                                       CURLcode          code,
                                                       bool              overflow,
                                                       const hdr_sink_t* hdr,
                                                       mdl_net_resp_t*   resp)
{
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (resp != nullptr) {
    resp->status = status;
    (void)__builtin_snprintf(resp->retry_after, sizeof(resp->retry_after), "%s", hdr->retry_after);
    (void)__builtin_snprintf(resp->etag, sizeof(resp->etag), "%s", hdr->etag);
    (void)__builtin_snprintf(resp->last_modified,
                             sizeof(resp->last_modified),
                             "%s",
                             hdr->last_modified);
    (void)
      __builtin_snprintf(resp->content_type, sizeof(resp->content_type), "%s", hdr->content_type);
  }
  return priv_mdl_net_curl_classify(code, overflow, status);
}

/**
 * @brief Vtable method: GET @p url into a caller buffer.
 * @details Applies bounded request state, performs the transfer, detaches headers, and terminates when room remains.
 * @param[in] ctx Initialised curl backend state.
 * @param[in] url Allowed absolute HTTP(S) URL.
 * @param[in] req Request metadata.
 * @param[out] buf Body destination.
 * @param[in] cap Destination capacity.
 * @param[out] out_len Optional body length.
 * @param[out] resp Optional response metadata.
 * @return Canonical transfer result.
 * @retval k_ra8_ok The complete response fit.
 * @retval other Validation, option, transport, HTTP, or size failure.
 * @pre Required pointers are non-NULL and @p buf is writable for @p cap bytes.
 * @pre @p ctx owns an idle easy handle.
 * @post Request headers are detached before return after attachment.
 * @post On success, @p out_len is written when non-NULL.
 * @note Not thread-safe: reuses backend request storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_curl_get_buf(void*                ctx,
                                                    const char*          url,
                                                    const mdl_net_req_t* req,
                                                    char*                buf,
                                                    size_t               cap,
                                                    size_t*              out_len,
                                                    mdl_net_resp_t*      resp)
{
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)ctx;
  if (!mdl_url_scheme_allowed(url)) {
    return k_ra8_err_invalid_arg; /* refuse file://, gopher://, ... before curl */
  }

  if (!internal_build_req_headers(req, &net->request_headers)) {
    return k_ra8_err_invalid_size;
  }
  struct curl_slist* const req_headers = net->request_headers.head;
  buf_sink_t               sink        = {.buf = buf, .cap = cap, .len = 0U, .overflow = false};
  hdr_sink_t               hdr         = {};
  if (!internal_apply_req(net, url, req) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_HTTPHEADER, req_headers)) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !internal_ok_code(
        curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, priv_mdl_net_curl_buf_write)) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink))) {
    internal_release_req_headers(net->curl, &net->request_headers);
    return k_ra8_fail;
  }

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = internal_finish_transfer(net->curl, code, sink.overflow, &hdr, resp);
  internal_release_req_headers(net->curl, &net->request_headers);
  if (rc != k_ra8_ok) {
    return rc;
  }

  if (sink.len < cap) {
    buf[sink.len] = '\0'; /* NUL-terminate when room remains. */
  }
  if (out_len != nullptr) {
    *out_len = sink.len;
  }
  return k_ra8_ok;
}

/**
 * @brief Vtable method: GET @p url through a caller-owned body sink.
 * @details Applies request policy, streams bounded chunks through the injected
 *          callback, and never opens or names a filesystem object.
 * @param[in] ctx Initialised curl backend state.
 * @param[in] url Allowed absolute HTTP(S) URL.
 * @param[in] req Request metadata.
 * @param[in,out] sink Reset caller-owned body destination.
 * @param[out] out_len Optional committed body length.
 * @param[out] resp Optional response metadata.
 * @return Canonical transfer result.
 * @retval k_ra8_ok A complete response body was accepted.
 * @retval other Validation, sink, option, transport, HTTP, or size failure.
 * @pre Required pointers are non-NULL and @p url is NUL-terminated.
 * @pre @p ctx owns an idle easy handle.
 * @post On success @p out_len receives the accepted body extent when requested.
 * @post Request headers are detached before return after attachment.
 * @note Not thread-safe: reuses backend request storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_curl_get_body(void*                ctx,
                                                     const char*          url,
                                                     const mdl_net_req_t* req,
                                                     mdl_net_body_sink_t* sink,
                                                     size_t*              out_len,
                                                     mdl_net_resp_t*      resp)
{
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)ctx;
  if (!mdl_url_scheme_allowed(url)) {
    return k_ra8_err_invalid_arg;
  }

  if (!internal_build_req_headers(req, &net->request_headers)) {
    return k_ra8_err_invalid_size;
  }
  struct curl_slist* const  req_headers = net->request_headers.head;
  mdl_net_curl_body_state_t body        = {.sink       = sink,
                                           .written    = 0U,
                                           .cap        = net->max_bytes,
                                           .sink_error = k_ra8_ok,
                                           .overflow   = false};
  hdr_sink_t                hdr         = {};
  /* MAXFILESIZE_LARGE checks an advertised Content-Length up front; 0 disables
   * it, exactly as cap == 0 disables the callback check below. */
  if (!internal_apply_req(net, url, req) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_HTTPHEADER, req_headers)) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !internal_ok_code(
        curl_easy_setopt(net->curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)net->max_bytes)) ||
      !internal_ok_code(
        curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, priv_mdl_net_curl_body_write)) ||
      !internal_ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &body))) {
    internal_release_req_headers(net->curl, &net->request_headers);
    return k_ra8_fail;
  }

  const CURLcode code = curl_easy_perform(net->curl);
  ra8_err_t      rc   = internal_finish_transfer(net->curl, code, body.overflow, &hdr, resp);
  internal_release_req_headers(net->curl, &net->request_headers);
  if (!body.overflow && (body.sink_error != k_ra8_ok)) {
    rc = body.sink_error;
  }
  if ((rc == k_ra8_ok) && (out_len != nullptr)) {
    *out_len = (size_t)body.written;
  }
  return rc;
}

/**
 * @brief Release the libcurl handle and clear this backend state.
 * @details Cleans up the easy handle when present, zeroes caller-owned storage, and releases global curl state.
 * @param[in,out] ctx Optional ::mdl_curl_ctx_t backend state.
 * @return Nothing.
 * @pre @p ctx is NULL or points to storage initialised by this backend.
 * @pre No transfer is active on the easy handle.
 * @post A non-NULL context contains only zero bytes.
 * @post Backend-owned libcurl resources are released.
 * @note Not thread-safe with concurrent operations on the same backend.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_curl_destroy(void* ctx)
{
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)ctx;
  if (net == nullptr) {
    return;
  }
  if (net->curl != nullptr) {
    curl_easy_cleanup(net->curl);
  }
  *net = (mdl_curl_ctx_t){};
}

/** @brief The libcurl backend's immutable method table. */
static const mdl_net_vtable_t s_curl_vtable = {
  .get_buf  = internal_curl_get_buf,
  .get_body = internal_curl_get_body,
  .destroy  = internal_curl_destroy,
};

ra8_err_t mdl_net_curl_init(mdl_net_iface_t*        net,
                            mdl_net_curl_storage_t* storage,
                            const mdl_net_policy_t* policy)
{
  if ((net == nullptr) || (storage == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *net                       = (mdl_net_iface_t){};
  *storage                   = (mdl_net_curl_storage_t){};
  static bool s_global_ready = false;
  if (!s_global_ready) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      return k_ra8_fail;
    }
    s_global_ready = true;
  }

  mdl_curl_ctx_t* ctx = (mdl_curl_ctx_t*)storage->bytes;
  if (policy != nullptr) {
    ctx->allow_private    = policy->allow_private_hosts;
    ctx->allow_cross_host = policy->allow_cross_host_redirect;
    ctx->max_bytes        = policy->max_response_bytes;
    ctx->proxy            = policy->proxy;
    ctx->socks5           = policy->socks5;
    ctx->cookies          = policy->cookies;
    ctx->ca_pem           = policy->ca_pem;
  }
  ctx->curl = curl_easy_init();
  if (ctx->curl == nullptr) {
    return k_ra8_fail;
  }
  ra8_err_t init_error = internal_apply_security_opts(ctx->curl, ctx);
  if (init_error == k_ra8_ok) {
    init_error = internal_apply_behavior_opts(ctx->curl, ctx);
  }
  if (init_error != k_ra8_ok) {
    curl_easy_cleanup(ctx->curl);
    *ctx = (mdl_curl_ctx_t){};
    return init_error;
  }
  net->vtable = &s_curl_vtable;
  net->ctx    = ctx;
  return k_ra8_ok;
}
