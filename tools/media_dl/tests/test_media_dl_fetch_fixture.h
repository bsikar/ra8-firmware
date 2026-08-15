/**
 * @file test_media_dl_fetch_fixture.h
 * @brief Isolated scripted filesystem and network fixture for fetch tests.
 * @details Defines fixed-capacity maps, counters, paths, and helper seams shared
 *          by split fetch tests without heap ownership or live network access.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_net.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "mdl_urlname.h"
#include "ra8_err.h"
#include "ra8_io_stream_ram.h"
#include "unity_minimal.h"

/** @brief Write one complete hosted fixture without a libc stream. */
[[maybe_unused]] RA8_INTERNAL static bool
internal_mdl_fetch_test_write_bytes(const char* path, const uint8_t* bytes, size_t length)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &bytes[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  return (close(descriptor) == 0) && (offset == length);
}

/** @brief Read one complete hosted fixture into bounded caller storage. */
[[maybe_unused]] RA8_INTERNAL static bool internal_mdl_fetch_test_read_bytes(const char* path,
                                                                             uint8_t*    bytes,
                                                                             size_t      capacity,
                                                                             size_t*     out_length)
{
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  size_t total = 0U;
  while (total < capacity) {
    const ssize_t got = read(descriptor, &bytes[total], capacity - total);
    if (got > 0) {
      total += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  uint8_t       extra = 0U;
  const ssize_t tail  = read(descriptor, &extra, 1U);
  const bool    ok    = (tail == 0) && (close(descriptor) == 0);
  if (ok) {
    *out_length = total;
  }
  return ok;
}

/** @brief Named constants for the fetch tests (no bare literals). */
typedef enum : uint16_t {
  k_map_max          = 8,    /**< Chapter->HTML map slots.                      */
  k_html_bytes       = 512,  /**< Per-chapter HTML buffer bytes.                */
  k_page_bytes       = 8192, /**< Chapter-HTML scratch bytes.                   */
  k_tmpl_bytes       = 64,   /**< mkdtemp template bytes.                       */
  k_req_timeout      = 1000, /**< Per-request budget, ms.                       */
  k_list_max         = 8,    /**< Chapter URLs in a scenario.                   */
  k_http_unavailable = 503,  /**< HTTP 503 the throttle-injection mock returns. */
} mdl_fetch_test_const_t;

/**
 * @brief Composed-path buffer size: sized past PATH_MAX plus a leaf so the
 *        compiler can prove a `<dir>/<page>` join never truncates (silences
 *        gcc -Wformat-truncation, since the dir source is a PATH_MAX array).
 */
typedef enum : uint32_t {
  k_join_bytes    = (uint32_t)PATH_MAX + 128U, /**< PATH_MAX + a page-leaf margin. */
  k_fs_work_bytes = 2048U,                     /**< Opaque FS handle workspace.    */
} mdl_fetch_join_t;

/** @brief Maximally aligned backend workspace. */
typedef union {
  max_align_t align;                  /**< Force natural maximum alignment. */
  uint8_t     bytes[k_fs_work_bytes]; /**< Opaque backend bytes.            */
} fs_workspace_t;

/**
 * @struct page_map_t
 * @brief One chapter URL mapped to the HTML the fake serves for it.
 * @since 0.1.0
 */
typedef struct {
  const char* url;  /**< Chapter page URL the fake recognises. */
  const char* html; /**< HTML body returned for that URL.      */
} page_map_t;

/**
 * @struct mock_net_t
 * @brief Fake ::mdl_net_iface_t backend: serves chapter HTML, writes page
 * bytes.
 * @details `get_buf` returns the mapped HTML for a chapter URL; `get_file`
 *          writes the URL's own bytes to the target (deterministic content) and
 *          can be scripted to fail on one call to simulate an interruption.
 * @invariant `get_file_calls` counts every page transfer attempt.
 * @since 0.1.0
 */
typedef struct {
  const page_map_t* map;                  /**< Chapter URL -> HTML.              */
  size_t            map_n;                /**< Entries in @ref map.              */
  size_t            get_buf_calls;        /**< Chapter-HTML fetches dispatched.  */
  size_t            get_file_calls;       /**< Page transfers attempted.         */
  size_t            fail_on_file_call;    /**< One call to fail once (0=never).  */
  size_t            busy_on_file_call;    /**< One call to return 503 (0=off).   */
  const char*       busy_retry_after;     /**< Retry-After for the busy reply.   */
  const char*       fail_url;             /**< URL that fails on EVERY attempt.  */
  size_t            not_mod_on_file_call; /**< 1-based call to return 304.       */
  const char*       resp_etag;            /**< ETag response header to return.   */
  const char*       resp_last_modified;   /**< Last-Modified response to return. */
  const char*       resp_content_type;    /**< Content-Type response to return.  */
  const char*       response_body;        /**< Optional 200 response bytes.      */
  const uint8_t*    response_prefix;      /**< Optional binary prefix.           */
  size_t            response_prefix_len;  /**< Bytes in @ref response_prefix.    */
  /** @brief Captured If-None-Match value. */
  char last_if_none_match[k_mdl_etag_max];
  /** Captured If-Modified-Since value. */
  char last_if_mod_since[k_mdl_last_mod_max];
} mock_net_t;

/** @brief Minimal supported image signatures used by the scripted file backend.
 */
[[maybe_unused]] static const uint8_t s_jpeg_magic[] = {0xFFU, 0xD8U, 0xFFU};
/** @brief Complete PNG signature; the production sniffer only needs its prefix.
 */
[[maybe_unused]] static const uint8_t s_png_magic[] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};

/** @brief Fake get_buf: serve the mapped HTML for a chapter URL. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_mdl_fetch_test_mock_get_buf(void*                ctx,
                                     const char*          url,
                                     const mdl_net_req_t* req,
                                     char*                buf,
                                     size_t               cap,
                                     size_t*              out_len,
                                     mdl_net_resp_t*      resp)
{
  (void)req;
  (void)resp;
  mock_net_t* f = (mock_net_t*)ctx;
  f->get_buf_calls += 1U;
  for (size_t i = 0U; i < f->map_n; ++i) {
    if (strcmp(f->map[i].url, url) == 0) {
      const int w = __builtin_snprintf(buf, cap, "%s", f->map[i].html);
      if (out_len != nullptr) {
        *out_len = (w < 0) ? 0U : (size_t)w;
      }
      return k_ra8_ok;
    }
  }
  return k_ra8_fail;
}

[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_mdl_fetch_test_prepare_response(mock_net_t*          mock,
                                         const mdl_net_req_t* req,
                                         mdl_net_resp_t*      resp)
{
  if (req != nullptr) {
    (void)__builtin_snprintf(mock->last_if_none_match,
                             sizeof(mock->last_if_none_match),
                             "%s",
                             (req->if_none_match != nullptr) ? req->if_none_match : "");
    (void)__builtin_snprintf(mock->last_if_mod_since,
                             sizeof(mock->last_if_mod_since),
                             "%s",
                             (req->if_modified_since != nullptr) ? req->if_modified_since : "");
  }
  if (resp != nullptr) {
    if (mock->resp_etag != nullptr) {
      (void)__builtin_snprintf(resp->etag, sizeof(resp->etag), "%s", mock->resp_etag);
    }
    if (mock->resp_last_modified != nullptr) {
      (void)__builtin_snprintf(resp->last_modified,
                               sizeof(resp->last_modified),
                               "%s",
                               mock->resp_last_modified);
    }
    if (mock->resp_content_type != nullptr) {
      (void)__builtin_snprintf(resp->content_type,
                               sizeof(resp->content_type),
                               "%s",
                               mock->resp_content_type);
    }
  }
  return k_ra8_ok;
}

/** @brief Fake get_body: stream the URL's own bytes, unless this call is
 * scripted to fail. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_mdl_fetch_test_mock_get_body(void*                ctx,
                                      const char*          url,
                                      const mdl_net_req_t* req,
                                      mdl_net_body_sink_t* sink,
                                      size_t*              out_len,
                                      mdl_net_resp_t*      resp)
{
  mock_net_t* f = (mock_net_t*)ctx;
  f->get_file_calls += 1U;
  (void)internal_mdl_fetch_test_prepare_response(f, req, resp);
  if ((f->not_mod_on_file_call != 0U) && (f->get_file_calls == f->not_mod_on_file_call)) {
    if (resp != nullptr) {
      resp->status = 304;
    }
    if (out_len != nullptr) {
      *out_len = 0U;
    }
    return k_ra8_ok;
  }
  if ((f->busy_on_file_call != 0U) && (f->get_file_calls == f->busy_on_file_call)) {
    if (resp != nullptr) {
      resp->status = (long)k_http_unavailable;
      (void)__builtin_snprintf(resp->retry_after,
                               sizeof(resp->retry_after),
                               "%s",
                               (f->busy_retry_after != nullptr) ? f->busy_retry_after : "");
    }
    return k_ra8_err_busy;
  }
  if ((f->fail_on_file_call != 0U) && (f->get_file_calls == f->fail_on_file_call)) {
    return k_ra8_fail;
  }
  if ((f->fail_url != nullptr) && (strcmp(f->fail_url, url) == 0)) {
    return k_ra8_fail;
  }
  if (resp != nullptr) {
    resp->status = 200;
  }
  const char*  body     = (f->response_body != nullptr) ? f->response_body : url;
  const size_t body_len = strlen(body);
  uint32_t     written  = 0U;
  if ((f->response_prefix != nullptr) && (f->response_prefix_len > 0U)) {
    const ra8_err_t error =
      sink->write(sink->ctx, f->response_prefix, (uint32_t)f->response_prefix_len, &written);
    if ((error != k_ra8_ok) || (written != (uint32_t)f->response_prefix_len)) {
      return (error == k_ra8_ok) ? k_ra8_err_invalid_state : error;
    }
  }
  const ra8_err_t error =
    sink->write(sink->ctx, (const uint8_t*)body, (uint32_t)body_len, &written);
  if ((error != k_ra8_ok) || (written != (uint32_t)body_len)) {
    return (error == k_ra8_ok) ? k_ra8_err_invalid_state : error;
  }
  if (out_len != nullptr) {
    *out_len = f->response_prefix_len + body_len;
  }
  return k_ra8_ok;
}

/** @brief Fake destroy: the handle is stack-owned, nothing to release. */
[[maybe_unused]] RA8_INTERNAL static void internal_mdl_fetch_test_mock_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The fake backend's method table. */
[[maybe_unused]] static const mdl_net_vtable_t s_mock_vtable = {
  .get_buf  = internal_mdl_fetch_test_mock_get_buf,
  .get_body = internal_mdl_fetch_test_mock_get_body,
  .destroy  = internal_mdl_fetch_test_mock_destroy,
};

/* ---- shared fixtures (large objects live off the stack) ------------------ */

/** @brief Scripted backend context. */
[[maybe_unused]] static mock_net_t s_mock;
/** @brief Session over the fake with an embedded 64-KiB buffer. */
[[maybe_unused]] static mdl_session_t s_sess;
/** @brief Selectors and disabled politeness bounds. */
[[maybe_unused]] static mdl_site_t s_site;
/** @brief Persistent state under test. */
[[maybe_unused]] static mdl_state_t s_state;
/** @brief Live chapter list for one scenario. */
[[maybe_unused]] static mdl_url_list_t s_chapters;
/** @brief Extracted-image scratch list. */
[[maybe_unused]] static mdl_url_list_t s_images;
/** @brief Chapter-HTML scratch buffer. */
[[maybe_unused]] static char s_page[k_page_bytes];
/** @brief Governor wired into internal_mdl_fetch_test_run, or NULL. */
[[maybe_unused]] static mdl_governor_t* s_fetch_gov;
/** @brief Failure log populated by internal_mdl_fetch_test_run. */
[[maybe_unused]] static mdl_fetch_faillog_t s_faillog;
/** @brief Cache-bypass setting for the current run. */
[[maybe_unused]] static bool s_refetch;
/** @brief Optional output-fault callback injected into the current run. */
[[maybe_unused]] static mdl_progress_fn s_fetch_progress_fn;
/** @brief Borrowed callback context paired with ::s_fetch_progress_fn. */
[[maybe_unused]] static void* s_fetch_progress_ctx;
/** @brief Portable filesystem dependency used by fetch orchestration. */
[[maybe_unused]] static fw_fs_t             s_fs;
[[maybe_unused]] static fw_fs_posix_state_t s_fs_posix = {.root_fd = -1};
[[maybe_unused]] static fs_workspace_t      s_file_work;
[[maybe_unused]] static fs_workspace_t      s_transaction_work;
[[maybe_unused]] static uint8_t             s_io_buffer[k_mdl_storage_io_bytes];
[[maybe_unused]] static mdl_storage_t       s_storage;

/**
 * @struct fetch_clock_t
 * @brief Virtual clock for the governed integration test (no real sleeps).
 * @since 0.1.0
 */
typedef struct {
  int64_t now_ms;      /**< Virtual now (ms).            */
  int64_t total_slept; /**< Sum of governor sleeps (ms). */
} fetch_clock_t;

/** @brief Injected clock: return the virtual now. */
/* cppcheck-suppress constParameterCallback ; ra8_governor clock-fn ABI is void*
 */
[[maybe_unused]] RA8_INTERNAL static int64_t internal_mdl_fetch_test_now(void* c)
{
  return ((const fetch_clock_t*)c)->now_ms;
}

/** @brief Injected sleeper: advance the virtual clock and tally the wait. */
[[maybe_unused]] RA8_INTERNAL static void internal_mdl_fetch_test_sleep(void* c, uint32_t ms)
{
  fetch_clock_t* k = (fetch_clock_t*)c;
  k->now_ms += (int64_t)ms;
  k->total_slept += (int64_t)ms;
}

/** @brief Configure the site descriptor the fake HTML is scraped against. */
[[maybe_unused]] RA8_INTERNAL static void internal_mdl_fetch_test_setup_site(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
  memset(&s_site, 0, sizeof(s_site));
  s_refetch                  = false;
  s_mock.response_prefix     = s_jpeg_magic;
  s_mock.response_prefix_len = sizeof(s_jpeg_magic);
  (void)__builtin_snprintf(s_site.page_img_attr, sizeof(s_site.page_img_attr), "%s", "data-src");
  s_site.page_img_url_contains[0] = '\0';
}

/** @brief Copy `n` chapter URLs into `s_chapters`. */
[[maybe_unused]] RA8_INTERNAL static void
internal_mdl_fetch_test_set_chapters(const char* const* urls, size_t n)
{
  s_chapters.count = 0U;
  for (size_t i = 0U; (i < n) && (i < (size_t)k_list_max); ++i) {
    (void)__builtin_snprintf(s_chapters.urls[s_chapters.count], k_mdl_url_max, "%s", urls[i]);
    s_chapters.count += 1U;
  }
}

/** @brief Make a fresh temp directory (resolved) and its `.mdl_state` path. */
[[maybe_unused]] RA8_INTERNAL static void internal_mdl_fetch_test_make_series_dir(char*  abs_dir,
                                                                                  size_t abs_cap,
                                                                                  char*  state_path,
                                                                                  size_t state_cap)
{
  char tmpl[k_tmpl_bytes];
  (void)__builtin_snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_fetch_XXXXXX");
  const char* made = mkdtemp(tmpl);
  TEST_ASSERT_NOT_NULL(made);
  if (realpath(made, abs_dir) == nullptr) {
    (void)__builtin_snprintf(abs_dir, abs_cap, "%s", made);
  }
  (void)__builtin_snprintf(state_path, state_cap, "%s/.mdl_state", abs_dir);
}

/** @brief Run the fetch loop for one scenario; returns its result code. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_mdl_fetch_test_run(const char*        abs_dir,
                            const char*        state_path,
                            mdl_fetch_layout_t layout,
                            const char*        combined_rel,
                            bool               update_only,
                            size_t             fail_on_file_call,
                            mdl_fetch_stats_t* out)
{
  s_mock.get_buf_calls     = 0U;
  s_mock.get_file_calls    = 0U;
  s_mock.fail_on_file_call = fail_on_file_call;
  memset(&s_faillog, 0, sizeof(s_faillog));
  uint8_t                   diagnostic_bytes[k_page_bytes];
  ra8_io_stream_t           diagnostic       = {};
  ra8_io_stream_ram_state_t diagnostic_state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&diagnostic,
                                        &diagnostic_state,
                                        diagnostic_bytes,
                                        sizeof(diagnostic_bytes)));
  mdl_net_iface_t iface = {.vtable = &s_mock_vtable, .ctx = &s_mock};
  mdl_session_init(&s_sess, &iface, "media_dl/test", &diagnostic, false);
  mdl_fetch_ctx_t ctx = {.session        = &s_sess,
                         .storage        = &s_storage,
                         .state          = &s_state,
                         .state_path     = state_path,
                         .series_abs_dir = abs_dir,
                         .series_url     = "http://s/series",
                         .site           = &s_site,
                         .gov            = s_fetch_gov,
                         .timeout_ms     = (uint32_t)k_req_timeout,
                         .page_buf       = s_page,
                         .page_cap       = sizeof(s_page),
                         .images         = &s_images,
                         .update_only    = update_only,
                         .refetch        = s_refetch,
                         .faillog        = &s_faillog,
                         .diagnostic     = &diagnostic,
                         .progress_fn    = s_fetch_progress_fn,
                         .progress_ctx   = s_fetch_progress_ctx};
  return mdl_fetch_run(&ctx, &s_chapters, layout, combined_rel, out);
}

/** @brief True when two files hold byte-identical content. */
[[maybe_unused]] RA8_INTERNAL static bool internal_mdl_fetch_test_files_equal(const char* a,
                                                                              const char* b)
{
  uint64_t ha = 0U;
  uint64_t hb = 0U;
  return (mdl_hash_file(&s_storage, a, &ha) == k_ra8_ok) &&
         (mdl_hash_file(&s_storage, b, &hb) == k_ra8_ok) && (ha == hb);
}

/** @brief True when a file exists at the composed `<abs_dir>/<rel>` path. */
[[maybe_unused]] RA8_INTERNAL static bool internal_mdl_fetch_test_page_exists(const char* abs_dir,
                                                                              const char* rel)
{
  char        path[k_join_bytes];
  struct stat st;
  (void)__builtin_snprintf(path, sizeof(path), "%s/%s", abs_dir, rel);
  return stat(path, &st) == 0;
}

/** @brief Hash one series-relative file, failing the test when it is
 * unreadable. */
[[maybe_unused]] RA8_INTERNAL static uint64_t internal_mdl_fetch_test_page_hash(const char* abs_dir,
                                                                                const char* rel)
{
  char     path[k_join_bytes];
  uint64_t hash = 0U;
  (void)__builtin_snprintf(path, sizeof(path), "%s/%s", abs_dir, rel);
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, mdl_hash_file(&s_storage, path, &hash));
  return hash;
}

/** @brief One chapter, one image, for governed fetch tests. */
[[maybe_unused]] static const page_map_t s_map1[] = {
  {"http://s/chapter-1", "<img data-src=\"http://cdn/a.jpg\">"},
};
