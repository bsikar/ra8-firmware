/**
 * @file mdl_session.c
 * @brief Implementation of the session identity + robots.txt gating.
 *
 * @details Builds the truthful tool identity, derives each request's robots
 * origin and match target, and connects the bounded robots cache to the
 * abstract network interface. Policy remains fail-closed for throttling and
 * server failures without introducing backend-specific dependencies.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_session.h"

#include <stdio.h>
#include <string.h>

#include "mdl_stream_internal.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief robots.txt fetch tunables. */
typedef enum : uint32_t {
  k_robots_timeout_ms = 10000U, /**< Per-robots.txt request budget, ms. */
} mdl_session_net_t;

/** @brief HTTP server-error range treated as "disallow all". */
typedef enum : uint16_t {
  k_http_too_many       = 429, /**< Rate limiting: do not bypass robots. */
  k_http_client_err_min = 400, /**< First 4xx unavailable status.        */
  k_http_client_err_max = 499, /**< Last 4xx unavailable status.         */
  k_http_server_err_min = 500, /**< First 5xx status.                    */
  k_http_server_err_max = 599, /**< Last 5xx status.                     */
} mdl_session_status_t;

/** @brief Product token used to match robots.txt `User-agent` groups. */
static const char* const s_ua_token = "media_dl";
/** @brief Tool version reported in the User-Agent. */
static const char* const s_ua_version = "0.1.0";
/** @brief Project URL reported in the User-Agent for a contact channel. */
static const char* const s_ua_url = "https://github.com/bsikar/ra8-firmware";

const char* mdl_session_ua_token(void)
{
  return s_ua_token;
}

bool mdl_session_build_ua(const char* contact, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return false;
  }
  const bool have = (contact != nullptr) && (contact[0] != '\0');
  if (have) {
    (void)snprintf(out, cap, "%s/%s (+%s; %s)", s_ua_token, s_ua_version, s_ua_url, contact);
  } else {
    (void)snprintf(out, cap, "%s/%s (+%s)", s_ua_token, s_ua_version, s_ua_url);
  }
  return have;
}

void mdl_session_init(mdl_session_t*   session,
                      mdl_net_iface_t* net,
                      const char*      user_agent,
                      ra8_io_stream_t* diagnostic,
                      bool             honor_robots)
{
  if (session == nullptr) {
    return;
  }
  session->net          = net;
  session->user_agent   = user_agent;
  session->diagnostic   = diagnostic;
  session->honor_robots = honor_robots;
  session->cache        = (mdl_robots_cache_t){};
}

/**
 * @brief ASCII case-insensitive comparison of one URL prefix.
 *
 * @details Lower-cases only bytes read from @p url and compares them with the
 * already lower-case literal @p prefix. The comparison stops at the end of the
 * prefix, so trailing URL bytes are intentionally ignored.
 *
 * @param[in] url NUL-terminated URL text to inspect.
 * @param[in] prefix NUL-terminated lower-case ASCII prefix.
 * @return Whether @p url begins with @p prefix ignoring ASCII case.
 * @retval true  Every prefix byte matched.
 * @retval false At least one byte differed.
 * @pre @p url and @p prefix are non-NULL and NUL-terminated.
 * @pre @p prefix contains lower-case ASCII comparison text.
 * @post Neither input string is modified.
 * @post The result depends only on the first `strlen(prefix)` URL bytes.
 * @note This is deliberately locale-independent and intended for URL schemes.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_url_prefix(const char* url, const char* prefix)
{
  size_t i = 0U;
  while (prefix[i] != '\0') {
    char c = url[i];
    if ((c >= 'A') && (c <= 'Z')) {
      c = (char)(c + ('a' - 'A'));
    }
    if (c != prefix[i]) {
      return false;
    }
    ++i;
  }
  return true;
}

/** @brief Return the validated lower-case HTTP(S) scheme, or NULL. */
RA8_INTERNAL static const char* internal_url_scheme(const char* url)
{
  if (internal_url_prefix(url, "https://")) {
    return "https";
  }
  if (internal_url_prefix(url, "http://")) {
    return "http";
  }
  return nullptr;
}

/**
 * @brief Extract path plus query, excluding fragment, for RFC 9309 matching.
 *
 * @details Skips the scheme and authority, preserves a query as part of the
 * robots match target, and substitutes `/` when the URL has no explicit path.
 * The fragment is never copied.
 *
 * @param[in] url Absolute HTTP(S) URL to inspect.
 * @param[out] out Destination for the NUL-terminated robots match target.
 * @param[in] cap Capacity of @p out in bytes.
 * @return Whether the complete target fit in @p out.
 * @retval true  A path/query target was written.
 * @retval false Arguments were invalid, no scheme separator exists, or output is too small.
 * @pre @p url is NUL-terminated when non-NULL.
 * @pre @p out is writable for @p cap bytes when non-NULL.
 * @post On true, @p out begins with `/` and contains no fragment.
 * @post On a size failure, a non-empty @p out is cleared.
 * @note This helper extracts syntax only; scheme and host policy are checked separately.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_robots_target(const char* url, char* out, size_t cap)
{
  if ((url == nullptr) || (out == nullptr) || (cap < 2U)) {
    return false;
  }
  const char* sep = strstr(url, "://");
  if (sep == nullptr) {
    return false;
  }
  const char* authority = sep + 3U;
  const char* start     = strpbrk(authority, "/?#");
  size_t      n         = 0U;
  if ((start == nullptr) || (*start == '#')) {
    out[0] = '/';
    out[1] = '\0';
    return true;
  }
  if (*start == '?') {
    out[n] = '/';
    ++n;
  }
  const char* p = start;
  while ((*p != '\0') && (*p != '#')) {
    if ((n + 1U) >= cap) {
      out[0] = '\0';
      return false;
    }
    out[n] = *p;
    ++n;
    ++p;
  }
  out[n] = '\0';
  return true;
}

/**
 * @brief robots.txt fetch callback: map an mdl_net GET to a fetch result.
 * @details Performs a bounded GET with the session identity and maps transport
 * and HTTP status classes onto the robots cache's allow/deny convention.
 * @param[in] ctx Initialised ::mdl_session_t.
 * @param[in] robots_url Absolute robots.txt URL.
 * @param[out] buf Caller buffer receiving a successful response body.
 * @param[in] cap Capacity of @p buf in bytes.
 * @param[out] out_len Successful body length.
 * @return Robots fetch outcome class.
 * @retval k_mdl_robots_fetch_ok A body was retrieved.
 * @retval k_mdl_robots_fetch_absent A client absence or transport failure permits access.
 * @retval k_mdl_robots_fetch_denied Throttle or server failure closes access.
 * @pre Pointer arguments are non-NULL and @p buf is writable for @p cap bytes.
 * @pre @p ctx contains a usable network interface and User-Agent.
 * @post On success, @p out_len describes the retrieved body.
 * @post The session's borrowed identity and backend pointers are unchanged.
 * @note Thread safety follows the network backend.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_robots_fetch_result_t
internal_session_fetch(void* ctx, const char* robots_url, char* buf, size_t cap, size_t* out_len)
{
  mdl_session_t*      s    = (mdl_session_t*)ctx;
  const mdl_net_req_t req  = {.user_agent = s->user_agent,
                              .referer    = nullptr,
                              .timeout_ms = k_robots_timeout_ms};
  mdl_net_resp_t      resp = {};
  if (mdl_net_get_buf(s->net, robots_url, &req, buf, cap, out_len, &resp) == k_ra8_ok) {
    return k_mdl_robots_fetch_ok;
  }
  if ((resp.status >= (long)k_http_client_err_min) &&
      (resp.status <= (long)k_http_client_err_max) && (resp.status != (long)k_http_too_many)) {
    return k_mdl_robots_fetch_absent;
  }
  if ((resp.status >= (long)k_http_server_err_min) &&
      (resp.status <= (long)k_http_server_err_max)) {
    return k_mdl_robots_fetch_denied;
  }
  return k_mdl_robots_fetch_denied;
}

/** @brief Report a failed robots consultation for one origin and path.
 * @details Writes a bounded robots-policy diagnostic through the injected stream.
 *          Any sink error is retained in the session for the caller to observe.
 * @param[in,out] session Caller-owned session state read and updated.
 * @param[in] host Validated host name.
 * @param[in] path Validated URL or filesystem path.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_report_robots_unavailable(mdl_session_t* session, const char* host, const char* path)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok,
                                         session->diagnostic,
                                         "media_dl: robots.txt unavailable, rate-limited, or "
                                         "oversized for ");
  error           = priv_mdl_stream_text(error, session->diagnostic, host);
  error           = priv_mdl_stream_text(error, session->diagnostic, "; refusing ");
  error           = priv_mdl_stream_text(error, session->diagnostic, path);
  (void)priv_mdl_stream_text(error, session->diagnostic, "\n");
}

/** @brief Report the robots rule that rejected one path.
 * @details Writes a bounded robots-policy diagnostic through the injected stream.
 *          Any sink error is retained in the session for the caller to observe.
 * @param[in,out] session Caller-owned session state read and updated.
 * @param[in] path Validated URL or filesystem path.
 * @param[in] host Validated host name.
 * @param[in] reason Human-readable policy reason.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_report_robots_disallow(mdl_session_t* session,
                                                         const char*    path,
                                                         const char*    host,
                                                         const char*    reason)
{
  ra8_err_t error =
    priv_mdl_stream_text(k_ra8_ok, session->diagnostic, "media_dl: robots.txt disallows ");
  error = priv_mdl_stream_text(error, session->diagnostic, path);
  error = priv_mdl_stream_text(error, session->diagnostic, " on ");
  error = priv_mdl_stream_text(error, session->diagnostic, host);
  error = priv_mdl_stream_text(error, session->diagnostic, " (rule \"");
  error = priv_mdl_stream_text(error, session->diagnostic, reason);
  (void)priv_mdl_stream_text(error, session->diagnostic, "\"); skipping\n");
}

bool mdl_session_url_allowed(mdl_session_t* session, const char* url, uint32_t* crawl_delay_ms)
{
  if (crawl_delay_ms != nullptr) {
    *crawl_delay_ms = 0U;
  }
  if ((session == nullptr) || (url == nullptr)) {
    return false;
  }
  if (!session->honor_robots) {
    return true;
  }
  const char* scheme = internal_url_scheme(url);
  if (scheme == nullptr) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               session->diagnostic,
                               "media_dl: cannot apply robots.txt to malformed/non-HTTP URL; "
                               "refusing\n");
    return false;
  }
  char host[k_mdl_robots_host_max];
  if (!mdl_url_host(url, host, sizeof(host))) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               session->diagnostic,
                               "media_dl: cannot identify URL origin for robots.txt; refusing\n");
    return false;
  }
  char path[k_mdl_robots_path_max];
  if (!internal_robots_target(url, path, sizeof(path))) {
    (void)priv_mdl_stream_text(k_ra8_ok,
                               session->diagnostic,
                               "media_dl: URL path/query exceeds robots.txt bound; refusing\n");
    return false;
  }
  const mdl_robots_t* rules = mdl_robots_cache_consult(&session->cache,
                                                       scheme,
                                                       host,
                                                       s_ua_token,
                                                       internal_session_fetch,
                                                       session,
                                                       session->scratch,
                                                       sizeof(session->scratch));
  if (rules == nullptr) {
    internal_report_robots_unavailable(session, host, path);
    return false;
  }
  if ((crawl_delay_ms != nullptr) && rules->have_crawl_delay) {
    *crawl_delay_ms = rules->crawl_delay_ms;
  }
  const char* reason = mdl_robots_disallow_reason(rules, path);
  if (reason != nullptr) {
    internal_report_robots_disallow(session, path, host, reason);
    return false;
  }
  return true;
}
