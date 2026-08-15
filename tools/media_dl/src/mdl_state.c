/**
 * @file mdl_state.c
 * @brief In-memory model and coverage queries for persistent series state.
 * @details Maintains bounded chapter and page records, metadata, completion,
 *          and coverage calculations independently of storage encoding.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_state.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdl_state_internal.h"
#include "ra8_attributes.h"

/** @brief Maximum missing chapter numbers rendered in one coverage summary. */
typedef enum : uint16_t {
  k_state_gap_cap         = 12,  /**< Max missing chapters listed. */
  k_state_http_status_min = 100, /**< Smallest valid HTTP status.  */
  k_state_http_status_max = 599, /**< Largest valid HTTP status.   */
} mdl_state_coverage_t;

/**
 * @brief Whether persisted HTTP observation metadata is canonical.
 * @details Accepts the exact legacy unknown pair or a nonnegative completion
 *          time paired with a three-digit HTTP response status.
 * @param[in] fetched_at Most recent fetch completion time in epoch seconds.
 * @param[in] response_status Most recent HTTP status, or zero if unknown.
 * @return Whether the observation pair can be retained in persistent state.
 * @retval true The pair is the legacy unknown value or a valid observation.
 * @retval false The time is negative or the response status is out of range.
 * @pre Integer arguments use their declared widths.
 * @pre Legacy callers encode unknown metadata as the exact pair `(0, 0)`.
 * @post The arguments are not modified.
 * @post No state, storage, or network operation occurs.
 * @note A zero epoch is valid when paired with an actual HTTP status.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_page_response_valid(int64_t  fetched_at,
                                                                uint16_t response_status)
{
  const bool legacy_unknown = (fetched_at == 0) && (response_status == 0U);
  const bool http_status    = (response_status >= (uint16_t)k_state_http_status_min) &&
                              (response_status <= (uint16_t)k_state_http_status_max);
  return legacy_unknown || ((fetched_at >= 0) && http_status);
}

void mdl_state_init(mdl_state_t* st)
{
  if (st == nullptr) {
    return;
  }
  memset(st, 0, sizeof(*st));
  st->version = (uint16_t)k_mdl_state_version;
}

/** @brief Copy `val` into a bounded field when `val` is non-NULL. */
RA8_PRIV void priv_mdl_state_set_opt(char* dst, size_t cap, const char* val)
{
  if (val != nullptr) {
    (void)snprintf(dst, cap, "%s", val);
  }
}

/** @brief True when a record field fits and cannot inject a line or column. */
RA8_PRIV bool priv_mdl_state_field_valid(const char* text, size_t cap)
{
  if (text == nullptr) {
    return false;
  }
  const size_t len = strnlen(text, cap);
  return (len < cap) && (strpbrk(text, "\t\r\n") == nullptr);
}

/**
 * @brief Validate a bounded relative path.
 * @details Accepts empty paths and rejects absolute, empty-component, and `..`
 *          component paths without accessing the filesystem.
 * @param[in] path Candidate NUL-terminated path.
 * @param[in] cap  Maximum field capacity, including NUL.
 * @return Whether the complete path is safe and bounded.
 * @retval true  The path is empty or safely relative.
 * @retval false The path is malformed, overlong, absolute, or traversing.
 * @pre @p path is non-NULL.
 * @pre @p cap is greater than zero.
 * @post @p path is unchanged.
 * @post No filesystem state is accessed or modified.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_state_relative_path_valid(const char* path, size_t cap)
{
  if (!priv_mdl_state_field_valid(path, cap)) {
    return false;
  }
  if (path[0] == '\0') {
    return true;
  }
  if ((path[0] == '/') || (path[0] == '\\')) {
    return false;
  }
  const char* part = path;
  while (*part != '\0') {
    const char*  end = strpbrk(part, "/\\");
    const size_t len = (end != nullptr) ? (size_t)(end - part) : strlen(part);
    if ((len == 0U) || ((len == 2U) && (part[0] == '.') && (part[1] == '.'))) {
      return false;
    }
    if (end == nullptr) {
      break;
    }
    part = end + 1;
  }
  return true;
}

/**
 * @brief Validate an explicit chapter-number tuple.
 * @details Requires a finite number and canonical 0.0 when presence is false.
 * @param[in] number Candidate parsed chapter number.
 * @param[in] known  Whether the number was explicitly present.
 * @return Whether the tuple satisfies the persisted invariant.
 * @retval true  The number is finite and canonical for @p known.
 * @retval false The number is non-finite or unknown-but-nonzero.
 * @pre @p known is a canonical C Boolean.
 * @pre @p number is passed by value.
 * @post No state is modified.
 * @post The result is deterministic for the supplied tuple.
 * @note Thread-safe: has no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_chapter_number_valid(double number, bool known)
{
  return isfinite(number) && (known || ((number == 0.0) && !signbit(number)));
}

/** @brief Validate every bound and cross-field invariant before persistence. */
RA8_PRIV bool priv_mdl_state_valid(const mdl_state_t* st)
{
  if ((st->version != (uint16_t)k_mdl_state_version) ||
      (st->chapter_count > (uint16_t)k_mdl_max_chapters) ||
      (st->page_rec_count > (uint32_t)k_mdl_max_page_recs) ||
      !priv_mdl_state_field_valid(st->series_url, sizeof(st->series_url)) ||
      !priv_mdl_state_field_valid(st->series_title, sizeof(st->series_title)) ||
      !priv_mdl_state_field_valid(st->site_name, sizeof(st->site_name)) ||
      !priv_mdl_state_field_valid(st->site_host, sizeof(st->site_host)) ||
      !priv_mdl_state_field_valid(st->config_path, sizeof(st->config_path)) ||
      !priv_mdl_state_field_valid(st->summary, sizeof(st->summary)) ||
      !priv_mdl_state_field_valid(st->writer, sizeof(st->writer)) ||
      !priv_mdl_state_field_valid(st->artist, sizeof(st->artist)) ||
      !priv_mdl_state_field_valid(st->cover_url, sizeof(st->cover_url)) ||
      !priv_mdl_state_relative_path_valid(st->cover_path, sizeof(st->cover_path)) ||
      !priv_mdl_state_field_valid(st->language, sizeof(st->language)) ||
      ((st->reading_direction != k_mdl_state_read_ltr) &&
       (st->reading_direction != k_mdl_state_read_rtl))) {
    return false;
  }
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    const mdl_chapter_rec_t* chapter = &st->chapters[i];
    if (!priv_mdl_state_field_valid(chapter->chapter_id, sizeof(chapter->chapter_id)) ||
        !priv_mdl_state_field_valid(chapter->source_url, sizeof(chapter->source_url)) ||
        !priv_mdl_state_field_valid(chapter->title, sizeof(chapter->title)) ||
        !internal_mdl_state_chapter_number_valid(chapter->number, chapter->number_known) ||
        (chapter->pages_done > chapter->page_count) ||
        (chapter->complete &&
         ((chapter->page_count == 0U) || (chapter->pages_done != chapter->page_count)))) {
      return false;
    }
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    const mdl_page_rec_t* page = &st->pages[i];
    if (!priv_mdl_state_field_valid(page->rel_path, sizeof(page->rel_path)) ||
        !priv_mdl_state_field_valid(page->etag, sizeof(page->etag)) ||
        !priv_mdl_state_field_valid(page->last_modified, sizeof(page->last_modified)) ||
        !internal_mdl_state_page_response_valid(page->fetched_at, page->response_status)) {
      return false;
    }
  }
  return true;
}

bool mdl_state_set_series_metadata(mdl_state_t*                  st,
                                   const char*                   summary,
                                   const char*                   writer,
                                   const char*                   artist,
                                   const char*                   cover_url,
                                   const char*                   cover_path,
                                   const char*                   language,
                                   mdl_state_reading_direction_t direction)
{
  if ((st == nullptr) || !priv_mdl_state_field_valid(summary, sizeof(st->summary)) ||
      !priv_mdl_state_field_valid(writer, sizeof(st->writer)) ||
      !priv_mdl_state_field_valid(artist, sizeof(st->artist)) ||
      !priv_mdl_state_field_valid(cover_url, sizeof(st->cover_url)) ||
      !priv_mdl_state_relative_path_valid(cover_path, sizeof(st->cover_path)) ||
      !priv_mdl_state_field_valid(language, sizeof(st->language)) ||
      ((direction != k_mdl_state_read_ltr) && (direction != k_mdl_state_read_rtl))) {
    return false;
  }
  char next_summary[k_mdl_summary_max]    = {};
  char next_writer[k_mdl_person_max]      = {};
  char next_artist[k_mdl_person_max]      = {};
  char next_cover_url[k_mdl_url_max]      = {};
  char next_cover_path[k_mdl_relpath_max] = {};
  char next_language[k_mdl_language_max]  = {};
  (void)snprintf(next_summary, sizeof(next_summary), "%s", summary);
  (void)snprintf(next_writer, sizeof(next_writer), "%s", writer);
  (void)snprintf(next_artist, sizeof(next_artist), "%s", artist);
  (void)snprintf(next_cover_url, sizeof(next_cover_url), "%s", cover_url);
  (void)snprintf(next_cover_path, sizeof(next_cover_path), "%s", cover_path);
  (void)snprintf(next_language, sizeof(next_language), "%s", language);
  memcpy(st->summary, next_summary, sizeof(st->summary));
  memcpy(st->writer, next_writer, sizeof(st->writer));
  memcpy(st->artist, next_artist, sizeof(st->artist));
  memcpy(st->cover_url, next_cover_url, sizeof(st->cover_url));
  memcpy(st->cover_path, next_cover_path, sizeof(st->cover_path));
  memcpy(st->language, next_language, sizeof(st->language));
  st->reading_direction = direction;
  return true;
}

void mdl_state_set_series(mdl_state_t* st,
                          const char*  url,
                          const char*  title,
                          const char*  site_name,
                          const char*  site_host,
                          const char*  config_path)
{
  if (st == nullptr) {
    return;
  }
  priv_mdl_state_set_opt(st->series_url, sizeof(st->series_url), url);
  priv_mdl_state_set_opt(st->series_title, sizeof(st->series_title), title);
  priv_mdl_state_set_opt(st->site_name, sizeof(st->site_name), site_name);
  priv_mdl_state_set_opt(st->site_host, sizeof(st->site_host), site_host);
  priv_mdl_state_set_opt(st->config_path, sizeof(st->config_path), config_path);
}

mdl_chapter_rec_t* mdl_state_find_chapter(mdl_state_t* st, const char* id)
{
  if ((st == nullptr) || (id == nullptr)) {
    return nullptr;
  }
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    if (strcmp(st->chapters[i].chapter_id, id) == 0) {
      return &st->chapters[i];
    }
  }
  return nullptr;
}

mdl_chapter_rec_t*
mdl_state_add_chapter(mdl_state_t* st, const char* id, const char* url, long number)
{
  return mdl_state_add_chapter_numbered(st, id, url, (double)number, number != 0L);
}

mdl_chapter_rec_t* mdl_state_add_chapter_numbered(mdl_state_t* st,
                                                  const char*  id,
                                                  const char*  url,
                                                  double       number,
                                                  bool         number_known)
{
  if ((st == nullptr) || (id == nullptr) || (url == nullptr)) {
    return nullptr;
  }
  if (!priv_mdl_state_field_valid(id, k_mdl_chapter_id_max) ||
      !priv_mdl_state_field_valid(url, k_mdl_url_max) ||
      !internal_mdl_state_chapter_number_valid(number, number_known)) {
    return nullptr;
  }
  mdl_chapter_rec_t* existing = mdl_state_find_chapter(st, id);
  if (existing != nullptr) {
    return existing;
  }
  if (st->chapter_count >= (uint16_t)k_mdl_max_chapters) {
    return nullptr;
  }
  mdl_chapter_rec_t* rec = &st->chapters[st->chapter_count];
  memset(rec, 0, sizeof(*rec));
  (void)snprintf(rec->chapter_id, sizeof(rec->chapter_id), "%s", id);
  (void)snprintf(rec->source_url, sizeof(rec->source_url), "%s", url);
  rec->number       = number;
  rec->number_known = number_known;
  st->chapter_count += 1U;
  return rec;
}

bool mdl_state_set_chapter_metadata(mdl_chapter_rec_t* chapter,
                                    const char*        title,
                                    double             number,
                                    bool               number_known)
{
  if ((chapter == nullptr) || !priv_mdl_state_field_valid(title, sizeof(chapter->title)) ||
      !internal_mdl_state_chapter_number_valid(number, number_known)) {
    return false;
  }
  (void)snprintf(chapter->title, sizeof(chapter->title), "%s", title);
  chapter->number       = number;
  chapter->number_known = number_known;
  return true;
}

bool mdl_state_chapter_complete(const mdl_state_t* st, const char* id)
{
  if ((st == nullptr) || (id == nullptr)) {
    return false;
  }
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    if (strcmp(st->chapters[i].chapter_id, id) == 0) {
      return st->chapters[i].complete;
    }
  }
  return false;
}

uint16_t mdl_state_chapter_pages(const mdl_state_t* st, const char* id)
{
  if ((st == nullptr) || (id == nullptr)) {
    return 0U;
  }
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    if (strcmp(st->chapters[i].chapter_id, id) == 0) {
      return st->chapters[i].page_count;
    }
  }
  return 0U;
}

const mdl_page_rec_t* mdl_state_find_page(const mdl_state_t* st, uint64_t url_hash)
{
  if (st == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    if (st->pages[i].url_hash == url_hash) {
      return &st->pages[i];
    }
  }
  return nullptr;
}

bool mdl_state_add_page(mdl_state_t* st,
                        uint64_t     url_hash,
                        uint64_t     content_hash,
                        const char*  rel_path,
                        const char*  etag,
                        const char*  last_modified,
                        int64_t      fetched_at,
                        uint16_t     response_status)
{
  if ((st == nullptr) || (rel_path == nullptr)) {
    return false;
  }
  if (!priv_mdl_state_field_valid(rel_path, k_mdl_relpath_max) ||
      !priv_mdl_state_field_valid((etag != nullptr) ? etag : "", k_mdl_etag_max) ||
      !priv_mdl_state_field_valid((last_modified != nullptr) ? last_modified : "",
                                  k_mdl_last_mod_max) ||
      !internal_mdl_state_page_response_valid(fetched_at, response_status)) {
    return false;
  }
  mdl_page_rec_t* rec = nullptr;
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    if (st->pages[i].url_hash == url_hash) {
      rec = &st->pages[i];
      break;
    }
  }
  if (rec == nullptr) {
    if (st->page_rec_count >= (uint32_t)k_mdl_max_page_recs) {
      return false;
    }
    rec = &st->pages[st->page_rec_count];
    st->page_rec_count += 1U;
  }
  rec->url_hash     = url_hash;
  rec->content_hash = content_hash;
  (void)snprintf(rec->rel_path, sizeof(rec->rel_path), "%s", rel_path);
  (void)snprintf(rec->etag, sizeof(rec->etag), "%s", (etag != nullptr) ? etag : "");
  (void)snprintf(rec->last_modified,
                 sizeof(rec->last_modified),
                 "%s",
                 (last_modified != nullptr) ? last_modified : "");
  rec->fetched_at      = fetched_at;
  rec->response_status = response_status;
  return true;
}

bool mdl_state_note_page_response(mdl_state_t* st,
                                  uint64_t     url_hash,
                                  int64_t      fetched_at,
                                  uint16_t     response_status)
{
  if ((st == nullptr) || !internal_mdl_state_page_response_valid(fetched_at, response_status) ||
      (response_status == 0U)) {
    return false;
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    if (st->pages[i].url_hash == url_hash) {
      st->pages[i].fetched_at      = fetched_at;
      st->pages[i].response_status = response_status;
      return true;
    }
  }
  return false;
}

/* ---- serialisation ------------------------------------------------------- */

/**
 * @brief Convert an exactly integral chapter number to `long`.
 * @details Rejects unknown, fractional, and out-of-range binary64 chapter
 *          numbers before performing the narrowing conversion.
 * @param[in] chapter Validated chapter record to inspect.
 * @param[out] out Receives the integral value on success.
 * @return Whether the chapter number has an exact `long` representation.
 * @retval true @p out was initialized with the exact integral value.
 * @retval false The number was unknown, fractional, or outside `long` range.
 * @pre @p chapter and @p out are non-NULL.
 * @pre @p chapter is a readable initialized record.
 * @post Success initializes @p out exactly once.
 * @post Failure leaves persistent state and chapter records unchanged.
 * @note Performs no locale-sensitive decimal conversion.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_chapter_number_as_long(const mdl_chapter_rec_t* chapter,
                                                                   long*                    out)
{
  if (!chapter->number_known || ((long double)chapter->number < (long double)LONG_MIN) ||
      ((long double)chapter->number > (long double)LONG_MAX)) {
    return false;
  }
  const long number = (long)chapter->number;
  if ((double)number != chapter->number) {
    return false;
  }
  *out = number;
  return true;
}

/**
 * @brief Number span [lo,hi] of complete integral chapters.
 * @details Scans bounded chapter records, considers only complete chapters with
 *          exact integral numbers, and returns their minimum, maximum, and count.
 * @param[in] st Validated bounded state to scan.
 * @param[out] lo Receives the minimum complete integral chapter number.
 * @param[out] hi Receives the maximum complete integral chapter number.
 * @param[out] n Receives the number of complete integral chapters.
 * @return Whether at least one qualifying chapter exists.
 * @retval true @p lo, @p hi, and @p n describe the complete integral span.
 * @retval false No complete integral chapter exists; outputs are not authoritative.
 * @pre @p st, @p lo, @p hi, and @p n are non-NULL.
 * @pre @p st contains no more than its fixed chapter capacity.
 * @post No state record is modified.
 * @post Success initializes all three output values.
 * @note Fractional and unknown chapter numbers do not contribute to the span.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_complete_span(const mdl_state_t* st, long* lo, long* hi, size_t* n)
{
  bool any = false;
  *n       = 0U;
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    const mdl_chapter_rec_t* chapter = &st->chapters[i];
    if (!chapter->complete) {
      continue;
    }
    *n += 1U;
    long num = 0L;
    if (!internal_mdl_state_chapter_number_as_long(chapter, &num)) {
      continue;
    }
    if (!any) {
      *lo = num;
      *hi = num;
      any = true;
    } else {
      *lo = (num < *lo) ? num : *lo;
      *hi = (num > *hi) ? num : *hi;
    }
  }
  return any;
}

/**
 * @brief True when a complete chapter with parsed number `num` exists.
 * @details Scans only the bounded chapter prefix and requires both completion
 *          and an exact integral-number match.
 * @param[in] st Validated bounded state to scan.
 * @param[in] num Integral chapter number being queried.
 * @return Whether a complete record has exactly @p num.
 * @retval true A complete exact-number match exists.
 * @retval false No complete exact-number match exists.
 * @pre @p st is non-NULL and readable.
 * @pre @p st contains no more than its fixed chapter capacity.
 * @post No state record or caller storage is modified.
 * @post The result reflects only records marked complete.
 * @note The scan is deterministic and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_has_complete_number(const mdl_state_t* st, long num)
{
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    long chapter_number = 0L;
    if (st->chapters[i].complete &&
        internal_mdl_state_chapter_number_as_long(&st->chapters[i], &chapter_number) &&
        (chapter_number == num)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Append the missing-chapter list within [lo,hi] to `buf` (bounded).
 * @details Walks the inclusive integral range, appending absent chapter numbers
 *          as a comma-separated list while preserving NUL termination at @p cap.
 * @param[in] st Validated bounded state used for membership checks.
 * @param[in] lo Inclusive lower chapter number.
 * @param[in] hi Inclusive upper chapter number.
 * @param[in,out] buf Existing report buffer to append to.
 * @param[in] cap Total capacity of @p buf in bytes.
 * @pre @p st and @p buf are non-NULL and @p buf is NUL-terminated within @p cap.
 * @pre @p lo is no greater than @p hi and range iteration cannot overflow `long`.
 * @post @p buf remains NUL-terminated within @p cap.
 * @post Persistent state and chapter records remain unchanged.
 * @note Values that do not fit are omitted rather than written partially.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_mdl_state_append_gaps(const mdl_state_t* st, long lo, long hi, char* buf, size_t cap)
{
  size_t len    = strlen(buf);
  size_t listed = 0U;
  bool   wrote  = false;
  for (long num = lo; (num <= hi) && (listed < (size_t)k_state_gap_cap); ++num) {
    if (internal_mdl_state_has_complete_number(st, num)) {
      continue;
    }
    const int w = snprintf(&buf[len],
                           (len < cap) ? (cap - len) : 0U,
                           "%s%ld",
                           wrote ? ", " : ", missing ",
                           num);
    if ((w < 0) || ((size_t)w >= ((len < cap) ? (cap - len) : 0U))) {
      return; /* out of room: stop rather than truncate mid-number */
    }
    len += (size_t)w;
    wrote = true;
    ++listed;
  }
}

void mdl_state_coverage(const mdl_state_t* st, char* buf, size_t cap)
{
  if ((st == nullptr) || (buf == nullptr) || (cap == 0U)) {
    return;
  }
  long   lo = 0;
  long   hi = 0;
  size_t n  = 0U;
  if (!internal_mdl_state_complete_span(st, &lo, &hi, &n)) {
    size_t complete = 0U;
    for (uint16_t i = 0U; i < st->chapter_count; ++i) {
      complete += st->chapters[i].complete ? 1U : 0U;
    }
    if (complete == 0U) {
      (void)snprintf(buf, cap, "no chapters complete");
    } else {
      (void)snprintf(buf, cap, "%zu chapter(s) complete", complete);
    }
    return;
  }
  (void)snprintf(buf, cap, "%zu chapter(s) complete, %ld..%ld", n, lo, hi);
  internal_mdl_state_append_gaps(st, lo, hi, buf, cap);
}
