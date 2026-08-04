/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_session.c
 * @brief Implementation of the session identity + robots.txt gating.
 */
#include "mdl_session.h"

#include <stdio.h>
#include <string.h>

#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief robots.txt fetch tunables. */
typedef enum : uint32_t {
  k_robots_timeout_ms = 10000U, /**< Per-robots.txt request budget, ms. */
} mdl_session_net_t;

/** @brief HTTP server-error range treated as "disallow all". */
typedef enum : uint16_t {
  k_http_server_err_min = 500, /**< First 5xx status. */
  k_http_server_err_max = 599, /**< Last 5xx status.  */
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
                      bool             honor_robots)
{
  if (session == nullptr) {
    return;
  }
  session->net          = net;
  session->user_agent   = user_agent;
  session->honor_robots = honor_robots;
  session->cache        = (mdl_robots_cache_t){};
}

/** @brief `"https"` when `url` is https, else `"http"`. */
RA8_INTERNAL static const char* url_scheme(const char* url)
{
  return (strncmp(url, "https://", sizeof("https://") - 1U) == 0) ? "https" : "http";
}

/** @brief robots.txt fetch callback: map an mdl_net GET to a fetch result. */
RA8_INTERNAL static mdl_robots_fetch_result_t
session_fetch(void* ctx, const char* robots_url, char* buf, size_t cap, size_t* out_len)
{
  mdl_session_t*      s    = (mdl_session_t*)ctx;
  const mdl_net_req_t req  = {.user_agent = s->user_agent,
                              .referer    = nullptr,
                              .timeout_ms = k_robots_timeout_ms};
  mdl_net_resp_t      resp = {};
  if (mdl_net_get_buf(s->net, robots_url, &req, buf, cap, out_len, &resp) == k_ra8_ok) {
    return k_mdl_robots_fetch_ok;
  }
  if ((resp.status >= (long)k_http_server_err_min) &&
      (resp.status <= (long)k_http_server_err_max)) {
    return k_mdl_robots_fetch_denied;
  }
  return k_mdl_robots_fetch_absent;
}

bool mdl_session_url_allowed(mdl_session_t* session, const char* url, uint32_t* crawl_delay_ms)
{
  if (crawl_delay_ms != nullptr) {
    *crawl_delay_ms = 0U;
  }
  if ((session == nullptr) || (url == nullptr) || !session->honor_robots) {
    return true;
  }
  char host[k_mdl_robots_host_max];
  if (!mdl_url_host(url, host, sizeof(host))) {
    return true; /* cannot identify the host -> cannot apply its rules */
  }
  char path[k_mdl_robots_path_max];
  if (!mdl_url_path(url, path, sizeof(path))) {
    (void)snprintf(path, sizeof(path), "/");
  }
  const mdl_robots_t* rules = mdl_robots_cache_consult(&session->cache,
                                                       url_scheme(url),
                                                       host,
                                                       s_ua_token,
                                                       session_fetch,
                                                       session,
                                                       session->scratch,
                                                       sizeof(session->scratch));
  if (rules == nullptr) {
    (void)
      fprintf(stderr, "media_dl: robots.txt (5xx) disallows all of %s; skipping %s\n", host, path);
    return false;
  }
  if ((crawl_delay_ms != nullptr) && rules->have_crawl_delay) {
    *crawl_delay_ms = rules->crawl_delay_ms;
  }
  const char* reason = mdl_robots_disallow_reason(rules, path);
  if (reason != nullptr) {
    (void)fprintf(stderr,
                  "media_dl: robots.txt disallows %s on %s (rule \"%s\"); skipping\n",
                  path,
                  host,
                  reason);
    return false;
  }
  return true;
}
