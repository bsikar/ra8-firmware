/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
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
 */
#include "mdl_net_curl.h"

#include <curl/curl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_atomic.h"
#include "mdl_net.h"
#include "mdl_net_curl_internal.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief Backend tunables. */
typedef enum : uint32_t {
  k_curl_max_redirects  = 5,     /**< Redirect hops to follow.                  */
  k_http_client_err_min = 400,   /**< First HTTP status treated as an error.    */
  k_http_not_found      = 404,   /**< Absent resource (client error).           */
  k_http_too_many_req   = 429,   /**< Too Many Requests (throttle).             */
  k_http_server_err_min = 500,   /**< First HTTP status that is a server error. */
  k_http_unavailable    = 503,   /**< Service Unavailable (throttle).           */
  k_connect_timeout_ms  = 15000, /**< TCP/TLS connect budget, ms.               */
  k_low_speed_bytes     = 64,    /**< Below this many B/s...                    */
  k_low_speed_secs      = 30,    /**< ...for this long, abort a stalled xfer.   */
  k_origin_host_max     = 256,   /**< Stored origin-host buffer bytes.          */
} mdl_curl_limits_t;

/** @brief Private state of the libcurl backend (the vtable's `ctx`). */
typedef struct {
  CURL*    curl;                           /**< Reused easy handle.          */
  bool     allow_private;                  /**< SSRF opt-in (private peers). */
  bool     allow_cross_host;               /**< Cross-host redirect opt-in.  */
  uint64_t max_bytes;                      /**< Per-response cap (0 = none). */
  char     origin_host[k_origin_host_max]; /**< Host of the current request. */
} mdl_curl_ctx_t;

/** @brief Size-bounded FILE* sink state for an image fetch. */
typedef struct {
  FILE*    fp;       /**< Destination file.                 */
  uint64_t written;  /**< Bytes written so far.             */
  uint64_t cap;      /**< Per-response cap (0 = unlimited). */
  bool     overflow; /**< Set once the body exceeds `cap`.  */
} file_sink_t;

/** @brief Response-header capture sink: keeps the final response's Retry-After. */
typedef struct {
  char retry_after[k_mdl_retry_after_max]; /**< Raw value, "" when absent. */
} hdr_sink_t;

/** @brief Lower-case an ASCII byte (locale-independent). */
RA8_INTERNAL static char ascii_lower(char c)
{
  if ((c >= 'A') && (c <= 'Z')) {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

/** @brief True when `line` begins with `prefix`, ASCII case-insensitively. */
RA8_INTERNAL static bool header_is(const char* line, size_t line_len, const char* prefix)
{
  size_t i = 0U;
  for (; (prefix[i] != '\0') && (i < line_len); ++i) {
    if (ascii_lower(line[i]) != prefix[i]) {
      return false;
    }
  }
  return prefix[i] == '\0';
}

/** @brief Copy a header value (after the colon), trimmed of CR/LF/space, bounded. */
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

/** @brief libcurl header callback: latch the final response's Retry-After value. */
/* cppcheck-suppress constParameterCallback ; libcurl HEADERFUNCTION ABI is char* */
RA8_INTERNAL static size_t on_header(char* buffer, size_t size, size_t nitems, void* user)
{
  const size_t len  = size * nitems;
  hdr_sink_t*  sink = (hdr_sink_t*)user;
  if (sink == nullptr) {
    return len;
  }
  /* A new status line starts a fresh response (redirect chain): drop stale values. */
  if (header_is(buffer, len, "http/")) {
    sink->retry_after[0] = '\0';
  } else if (header_is(buffer, len, "retry-after:")) {
    header_value(buffer, len, sink->retry_after, sizeof(sink->retry_after));
  }
  return len;
}

/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
RA8_PRIV size_t mdl_net_curl_buf_write(char* data, size_t size, size_t nmemb, void* user)
{
  buf_sink_t*  sink  = (buf_sink_t*)user;
  const size_t bytes = size * nmemb;
  if (sink == nullptr) {
    return 0U;
  }
  if ((sink->len + bytes) > sink->cap) {
    sink->overflow = true;
    return 0U; /* Signals libcurl to abort the transfer. */
  }
  memcpy(sink->buf + sink->len, data, bytes);
  sink->len += bytes;
  return nmemb;
}

/** @brief libcurl write callback: append to a FILE*, enforcing the size cap. */
/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
RA8_INTERNAL static size_t on_file_write(char* data, size_t size, size_t nmemb, void* user)
{
  file_sink_t* sink = (file_sink_t*)user;
  if ((sink == nullptr) || (sink->fp == nullptr)) {
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

/** @brief Extract the host of the connection curl is about to request. */
RA8_INTERNAL static bool redirect_host_ok(mdl_curl_ctx_t* net)
{
  if (net->allow_cross_host || (net->origin_host[0] == '\0')) {
    return true;
  }
  char* eff = nullptr;
  if ((curl_easy_getinfo(net->curl, CURLINFO_EFFECTIVE_URL, &eff) != CURLE_OK) ||
      (eff == nullptr)) {
    return true; /* Cannot determine the target host -> do not block on host. */
  }
  char host[k_origin_host_max];
  if (!mdl_url_host(eff, host, sizeof(host))) {
    return true;
  }
  return strcmp(host, net->origin_host) == 0;
}

/** @brief libcurl prereq callback: refuse SSRF and cross-host redirect peers. */
/* The libcurl CURLOPT_PREREQFUNCTION ABI fixes these parameter types as
 * non-const `char*`; conn_local_ip is unused here but cannot be re-qualified. */
RA8_INTERNAL static int on_prereq(void* clientp,
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

/** @brief True if `code` is CURLE_OK (setopt success). */
RA8_INTERNAL static bool ok_code(CURLcode code)
{
  return code == CURLE_OK;
}

/** @brief Apply the security-critical, life-of-handle options (all checked). */
RA8_INTERNAL static bool apply_security_opts(CURL* curl, mdl_curl_ctx_t* net)
{
  return ok_code(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https")) &&
         /* Defaults are correct today; assert them so the guarantee is in code. */
         ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PROXY, "")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)k_curl_max_redirects)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, on_prereq)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_PREREQDATA, net));
}

/** @brief Apply the behavioural, life-of-handle options (all checked). */
RA8_INTERNAL static bool apply_behavior_opts(CURL* curl)
{
  return ok_code(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "")) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)k_connect_timeout_ms)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)k_low_speed_bytes)) &&
         ok_code(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)k_low_speed_secs));
}

/** @brief Apply the per-request options shared by both fetch paths. */
RA8_INTERNAL static bool apply_req(mdl_curl_ctx_t* net, const char* url, const mdl_net_req_t* req)
{
  if (!mdl_url_host(url, net->origin_host, sizeof(net->origin_host))) {
    net->origin_host[0] = '\0';
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

/** @brief Read status + captured headers, fill @p resp, and classify the result. */
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
  }
  return mdl_net_curl_classify(code, overflow, status);
}

/** @brief Vtable method: GET `url` into a caller buffer. */
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

  buf_sink_t sink = {.buf = buf, .cap = cap, .len = 0U, .overflow = false};
  hdr_sink_t hdr  = {};
  if (!apply_req(net, url, req) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, mdl_net_curl_buf_write)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink))) {
    return k_ra8_fail;
  }

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = finish_transfer(net->curl, code, sink.overflow, &hdr, resp);
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

/** @brief Vtable method: GET `url` and stream the body to a file. */
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
    return k_ra8_fail;
  }

  file_sink_t sink = {.fp = fp, .written = 0U, .cap = net->max_bytes, .overflow = false};
  hdr_sink_t  hdr  = {};
  /* MAXFILESIZE_LARGE checks an advertised Content-Length up front; 0 disables
   * it, exactly as cap == 0 disables the callback check below. */
  if (!apply_req(net, url, req) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_HEADERDATA, &hdr)) ||
      !ok_code(
        curl_easy_setopt(net->curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)net->max_bytes)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, on_file_write)) ||
      !ok_code(curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink))) {
    (void)fclose(fp);
    mdl_atomic_abort(tmp_path);
    return k_ra8_fail;
  }

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = finish_transfer(net->curl, code, sink.overflow, &hdr, resp);

  const long fsize = ftell(fp);
  if (fclose(fp) != 0) {
    mdl_atomic_abort(tmp_path);
    return k_ra8_fail;
  }
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(tmp_path); /* Truncated/oversized: destination untouched. */
    return rc;
  }
  if (!mdl_atomic_commit(tmp_path, out_path)) {
    return k_ra8_fail;
  }
  if (out_len != nullptr) {
    *out_len = (fsize < 0) ? 0U : (size_t)fsize;
  }
  return k_ra8_ok;
}

/** @brief Vtable method: release the libcurl handle and this backend state. */
RA8_INTERNAL static void curl_destroy(void* ctx)
{
  mdl_curl_ctx_t* net = (mdl_curl_ctx_t*)ctx;
  if (net == nullptr) {
    return;
  }
  if (net->curl != nullptr) {
    curl_easy_cleanup(net->curl);
  }
  free(net);
}

/** @brief The libcurl backend's immutable method table. */
static const mdl_net_vtable_t s_curl_vtable = {
  .get_buf  = curl_get_buf,
  .get_file = curl_get_file,
  .destroy  = curl_destroy,
};

RA8_DI_SLOT("net_iface")
mdl_net_iface_t* mdl_net_curl_create(const mdl_net_policy_t* policy)
{
  static bool s_global_ready = false;
  if (!s_global_ready) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      return nullptr;
    }
    s_global_ready = true;
  }

  mdl_curl_ctx_t* ctx = (mdl_curl_ctx_t*)calloc(1U, sizeof(*ctx));
  if (ctx == nullptr) {
    return nullptr;
  }
  if (policy != nullptr) {
    ctx->allow_private    = policy->allow_private_hosts;
    ctx->allow_cross_host = policy->allow_cross_host_redirect;
    ctx->max_bytes        = policy->max_response_bytes;
  }
  ctx->curl = curl_easy_init();
  if (ctx->curl == nullptr) {
    free(ctx);
    return nullptr;
  }
  if (!apply_security_opts(ctx->curl, ctx) || !apply_behavior_opts(ctx->curl)) {
    curl_easy_cleanup(ctx->curl);
    free(ctx);
    return nullptr;
  }

  mdl_net_iface_t* net = (mdl_net_iface_t*)calloc(1U, sizeof(*net));
  if (net == nullptr) {
    curl_easy_cleanup(ctx->curl);
    free(ctx);
    return nullptr;
  }
  net->vtable = &s_curl_vtable;
  net->ctx    = ctx;
  return net;
}
