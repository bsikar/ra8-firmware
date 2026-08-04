/**
 * @file test_media_dl_net.c
 * @brief Host unit tests for the mdl_net vtable seam and the politeness clock.
 *
 * @details
 * The network-layer counterpart to `test_media_dl.c`, split out so neither file
 * exceeds the size cap. A scripted fake ::mdl_net_iface_t backend drives the
 * dispatchers with no network -- proving canned responses can be injected and
 * the request sequence observed -- and covers the argument-guard MC/DC vectors,
 * the libcurl backend's pure transfer classifier and bounded write callback, and
 * the seeded jitter + injectable-clock politeness behaviour. Uses the repo's
 * `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <curl/curl.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mdl_atomic.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "mdl_net_curl_internal.h"
#include "mdl_politeness.h"
#include "unity_minimal.h"

/* ---- #310: mdl_net vtable seam -- scripted fake backend + net tests ------ */

/** @brief Named constants for the network-seam tests (no bare literals). */
typedef enum : uint16_t {
  k_fake_max_calls = 8,   /**< Recorded request slots in the fake backend. */
  k_fake_url_max   = 256, /**< Recorded URL bytes per call.                */
  k_net_buf        = 256, /**< get_buf destination buffer bytes.           */
  k_net_dst        = 128, /**< buf-write test destination buffer bytes.    */
  k_http_ok        = 200, /**< HTTP 200 OK.                                */
  k_http_moved     = 301, /**< HTTP 301 (redirect, still < 400).           */
  k_http_not_found = 404, /**< HTTP 404 (not found -> skip).               */
  k_http_too_many  = 429, /**< HTTP 429 (throttle -> back off).            */
  k_http_srv_err   = 500, /**< HTTP 500 (server error).                    */
  k_http_unavail   = 503, /**< HTTP 503 (throttle -> back off).            */
} mdl_net_test_const_t;

/**
 * @struct fake_reply_t
 * @brief One scripted reply the fake network backend hands back.
 * @details A test wires a sequence of these; each call consumes the next (the
 *          last repeats once exhausted), so a whole fetch conversation is
 *          canned with no network.
 * @since 0.1.0
 */
typedef struct {
  ra8_err_t   rc;          /**< Result the fetch call returns.                    */
  long        status;      /**< HTTP status reported through `resp`.              */
  const char* body;        /**< Body copied into the buffer on ok, or NULL.       */
  const char* retry_after; /**< Raw Retry-After surfaced through `resp`, or NULL. */
} fake_reply_t;

/**
 * @struct fake_net_t
 * @brief Fake ::mdl_net_iface_t backend: scripts replies, records requests.
 * @details The `ctx` behind a fake vtable. Proves the seam lets a test inject
 *          canned responses and assert exactly what was sent.
 * @invariant `call` counts every dispatched fetch.
 * @since 0.1.0
 */
typedef struct {
  const fake_reply_t* replies;                                /**< Scripted replies (>= 1). */
  size_t              n;                                      /**< Number of replies.       */
  size_t              call;                                   /**< Fetches dispatched.      */
  char                urls[k_fake_max_calls][k_fake_url_max]; /**< Recorded URLs.           */
  const char*         referers[k_fake_max_calls];             /**< Recorded referers.       */
} fake_net_t;

/** @brief Record the URL + referer of the current request. */
static void fake_record(fake_net_t* f, const char* url, const mdl_net_req_t* req)
{
  if (f->call < (size_t)k_fake_max_calls) {
    (void)snprintf(f->urls[f->call], sizeof(f->urls[f->call]), "%s", url);
    f->referers[f->call] = (req != nullptr) ? req->referer : nullptr;
  }
}

/** @brief The reply for the current call (the last one repeats once exhausted). */
static const fake_reply_t* fake_next(const fake_net_t* f)
{
  const size_t i = (f->call < f->n) ? f->call : (f->n - 1U);
  return &f->replies[i];
}

/** @brief Surface a scripted reply's status + Retry-After through `resp`. */
static void fake_fill_resp(const fake_reply_t* r, mdl_net_resp_t* resp)
{
  if (resp != nullptr) {
    resp->status = r->status;
    (void)snprintf(resp->retry_after,
                   sizeof(resp->retry_after),
                   "%s",
                   (r->retry_after != nullptr) ? r->retry_after : "");
  }
}

/** @brief Fake get_buf: record, hand back the scripted body/result. */
static ra8_err_t fake_get_buf(void*                ctx,
                              const char*          url,
                              const mdl_net_req_t* req,
                              char*                buf,
                              size_t               cap,
                              size_t*              out_len,
                              mdl_net_resp_t*      resp)
{
  fake_net_t*         f = (fake_net_t*)ctx;
  const fake_reply_t* r = fake_next(f);
  fake_record(f, url, req);
  fake_fill_resp(r, resp);
  f->call += 1U;
  size_t got = 0U;
  if ((r->rc == k_ra8_ok) && (r->body != nullptr)) {
    const int w = snprintf(buf, cap, "%s", r->body);
    got         = (w < 0) ? 0U : (size_t)w;
    if (got >= cap) {
      got = cap - 1U;
    }
  }
  if (out_len != nullptr) {
    *out_len = got;
  }
  return r->rc;
}

/** @brief Fake get_file: record, hand back the scripted result (writes nothing). */
static ra8_err_t fake_get_file(void*                ctx,
                               const char*          url,
                               const mdl_net_req_t* req,
                               const char*          out_path,
                               size_t*              out_len,
                               mdl_net_resp_t*      resp)
{
  (void)out_path;
  fake_net_t*         f = (fake_net_t*)ctx;
  const fake_reply_t* r = fake_next(f);
  fake_record(f, url, req);
  fake_fill_resp(r, resp);
  f->call += 1U;
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  return r->rc;
}

/** @brief Fake destroy: the handle is stack-owned, so nothing to release. */
static void fake_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The fake backend's method table. */
static const mdl_net_vtable_t s_fake_vtable = {
  .get_buf  = fake_get_buf,
  .get_file = fake_get_file,
  .destroy  = fake_destroy,
};

/** @brief Build a stack ::mdl_net_iface_t around a fake context. */
static mdl_net_iface_t fake_iface(fake_net_t* f)
{
  return (mdl_net_iface_t){.vtable = &s_fake_vtable, .ctx = f};
}

/**
 * @test test_net_dispatch_guard
 *
 * @par MC/DC:
 * Decision: mdl_net_get_buf's argument guard
 * `(net->vtable==NULL) || (url==NULL) || (req==NULL) || (buf==NULL) || (cap==0)`
 * (5 conditions, OR; N+1 = 6 vectors). A separate `net==NULL` guard precedes it.
 * - V1: vtable set, url,req,buf ok, cap>0 -> false (control: fetch dispatched, ok)
 * - V2: vtable=NULL, rest ok             -> true  (varies vtable)
 * - V3: url=NULL, rest ok                -> true  (varies url)
 * - V4: req=NULL, rest ok                -> true  (varies req)
 * - V5: buf=NULL, rest ok                -> true  (varies buf)
 * - V6: cap=0, rest ok                   -> true  (varies cap)
 * V1 pairs with each of V2..V6 to show that condition independently drives the
 * outcome. The preceding `net==NULL` guard is exercised by its own vector.
 */
static void test_net_dispatch_guard(void)
{
  TEST_BEGIN("net dispatch guard");
  const fake_reply_t  ok   = {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = "OK"};
  fake_net_t          f    = {.replies = &ok, .n = 1U};
  mdl_net_iface_t     good = fake_iface(&f);
  mdl_net_iface_t     badv = {.vtable = nullptr, .ctx = nullptr};
  const mdl_net_req_t req  = {.user_agent = "ua", .referer = nullptr, .timeout_ms = 1000U};
  char                buf[k_net_buf];
  size_t              got = 0U;
  /* V1 control: all conditions false -> the fetch is dispatched to the fake. */
  TEST_ASSERT(mdl_net_get_buf(&good, "http://h/a", &req, buf, sizeof(buf), &got, nullptr) ==
              k_ra8_ok);
  TEST_ASSERT(strcmp(buf, "OK") == 0);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)got);
  /* V2 vtable NULL */
  TEST_ASSERT(mdl_net_get_buf(&badv, "http://h/a", &req, buf, sizeof(buf), &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* V3 url NULL */
  TEST_ASSERT(mdl_net_get_buf(&good, nullptr, &req, buf, sizeof(buf), &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* V4 req NULL */
  TEST_ASSERT(mdl_net_get_buf(&good, "http://h/a", nullptr, buf, sizeof(buf), &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* V5 buf NULL */
  TEST_ASSERT(mdl_net_get_buf(&good, "http://h/a", &req, nullptr, sizeof(buf), &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* V6 cap 0 */
  TEST_ASSERT(mdl_net_get_buf(&good, "http://h/a", &req, buf, 0U, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* Separate net==NULL guard */
  TEST_ASSERT(mdl_net_get_buf(nullptr, "http://h/a", &req, buf, sizeof(buf), &got, nullptr) ==
              k_ra8_err_invalid_arg);
  /* Only the control vector reached the backend. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.call);
  TEST_END("net dispatch guard");
}

/**
 * @test test_net_get_file_guard
 *
 * @par MC/DC:
 * Decision: mdl_net_get_file's guard
 * `(net->vtable==NULL) || (url==NULL) || (req==NULL) || (out_path==NULL)`
 * (4 conditions, OR; N+1 = 5 vectors), after a separate `net==NULL` guard.
 * - V1: all ok           -> false (control: dispatched, ok)
 * - V2: vtable=NULL       -> true  (varies vtable)
 * - V3: url=NULL          -> true  (varies url)
 * - V4: req=NULL          -> true  (varies req)
 * - V5: out_path=NULL     -> true  (varies out_path)
 * V1 pairs with each to isolate one condition; `net==NULL` has its own vector.
 */
static void test_net_get_file_guard(void)
{
  TEST_BEGIN("net get_file guard");
  const fake_reply_t  ok   = {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = nullptr};
  fake_net_t          f    = {.replies = &ok, .n = 1U};
  mdl_net_iface_t     good = fake_iface(&f);
  mdl_net_iface_t     badv = {.vtable = nullptr, .ctx = nullptr};
  const mdl_net_req_t req  = {.user_agent = "ua", .referer = nullptr, .timeout_ms = 1000U};
  size_t              got  = 0U;
  TEST_ASSERT(mdl_net_get_file(&good, "http://h/i.jpg", &req, "/dev/null", &got, nullptr) ==
              k_ra8_ok);
  TEST_ASSERT(mdl_net_get_file(&badv, "http://h/i.jpg", &req, "/dev/null", &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_file(&good, nullptr, &req, "/dev/null", &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_file(&good, "http://h/i.jpg", nullptr, "/dev/null", &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_file(&good, "http://h/i.jpg", &req, nullptr, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_file(nullptr, "http://h/i.jpg", &req, "/dev/null", &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.call);
  TEST_END("net get_file guard");
}

/**
 * @test test_net_fake_scripts_and_records
 *
 * @par MC/DC:
 * (no compound decisions in this test; it drives the vtable dispatchers against
 * a scripted fake and asserts the injected bodies/statuses and the recorded
 * request sequence -- proof the seam admits canned responses and observes what
 * was sent, with no network.)
 */
static void test_net_fake_scripts_and_records(void)
{
  TEST_BEGIN("net fake scripts + records");
  const fake_reply_t seq[] = {
    {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = "PAGE"},
    {.rc = k_ra8_err_busy, .status = (long)k_http_unavail, .body = nullptr, .retry_after = "42"},
    {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = nullptr},
  };
  fake_net_t          f   = {.replies = seq, .n = sizeof(seq) / sizeof(seq[0])};
  mdl_net_iface_t     net = fake_iface(&f);
  char                buf[k_net_buf];
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  const mdl_net_req_t r0   = {.user_agent = "ua",
                              .referer    = "http://site/series",
                              .timeout_ms = 1000U};
  const mdl_net_req_t r1 = {.user_agent = "ua", .referer = "http://site/ch1", .timeout_ms = 1000U};

  /* 1st: a scripted OK page body is delivered, with its status through resp. */
  TEST_ASSERT(mdl_net_get_buf(&net, "http://site/ch1", &r0, buf, sizeof(buf), &got, &resp) ==
              k_ra8_ok);
  TEST_ASSERT(strcmp(buf, "PAGE") == 0);
  TEST_ASSERT_EQ((int64_t)k_http_ok, resp.status);
  TEST_ASSERT(resp.retry_after[0] == '\0'); /* no header on the OK reply */
  /* 2nd: a scripted throttle -- its 503 + Retry-After surface through resp. */
  TEST_ASSERT(mdl_net_get_file(&net, "http://cdn/img1.jpg", &r1, "/dev/null", &got, &resp) ==
              k_ra8_err_busy);
  TEST_ASSERT_EQ((int64_t)k_http_unavail, resp.status);
  TEST_ASSERT(strcmp(resp.retry_after, "42") == 0);
  /* 3rd: a scripted OK file; a NULL resp is accepted (no plumbing required). */
  TEST_ASSERT(mdl_net_get_file(&net, "http://cdn/img2.jpg", &r1, "/dev/null", &got, nullptr) ==
              k_ra8_ok);

  /* The recorded request sequence is exactly what was dispatched. */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)f.call);
  TEST_ASSERT(strcmp(f.urls[0], "http://site/ch1") == 0);
  TEST_ASSERT(strcmp(f.urls[1], "http://cdn/img1.jpg") == 0);
  TEST_ASSERT(strcmp(f.urls[2], "http://cdn/img2.jpg") == 0);
  TEST_ASSERT(strcmp(f.referers[0], "http://site/series") == 0);
  TEST_ASSERT(strcmp(f.referers[1], "http://site/ch1") == 0); /* image referer = chapter */
  TEST_END("net fake scripts + records");
}

/**
 * @test test_net_classify
 *
 * @par MC/DC:
 * mdl_net_curl_classify's HTTP branches are reached only when `code ==
 * CURLE_OK`. The throttle test `status == 429 || status == 503` is a
 * two-condition OR: vector 429 varies the first condition true (503 false),
 * vector 503 varies the second true (429 false), and a non-throttle status
 * (200/404/500) holds both false -- minimal N+1 for the OR. The remaining
 * relational branches (`>= 500`, `>= 400`, else) plus the overflow, timeout and
 * transport-error precedence guards each get their own vector, so every class
 * the classifier distinguishes is exercised.
 */
static void test_net_classify(void)
{
  TEST_BEGIN("net classify");
  /* overflow outranks everything, even an OK code + 200 status. */
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, true, (long)k_http_ok) == k_ra8_err_no_mem);
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OPERATION_TIMEDOUT, false, 0) == k_ra8_err_timeout);
  TEST_ASSERT(mdl_net_curl_classify(CURLE_COULDNT_CONNECT, false, 0) == k_ra8_fail);
  /* throttle OR: 429 true/503 false, then 503 true/429 false -> busy. */
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_too_many) == k_ra8_err_busy);
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_unavail) == k_ra8_err_busy);
  /* 5xx (both throttle conditions false) -> server error. */
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_srv_err) == k_ra8_fail);
  /* 404 / other 4xx (both false) -> not found. */
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_not_found) ==
              k_ra8_err_not_found);
  /* < 400 -> ok. */
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_ok) == k_ra8_ok);
  TEST_ASSERT(mdl_net_curl_classify(CURLE_OK, false, (long)k_http_moved) == k_ra8_ok);
  TEST_END("net classify");
}

/**
 * @test test_net_buf_write_overflow
 *
 * @par MC/DC:
 * mdl_net_curl_buf_write has two single-condition guards (`user==NULL` and
 * `(len+bytes) > cap`), not a compound decision. Vectors: a within-cap append,
 * an at-cap append, an over-cap write that returns 0 and latches overflow, and
 * a NULL sink that returns 0.
 */
static void test_net_buf_write_overflow(void)
{
  TEST_BEGIN("net buf write overflow");
  char       dst[k_net_dst];
  char       chunk[] = "abcde";
  buf_sink_t sink    = {.buf = dst, .cap = 4U, .len = 0U, .overflow = false};
  /* within cap: 3 bytes into cap 4. */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)mdl_net_curl_buf_write(chunk, 1U, 3U, &sink));
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)sink.len);
  TEST_ASSERT(!sink.overflow);
  /* one more byte exactly fills cap. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)mdl_net_curl_buf_write(chunk + 3, 1U, 1U, &sink));
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sink.len);
  /* the next byte would exceed cap -> abort (0) and latch overflow. */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)mdl_net_curl_buf_write(chunk + 4, 1U, 1U, &sink));
  TEST_ASSERT(sink.overflow);
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sink.len); /* unchanged after overflow */
  /* NULL sink -> 0. */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)mdl_net_curl_buf_write(chunk, 1U, 1U, nullptr));
  TEST_END("net buf write overflow");
}

/* ---- #310: politeness jitter + injectable clock ------------------------- */

/** @brief Named constants for the politeness (jitter/clock) tests. */
typedef enum : uint32_t {
  k_pol_seed      = 1,   /**< Fixed PRNG seed.                          */
  k_pol_span_max  = 999, /**< Upper bound giving span 1000.             */
  k_pol_d0        = 761, /**< 1st pinned delay for seed 1 over [0,999]. */
  k_pol_d1        = 505, /**< 2nd pinned delay.                         */
  k_pol_d2        = 457, /**< 3rd pinned delay.                         */
  k_pol_min       = 100, /**< Lower bound for the clamp/bounds tests.   */
  k_pol_below_min = 50,  /**< max < min, to exercise the clamp.         */
  k_pol_max       = 200, /**< Upper bound for the clamp/bounds tests.   */
  k_pol_iters     = 64,  /**< Draws in the bounds test.                 */
} mdl_pol_test_const_t;

/**
 * @struct fake_clock_t
 * @brief Recording fake clock: captures requested sleeps without sleeping.
 * @since 0.1.0
 */
typedef struct {
  uint32_t last_ms;  /**< Last sleep requested.  */
  uint32_t total_ms; /**< Sum of requested ms.   */
  uint32_t calls;    /**< Number of sleep calls. */
} fake_clock_t;

/** @brief Injected sleeper: record the request and return immediately. */
static void fake_clock_sleep(void* ctx, uint32_t ms)
{
  fake_clock_t* c = (fake_clock_t*)ctx;
  c->last_ms      = ms;
  c->total_ms += ms;
  c->calls += 1U;
}

/**
 * @test test_politeness_determinism
 *
 * @par MC/DC:
 * (no compound decisions in this test; it pins the seeded xorshift64 delay
 * sequence and shows two states with the same seed agree, through a fake clock
 * so no real time elapses.)
 */
static void test_politeness_determinism(void)
{
  TEST_BEGIN("politeness determinism");
  fake_clock_t     clk = {};
  mdl_politeness_t a;
  mdl_politeness_init_clock(&a, k_pol_seed, fake_clock_sleep, &clk);
  /* Pinned sequence for seed=1 over [0,999]: 761, 505, 457 (xorshift64 13/7/17). */
  TEST_ASSERT_EQ((uint32_t)k_pol_d0, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d1, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d2, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  /* The injected clock was asked to sleep the returned durations; no real wait. */
  TEST_ASSERT_EQ((uint32_t)k_pol_d2, clk.last_ms);
  TEST_ASSERT_EQ((uint32_t)3, clk.calls);
  /* Determinism: a second state with the same seed reproduces the sequence. */
  fake_clock_t     clk2 = {};
  mdl_politeness_t b;
  mdl_politeness_init_clock(&b, k_pol_seed, fake_clock_sleep, &clk2);
  TEST_ASSERT_EQ((uint32_t)k_pol_d0, mdl_politeness_wait(&b, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d1, mdl_politeness_wait(&b, 0U, k_pol_span_max));
  TEST_END("politeness determinism");
}

/**
 * @test test_politeness_clamp
 *
 * @par MC/DC:
 * Decision: `max_ms < min_ms` in mdl_politeness_wait (single condition, N+1 = 2).
 * - V1: max_ms < min_ms (50 < 100) -> clamp; span 1 -> returns exactly min_ms.
 * - V2: max_ms >= min_ms (200 >= 100) -> no clamp -> a value in [100, 200].
 */
static void test_politeness_clamp(void)
{
  TEST_BEGIN("politeness clamp");
  fake_clock_t     clk = {};
  mdl_politeness_t p;
  mdl_politeness_init_clock(&p, k_pol_seed, fake_clock_sleep, &clk);
  const uint32_t d = mdl_politeness_wait(&p, k_pol_min, k_pol_below_min); /* max < min      */
  TEST_ASSERT_EQ((uint32_t)k_pol_min, d);                                 /* clamped to min */
  TEST_ASSERT_EQ((uint32_t)k_pol_min, clk.last_ms);
  const uint32_t d2 = mdl_politeness_wait(&p, k_pol_min, k_pol_max);
  TEST_ASSERT(d2 >= (uint32_t)k_pol_min);
  TEST_ASSERT(d2 <= (uint32_t)k_pol_max);
  TEST_END("politeness clamp");
}

/**
 * @test test_politeness_bounds
 *
 * @par MC/DC:
 * (no compound decisions in this test; it checks every drawn delay lies within
 * [min, max] and that the injected clock was asked for exactly that value.)
 */
static void test_politeness_bounds(void)
{
  TEST_BEGIN("politeness bounds");
  fake_clock_t     clk = {};
  mdl_politeness_t p;
  mdl_politeness_init_clock(&p, k_pol_seed, fake_clock_sleep, &clk);
  for (uint32_t i = 0U; i < (uint32_t)k_pol_iters; ++i) {
    const uint32_t d = mdl_politeness_wait(&p, k_pol_min, k_pol_max);
    TEST_ASSERT(d >= (uint32_t)k_pol_min);
    TEST_ASSERT(d <= (uint32_t)k_pol_max);
    TEST_ASSERT_EQ(d, clk.last_ms); /* asked the clock for exactly d */
  }
  TEST_ASSERT_EQ((uint32_t)k_pol_iters, clk.calls);
  TEST_END("politeness bounds");
}

/**
 * @test test_politeness_null
 *
 * @par MC/DC:
 * Decision: `p == NULL` in mdl_politeness_wait (single condition, N+1 = 2).
 * - V1: p == NULL -> returns 0, no sleep.
 * - V2: p != NULL -> draws and sleeps (covered by the tests above).
 * Also asserts the NULL-`p` no-op init paths do not crash.
 */
static void test_politeness_null(void)
{
  TEST_BEGIN("politeness null");
  TEST_ASSERT_EQ((uint32_t)0, mdl_politeness_wait(nullptr, 10U, 20U));
  mdl_politeness_init(nullptr, k_pol_seed);                         /* no crash */
  mdl_politeness_init_clock(nullptr, k_pol_seed, nullptr, nullptr); /* no crash */
  TEST_END("politeness null");
}

/* ---- a failed re-fetch must not destroy the file already on disk --------- */

/** @brief Fixture sizes for the atomic-write regression tests. */
typedef enum : uint16_t {
  k_atom_path_max   = 256,  /**< Fixture path buffer bytes.                  */
  k_atom_body_max   = 128,  /**< Fixture file-content buffer bytes.          */
  k_atom_timeout_ms = 5000, /**< Budget for the fetch that is meant to fail. */
} mdl_atom_test_const_t;

/**
 * @var s_atom_good
 * @brief The bytes a previously-downloaded, still-good page holds.
 * @details Distinctive so a truncation to zero bytes, a partial write, or a
 *          deletion are all distinguishable from "survived intact".
 * @note Read-only fixture data.
 * @since 0.1.0
 */
static const char s_atom_good[] = "GOOD-PAGE-BYTES";

/** @brief Write `body` to `path`, returning whether every byte landed. */
static bool atom_write(const char* path, const char* body)
{
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  const size_t n  = strlen(body);
  const bool   ok = (fwrite(body, 1U, n, f) == n);
  return (fclose(f) == 0) && ok;
}

/** @brief Read `path` into `out`; returns the byte count, or -1 when absent. */
static long atom_read(const char* path, char* out, size_t cap)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return -1;
  }
  const size_t n = fread(out, 1U, cap - 1U, f);
  out[n]         = '\0';
  (void)fclose(f);
  return (long)n;
}

/** @brief Whether `dir` still holds any `.mdl-tmp-` debris from a failed write. */
static bool atom_has_debris(const char* dir)
{
  DIR* d = opendir(dir);
  if (d == nullptr) {
    return false;
  }
  bool                 found = false;
  const struct dirent* e     = readdir(d);
  while (e != nullptr) {
    if (strncmp(e->d_name, ".mdl-tmp-", strlen(".mdl-tmp-")) == 0) {
      found = true;
      break;
    }
    e = readdir(d);
  }
  (void)closedir(d);
  return found;
}

/**
 * @test test_atomic_tmp_path_shape
 *
 * @par MC/DC:
 * Decision: `(final_path == NULL) || (out == NULL) || (cap == 0)` in
 * mdl_atomic_tmp_path (3 conditions, N+1 = 4 vectors).
 * - V1: final="/d/f.cbz", out=buf,  cap=sizeof(buf) -> false (control)
 * - V2: final=NULL,       out=buf,  cap=sizeof(buf) -> true  (varies final)
 * - V3: final="/d/f.cbz", out=NULL, cap=sizeof(buf) -> true  (varies out)
 * - V4: final="/d/f.cbz", out=buf,  cap=0           -> true  (varies cap)
 * V1+V2 prove final_path independently drives the outcome; V1+V3 and V1+V4 do
 * the same for out and cap.
 */
static void test_atomic_tmp_path_shape(void)
{
  TEST_BEGIN("atomic tmp path shape");
  char buf[k_atom_path_max];

  TEST_ASSERT(mdl_atomic_tmp_path("/d/f.cbz", buf, sizeof(buf)));      /* V1 */
  TEST_ASSERT(!mdl_atomic_tmp_path(nullptr, buf, sizeof(buf)));        /* V2 */
  TEST_ASSERT(!mdl_atomic_tmp_path("/d/f.cbz", nullptr, sizeof(buf))); /* V3 */
  TEST_ASSERT(!mdl_atomic_tmp_path("/d/f.cbz", buf, 0U));              /* V4 */

  /* A sibling in the destination's own directory, so rename() cannot EXDEV. */
  TEST_ASSERT(mdl_atomic_tmp_path("/d/f.cbz", buf, sizeof(buf)));
  TEST_ASSERT(strncmp(buf, "/d/.mdl-tmp-", strlen("/d/.mdl-tmp-")) == 0);
  /* The extension survives: `rar` appends `.rar` to a name that lacks one. */
  TEST_ASSERT(strcmp(buf + strlen(buf) - strlen(".cbz"), ".cbz") == 0);
  /* Never the destination itself -- that is the whole point. */
  TEST_ASSERT(strcmp(buf, "/d/f.cbz") != 0);

  /* A bare leaf gets a dot-prefixed name in the current directory. */
  TEST_ASSERT(mdl_atomic_tmp_path("f.jpg", buf, sizeof(buf)));
  TEST_ASSERT(buf[0] == '.');

  /* Too small to hold the prefixed name: refuse rather than retarget. */
  char tiny[8];
  TEST_ASSERT(!mdl_atomic_tmp_path("/d/some-long-name.cbz", tiny, sizeof(tiny)));
  TEST_END("atomic tmp path shape");
}

/**
 * @test test_atomic_commit_and_abort
 *
 * @par MC/DC:
 * Decision: `(tmp_path == NULL) || (final_path == NULL)` in mdl_atomic_commit
 * (2 conditions, N+1 = 3 vectors).
 * - V1: tmp=valid, final=valid -> false (control: the real rename runs)
 * - V2: tmp=NULL,  final=valid -> true  (varies tmp)
 * - V3: tmp=valid, final=NULL  -> true  (varies final)
 * V1+V2 prove tmp_path independently drives the outcome; V1+V3 the same for
 * final_path. Also asserts the behavioural contract either side of the guard:
 * commit REPLACES the destination, abort LEAVES it exactly as it was.
 */
static void test_atomic_commit_and_abort(void)
{
  TEST_BEGIN("atomic commit and abort");
  char tmpl[k_atom_path_max];
  (void)snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_atomic_XXXXXX");
  const char* dir = mkdtemp(tmpl);
  TEST_ASSERT(dir != nullptr);

  char dst[k_atom_path_max];
  char tmp[k_atom_path_max];
  char body[k_atom_body_max];
  (void)snprintf(dst, sizeof(dst), "%s/page_0001.jpg", dir);

  TEST_ASSERT(atom_write(dst, s_atom_good));
  TEST_ASSERT(mdl_atomic_tmp_path(dst, tmp, sizeof(tmp)));

  /* abort: the destination keeps the bytes it already had. */
  TEST_ASSERT(atom_write(tmp, "HALF"));
  mdl_atomic_abort(tmp);
  TEST_ASSERT_EQ((long)strlen(s_atom_good), atom_read(dst, body, sizeof(body)));
  TEST_ASSERT(strcmp(body, s_atom_good) == 0);
  TEST_ASSERT(!atom_has_debris(dir));

  /* commit: and only now is the destination replaced. */
  TEST_ASSERT(atom_write(tmp, "NEWBYTES"));
  TEST_ASSERT(mdl_atomic_commit(tmp, dst)); /* V1 */
  TEST_ASSERT_EQ((long)strlen("NEWBYTES"), atom_read(dst, body, sizeof(body)));
  TEST_ASSERT(strcmp(body, "NEWBYTES") == 0);
  TEST_ASSERT(!atom_has_debris(dir));

  TEST_ASSERT(!mdl_atomic_commit(nullptr, dst)); /* V2                         */
  TEST_ASSERT(!mdl_atomic_commit(dst, nullptr)); /* V3                         */
  mdl_atomic_abort(nullptr);                     /* no crash on the NULL no-op */

  (void)unlink(dst);
  (void)rmdir(dir);
  TEST_END("atomic commit and abort");
}

/**
 * @test test_curl_get_file_failure_keeps_existing
 *
 * @par MC/DC:
 * Decision: `rc != k_ra8_ok` on curl_get_file's transfer-result path (single
 * condition, N+1 = 2 vectors). This test drives the TRUE arm -- the arm that
 * used to `remove(out_path)` -- and asserts the destination is untouched; the
 * FALSE arm (a successful transfer commits the temp) is covered by the
 * integration suite, which fetches real pages.
 *
 * @details
 * The regression this exists for: `curl_get_file` opened `out_path` directly
 * with `fopen(..., "wb")`, so the previously-downloaded page was TRUNCATED the
 * instant the open succeeded, and the `remove(out_path)` on the failure path
 * then deleted the remains. A re-fetch that hit a connection error therefore
 * destroyed data the user already had. The fetch below is guaranteed to fail
 * (loopback port 1 refuses the connection, so no network is touched) and the
 * good file must come through it byte-for-byte, with no temp debris left.
 */
static void test_curl_get_file_failure_keeps_existing(void)
{
  TEST_BEGIN("curl get_file failure keeps existing");
  char tmpl[k_atom_path_max];
  (void)snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_refetch_XXXXXX");
  const char* dir = mkdtemp(tmpl);
  TEST_ASSERT(dir != nullptr);

  char dst[k_atom_path_max];
  char body[k_atom_body_max];
  (void)snprintf(dst, sizeof(dst), "%s/page_0001.jpg", dir);
  TEST_ASSERT(atom_write(dst, s_atom_good));

  /* allow_private_hosts so the SSRF guard does not reject the loopback URL
   * before libcurl ever runs -- the failure under test must be the TRANSFER
   * failing, not the request being refused up front. */
  const mdl_net_policy_t pol = {.allow_private_hosts       = true,
                                .allow_cross_host_redirect = false,
                                .max_response_bytes        = 0U};
  mdl_net_iface_t*       net = mdl_net_curl_create(&pol);
  TEST_ASSERT(net != nullptr);

  const mdl_net_req_t req  = {.user_agent = "media_dl-test",
                              .referer    = nullptr,
                              .timeout_ms = (uint32_t)k_atom_timeout_ms};
  size_t              len  = 0U;
  mdl_net_resp_t      resp = {};
  /* Port 1 on loopback refuses immediately: deterministic, offline, fast. */
  const ra8_err_t rc = mdl_net_get_file(net, "http://127.0.0.1:1/page.jpg", &req, dst, &len, &resp);
  TEST_ASSERT(rc != k_ra8_ok);

  /* THE POINT: the file the user already had is still there, intact. */
  TEST_ASSERT_EQ((long)strlen(s_atom_good), atom_read(dst, body, sizeof(body)));
  TEST_ASSERT(strcmp(body, s_atom_good) == 0);
  /* ...and the failed attempt left no half-downloaded sibling behind. */
  TEST_ASSERT(!atom_has_debris(dir));

  mdl_net_destroy(net);
  (void)unlink(dst);
  (void)rmdir(dir);
  TEST_END("curl get_file failure keeps existing");
}

/**
 * @brief Run every mdl_net + politeness unit test in sequence.
 * @return 0 when all tests passed, non-zero on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_net_dispatch_guard();
  test_net_get_file_guard();
  test_net_fake_scripts_and_records();
  test_net_classify();
  test_net_buf_write_overflow();
  test_politeness_determinism();
  test_politeness_clamp();
  test_politeness_bounds();
  test_politeness_null();
  test_atomic_tmp_path_shape();
  test_atomic_commit_and_abort();
  test_curl_get_file_failure_keeps_existing();
  (void)fprintf(stderr, "[OK  ] test_media_dl_net.c\n");
  return 0;
}
