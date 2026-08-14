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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "mdl_atomic.h"
#include "mdl_net.h"
#include "mdl_net_curl_internal.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief Backend tunables. */
typedef enum : uint32_t {
  k_curl_max_redirects   = 5,     /**< Redirect hops to follow.                  */
  k_http_not_modified    = 304,   /**< Conditional GET reused the held entity.  */
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
  CURL*             curl;                           /**< Reused easy handle.          */
  bool              allow_private;                  /**< SSRF opt-in (private peers). */
  bool              allow_cross_host;               /**< Cross-host redirect opt-in.  */
  uint64_t          max_bytes;                      /**< Per-response cap (0 = none). */
  const char*       proxy;                          /**< HTTP/HTTPS proxy URL.        */
  const char*       socks5;                         /**< SOCKS5 proxy URL.            */
  const char*       cookie_file;                    /**< Cookie file path.            */
  const char*       ca_file;                        /**< Custom PEM CA bundle.         */
  char              origin_host[k_origin_host_max]; /**< Host of the current request. */
  mdl_req_headers_t request_headers;                /**< Stable conditional headers.  */
} mdl_curl_ctx_t;

static_assert(sizeof(mdl_curl_ctx_t) <= k_mdl_net_curl_storage_bytes,
              "mdl_net_curl_storage_t is too small for the private context");
static_assert(alignof(mdl_net_curl_storage_t) >= alignof(mdl_curl_ctx_t),
              "mdl_net_curl_storage_t is insufficiently aligned");

/** @brief Size-bounded FILE* sink state for an image fetch. */
typedef struct {
  FILE*    fp;       /**< Destination file.                 */
  uint64_t written;  /**< Bytes written so far.             */
  uint64_t cap;      /**< Per-response cap (0 = unlimited). */
  bool     overflow; /**< Set once the body exceeds `cap`.  */
} file_sink_t;

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
RA8_INTERNAL static char ascii_lower(char c)
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
RA8_INTERNAL static bool header_is(const char* line, size_t line_len, const char* prefix)
{
  size_t i = 0U;
  for (; (i < line_len) && (prefix[i] != '\0'); ++i) {
    if (ascii_lower(line[i]) != prefix[i]) {
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
RA8_INTERNAL static void header_value(const char* line, size_t line_len, char* out, size_t cap)
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

/* cppcheck-suppress constParameterCallback ; libcurl HEADERFUNCTION ABI is char* */
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
RA8_INTERNAL static size_t on_header(char* buffer, size_t size, size_t nitems, void* user)
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
  if (header_is(buffer, len, "http/")) {
    sink->retry_after[0]   = '\0';
    sink->etag[0]          = '\0';
    sink->last_modified[0] = '\0';
    sink->content_type[0]  = '\0';
  } else if (header_is(buffer, len, "retry-after:")) {
    header_value(buffer, len, sink->retry_after, sizeof(sink->retry_after));
  } else if (header_is(buffer, len, "etag:")) {
    header_value(buffer, len, sink->etag, sizeof(sink->etag));
  } else if (header_is(buffer, len, "last-modified:")) {
    header_value(buffer, len, sink->last_modified, sizeof(sink->last_modified));
  } else if (header_is(buffer, len, "content-type:")) {
    header_value(buffer, len, sink->content_type, sizeof(sink->content_type));
  }
  return len;
}

/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
RA8_PRIV size_t mdl_net_curl_buf_write(char* data, size_t size, size_t nmemb, void* user)
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

/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
/**
 * @brief Append response bytes to a file while enforcing the configured cap.
 * @details Rejects overflow before writing and returns the exact byte count accepted by `fwrite`.
 * @param[in] data Response bytes supplied by libcurl.
 * @param[in] size Element size.
 * @param[in] nmemb Element count.
 * @param[in,out] user ::file_sink_t callback state.
 * @return Number of bytes written, or zero to abort transfer.
 * @retval 0 Invalid state, overflow, or a failed write.
 * @retval other Bytes accepted by the file stream.
 * @pre @p data is readable for the representable requested byte count.
 * @pre @p user points to an open file sink.
 * @post `written` advances by exactly the returned count.
 * @post A cap or arithmetic overflow sets the overflow flag.
 * @note Signature is fixed by libcurl's callback ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t on_file_write(char* data, size_t size, size_t nmemb, void* user)
{
  file_sink_t* sink = (file_sink_t*)user;
  if ((sink == nullptr) || (sink->fp == nullptr)) {
    return 0U;
  }
  if ((nmemb != 0U) && (size > (SIZE_MAX / nmemb))) {
    sink->overflow = true;
    return 0U;
  }
  const size_t bytes = size * nmemb;
  if (mdl_size_exceeds(sink->written, (uint64_t)bytes, sink->cap)) {
    sink->overflow = true;
    return 0U; /* Chunked responses have no Content-Length; cap them here. */
  }
  const size_t wrote = fwrite(data, 1U, bytes, sink->fp);
  sink->written += (uint64_t)wrote;
  return wrote;
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
RA8_INTERNAL static bool redirect_host_ok(mdl_curl_ctx_t* net)
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
RA8_INTERNAL static int
on_prereq(void* clientp,
          /* cppcheck-suppress constParameterCallback ; CURLOPT_PREREQFUNCTION ABI fixes char* */
          char* conn_primary_ip,
          char* conn_local_ip, // NOLINT(readability-non-const-parameter)
          int   conn_primary_port,
          int   conn_local_port)
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
  if (!redirect_host_ok(net)) {
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
RA8_INTERNAL static bool ok_code(CURLcode code)
{
  return code == CURLE_OK;
}

/**
 * @brief Apply the security-critical, life-of-handle options (all checked).
 * @details Configures proxy policy, TLS verification, protocol limits, redirects, and peer checks.
 * @param[in,out] curl Easy handle being hardened.
 * @param[in] net Backend security policy and callback state.
 * @return Whether the complete security policy was applied.
 * @retval true Every required option succeeded.
 * @retval false Proxy policy is unsafe or an option failed.
 * @pre @p curl and @p net are non-NULL.
 * @pre Policy strings, when present, are NUL-terminated.
 * @post On true, the handle enforces the documented transport policy.
 * @post On false, the caller destroys the partial handle.
 * @note A proxy without the private-address escape hatch is rejected fail-closed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_security_opts(CURL* curl, mdl_curl_ctx_t* net)
{
  const bool using_proxy = ((net->socks5 != nullptr) && (net->socks5[0] != '\0')) ||
                           ((net->proxy != nullptr) && (net->proxy[0] != '\0'));
  /* CURLOPT_PREREQFUNCTION sees the proxy peer, not the proxy's resolution of
   * the target. Requiring the explicit private-host escape hatch avoids
   * presenting the default policy as an SSRF guarantee it cannot provide. */
  if (using_proxy && !net->allow_private) {
    return false;
  }
  bool proxy_ok = true;
  if ((net->socks5 != nullptr) && (net->socks5[0] != '\0')) {
    proxy_ok = ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, net->socks5)) &&
               ok_code(curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME));
  } else if ((net->proxy != nullptr) && (net->proxy[0] != '\0')) {
    proxy_ok = ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, net->proxy));
  } else {
    proxy_ok = ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, ""));
  }

  const bool ca_ok = ((net->ca_file == nullptr) || (net->ca_file[0] == '\0')) ||
                     ok_code(curl_easy_setopt(curl, CURLOPT_CAINFO, net->ca_file));
  return proxy_ok && ca_ok &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https")) &&
         /* Defaults are correct today; assert them so the guarantee is in code. */
         ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)k_curl_max_redirects)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, on_prereq)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PREREQDATA, net));
}

/**
 * @brief Apply the behavioural, life-of-handle options (all checked).
 *
 * @details Enables transparent content decoding, configures the caller's cookie
 * file, installs the response-header callback, and enforces connect and
 * low-speed time bounds. Any failed libcurl option rejects the handle.
 *
 * @param[in,out] curl Easy handle being configured.
 * @param[in] net Backend policy containing the optional cookie-file path.
 * @return Whether every behavioural option was accepted by libcurl.
 * @retval true  All options were applied.
 * @retval false At least one option failed.
 * @pre @p curl is a valid easy handle.
 * @pre @p net is NULL or remains readable for this call.
 * @post On true, the handle has the complete required behavioural policy.
 * @post On false, the caller will destroy rather than use the partial handle.
 * @note The security-critical transport options are applied separately.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_behavior_opts(CURL* curl, const mdl_curl_ctx_t* net)
{
  const char* cfile = (net != nullptr && net->cookie_file != nullptr) ? net->cookie_file : "";
  return ok_code(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cfile)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)k_connect_timeout_ms)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)k_low_speed_bytes)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)k_low_speed_secs));
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
append_req_header(mdl_req_headers_t* headers, const char* name, const char* value)
{
  if (headers->count >= (size_t)k_request_header_count) {
    return false;
  }
  const size_t i = headers->count;
  const int    n = snprintf(headers->values[i], sizeof(headers->values[i]), "%s: %s", name, value);
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
RA8_INTERNAL static bool build_req_headers(const mdl_net_req_t* req, mdl_req_headers_t* headers)
{
  *headers = (mdl_req_headers_t){};
  if ((req != nullptr) && (req->if_none_match != nullptr) && (req->if_none_match[0] != '\0')) {
    if (!append_req_header(headers, "If-None-Match", req->if_none_match)) {
      return false;
    }
  }
  if ((req != nullptr) && (req->if_modified_since != nullptr) &&
      (req->if_modified_since[0] != '\0')) {
    if (!append_req_header(headers, "If-Modified-Since", req->if_modified_since)) {
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
RA8_INTERNAL static bool apply_req(mdl_curl_ctx_t* net, const char* url, const mdl_net_req_t* req)
{
  if (!mdl_url_host(url, net->origin_host, sizeof(net->origin_host))) {
    net->origin_host[0] = '\0';
    return false;
  }
  CURL* curl = net->curl;
  bool  ok   = ok_code(curl_easy_setopt(curl, CURLOPT_URL, url)) &&
               ok_code(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms)) &&
               /* CURLOPT_REFERER with NULL clears any prior value -- what we want. */
               ok_code(curl_easy_setopt(curl, CURLOPT_REFERER, req->referer));
  if (ok && (req->user_agent != nullptr)) {
    ok = ok_code(curl_easy_setopt(curl, CURLOPT_USERAGENT, req->user_agent));
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
RA8_INTERNAL static void release_req_headers(CURL* curl, const mdl_req_headers_t* headers)
{
  if (headers->head == nullptr) {
    return;
  }
  struct curl_slist* const no_headers = nullptr;
  (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, no_headers);
}

RA8_PRIV ra8_err_t mdl_net_curl_classify(CURLcode code, bool overflow, long status)
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
  if ((status == (long)k_http_too_many_req) || (status == (long)k_http_unavailable)) {
    return k_ra8_err_busy; /* 429 / 503: throttled -- the governor must back off. */
  }
  if (status >= (long)k_http_server_err_min) {
    return k_ra8_fail; /* 5xx: server error -- retryable later. */
  }
  if (status >= (long)k_http_client_err_min) {
    return k_ra8_err_not_found; /* 404 and other 4xx: skip this resource. */
  }
  return k_ra8_ok;
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
RA8_INTERNAL static ra8_err_t finish_transfer(CURL*             curl,
                                              CURLcode          code,
                                              bool              overflow,
                                              const hdr_sink_t* hdr,
                                              mdl_net_resp_t*   resp)
{
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (resp != nullptr) {
    resp->status = status;
    (void)snprintf(resp->retry_after, sizeof(resp->retry_after), "%s", hdr->retry_after);
    (void)snprintf(resp->etag, sizeof(resp->etag), "%s", hdr->etag);
    (void)snprintf(resp->last_modified, sizeof(resp->last_modified), "%s", hdr->last_modified);
    (void)snprintf(resp->content_type, sizeof(resp->content_type), "%s", hdr->content_type);
  }
  return mdl_net_curl_classify(code, overflow, status);
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
RA8_INTERNAL static ra8_err_t curl_get_buf(void*                ctx,
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

  if (!build_req_headers(req, &net->request_headers)) {
    return k_ra8_err_invalid_size;
  }
  struct curl_slist* const req_headers = net->request_headers.head;
  buf_sink_t               sink        = {.buf = buf, .cap = cap, .len = 0U, .overflow = false};
  hdr_sink_t               hdr         = {};
  if (!apply_req(net, url, req) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HTTPHEADER, req_headers)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, mdl_net_curl_buf_write)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink))) {
    release_req_headers(net->curl, &net->request_headers);
    return k_ra8_fail;
  }

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = finish_transfer(net->curl, code, sink.overflow, &hdr, resp);
  release_req_headers(net->curl, &net->request_headers);
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
 * @brief Vtable method: GET @p url and atomically replace a file.
 * @details Streams into a bounded sibling temporary file and commits it only after a successful complete transfer.
 * @param[in] ctx Initialised curl backend state.
 * @param[in] url Allowed absolute HTTP(S) URL.
 * @param[in] req Request metadata.
 * @param[in] out_path Destination path.
 * @param[out] out_len Optional committed body length.
 * @param[out] resp Optional response metadata.
 * @return Canonical transfer result.
 * @retval k_ra8_ok A complete file was atomically committed.
 * @retval other Validation, file, option, transport, HTTP, or size failure.
 * @pre Required pointers are non-NULL and strings are NUL-terminated.
 * @pre @p ctx owns an idle easy handle.
 * @post On failure, the prior destination remains unchanged and temporary debris is removed.
 * @post Request headers are detached before return after attachment.
 * @note Not thread-safe: reuses backend request storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t curl_get_file(void*                ctx,
                                            const char*          url,
                                            const mdl_net_req_t* req,
                                            const char*          out_path,
                                            size_t*              out_len,
                                            mdl_net_resp_t*      resp)
{
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)ctx;
  if (!mdl_url_scheme_allowed(url)) {
    return k_ra8_err_invalid_arg;
  }

  /* Download to a sibling temp and rename in only once a complete good copy
   * exists. Opening out_path directly would TRUNCATE an already-downloaded
   * page the instant fopen() succeeded, and the remove() on the failure paths
   * below would then delete the remains -- so a re-fetch that hit a 503 cost
   * the user a file they already had. */
  char tmp_path[PATH_MAX];
  if (!mdl_atomic_tmp_path(out_path, tmp_path, sizeof(tmp_path))) {
    return k_ra8_fail;
  }

  FILE* fp = fopen(tmp_path, "wb");
  if (fp == nullptr) {
    mdl_atomic_abort(tmp_path);
    return k_ra8_fail;
  }

  if (!build_req_headers(req, &net->request_headers)) {
    (void)fclose(fp);
    mdl_atomic_abort(tmp_path);
    return k_ra8_err_invalid_size;
  }
  struct curl_slist* const req_headers = net->request_headers.head;
  file_sink_t sink = {.fp = fp, .written = 0U, .cap = net->max_bytes, .overflow = false};
  hdr_sink_t  hdr  = {};
  /* MAXFILESIZE_LARGE checks an advertised Content-Length up front; 0 disables
   * it, exactly as cap == 0 disables the callback check below. */
  if (!apply_req(net, url, req) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HTTPHEADER, req_headers)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !ok_code(
        curl_easy_setopt(net->curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)net->max_bytes)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, on_file_write)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink))) {
    (void)fclose(fp);
    release_req_headers(net->curl, &net->request_headers);
    mdl_atomic_abort(tmp_path);
    return k_ra8_fail;
  }

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = finish_transfer(net->curl, code, sink.overflow, &hdr, resp);
  release_req_headers(net->curl, &net->request_headers);

  const long fsize = ftell(fp);
  if (fclose(fp) != 0) {
    mdl_atomic_abort(tmp_path);
    return k_ra8_fail;
  }
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(tmp_path); /* Truncated/oversized: destination untouched. */
    return rc;
  }
  long status = 0;
  if (resp != nullptr) {
    status = resp->status;
  } else {
    curl_easy_getinfo(net->curl, CURLINFO_RESPONSE_CODE, &status);
  }
  if (status == (long)k_http_not_modified) {
    /* 304 Not Modified: discard empty temp file, retain existing destination file. */
    mdl_atomic_abort(tmp_path);
    if (out_len != nullptr) {
      struct stat st;
      *out_len = (stat(out_path, &st) == 0) ? (size_t)st.st_size : 0U;
    }
    return k_ra8_ok;
  }
  if (!mdl_atomic_commit(tmp_path, out_path)) {
    return k_ra8_fail;
  }
  if (out_len != nullptr) {
    *out_len = (fsize < 0) ? 0U : (size_t)fsize;
  }
  return k_ra8_ok;
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
RA8_INTERNAL static void curl_destroy(void* ctx)
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
  .get_buf  = curl_get_buf,
  .get_file = curl_get_file,
  .destroy  = curl_destroy,
};

RA8_DI_SLOT("net_iface")
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
    ctx->cookie_file      = policy->cookie_file;
    ctx->ca_file          = policy->ca_file;
  }
  ctx->curl = curl_easy_init();
  if (ctx->curl == nullptr) {
    return k_ra8_fail;
  }
  if (!apply_security_opts(ctx->curl, ctx) || !apply_behavior_opts(ctx->curl, ctx)) {
    curl_easy_cleanup(ctx->curl);
    *ctx = (mdl_curl_ctx_t){};
    return k_ra8_fail;
  }
  net->vtable = &s_curl_vtable;
  net->ctx    = ctx;
  return k_ra8_ok;
}
