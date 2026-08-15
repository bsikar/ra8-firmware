/**
 * @file test_media_dl_net.c
 * @brief Host unit tests for the mdl_net vtable seam and the politeness clock.
 *
 * @details
 * The network-layer counterpart to `test_media_dl.c`, split out so neither file
 * exceeds the size cap. A scripted fake ::mdl_net_iface_t backend drives the
 * dispatchers with no network -- proving canned responses can be injected and
 * the request sequence observed -- and covers the argument-guard MC/DC vectors,
 * the libcurl backend's pure transfer classifier and bounded write callback,
 * and the seeded jitter + injectable-clock politeness behaviour. Uses the
 * repo's `unity_minimal.h` harness, mirroring `tests/test_*.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "mdl_net_curl_internal.h"
#include "mdl_politeness.h"
#include "test_media_dl_net_curl_internal.h"
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
  ra8_err_t   rc;            /**< Result the fetch call returns.                      */
  long        status;        /**< HTTP status reported through `resp`.                */
  const char* body;          /**< Body copied into the buffer on ok, or NULL.         */
  const char* retry_after;   /**< Raw Retry-After surfaced through `resp`, or NULL.   */
  const char* etag;          /**< Raw ETag surfaced through `resp`, or NULL.          */
  const char* last_modified; /**< Raw Last-Modified surfaced through `resp`, or NULL. */
  const char* content_type;  /**< Raw Content-Type surfaced through `resp`, or NULL.  */
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
  const char*         if_none_matches[k_fake_max_calls];      /**< Recorded If-None-Match.  */
  const char*         if_modified_sinces[k_fake_max_calls];   /**< Recorded If-Mod-Since.   */
} fake_net_t;

/** @brief Bounded in-memory body sink for network seam tests. */
typedef struct {
  uint8_t   bytes[k_net_buf]; /**< Accepted response bytes. */
  uint32_t  length;           /**< Accepted byte count.     */
  uint32_t  resets;           /**< Reset callback count.    */
  ra8_err_t failure;          /**< Injected write failure.  */
  bool      short_write;      /**< Report one byte short.   */
} net_test_body_t;

/** @copydoc mdl_net_body_reset_fn */
RA8_INTERNAL static ra8_err_t internal_net_test_body_reset(void* context)
{
  net_test_body_t* body = (net_test_body_t*)context;
  if (body == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  body->length = 0U;
  body->resets += 1U;
  return k_ra8_ok;
}

/** @copydoc mdl_net_body_write_fn */
RA8_INTERNAL static ra8_err_t internal_net_test_body_write(void*          context,
                                                           const uint8_t* bytes,
                                                           uint32_t       length,
                                                           uint32_t*      out_written)
{
  net_test_body_t* body = (net_test_body_t*)context;
  if ((body == nullptr) || (out_written == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    return k_ra8_err_invalid_arg;
  }
  *out_written = 0U;
  if (body->failure != k_ra8_ok) {
    return body->failure;
  }
  uint32_t accepted = body->short_write && (length != 0U) ? length - 1U : length;
  if (accepted > ((uint32_t)sizeof(body->bytes) - body->length)) {
    return k_ra8_err_no_mem;
  }
  memcpy(body->bytes + body->length, bytes, accepted);
  body->length += accepted;
  *out_written = accepted;
  return k_ra8_ok;
}

/**
 * @brief Bind one caller-owned in-memory body state.
 * @details Constructs the reset/write view used by network seam tests.
 * @param[in,out] body Writable bounded fixture state.
 * @return Complete synchronous body sink view.
 * @retval mdl_net_body_sink_t Callbacks and context borrowing @p body.
 * @pre @p body is non-NULL and remains live through dispatch.
 * @pre The fixture is exclusively owned by the current test.
 * @post The returned callbacks target exactly @p body.
 * @post No body byte or counter changes.
 * @note Test-only helper with no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_net_body_sink_t internal_net_test_body_sink(net_test_body_t* body)
{
  return (mdl_net_body_sink_t){.reset = internal_net_test_body_reset,
                               .write = internal_net_test_body_write,
                               .ctx   = body};
}

/** @brief Record the URL + referer of the current request.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] f Caller-owned fake backend state.
 * @param[in] url Validated resource URL requested by the caller.
 * @param[in] req Immutable request metadata for the operation.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_fake_record(fake_net_t* f, const char* url, const mdl_net_req_t* req)
{
  if (f->call < (size_t)k_fake_max_calls) {
    (void)__builtin_snprintf(f->urls[f->call], sizeof(f->urls[f->call]), "%s", url);
    f->referers[f->call]           = (req != nullptr) ? req->referer : nullptr;
    f->if_none_matches[f->call]    = (req != nullptr) ? req->if_none_match : nullptr;
    f->if_modified_sinces[f->call] = (req != nullptr) ? req->if_modified_since : nullptr;
  }
}

/** @brief The reply for the current call (the last one repeats once exhausted).
 */
RA8_INTERNAL static const fake_reply_t* internal_fake_next(const fake_net_t* f)
{
  const size_t i = (f->call < f->n) ? f->call : (f->n - 1U);
  return &f->replies[i];
}

/** @brief Surface a scripted reply's status + Retry-After through `resp`.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in] r Caller-owned scripted response state.
 * @param[out] resp Receives canonical response metadata.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fake_fill_resp(const fake_reply_t* r, mdl_net_resp_t* resp)
{
  if (resp != nullptr) {
    resp->status = r->status;
    (void)__builtin_snprintf(resp->retry_after,
                             sizeof(resp->retry_after),
                             "%s",
                             (r->retry_after != nullptr) ? r->retry_after : "");
    (void)
      __builtin_snprintf(resp->etag, sizeof(resp->etag), "%s", (r->etag != nullptr) ? r->etag : "");
    (void)__builtin_snprintf(resp->last_modified,
                             sizeof(resp->last_modified),
                             "%s",
                             (r->last_modified != nullptr) ? r->last_modified : "");
    (void)__builtin_snprintf(resp->content_type,
                             sizeof(resp->content_type),
                             "%s",
                             (r->content_type != nullptr) ? r->content_type : "");
  }
}

/** @brief Fake get_buf: record, hand back the scripted body/result.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @param[in] url Validated resource URL requested by the caller.
 * @param[in] req Immutable request metadata for the operation.
 * @param[in,out] buf Caller-owned bounded byte buffer.
 * @param[in] cap Supplied capacity of the destination buffer, in bytes.
 * @param[out] out_len Receives the exact produced byte count on success.
 * @param[out] resp Receives canonical response metadata.
 * @return Canonical media-downloader or adapter status.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fake_get_buf(void*                ctx,
                                                    const char*          url,
                                                    const mdl_net_req_t* req,
                                                    char*                buf,
                                                    size_t               cap,
                                                    size_t*              out_len,
                                                    mdl_net_resp_t*      resp)
{
  fake_net_t*         f = (fake_net_t*)ctx;
  const fake_reply_t* r = internal_fake_next(f);
  internal_fake_record(f, url, req);
  internal_fake_fill_resp(r, resp);
  f->call += 1U;
  size_t got = 0U;
  if ((r->rc == k_ra8_ok) && (r->body != nullptr)) {
    const int w = __builtin_snprintf(buf, cap, "%s", r->body);
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

/** @copydoc mdl_net_vtable_t::get_body */
RA8_INTERNAL static ra8_err_t internal_fake_get_body(void*                ctx,
                                                     const char*          url,
                                                     const mdl_net_req_t* req,
                                                     mdl_net_body_sink_t* sink,
                                                     size_t*              out_len,
                                                     mdl_net_resp_t*      resp)
{
  fake_net_t*         f = (fake_net_t*)ctx;
  const fake_reply_t* r = internal_fake_next(f);
  internal_fake_record(f, url, req);
  internal_fake_fill_resp(r, resp);
  f->call += 1U;
  uint32_t written = 0U;
  if ((r->rc == k_ra8_ok) && (r->status != 304L) && (r->body != nullptr)) {
    const size_t    length = strlen(r->body);
    const ra8_err_t error =
      sink->write(sink->ctx, (const uint8_t*)r->body, (uint32_t)length, &written);
    if ((error != k_ra8_ok) || (written != (uint32_t)length)) {
      return (error == k_ra8_ok) ? k_ra8_err_invalid_state : error;
    }
  }
  if (out_len != nullptr) {
    *out_len = written;
  }
  return r->rc;
}

/** @brief Fake destroy: the handle is stack-owned, so nothing to release.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fake_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The fake backend's method table. */
static const mdl_net_vtable_t s_fake_vtable = {
  .get_buf  = internal_fake_get_buf,
  .get_body = internal_fake_get_body,
  .destroy  = internal_fake_destroy,
};

/** @brief Build a stack ::mdl_net_iface_t around a fake context.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] f Caller-owned fake backend state.
 * @return Value produced by the bounded test helper.
 * @retval 0 The helper produced its zero-valued boundary result.
 * @retval nonzero The helper produced its documented nonzero result.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_net_iface_t internal_fake_iface(fake_net_t* f)
{
  return (mdl_net_iface_t){.vtable = &s_fake_vtable, .ctx = f};
}

/**
 * @test internal_test_net_dispatch_guard
 *
 * @par MC/DC:
 * Decision: mdl_net_get_buf's argument guard
 * `(net->vtable==NULL) || (url==NULL) || (req==NULL) || (buf==NULL) ||
 * (cap==0)` (5 conditions, OR; N+1 = 6 vectors). A separate `net==NULL` guard
 * precedes it.
 * - V1: vtable set, url,req,buf ok, cap>0 -> false (control: fetch dispatched,
 * ok)
 * - V2: vtable=NULL, rest ok             -> true  (varies vtable)
 * - V3: url=NULL, rest ok                -> true  (varies url)
 * - V4: req=NULL, rest ok                -> true  (varies req)
 * - V5: buf=NULL, rest ok                -> true  (varies buf)
 * - V6: cap=0, rest ok                   -> true  (varies cap)
 * V1 pairs with each of V2..V6 to show that condition independently drives the
 * outcome. The preceding `net==NULL` guard is exercised by its own vector.
 * @brief Exercise the net dispatch guard media-downloader scenario.
 * @details Exercises the net dispatch guard scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_dispatch_guard(void)
{
  TEST_BEGIN("net dispatch guard");
  const fake_reply_t  ok   = {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = "OK"};
  fake_net_t          f    = {.replies = &ok, .n = 1U};
  mdl_net_iface_t     good = internal_fake_iface(&f);
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
 * @test internal_test_net_get_body_guard
 *
 * @par MC/DC:
 * Decision: mdl_net_get_body rejects missing handle, URL, request, sink, sink
 * callbacks, and context before dispatch. The control proves reset precedes the
 * backend and a NULL sink independently changes the result.
 * @brief Exercise the net get body guard media-downloader scenario.
 * @details Exercises the net get body guard scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_get_body_guard(void)
{
  TEST_BEGIN("net get_body guard");
  const fake_reply_t  ok   = {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = nullptr};
  fake_net_t          f    = {.replies = &ok, .n = 1U};
  mdl_net_iface_t     good = internal_fake_iface(&f);
  mdl_net_iface_t     badv = {.vtable = nullptr, .ctx = nullptr};
  const mdl_net_req_t req  = {.user_agent = "ua", .referer = nullptr, .timeout_ms = 1000U};
  size_t              got  = 0U;
  net_test_body_t     body = {};
  mdl_net_body_sink_t sink = internal_net_test_body_sink(&body);
  TEST_ASSERT(mdl_net_get_body(&good, "http://h/i.jpg", &req, &sink, &got, nullptr) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)body.resets);
  TEST_ASSERT(mdl_net_get_body(&badv, "http://h/i.jpg", &req, &sink, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_body(&good, nullptr, &req, &sink, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_body(&good, "http://h/i.jpg", nullptr, &sink, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_body(&good, "http://h/i.jpg", &req, nullptr, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  mdl_net_body_sink_t missing_write = sink;
  missing_write.write               = nullptr;
  TEST_ASSERT(mdl_net_get_body(&good, "http://h/i.jpg", &req, &missing_write, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(mdl_net_get_body(nullptr, "http://h/i.jpg", &req, &sink, &got, nullptr) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)f.call);
  TEST_END("net get_body guard");
}

/**
 * @test internal_test_net_fake_scripts_and_records
 *
 * @par MC/DC:
 * (no compound decisions in this test; it drives the vtable dispatchers against
 * a scripted fake and asserts the injected bodies/statuses and the recorded
 * request sequence -- proof the seam admits canned responses and observes what
 * was sent, with no network.)
 * @brief Exercise the net fake scripts and records media-downloader scenario.
 * @details Exercises the net fake scripts and records scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_fake_scripts_and_records(void)
{
  TEST_BEGIN("net fake scripts + records");
  const fake_reply_t seq[] = {
    {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = "PAGE"},
    {.rc = k_ra8_err_busy, .status = (long)k_http_unavail, .body = nullptr, .retry_after = "42"},
    {.rc = k_ra8_ok, .status = (long)k_http_ok, .body = nullptr},
  };
  fake_net_t          f   = {.replies = seq, .n = sizeof(seq) / sizeof(seq[0])};
  mdl_net_iface_t     net = internal_fake_iface(&f);
  char                buf[k_net_buf];
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  const mdl_net_req_t r0   = {.user_agent = "ua",
                              .referer    = "http://site/series",
                              .timeout_ms = 1000U};
  const mdl_net_req_t r1 = {.user_agent = "ua", .referer = "http://site/ch1", .timeout_ms = 1000U};
  net_test_body_t     body = {};
  mdl_net_body_sink_t sink = internal_net_test_body_sink(&body);

  /* 1st: a scripted OK page body is delivered, with its status through resp. */
  TEST_ASSERT(mdl_net_get_buf(&net, "http://site/ch1", &r0, buf, sizeof(buf), &got, &resp) ==
              k_ra8_ok);
  TEST_ASSERT(strcmp(buf, "PAGE") == 0);
  TEST_ASSERT_EQ((int64_t)k_http_ok, resp.status);
  TEST_ASSERT(resp.retry_after[0] == '\0'); /* no header on the OK reply */
  /* 2nd: a scripted throttle -- its 503 + Retry-After surface through resp. */
  TEST_ASSERT(mdl_net_get_body(&net, "http://cdn/img1.jpg", &r1, &sink, &got, &resp) ==
              k_ra8_err_busy);
  TEST_ASSERT_EQ((int64_t)k_http_unavail, resp.status);
  TEST_ASSERT(strcmp(resp.retry_after, "42") == 0);
  /* 3rd: a scripted OK file; a NULL resp is accepted (no plumbing required). */
  TEST_ASSERT(mdl_net_get_body(&net, "http://cdn/img2.jpg", &r1, &sink, &got, nullptr) == k_ra8_ok);

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
 * @test internal_test_net_classify
 *
 * @par MC/DC:
 * priv_mdl_net_curl_classify's HTTP branches are reached only when `code ==
 * CURLE_OK`. The throttle test `status == 429 || status == 503` is a
 * two-condition OR: vector 429 varies the first condition true (503 false),
 * vector 503 varies the second true (429 false), and a non-throttle status
 * (200/404/500) holds both false -- minimal N+1 for the OR. The remaining
 * relational branches (`>= 500`, `>= 400`, else) plus the overflow, timeout and
 * transport-error precedence guards each get their own vector, so every class
 * the classifier distinguishes is exercised.
 * @brief Exercise the net classify media-downloader scenario.
 * @details Exercises the net classify scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_classify(void)
{
  TEST_BEGIN("net classify");
  /* overflow outranks everything, even an OK code + 200 status. */
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, true, (long)k_http_ok) == k_ra8_err_no_mem);
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OPERATION_TIMEDOUT, false, 0) == k_ra8_err_timeout);
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_COULDNT_CONNECT, false, 0) == k_ra8_fail);
  /* throttle OR: 429 true/503 false, then 503 true/429 false -> busy. */
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_too_many) == k_ra8_err_busy);
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_unavail) == k_ra8_err_busy);
  /* 5xx (both throttle conditions false) -> server error. */
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_srv_err) == k_ra8_fail);
  /* 404 / other 4xx (both false) -> not found. */
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_not_found) ==
              k_ra8_err_not_found);
  /* < 400 -> ok. */
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_ok) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_net_curl_classify(CURLE_OK, false, (long)k_http_moved) == k_ra8_ok);
  TEST_END("net classify");
}

/**
 * @test internal_test_net_buf_write_overflow
 *
 * @par MC/DC:
 * priv_mdl_net_curl_buf_write has two single-condition guards (`user==NULL` and
 * `(len+bytes) > cap`), not a compound decision. Vectors: a within-cap append,
 * an at-cap append, an over-cap write that returns 0 and latches overflow, and
 * a NULL sink that returns 0.
 * @brief Exercise the net buf write overflow media-downloader scenario.
 * @details Exercises the net buf write overflow scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_buf_write_overflow(void)
{
  TEST_BEGIN("net buf write overflow");
  char       dst[k_net_dst] = {};
  char       chunk[]        = "abcde";
  buf_sink_t sink           = {.buf = dst, .cap = 4U, .len = 0U, .overflow = false};
  /* within cap: 3 bytes into cap 4. */
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)priv_mdl_net_curl_buf_write(chunk, 1U, 3U, &sink));
  TEST_ASSERT_EQ((uint16_t)3, (uint16_t)sink.len);
  TEST_ASSERT(!sink.overflow);
  /* one more byte exactly fills cap. */
  TEST_ASSERT_EQ((uint16_t)1, (uint16_t)priv_mdl_net_curl_buf_write(chunk + 3, 1U, 1U, &sink));
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sink.len);
  /* the next byte would exceed cap -> abort (0) and latch overflow. */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_buf_write(chunk + 4, 1U, 1U, &sink));
  TEST_ASSERT(sink.overflow);
  TEST_ASSERT_EQ((uint16_t)4, (uint16_t)sink.len); /* unchanged after overflow */
  /* NULL sink -> 0. */
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_buf_write(chunk, 1U, 1U, nullptr));
  TEST_END("net buf write overflow");
}

/**
 * @test internal_test_net_body_write_faults
 * @brief Qualify the libcurl-to-body-sink adapter's exact progress contract.
 * @details Exercises exact multi-byte elements, bounded overflow,
 * multiplication overflow, injected sink failure, and successful short
 * progress.
 * @pre The bounded in-memory sink callbacks are initialized.
 * @pre The callback state is exclusively owned by this test.
 * @post Each failure is latched without advancing the accepted byte total.
 * @post Exact multi-element success advances by the total byte count.
 * @note No network or filesystem is used.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_body_write_faults(void)
{
  TEST_BEGIN("curl body callback faults");
  char                      chunk[] = "abcdefgh";
  net_test_body_t           body    = {};
  mdl_net_body_sink_t       view    = internal_net_test_body_sink(&body);
  mdl_net_curl_body_state_t state   = {.sink = &view, .cap = sizeof(body.bytes)};

  TEST_ASSERT_EQ((uint16_t)6, (uint16_t)priv_mdl_net_curl_body_write(chunk, 2U, 3U, &state));
  TEST_ASSERT_EQ((uint16_t)6, (uint16_t)body.length);
  TEST_ASSERT_EQ((uint16_t)6, (uint16_t)state.written);

  state.cap = state.written;
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_body_write(chunk, 1U, 1U, &state));
  TEST_ASSERT(state.overflow);
  TEST_ASSERT_EQ((uint16_t)6, (uint16_t)body.length);

  state = (mdl_net_curl_body_state_t){.sink = &view, .cap = UINT64_MAX};
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_body_write(chunk, SIZE_MAX, 2U, &state));
  TEST_ASSERT(state.overflow);

  body.failure = k_ra8_err_hw_error;
  state        = (mdl_net_curl_body_state_t){.sink = &view, .cap = sizeof(body.bytes)};
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_body_write(chunk, 1U, 2U, &state));
  TEST_ASSERT(state.sink_error == k_ra8_err_hw_error);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)state.written);

  body.failure     = k_ra8_ok;
  body.short_write = true;
  state            = (mdl_net_curl_body_state_t){.sink = &view, .cap = sizeof(body.bytes)};
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_body_write(chunk, 1U, 2U, &state));
  TEST_ASSERT(state.sink_error == k_ra8_err_invalid_state);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)state.written);

  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)priv_mdl_net_curl_body_write(chunk, 1U, 1U, nullptr));
  TEST_END("curl body callback faults");
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

/** @brief Injected sleeper: record the request and return immediately.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @param[in] ms Requested virtual sleep duration in milliseconds.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fake_clock_sleep(void* ctx, uint32_t ms)
{
  fake_clock_t* c = (fake_clock_t*)ctx;
  c->last_ms      = ms;
  c->total_ms += ms;
  c->calls += 1U;
}

/**
 * @test internal_test_politeness_determinism
 *
 * @par MC/DC:
 * (no compound decisions in this test; it pins the seeded xorshift64 delay
 * sequence and shows two states with the same seed agree, through a fake clock
 * so no real time elapses.)
 * @brief Exercise the politeness determinism media-downloader scenario.
 * @details Exercises the politeness determinism scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_politeness_determinism(void)
{
  TEST_BEGIN("politeness determinism");
  fake_clock_t     clk = {};
  mdl_politeness_t a;
  mdl_politeness_init_clock(&a, k_pol_seed, internal_fake_clock_sleep, &clk);
  /* Pinned sequence for seed=1 over [0,999]: 761, 505, 457 (xorshift64
   * 13/7/17). */
  TEST_ASSERT_EQ((uint32_t)k_pol_d0, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d1, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d2, mdl_politeness_wait(&a, 0U, k_pol_span_max));
  /* The injected clock was asked to sleep the returned durations; no real wait.
   */
  TEST_ASSERT_EQ((uint32_t)k_pol_d2, clk.last_ms);
  TEST_ASSERT_EQ((uint32_t)3, clk.calls);
  /* Determinism: a second state with the same seed reproduces the sequence. */
  fake_clock_t     clk2 = {};
  mdl_politeness_t b;
  mdl_politeness_init_clock(&b, k_pol_seed, internal_fake_clock_sleep, &clk2);
  TEST_ASSERT_EQ((uint32_t)k_pol_d0, mdl_politeness_wait(&b, 0U, k_pol_span_max));
  TEST_ASSERT_EQ((uint32_t)k_pol_d1, mdl_politeness_wait(&b, 0U, k_pol_span_max));
  TEST_END("politeness determinism");
}

/**
 * @test internal_test_politeness_clamp
 *
 * @par MC/DC:
 * Decision: `max_ms < min_ms` in mdl_politeness_wait (single condition, N+1 =
 * 2).
 * - V1: max_ms < min_ms (50 < 100) -> clamp; span 1 -> returns exactly min_ms.
 * - V2: max_ms >= min_ms (200 >= 100) -> no clamp -> a value in [100, 200].
 * @brief Exercise the politeness clamp media-downloader scenario.
 * @details Exercises the politeness clamp scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_politeness_clamp(void)
{
  TEST_BEGIN("politeness clamp");
  fake_clock_t     clk = {};
  mdl_politeness_t p;
  mdl_politeness_init_clock(&p, k_pol_seed, internal_fake_clock_sleep, &clk);
  const uint32_t d = mdl_politeness_wait(&p, k_pol_min, k_pol_below_min); /* max < min      */
  TEST_ASSERT_EQ((uint32_t)k_pol_min, d);                                 /* clamped to min */
  TEST_ASSERT_EQ((uint32_t)k_pol_min, clk.last_ms);
  const uint32_t d2 = mdl_politeness_wait(&p, k_pol_min, k_pol_max);
  TEST_ASSERT(d2 >= (uint32_t)k_pol_min);
  TEST_ASSERT(d2 <= (uint32_t)k_pol_max);
  TEST_END("politeness clamp");
}

/**
 * @test internal_test_politeness_bounds
 *
 * @par MC/DC:
 * (no compound decisions in this test; it checks every drawn delay lies within
 * [min, max] and that the injected clock was asked for exactly that value.)
 * @brief Exercise the politeness bounds media-downloader scenario.
 * @details Exercises the politeness bounds scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_politeness_bounds(void)
{
  TEST_BEGIN("politeness bounds");
  fake_clock_t     clk = {};
  mdl_politeness_t p;
  mdl_politeness_init_clock(&p, k_pol_seed, internal_fake_clock_sleep, &clk);
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
 * @test internal_test_politeness_null
 *
 * @par MC/DC:
 * Decision: `p == NULL` in mdl_politeness_wait (single condition, N+1 = 2).
 * - V1: p == NULL -> returns 0, no sleep.
 * - V2: p != NULL -> draws and sleeps (covered by the tests above).
 * Also asserts the NULL-`p` no-op init paths do not crash.
 * @brief Exercise the politeness null media-downloader scenario.
 * @details Exercises the politeness null scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_politeness_null(void)
{
  TEST_BEGIN("politeness null");
  TEST_ASSERT_EQ((uint32_t)0, mdl_politeness_wait(nullptr, 10U, 20U));
  mdl_politeness_init(nullptr, k_pol_seed);                         /* no crash */
  mdl_politeness_init_clock(nullptr, k_pol_seed, nullptr, nullptr); /* no crash */
  TEST_END("politeness null");
}

/**
 * @brief Exercise conditional request headers and 304 response handling.
 * @details Executes the net conditional and response headers scenario through
 * production interfaces and checks its observable success, rejection, and
 * boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_net_conditional_and_response_headers(void)
{
  TEST_BEGIN("net conditional and response headers");
  const fake_reply_t seq[] = {
    {.rc            = k_ra8_ok,
     .status        = 200,
     .body          = "DATA",
     .etag          = "\"etag-123\"",
     .last_modified = "Wed, 21 Oct 2015 07:28:00 GMT"},
    {.rc = k_ra8_ok, .status = 304, .body = nullptr, .etag = "\"etag-123\""},
  };
  fake_net_t          f   = {.replies = seq, .n = sizeof(seq) / sizeof(seq[0])};
  mdl_net_iface_t     net = internal_fake_iface(&f);
  char                buf[k_net_buf];
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  const mdl_net_req_t r0   = {.user_agent        = "ua",
                              .referer           = "http://site/series",
                              .if_none_match     = "\"etag-123\"",
                              .if_modified_since = "Wed, 21 Oct 2015 07:28:00 GMT",
                              .timeout_ms        = 1000U};
  net_test_body_t     body = {};
  mdl_net_body_sink_t sink = internal_net_test_body_sink(&body);

  TEST_ASSERT(mdl_net_get_buf(&net, "http://site/ch1", &r0, buf, sizeof(buf), &got, &resp) ==
              k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)200, resp.status);
  TEST_ASSERT(strcmp(resp.etag, "\"etag-123\"") == 0);
  TEST_ASSERT(strcmp(resp.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_ASSERT(f.if_none_matches[0] != nullptr && strcmp(f.if_none_matches[0], "\"etag-123\"") == 0);
  TEST_ASSERT(f.if_modified_sinces[0] != nullptr &&
              strcmp(f.if_modified_sinces[0], "Wed, 21 Oct 2015 07:28:00 GMT") == 0);

  TEST_ASSERT(mdl_net_get_body(&net, "http://site/img.jpg", &r0, &sink, &got, &resp) == k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)304, resp.status);
  TEST_ASSERT(strcmp(resp.etag, "\"etag-123\"") == 0);
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)f.call);
  TEST_END("net conditional and response headers");
}

int32_t main(void)
{
  internal_test_net_conditional_and_response_headers();
  internal_test_net_dispatch_guard();
  internal_test_net_get_body_guard();
  internal_test_net_fake_scripts_and_records();
  internal_test_net_classify();
  internal_test_net_buf_write_overflow();
  internal_test_net_body_write_faults();
  internal_test_politeness_determinism();
  internal_test_politeness_clamp();
  internal_test_politeness_bounds();
  internal_test_politeness_null();
  priv_test_mdl_net_curl_run();
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_net.c\n",
              sizeof("[OK  ] test_media_dl_net.c\n") - 1U);
  return 0;
}
