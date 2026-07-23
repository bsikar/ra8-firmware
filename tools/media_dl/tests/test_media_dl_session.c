/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_media_dl_session.c
 * @brief Host unit tests for the session identity + robots.txt gating, driven
 *        through the #310 mdl_net vtable mock (no network).
 *
 * @details
 * These tests close the loop the parser-only tests could not: they prove the
 * whole honest-identity + robots feature works end-to-end through the
 * dependency-inversion seam. A scripted fake ::mdl_net_iface_t backend serves
 * `/robots.txt` and records the transmitted `User-Agent`, so -- with no real
 * network -- the suite asserts that:
 *   - the honest, configurable User-Agent (never a browser-impersonation
 *     string) is the one actually sent on the wire;
 *   - a `Disallow` for our product token refuses a URL before any content
 *     request is made;
 *   - a `Crawl-delay` is extracted and raises the per-host politeness floor,
 *     and never lowers a stricter configured delay; and
 *   - a robots.txt `5xx` is treated as "disallow all" while an absent file
 *     imposes no restriction.
 * Uses the repo's `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdl_net.h"
#include "mdl_politeness.h"
#include "mdl_session.h"
#include "unity_minimal.h"

/** @brief Named constants for the session tests (no bare literals). */
typedef enum : uint32_t {
  k_ua_cap         = 256U,   /**< Built User-Agent buffer bytes.             */
  k_rec_max        = 128U,   /**< Recorded URL / User-Agent bytes per call.  */
  k_http_ok        = 200U,   /**< HTTP 200 OK.                               */
  k_http_not_found = 404U,   /**< HTTP 404 (absent -> allow all).            */
  k_http_srv_err   = 500U,   /**< HTTP 500 (5xx -> disallow all).            */
  k_http_over_5xx  = 600U,   /**< Just above the 5xx band (-> allow all).    */
  k_crawl_ms       = 7000U,  /**< The served Crawl-delay in milliseconds.    */
  k_base_lo_min    = 100U,   /**< Base per-host floor below the crawl delay. */
  k_base_lo_max    = 200U,   /**< Base per-host ceiling below the crawl.     */
  k_base_hi_min    = 9000U,  /**< Base floor already above the crawl delay.  */
  k_base_hi_max    = 10000U, /**< Base ceiling above the crawl delay.        */
  k_pol_seed       = 1U,     /**< Fixed PRNG seed for the politeness clock.  */
  k_nonzero_marker = 5U,     /**< Non-zero seed, proven overwritten to 0.    */
} mdl_session_test_const_t;

/**
 * @struct robo_net_t
 * @brief Fake ::mdl_net_iface_t backend: serves one robots.txt, records the UA.
 * @details The `ctx` behind a fake vtable. Every fetch the session issues is a
 *          `/robots.txt` GET; this records the URL and the transmitted
 *          `User-Agent` so a test can assert exactly what reached the wire.
 * @invariant `calls` counts every dispatched fetch.
 * @since 0.1.0
 */
typedef struct {
  const char* body;                /**< robots.txt body served on ok.         */
  ra8_err_t   rc;                  /**< Result the fetch returns.             */
  long        status;              /**< Status ::mdl_net_last_status reports. */
  size_t      calls;               /**< Fetches dispatched.                   */
  char        last_ua[k_rec_max];  /**< User-Agent of the most recent GET.    */
  char        last_url[k_rec_max]; /**< URL of the most recent GET.           */
} robo_net_t;

/** @brief Fake get_buf: record the UA + URL, serve the scripted robots body. */
static ra8_err_t robo_get_buf(void*                ctx,
                              const char*          url,
                              const mdl_net_req_t* req,
                              char*                buf,
                              size_t               cap,
                              size_t*              out_len)
{
  robo_net_t* f = (robo_net_t*)ctx;
  f->calls += 1U;
  (void)snprintf(f->last_url, sizeof(f->last_url), "%s", url);
  (void)snprintf(f->last_ua,
                 sizeof(f->last_ua),
                 "%s",
                 (req->user_agent != nullptr) ? req->user_agent : "");
  size_t got = 0U;
  if ((f->rc == k_ra8_ok) && (f->body != nullptr)) {
    const int w = snprintf(buf, cap, "%s", f->body);
    got         = (w < 0) ? 0U : (size_t)w;
    if (got >= cap) {
      got = cap - 1U;
    }
  }
  if (out_len != nullptr) {
    *out_len = got;
  }
  return f->rc;
}

/** @brief Fake get_file: the session never streams a body, so this is unused. */
static ra8_err_t robo_get_file(void*                ctx,
                               const char*          url,
                               const mdl_net_req_t* req,
                               const char*          out_path,
                               size_t*              out_len)
{
  (void)ctx;
  (void)url;
  (void)req;
  (void)out_path;
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  return k_ra8_ok;
}

/** @brief Fake last_status: the status of the scripted robots reply. */
static long robo_last_status(void* ctx)
{
  return ((const robo_net_t*)ctx)->status;
}

/** @brief Fake destroy: the handle is stack-owned, nothing to release. */
static void robo_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The fake backend's method table. */
static const mdl_net_vtable_t s_robo_vtable = {
  .get_buf     = robo_get_buf,
  .get_file    = robo_get_file,
  .last_status = robo_last_status,
  .destroy     = robo_destroy,
};

/** @brief Build a stack ::mdl_net_iface_t around a fake context. */
static mdl_net_iface_t robo_iface(robo_net_t* f)
{
  return (mdl_net_iface_t){.vtable = &s_robo_vtable, .ctx = f};
}

/** @brief Session under test; too large (embeds a 64 KiB scratch) for the stack. */
static mdl_session_t s_sess;

/** @brief Reset the shared session over `net` with UA `ua` and gating `honor`. */
static void sess_reset(mdl_net_iface_t* net, const char* ua, bool honor)
{
  memset(&s_sess, 0, sizeof(s_sess));
  mdl_session_init(&s_sess, net, ua, honor);
}

/**
 * @struct rec_clock_t
 * @brief Recording fake clock: captures the requested sleep without sleeping.
 * @since 0.1.0
 */
typedef struct {
  uint32_t last_ms; /**< Last sleep requested.  */
  uint32_t calls;   /**< Number of sleep calls. */
} rec_clock_t;

/** @brief Injected sleeper: record the request and return immediately. */
static void rec_clock_sleep(void* ctx, uint32_t ms)
{
  rec_clock_t* c = (rec_clock_t*)ctx;
  c->last_ms     = ms;
  c->calls += 1U;
}

/** @brief max(a, b), mirroring main.c's per-host politeness-floor raise. */
static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/**
 * @test test_session_ua_build_honest
 *
 * @par MC/DC:
 * Decision A: build_ua's contact clause `(contact != NULL) && (contact[0] != 0)`
 * (2 conditions, N+1 = 3):
 * - V1: contact="ops@x" -> both true       -> clause present, returns true
 * - V2: contact=NULL     -> c1 false        -> no clause, returns false (varies c1)
 * - V3: contact=""       -> c1 true,c2 false -> no clause, returns false (varies c2)
 * Decision B: build_ua's guard `(out == NULL) || (cap == 0)` (2 conditions):
 * - out=NULL -> true (returns false); cap=0 -> true (returns false); both ok -> false.
 */
static void test_session_ua_build_honest(void)
{
  TEST_BEGIN("session ua build honest");
  char ua[k_ua_cap];
  /* A/V1 + B control: a real contact yields the full honest identity. */
  TEST_ASSERT(mdl_session_build_ua("ops@example.org", ua, sizeof(ua)));
  TEST_ASSERT(strstr(ua, "media_dl/") == ua);
  TEST_ASSERT(strstr(ua, "github.com/bsikar/ra8-firmware") != nullptr);
  TEST_ASSERT(strstr(ua, "ops@example.org") != nullptr);
  /* No browser-impersonation token survives anywhere in the string. */
  TEST_ASSERT(strstr(ua, "Mozilla") == nullptr);
  TEST_ASSERT(strstr(ua, "Chrome") == nullptr);
  TEST_ASSERT(strstr(ua, "AppleWebKit") == nullptr);
  TEST_ASSERT(strstr(ua, "Safari") == nullptr);
  /* A/V2: NULL contact -> no clause, false; still honest, still no impersonation. */
  TEST_ASSERT(!mdl_session_build_ua(nullptr, ua, sizeof(ua)));
  TEST_ASSERT(strstr(ua, "media_dl/") == ua);
  TEST_ASSERT(strstr(ua, "Mozilla") == nullptr);
  /* A/V3: empty contact -> treated as no contact. */
  TEST_ASSERT(!mdl_session_build_ua("", ua, sizeof(ua)));
  /* B: NULL out and zero cap are refused. */
  TEST_ASSERT(!mdl_session_build_ua("ops@example.org", nullptr, sizeof(ua)));
  TEST_ASSERT(!mdl_session_build_ua("ops@example.org", ua, 0U));
  TEST_END("session ua build honest");
}

/**
 * @test test_session_ua_sent_on_wire
 *
 * @par MC/DC:
 * (No compound decision under test here; it proves the honest UA the session
 * holds is exactly the `User-Agent` transmitted on the robots.txt GET through
 * the mdl_net seam, and that the request targeted `/robots.txt` on the host.)
 */
static void test_session_ua_sent_on_wire(void)
{
  TEST_BEGIN("session ua sent on wire");
  char ua[k_ua_cap];
  (void)mdl_session_build_ua("ops@example.org", ua, sizeof(ua));
  robo_net_t f = {.body = "User-agent: *\nAllow: /\n", .rc = k_ra8_ok, .status = (long)k_http_ok};
  mdl_net_iface_t net = robo_iface(&f);
  sess_reset(&net, ua, true);
  uint32_t crawl = 0U;
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://h1/series/1", &crawl));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.calls);
  TEST_ASSERT(strcmp(f.last_url, "https://h1/robots.txt") == 0);
  TEST_ASSERT(strcmp(f.last_ua, ua) == 0); /* the honest UA reached the wire */
  TEST_END("session ua sent on wire");
}

/**
 * @test test_session_disallow_blocks
 *
 * @par MC/DC:
 * (Drives the url-allowed guard's all-false control -- session/url set, robots
 * honoured -- through to a refusal, and asserts the block happens before any
 * content fetch; the single-condition-true guard vectors are in
 * test_session_gating_off_and_null.)
 */
static void test_session_disallow_blocks(void)
{
  TEST_BEGIN("session disallow blocks");
  robo_net_t      f   = {.body   = "User-agent: *\nDisallow: /premium\n",
                         .rc     = k_ra8_ok,
                         .status = (long)k_http_ok};
  mdl_net_iface_t net = robo_iface(&f);
  sess_reset(&net, "media_dl/0.1.0", true);
  uint32_t crawl = 0U;
  /* A disallowed path is refused; the only network call was robots.txt. */
  TEST_ASSERT(!mdl_session_url_allowed(&s_sess, "https://h/premium/ch1", &crawl));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.calls);
  /* An allowed path on the same host is permitted from cache (no re-fetch). */
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://h/free/ch1", &crawl));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.calls);
  TEST_END("session disallow blocks");
}

/**
 * @test test_session_gating_off_and_null
 *
 * @par MC/DC:
 * Decision: mdl_session_url_allowed's guard
 * `(session == NULL) || (url == NULL) || !session->honor_robots` (3 conditions,
 * N+1 = 4):
 * - V1 control: session set, url set, honor_robots true -> guard false -> the
 *   robots fetch runs and the allow-all body permits the URL (true, one fetch).
 * - V2: honor_robots false -> guard true -> allowed with NO fetch (varies c3).
 * - V3: url = NULL          -> guard true -> allowed (varies c2).
 * - V4: session = NULL      -> guard true -> allowed (varies c1).
 * V1 pairs with each of V2..V4 to isolate one condition.
 */
static void test_session_gating_off_and_null(void)
{
  TEST_BEGIN("session gating off / null");
  robo_net_t f = {.body = "User-agent: *\nAllow: /\n", .rc = k_ra8_ok, .status = (long)k_http_ok};
  mdl_net_iface_t net   = robo_iface(&f);
  uint32_t        crawl = 0U;
  /* V1 control: gating on, valid args -> fetch + allow. */
  sess_reset(&net, "media_dl/0.1.0", true);
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://h/a", &crawl));
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.calls);
  /* V2: gating off -> allowed, and no robots fetch is issued. */
  f.calls = 0U;
  sess_reset(&net, "media_dl/0.1.0", false);
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://h/a", &crawl));
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)f.calls);
  /* V3: NULL url -> allowed. V4: NULL session -> allowed. */
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, nullptr, &crawl));
  TEST_ASSERT(mdl_session_url_allowed(nullptr, "https://h/a", &crawl));
  TEST_END("session gating off / null");
}

/**
 * @test test_session_crawl_delay_respected
 *
 * @par MC/DC:
 * Decision: mdl_session_url_allowed's crawl report
 * `(crawl_delay_ms != NULL) && rules->have_crawl_delay` (2 conditions, N+1 = 3):
 * - V1: out non-NULL, robots has Crawl-delay -> *out = the delay (both true)
 * - V2: out NULL,     robots has Crawl-delay -> nothing written (c1 false)
 * - V3: out non-NULL, robots has none         -> *out stays 0 (c2 false)
 * The extracted delay then RAISES a lower politeness floor and never LOWERS a
 * stricter one, proven through the injectable clock.
 */
static void test_session_crawl_delay_respected(void)
{
  TEST_BEGIN("session crawl-delay respected");
  robo_net_t      f   = {.body   = "User-agent: *\nCrawl-delay: 7\n",
                         .rc     = k_ra8_ok,
                         .status = (long)k_http_ok};
  mdl_net_iface_t net = robo_iface(&f);
  sess_reset(&net, "media_dl/0.1.0", true);
  /* V1: the host's Crawl-delay is reported in milliseconds. */
  uint32_t crawl = 0U;
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://slow/p", &crawl));
  TEST_ASSERT_EQ((uint32_t)k_crawl_ms, crawl);
  /* V2: a NULL out pointer is accepted (no crash, nothing written). */
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://slow/q", nullptr));
  /* V3: a host with no Crawl-delay reports zero. */
  robo_net_t      f2   = {.body   = "User-agent: *\nDisallow: /no\n",
                          .rc     = k_ra8_ok,
                          .status = (long)k_http_ok};
  mdl_net_iface_t net2 = robo_iface(&f2);
  sess_reset(&net2, "media_dl/0.1.0", true);
  uint32_t none = k_nonzero_marker;
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://fast/p", &none));
  TEST_ASSERT_EQ((uint32_t)0, none);
  /* Respected: the crawl floor RAISES a lower base delay ... */
  rec_clock_t      clk = {};
  mdl_politeness_t pol;
  mdl_politeness_init_clock(&pol, k_pol_seed, rec_clock_sleep, &clk);
  (void)mdl_politeness_wait(&pol,
                            max_u32(k_base_lo_min, k_crawl_ms),
                            max_u32(k_base_lo_max, k_crawl_ms));
  TEST_ASSERT_EQ((uint32_t)k_crawl_ms, clk.last_ms);
  /* ... and never LOWERS a stricter configured base delay. */
  (void)mdl_politeness_wait(&pol,
                            max_u32(k_base_hi_min, k_crawl_ms),
                            max_u32(k_base_hi_max, k_crawl_ms));
  TEST_ASSERT(clk.last_ms >= (uint32_t)k_base_hi_min);
  TEST_END("session crawl-delay respected");
}

/**
 * @test test_session_robots_5xx_denies
 *
 * @par MC/DC:
 * Decision: session_fetch's 5xx classifier `(status >= 500) && (status <= 599)`
 * (2 conditions, N+1 = 3), reached when the robots.txt GET fails:
 * - V1: status 500 -> both true        -> "disallow all": every path refused.
 * - V2: status 404 -> c1 false          -> "absent": no restrictions (allow all).
 * - V3: status 600 -> c1 true, c2 false -> "absent": allow all.
 */
static void test_session_robots_5xx_denies(void)
{
  TEST_BEGIN("session robots 5xx denies");
  uint32_t crawl = 0U;
  /* V1: a 5xx robots.txt disallows the whole host. */
  robo_net_t      f5 = {.body = nullptr, .rc = k_ra8_fail, .status = (long)k_http_srv_err};
  mdl_net_iface_t n5 = robo_iface(&f5);
  sess_reset(&n5, "media_dl/0.1.0", true);
  TEST_ASSERT(!mdl_session_url_allowed(&s_sess, "https://a/x", &crawl));
  /* V2: a 404 robots.txt is "absent" -> allow all. */
  robo_net_t      f4 = {.body = nullptr, .rc = k_ra8_fail, .status = (long)k_http_not_found};
  mdl_net_iface_t n4 = robo_iface(&f4);
  sess_reset(&n4, "media_dl/0.1.0", true);
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://b/x", &crawl));
  /* V3: a status just above the 5xx band is also treated as absent. */
  robo_net_t      f6 = {.body = nullptr, .rc = k_ra8_fail, .status = (long)k_http_over_5xx};
  mdl_net_iface_t n6 = robo_iface(&f6);
  sess_reset(&n6, "media_dl/0.1.0", true);
  TEST_ASSERT(mdl_session_url_allowed(&s_sess, "https://c/x", &crawl));
  TEST_END("session robots 5xx denies");
}

/**
 * @brief Run every session identity + robots gating unit test in sequence.
 * @return 0 when all tests passed; a failing assertion aborts via the harness.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_session_ua_build_honest();
  test_session_ua_sent_on_wire();
  test_session_disallow_blocks();
  test_session_gating_off_and_null();
  test_session_crawl_delay_respected();
  test_session_robots_5xx_denies();
  (void)fprintf(stderr, "[OK  ] test_media_dl_session.c\n");
  return 0;
}
