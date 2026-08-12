/**
 * @file main.c
 * @brief media_dl -- host CLI for the e-reader media downloader.
 *
 * @details
 * Native host binary that links the firmware error contract (`ra8_err_t`) and
 * drives the downloader logic that will later run on the RA8 (only the injected
 * libcurl backend is host-specific). Modes:
 *
 *  - **series** (`--config S.conf --series URL`): read a site descriptor, list a
 *    series' chapters, and download them into a reader-openable file. Selection
 *    is by chapter identity, not list position: `--from CHAP` starts at the
 *    chapter NUMBERED CHAP, and `--update` fetches only chapters not already
 *    recorded complete in the per-series library state. Downloads resume after
 *    interruption and never re-fetch a page whose bytes are already held.
 *  - **library** (over `--out`): `--list` shows tracked series with coverage and
 *    gaps, `--update-all` incrementally updates every tracked series, `--remove`
 *    drops one series.
 *  - **pack** (`--pack DIR --format FMT`): package an existing image folder with
 *    no network.
 *  - **page** (bare `URL`): fetch one page and download its `<img>` URLs.
 *
 * The tool identifies itself honestly, honours robots.txt by default, and
 * sanitises every untrusted name before it reaches the filesystem.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_discover.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_library.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "mdl_pack.h"
#include "mdl_pathfs.h"
#include "mdl_politeness.h"
#include "mdl_report.h"
#include "mdl_sanitize.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_urlname.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Fixed sizing for the CLI (large buffers live in .bss). */
typedef enum : uint32_t {
  k_page_buf_bytes = 8U * 1024U * 1024U, /**< Max HTML page size. */
} mdl_cli_limits_t;

/** @brief On-stack string buffer sizes and page-mode delays. */
typedef enum : uint16_t {
  k_slug_bytes         = 128,  /**< Series slug buffer.              */
  k_leaf_name_bytes    = 256,  /**< Composed archive/dir leaf name.  */
  k_dir_path_bytes     = 1024, /**< Directory-path buffer.           */
  k_file_path_bytes    = 1200, /**< File-path buffer.                */
  k_cov_bytes          = 256,  /**< Coverage summary line buffer.    */
  k_ext_bytes          = 8,    /**< Image-extension buffer.          */
  k_page_img_delay_min = 400,  /**< page-mode per-image floor, ms.   */
  k_page_img_delay_max = 800,  /**< page-mode per-image ceiling, ms. */
} mdl_bufsize_t;

/** @brief `--polite` per-image delay floors for page mode (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms = 2000, /**< Polite per-image floor.   */
  k_polite_img_max_ms = 4000, /**< Polite per-image ceiling. */
} mdl_polite_floor_t;

/** @brief Filesystem constants. */
typedef enum : uint16_t {
  k_dir_mode = 0755, /**< mkdir() permission bits. */
} mdl_misc_t;

/**
 * @var s_session
 * @brief The one download session (identity + robots cache) for this run.
 * @details Embeds a large per-host cache and fetch scratch, so it lives in
 *          `.bss` rather than on the stack.
 * @warning Single-threaded; reused across every fetch in one run.
 * @since 0.1.0
 */
static mdl_session_t s_session;

/**
 * @var s_page
 * @brief Whole-page HTML scratch buffer for the current fetch.
 * @details Sized by ::k_page_buf_bytes and placed in `.bss` rather than on the
 *          stack, keeping the CLI's zero-dynamic-allocation shape.
 * @warning Single-threaded: each fetch overwrites the previous page.
 * @since 0.1.0
 */
static char s_page[k_page_buf_bytes];

/**
 * @var s_chapters
 * @brief Chapter URLs extracted from the current series page.
 * @details Filled by mdl_extract_anchors(), then filtered and ordered in place.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_url_list_t s_chapters;

/**
 * @var s_selected
 * @brief The `--from`/`--chapters` window copied out of ::s_chapters.
 * @warning Single-threaded; rebuilt per series.
 * @since 0.1.0
 */
static mdl_url_list_t s_selected;

/**
 * @var s_images
 * @brief Page-image URLs extracted from the current chapter page.
 * @warning Single-threaded; overwritten per chapter.
 * @since 0.1.0
 */
static mdl_url_list_t s_images;

/**
 * @var s_state
 * @brief The persistent library state of the series being processed.
 * @details About 2 MiB (chapter table + page pool), so it lives in `.bss`.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_state_t s_state;

/**
 * @var s_faillog
 * @brief The per-run failure log surfaced in the end-of-run summary.
 * @details About 135 KiB (a fixed failure table), so it lives in `.bss`. Cleared
 *          before each series run; the fetch loop only appends.
 * @warning Single-threaded; reused across series within one run.
 * @since 0.1.0
 */
static mdl_fetch_faillog_t s_faillog;

/**
 * @var s_rowtmp
 * @brief Scratch row used while reordering ::s_chapters in place.
 * @warning Single-threaded; holds no meaningful value between sorts.
 * @since 0.1.0
 */
static char s_rowtmp[k_mdl_url_max];

/**
 * @var s_results
 * @brief Titled search/browse hits parsed from a discovery results page.
 * @details About 98 KiB (a fixed title+URL table), so it lives in `.bss`. Only
 *          used before a download begins, so it never overlaps ::s_chapters.
 * @warning Single-threaded; rebuilt per discovery run.
 * @since 0.1.0
 */
static mdl_hit_list_t s_results;

/** @brief Larger of two unsigned values. */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/** @brief Swap two rows of a URL list. */
RA8_INTERNAL static void swap_rows(mdl_url_list_t* l, size_t a, size_t b)
{
  if (a == b) {
    return;
  }
  memcpy(s_rowtmp, l->urls[a], k_mdl_url_max);
  memcpy(l->urls[a], l->urls[b], k_mdl_url_max);
  memcpy(l->urls[b], s_rowtmp, k_mdl_url_max);
}

/** @brief Reverse a URL list in place. */
RA8_INTERNAL static void reverse_list(mdl_url_list_t* l)
{
  for (size_t i = 0U; i < (l->count / 2U); ++i) {
    swap_rows(l, i, l->count - 1U - i);
  }
}

/** @brief Selection-sort a URL list by parsed chapter number, ascending. */
RA8_INTERNAL static void sort_by_chapter_num(mdl_url_list_t* l)
{
  for (size_t i = 0U; i + 1U < l->count; ++i) {
    size_t min = i;
    for (size_t j = i + 1U; j < l->count; ++j) {
      if (mdl_urlname_chapter_number(l->urls[j]) < mdl_urlname_chapter_number(l->urls[min])) {
        min = j;
      }
    }
    swap_rows(l, i, min);
  }
}

/** @brief Apply the configured chapter ordering in place. */
RA8_INTERNAL static void apply_order(mdl_url_list_t* l, mdl_chapter_order_t order)
{
  if (order == k_mdl_order_reverse) {
    reverse_list(l);
  } else if (order == k_mdl_order_asc) {
    sort_by_chapter_num(l);
  }
}

/** @brief Keep only list entries whose URL starts with `prefix`. */
RA8_INTERNAL static void filter_prefix(mdl_url_list_t* l, const char* prefix)
{
  const size_t plen = strlen(prefix);
  size_t       w    = 0U;
  for (size_t r = 0U; r < l->count; ++r) {
    if (strncmp(l->urls[r], prefix, plen) == 0) {
      if (w != r) {
        memcpy(l->urls[w], l->urls[r], k_mdl_url_max);
      }
      ++w;
    }
  }
  l->count = w;
}

/** @brief One-time warning printed when no operator contact is configured. */
RA8_INTERNAL static void warn_no_contact(void)
{
  static bool s_warned = false;
  if (!s_warned) {
    (void)fprintf(stderr,
                  "media_dl: WARNING: no contact configured; pass --contact "
                  "<email|url> so a site operator can reach you before banning\n");
    s_warned = true;
  }
}

/** @brief Fetch the series page and build the ordered, filtered chapter list. */
RA8_INTERNAL static ra8_err_t
prepare_chapters(const mdl_site_t* site, const char* series_url, uint32_t timeout)
{
  if (!mdl_session_url_allowed(&s_session, series_url, nullptr)) {
    return k_ra8_fail; /* robots refused the series page (message printed) */
  }
  const mdl_net_req_t req = {.user_agent = s_session.user_agent,
                             .referer    = nullptr,
                             .timeout_ms = timeout};
  size_t              len = 0U;
  ra8_err_t           rc =
    mdl_net_get_buf(s_session.net, series_url, &req, s_page, sizeof(s_page), &len, nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  (void)mdl_extract_anchors(s_page, len, series_url, site->chapter_url_contains, &s_chapters);

  /* Keep only this series' chapters (drop cross-series relation links). */
  char prefix[k_dir_path_bytes];
  (void)snprintf(prefix, sizeof(prefix), "%s", series_url);
  size_t pl = strlen(prefix);
  if ((pl > 0U) && (prefix[pl - 1U] != '/') && (pl + 1U < sizeof(prefix))) {
    prefix[pl]      = '/';
    prefix[pl + 1U] = '\0';
  }
  filter_prefix(&s_chapters, prefix);
  apply_order(&s_chapters, site->chapter_order);
  return k_ra8_ok;
}

/** @brief Build the session UA + identity for a run; warns on missing contact. */
RA8_INTERNAL static void start_session(mdl_net_iface_t*      net,
                                       const mdl_run_opts_t* opts,
                                       const char*           cfg_contact,
                                       char*                 ua,
                                       size_t                ua_cap)
{
  const char* contact = opts->contact;
  if ((contact == nullptr) && (cfg_contact != nullptr) && (cfg_contact[0] != '\0')) {
    contact = cfg_contact; /* CLI --contact overrides the site descriptor's key */
  }
  if (!mdl_session_build_ua(contact, ua, ua_cap)) {
    warn_no_contact();
  }
  mdl_session_init(&s_session, net, ua, opts->honor_robots);
}

/** @brief Build slug + absolute series dir under `out_dir`; false on failure. */
RA8_INTERNAL static bool prepare_series_dir(const char* out_dir,
                                            const char* series_url,
                                            char*       slug,
                                            size_t      slug_cap,
                                            char*       abs_dir)
{
  mdl_urlname_last_segment(series_url, slug, slug_cap);
  char series_dir[k_dir_path_bytes];
  if (!mdl_path_join(out_dir, slug, series_dir, sizeof(series_dir))) {
    (void)fprintf(stderr, "media_dl: cannot build series dir for slug '%s'\n", slug);
    return false;
  }
  (void)mkdir(out_dir, (mode_t)k_dir_mode);
  (void)mkdir(series_dir, (mode_t)k_dir_mode);
  if (realpath(series_dir, abs_dir) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve %s\n", series_dir);
    return false;
  }
  return true;
}

/** @brief Chapter-number span [lo,hi] of a selected chapter list. */
RA8_INTERNAL static void list_range(const mdl_url_list_t* l, long* lo, long* hi)
{
  *lo = 0;
  *hi = 0;
  if (l->count == 0U) {
    return;
  }
  *lo = mdl_urlname_chapter_number(l->urls[0]);
  *hi = *lo;
  for (size_t i = 0U; i < l->count; ++i) {
    const long num = mdl_urlname_chapter_number(l->urls[i]);
    *lo            = (num < *lo) ? num : *lo;
    *hi            = (num > *hi) ? num : *hi;
  }
}

/** @brief Copy the `[from, from+count)` chapter window out of ::s_chapters. */
RA8_INTERNAL static void
select_window(bool from_present, long from_num, size_t count, mdl_url_list_t* out)
{
  size_t start = 0U;
  if (from_present) {
    while ((start < s_chapters.count) &&
           (mdl_urlname_chapter_number(s_chapters.urls[start]) < from_num)) {
      ++start;
    }
  }
  out->count = 0U;
  for (size_t i = start; (i < s_chapters.count) && (out->count < count); ++i) {
    memcpy(out->urls[out->count], s_chapters.urls[i], k_mdl_url_max);
    out->count += 1U;
  }
}

/** @brief Absolute path of a series' `.mdl_state` file; false if too long. */
RA8_INTERNAL static bool state_path_of(const char* abs_dir, char* out, size_t cap)
{
  const int n = snprintf(out, cap, "%s/.mdl_state", abs_dir);
  if (n < 0) {
    return false;
  }
  return (size_t)n < cap;
}

/** @brief Export every chapter in `sel` that completed at/after `run_start`. */
RA8_INTERNAL static size_t export_fresh_separate(mdl_format_t          format,
                                                 const char*           abs_dir,
                                                 const mdl_url_list_t* sel,
                                                 int64_t               run_start)
{
  size_t fails = 0U;
  for (size_t i = 0U; i < sel->count; ++i) {
    char id[k_mdl_chapter_id_max];
    mdl_urlname_last_segment(sel->urls[i], id, sizeof(id));
    const mdl_chapter_rec_t* rec = mdl_state_find_chapter(&s_state, id);
    if ((rec != nullptr) && rec->complete && (rec->fetched_at >= run_start)) {
      fails += mdl_pack_one(format, abs_dir, id);
    }
  }
  return fails;
}

/** @brief Print the per-run download tally line. */
RA8_INTERNAL static void report_stats(const char* abs_dir, const mdl_fetch_stats_t* st)
{
  (void)printf("done into %s/: %zu chapter(s) fetched, %zu skipped, %zu failed; "
               "pages %zu new / %zu reused / %zu failed\n",
               abs_dir,
               st->chapters_completed,
               st->chapters_skipped,
               st->chapters_failed,
               st->pages_fetched,
               st->pages_reused,
               st->pages_failed);
}

/**
 * @struct series_run_t
 * @brief The parameters of one series download, bundled for the run entry point.
 * @details Keeps ::run_series to a single options pointer rather than a long,
 *          error-prone positional parameter list (the library `--update-all`
 *          path reuses the same struct).
 * @since 0.1.0
 */
typedef struct {
  const char*           cfg_path;     /**< Site descriptor path.          */
  const char*           series_url;   /**< Series page URL.               */
  const char*           out_dir;      /**< Output library root.           */
  mdl_format_t          format;       /**< Output container.              */
  bool                  combine;      /**< Combine into one archive.      */
  bool                  update;       /**< Incremental (skip complete).   */
  bool                  from_present; /**< Whether @ref from_num applies. */
  long                  from_num;     /**< First chapter number to fetch. */
  size_t                chapters;     /**< Max chapters (window mode).    */
  uint64_t              seed;         /**< Politeness jitter seed.        */
  uint32_t              timeout;      /**< Per-request budget, ms.        */
  const mdl_run_opts_t* opts;         /**< Identity/security knobs.       */
} series_run_t;

/** @brief Choose the layout + combined dir name for a series run. */
RA8_INTERNAL static mdl_fetch_layout_t choose_layout(const series_run_t*   r,
                                                     const mdl_url_list_t* sel,
                                                     char*                 combined_rel,
                                                     size_t                cap,
                                                     const char*           slug)
{
  combined_rel[0] = '\0';
  if (r->update || !r->combine || (r->format == k_mdl_fmt_loose)) {
    return k_mdl_layout_separate;
  }
  long lo = 0;
  long hi = 0;
  list_range(sel, &lo, &hi);
  (void)snprintf(combined_rel, cap, "%s-%ld-%ld", slug, lo, hi);
  return k_mdl_layout_combined;
}

/** @brief Assemble the fetch context for one series run over the shared buffers. */
RA8_INTERNAL static mdl_fetch_ctx_t make_ctx(const series_run_t* r,
                                             const mdl_site_t*   site,
                                             const char*         abs_dir,
                                             const char*         state_path,
                                             mdl_governor_t*     gov)
{
  return (mdl_fetch_ctx_t){.session        = &s_session,
                           .state          = &s_state,
                           .state_path     = state_path,
                           .series_abs_dir = abs_dir,
                           .series_url     = r->series_url,
                           .site           = site,
                           .gov            = gov,
                           .timeout_ms     = r->timeout,
                           .page_buf       = s_page,
                           .page_cap       = sizeof(s_page),
                           .images         = &s_images,
                           .update_only    = r->update,
                           .faillog        = &s_faillog,
                           .progress_fn    = mdl_report_progress,
                           .progress_ctx   = nullptr};
}

/** @brief Package the freshly-downloaded output; returns the export-failure count. */
RA8_INTERNAL static size_t export_after(const series_run_t*      r,
                                        const char*              abs_dir,
                                        mdl_fetch_layout_t       layout,
                                        const char*              combined_rel,
                                        const mdl_url_list_t*    sel,
                                        const mdl_fetch_stats_t* stats,
                                        int64_t                  run_start)
{
  if (r->format == k_mdl_fmt_loose) {
    return 0U;
  }
  if (layout == k_mdl_layout_combined) {
    return mdl_pack_combined(r->format, r->opts->allow_incomplete, abs_dir, combined_rel, stats);
  }
  return export_fresh_separate(r->format, abs_dir, sel, run_start);
}

/** @brief Drive one prepared series through the fetch orchestrator + export. */
RA8_INTERNAL static int run_prepared(const series_run_t* r,
                                     const mdl_site_t*   site,
                                     const char*         abs_dir,
                                     const char*         slug,
                                     const char*         state_path)
{
  const mdl_url_list_t* sel = &s_chapters;
  if (!r->update) {
    select_window(r->from_present, r->from_num, r->chapters, &s_selected);
    sel = &s_selected;
    if (sel->count == 0U) {
      (void)fprintf(stderr, "media_dl: no chapters match the requested range\n");
      return 1;
    }
  }
  char               combined_rel[k_leaf_name_bytes];
  mdl_fetch_layout_t layout = choose_layout(r, sel, combined_rel, sizeof(combined_rel), slug);

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(site);
  mdl_governor_init(&gov, &cfg, r->seed);
  memset(&s_faillog, 0, sizeof(s_faillog));
  mdl_fetch_ctx_t   ctx       = make_ctx(r, site, abs_dir, state_path, &gov);
  const int64_t     run_start = (int64_t)time(nullptr);
  mdl_fetch_stats_t stats;
  const ra8_err_t   frc = mdl_fetch_run(&ctx,
                                        sel,
                                        layout,
                                        (layout == k_mdl_layout_combined) ? combined_rel : nullptr,
                                        &stats);
  (void)mdl_state_save(state_path, &s_state);
  report_stats(abs_dir, &stats);
  mdl_report_failures(&s_faillog);
  const size_t efail = export_after(r, abs_dir, layout, combined_rel, sel, &stats, run_start);
  return ((frc == k_ra8_ok) && (efail == 0U)) ? 0 : 1;
}

/** @brief series mode: config + series URL -> chapter selection -> download. */
RA8_INTERNAL static int run_series(const series_run_t* r)
{
  mdl_site_t site;
  if (mdl_config_load(r->cfg_path, &site) != k_ra8_ok) {
    return 1;
  }
  if (r->opts->polite) {
    mdl_config_apply_polite(&site);
  }
  (void)printf("site: %s (host %s, kind %s)\n", site.name, site.host, site.kind);

  mdl_net_iface_t* net = mdl_net_curl_create(&r->opts->policy);
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(net, r->opts, site.contact, ua, sizeof(ua));

  int rc = 1;
  if (prepare_chapters(&site, r->series_url, r->timeout) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: series fetch failed\n");
  } else if (s_chapters.count == 0U) {
    (void)fprintf(stderr, "media_dl: no chapters (check chapter_url_contains)\n");
  } else {
    (void)printf("chapters found: %zu\n", s_chapters.count);
    char slug[k_slug_bytes];
    char abs_dir[PATH_MAX];
    char state_path[PATH_MAX];
    if (prepare_series_dir(r->out_dir, r->series_url, slug, sizeof(slug), abs_dir) &&
        state_path_of(abs_dir, state_path, sizeof(state_path))) {
      (void)mdl_state_load(state_path, &s_state); /* corrupt -> rebuild (message printed) */
      mdl_state_set_series(&s_state, r->series_url, slug, site.name, site.host, r->cfg_path);
      rc = run_prepared(r, &site, abs_dir, slug, state_path);
    }
  }
  mdl_net_destroy(net);
  return rc;
}

/** @brief `--list`: print one tracked series' URL and coverage summary. */
RA8_INTERNAL static ra8_err_t list_cb(const char* series_dir, const char* state_path, void* ctx)
{
  size_t* n = (size_t*)ctx;
  *n += 1U;
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)printf("  %s\n    (state file unreadable)\n", series_dir);
    return k_ra8_ok;
  }
  char cov[k_cov_bytes];
  mdl_state_coverage(&s_state, cov, sizeof(cov));
  (void)printf("  %s\n    url: %s\n    %s\n",
               series_dir,
               (s_state.series_url[0] != '\0') ? s_state.series_url : "(unknown)",
               cov);
  return k_ra8_ok;
}

/** @brief `--list`: enumerate tracked series under `out_dir`. */
RA8_INTERNAL static int run_list(const char* out_dir)
{
  size_t          n  = 0U;
  const ra8_err_t rc = mdl_library_for_each(out_dir, list_cb, &n);
  if (n == 0U) {
    (void)printf("no tracked series under %s\n", out_dir);
  }
  return (rc == k_ra8_ok) ? 0 : 1;
}

/** @brief `--remove`: drop the series whose slug matches `url_or_slug`. */
RA8_INTERNAL static int run_remove(const char* out_dir, const char* url_or_slug)
{
  char slug[k_slug_bytes];
  mdl_urlname_last_segment(url_or_slug, slug, sizeof(slug));
  char dir[k_dir_path_bytes];
  if (!mdl_path_join(out_dir, slug, dir, sizeof(dir))) {
    (void)fprintf(stderr, "media_dl: cannot resolve series '%s'\n", url_or_slug);
    return 1;
  }
  if (mdl_library_remove_tree(dir) != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: failed to remove %s\n", dir);
    return 1;
  }
  (void)printf("removed %s\n", dir);
  return 0;
}

/**
 * @struct update_all_ctx_t
 * @brief Context threaded through ::update_all_cb for `--update-all`.
 * @since 0.1.0
 */
typedef struct {
  const series_run_t* base;    /**< Template run (format/knobs); url filled per series. */
  size_t              updated; /**< Count of series updated.                            */
} update_all_ctx_t;

/** @brief `--update-all`: run an incremental update of one tracked series. */
RA8_INTERNAL static ra8_err_t
update_all_cb(const char* series_dir, const char* state_path, void* ctx)
{
  update_all_ctx_t* p = (update_all_ctx_t*)ctx;
  if (mdl_state_load(state_path, &s_state) != k_ra8_ok) {
    (void)printf("skip %s (state file unreadable)\n", series_dir);
    return k_ra8_ok;
  }
  char url[k_mdl_url_max];
  char cfg[k_mdl_cfgpath_max];
  (void)snprintf(url, sizeof(url), "%s", s_state.series_url);
  (void)snprintf(cfg,
                 sizeof(cfg),
                 "%s",
                 (s_state.config_path[0] != '\0') ? s_state.config_path : p->base->cfg_path);
  if ((url[0] == '\0') || (cfg[0] == '\0')) {
    (void)printf("skip %s (no series URL / descriptor recorded)\n", series_dir);
    return k_ra8_ok;
  }
  (void)printf("updating %s\n", url);
  series_run_t run = *p->base;
  run.series_url   = url;
  run.cfg_path     = cfg;
  run.update       = true;
  (void)run_series(&run);
  p->updated += 1U;
  return k_ra8_ok;
}

/** @brief `--update-all`: incrementally update every tracked series. */
RA8_INTERNAL static int run_update_all(const series_run_t* base)
{
  update_all_ctx_t c  = {.base = base, .updated = 0U};
  const ra8_err_t  rc = mdl_library_for_each(base->out_dir, update_all_cb, &c);
  if (c.updated == 0U) {
    (void)printf("no tracked series to update under %s\n", base->out_dir);
  }
  return (rc == k_ra8_ok) ? 0 : 1;
}

/** @brief Gate, space, and download one page-mode image; 0 ok, 1 on skip/fail. */
RA8_INTERNAL static size_t download_page_image(const char*       url,
                                               const char*       out_dir,
                                               uint32_t          dmin,
                                               uint32_t          dmax,
                                               uint32_t          timeout,
                                               mdl_politeness_t* pol,
                                               size_t            idx)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(&s_session, s_images.urls[idx], &crawl)) {
    return 1U;
  }
  (void)mdl_politeness_wait(pol, max_u32(dmin, crawl), max_u32(dmax, crawl));
  char ext[k_ext_bytes];
  mdl_urlname_ext(s_images.urls[idx], ext, sizeof(ext));
  char path[k_file_path_bytes];
  (void)snprintf(path, sizeof(path), "%s/page_%03zu.%s", out_dir, idx + 1U, ext);
  const mdl_net_req_t ir   = {.user_agent = s_session.user_agent,
                              .referer    = url,
                              .timeout_ms = timeout};
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  const ra8_err_t rc = mdl_net_get_file(s_session.net, s_images.urls[idx], &ir, path, &got, &resp);
  if (rc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(rc, resp.status, reason, sizeof(reason));
    (void)fprintf(stderr, "  page %zu FAILED %s -- %s\n", idx + 1U, s_images.urls[idx], reason);
    return 1U;
  }
  return 0U;
}

/** @brief Download the extracted page images into `out_dir`; returns failures. */
RA8_INTERNAL static size_t download_page_images(const char* url,
                                                const char* out_dir,
                                                uint32_t    max_imgs,
                                                uint64_t    seed,
                                                uint32_t    timeout,
                                                bool        polite)
{
  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);
  const uint32_t dmin  = polite ? (uint32_t)k_polite_img_min_ms : (uint32_t)k_page_img_delay_min;
  const uint32_t dmax  = polite ? (uint32_t)k_polite_img_max_ms : (uint32_t)k_page_img_delay_max;
  const size_t   limit = (max_imgs == 0U) ? s_images.count : (size_t)max_imgs;
  size_t         fail  = 0U;
  for (size_t i = 0U; (i < s_images.count) && (i < limit); ++i) {
    fail += download_page_image(url, out_dir, dmin, dmax, timeout, &pol, i);
  }
  (void)printf("done: %zu ok, %zu failed, into %s/\n",
               (s_images.count < limit ? s_images.count : limit) - fail,
               fail,
               out_dir);
  return fail;
}

/** @brief page mode: fetch one URL, download its `<img>` URLs (debug path). */
RA8_INTERNAL static int run_page(const char*           url,
                                 const char*           out_dir,
                                 const char*           attr,
                                 uint32_t              max_imgs,
                                 uint64_t              seed,
                                 uint32_t              timeout,
                                 const mdl_run_opts_t* opts)
{
  mdl_net_iface_t* net = mdl_net_curl_create(&opts->policy);
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(net, opts, nullptr, ua, sizeof(ua));

  if (!mdl_session_url_allowed(&s_session, url, nullptr)) {
    mdl_net_destroy(net);
    return 1;
  }
  const mdl_net_req_t req  = {.user_agent = s_session.user_agent,
                              .referer    = nullptr,
                              .timeout_ms = timeout};
  size_t              len  = 0U;
  mdl_net_resp_t      resp = {};
  ra8_err_t rc = mdl_net_get_buf(s_session.net, url, &req, s_page, sizeof(s_page), &len, &resp);
  if (rc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(rc, resp.status, reason, sizeof(reason));
    (void)fprintf(stderr, "media_dl: fetch failed -- %s\n", reason);
    mdl_net_destroy(net);
    return 1;
  }
  (void)mdl_extract_images(s_page, len, url, attr, nullptr, &s_images);
  (void)printf("found %zu image(s)\n", s_images.count);
  (void)mkdir(out_dir, (mode_t)k_dir_mode);

  const size_t fail = download_page_images(url, out_dir, max_imgs, seed, timeout, opts->polite);
  mdl_net_destroy(net);
  return (fail == 0U) ? 0 : 1;
}

/** @brief pack mode: package an existing folder of images into `format`. */
RA8_INTERNAL static int run_pack(const char* dir, mdl_format_t format)
{
  if ((format == k_mdl_fmt_loose) || (format == k_mdl_fmt_invalid)) {
    (void)fprintf(stderr,
                  "media_dl: --pack needs a --format "
                  "(cbz|cbt|cbt.gz|cbt.xz|cbr|epub|jof|rabook)\n");
    return 2;
  }
  char abs[PATH_MAX];
  if (realpath(dir, abs) == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot resolve '%s'\n", dir);
    return 1;
  }
  const char* ext = mdl_format_ext(format);
  if (mdl_format_is_dir_output(format)) {
    /* JOF writes per-page `.jof` siblings into the packed directory itself;
     * report that directory rather than a container file it never creates. */
    const ra8_err_t drc = mdl_export_chapter(format, abs, abs);
    if (drc != k_ra8_ok) {
      (void)
        fprintf(stderr, "media_dl: pack '%s' as .%s FAILED (err 0x%X)\n", dir, ext, (unsigned)drc);
      return 1;
    }
    (void)printf("packed %s -> %s/*.%s\n", dir, abs, ext);
    return 0;
  }
  char      out[PATH_MAX];
  const int n = snprintf(out, sizeof(out), "%s.%s", abs, ext);
  if ((n < 0) || ((size_t)n >= sizeof(out))) {
    (void)fprintf(stderr, "media_dl: output path for '%s' is too long\n", dir);
    return 1;
  }
  const ra8_err_t rc = mdl_export_chapter(format, abs, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "media_dl: pack '%s' as .%s FAILED (err 0x%X)\n", dir, ext, (unsigned)rc);
    return 1;
  }
  (void)printf("packed %s -> %s\n", dir, out);
  return 0;
}

/** @brief Assemble a ::series_run_t from parsed args + validated scalars. */
RA8_INTERNAL static series_run_t
build_run(const mdl_args_t* a, mdl_format_t format, const mdl_run_opts_t* opts, const mdl_nums_t* n)
{
  return (series_run_t){.cfg_path     = a->cfg,
                        .series_url   = a->series,
                        .out_dir      = a->out,
                        .format       = format,
                        .combine      = !a->separate,
                        .update       = a->update,
                        .from_present = n->from_present,
                        .from_num     = n->from_num,
                        .chapters     = n->chapters,
                        .seed         = n->seed,
                        .timeout      = n->timeout,
                        .opts         = opts};
}

/** @brief search/browse discovery: fetch a results page, list hits, and -- when
 *  `--pick N` selected one -- download it as a series via @p base. */
RA8_INTERNAL static int run_discover(const mdl_args_t*     a,
                                     const mdl_run_opts_t* opts,
                                     const mdl_nums_t*     n,
                                     const series_run_t*   base)
{
  if (a->cfg == nullptr) {
    (void)fprintf(stderr, "media_dl: --search/--browse requires --config SITE.conf\n");
    return 2;
  }
  mdl_site_t site;
  if (mdl_config_load(a->cfg, &site) != k_ra8_ok) {
    return 1;
  }
  if (opts->polite) {
    mdl_config_apply_polite(&site);
  }
  mdl_net_iface_t* net = mdl_net_curl_create(&opts->policy);
  if (net == nullptr) {
    (void)fprintf(stderr, "media_dl: network init failed\n");
    return 1;
  }
  char ua[k_mdl_ua_max];
  start_session(net, opts, site.contact, ua, sizeof(ua));

  mdl_governor_t      gov;
  const mdl_gov_cfg_t cfg = mdl_config_gov_cfg(&site);
  mdl_governor_init(&gov, &cfg, n->seed);

  const mdl_discover_req_t req = {
    .session    = &s_session,
    .gov        = &gov,
    .site       = &site,
    .mode       = a->browse ? k_mdl_discover_browse : k_mdl_discover_search,
    .term       = a->search,
    .timeout_ms = n->timeout,
    .page_buf   = s_page,
    .page_cap   = sizeof(s_page),
    .hits       = &s_results,
  };
  char      picked[k_mdl_url_max] = {};
  const int rc                    = mdl_discover_run(&req, n->pick, picked, sizeof(picked));
  mdl_net_destroy(net);
  if ((rc != 0) || (picked[0] == '\0')) {
    return rc; /* listed results, or an error, with nothing to download */
  }
  series_run_t run = *base;
  run.series_url   = picked; /* combined search-and-select: feed the pick in */
  return run_series(&run);
}

/** @brief Dispatch a library command (`--list`/`--remove`/`--update-all`). */
RA8_INTERNAL static int run_library(const mdl_args_t* a, const series_run_t* run)
{
  if (a->list) {
    return run_list(a->out);
  }
  if (a->remove_series != nullptr) {
    return run_remove(a->out, a->remove_series);
  }
  if (a->cfg == nullptr) {
    (void)fprintf(stderr, "media_dl: --update-all requires --config SITE.conf\n");
    return 2;
  }
  return run_update_all(run);
}

/** @brief Select and run the mode implied by the parsed args; return the exit code. */
RA8_INTERNAL static int dispatch_run(const mdl_args_t*     a,
                                     mdl_format_t          format,
                                     const mdl_run_opts_t* opts,
                                     const mdl_nums_t*     nums,
                                     const series_run_t*   run,
                                     const char*           prog)
{
  if (a->list || (a->remove_series != nullptr) || a->update_all) {
    return run_library(a, run);
  }
  if ((a->search != nullptr) || a->browse) {
    return run_discover(a, opts, nums, run);
  }
  if (a->pack != nullptr) {
    return run_pack(a->pack, format);
  }
  if (a->cfg != nullptr) {
    if (a->series == nullptr) {
      (void)fprintf(stderr, "media_dl: --config requires --series URL\n");
      return 2;
    }
    return run_series(run);
  }
  if (a->page_url != nullptr) {
    return run_page(a->page_url, a->out, a->attr, nums->max_imgs, nums->seed, nums->timeout, opts);
  }
  mdl_cli_usage(prog);
  return 2;
}

/**
 * @brief Program entry point: parse the command line and select a run mode.
 * @details Parses and validates the arguments, then hands off to ::dispatch_run,
 *          which selects a library command (`--list`/`--remove`/`--update-all`),
 *          search/browse discovery (`--search`/`--browse`), pack mode (`--pack`),
 *          series mode (`--config` + `--series`), or single-page mode (a bare
 *          URL), in that precedence.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, 1 on a download/export failure, 2 on a usage error.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  mdl_args_t a = {};
  a.out        = "downloads";
  a.attr       = "data-src";
  mdl_cli_parse(argc, argv, &a);
  if (a.bad) {
    mdl_cli_usage(argv[0]);
    return 2;
  }
  if (a.ignore_robots) {
    (void)fprintf(stderr,
                  "media_dl: WARNING: --ignore-robots set; robots.txt will NOT be honoured\n");
  }

  mdl_nums_t nums = {};
  if (!mdl_cli_parse_nums(&a, &nums)) {
    return 2;
  }
  const mdl_run_opts_t opts = mdl_cli_run_opts(&a);

  const mdl_format_t format = mdl_format_from_str(a.format);
  if (format == k_mdl_fmt_invalid) {
    (void)fprintf(stderr,
                  "media_dl: bad --format '%s' "
                  "(loose|cbz|cbt|cbr|cbt.xz|cbt.gz|epub|jof|rabook)\n",
                  a.format);
    return 2;
  }

  const series_run_t run = build_run(&a, format, &opts, &nums);
  return dispatch_run(&a, format, &opts, &nums, &run, argv[0]);
}
