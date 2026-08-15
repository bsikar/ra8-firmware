/**
 * @file mdl_state.c
 * @brief Persistent per-series library state: parse, serialise (atomic), query.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"

/** @brief Parser/serialiser limits and radices. */
typedef enum : uint16_t {
  k_state_line_max = 1200, /**< Max serialised line length.       */
  k_state_max_flds = 10,   /**< Max TAB fields split from a line. */
  k_state_hex_base = 16,   /**< Radix for the hex hash fields.    */
  k_state_dec_base = 10,   /**< Radix for the decimal fields.     */
  k_state_gap_cap  = 12,   /**< Max missing chapters listed.      */
} mdl_state_parse_t;

/** @brief Field index of each column on a `C` (chapter) record line. */
typedef enum : uint8_t {
  k_c_id        = 1,  /**< Chapter identifier.               */
  k_c_known     = 2,  /**< Explicit parsed-number flag (v2). */
  k_c_number    = 3,  /**< Parsed chapter number (v2).       */
  k_c_done      = 4,  /**< Complete flag (0/1).              */
  k_c_pages     = 5,  /**< Total page count.                 */
  k_c_ready     = 6,  /**< Pages fetched + verified.         */
  k_c_epoch     = 7,  /**< Fetch time (epoch s).             */
  k_c_url       = 8,  /**< Source URL.                       */
  k_c_title     = 9,  /**< Display title (v2).               */
  k_c_fields_v2 = 10, /**< Fields a v2 `C` line needs.       */
  k_c_v1_number = 2,  /**< Integral parsed chapter number.   */
  k_c_v1_done   = 3,  /**< Complete flag (0/1).              */
  k_c_v1_pages  = 4,  /**< Total page count.                 */
  k_c_v1_ready  = 5,  /**< Pages fetched + verified.         */
  k_c_v1_epoch  = 6,  /**< Fetch time (epoch s).             */
  k_c_v1_url    = 7,  /**< Source URL.                       */
  k_c_fields_v1 = 8,  /**< Fields a legacy `C` line needs.   */
} mdl_c_col_t;

/** @brief Field index of each column on a `P` (page) record line. */
typedef enum : uint8_t {
  k_p_urlhash = 1, /**< Source-URL hash (hex).           */
  k_p_content = 2, /**< Content hash (hex).              */
  k_p_relpath = 3, /**< Path under the series.           */
  k_p_etag    = 4, /**< Cached ETag (optional).          */
  k_p_lastmod = 5, /**< Cached Last-Modified (optional). */
  k_p_fields  = 4, /**< Minimum fields a `P` line needs. */
} mdl_p_col_t;

void mdl_state_init(mdl_state_t* st)
{
  if (st == nullptr) {
    return;
  }
  memset(st, 0, sizeof(*st));
  st->version = (uint16_t)k_mdl_state_version;
}

/** @brief Copy `val` into a bounded field when `val` is non-NULL. */
RA8_INTERNAL static void set_opt(char* dst, size_t cap, const char* val)
{
  if (val != nullptr) {
    (void)snprintf(dst, cap, "%s", val);
  }
}

/** @brief True when a record field fits and cannot inject a line or column. */
RA8_INTERNAL static bool field_valid(const char* text, size_t cap)
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
RA8_INTERNAL static bool relative_path_valid(const char* path, size_t cap)
{
  if (!field_valid(path, cap)) {
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
RA8_INTERNAL static bool chapter_number_valid(double number, bool known)
{
  return isfinite(number) && (known || (number == 0.0));
}

/** @brief Validate every bound and cross-field invariant before persistence. */
RA8_INTERNAL static bool state_valid(const mdl_state_t* st)
{
  if ((st->version != (uint16_t)k_mdl_state_version) ||
      (st->chapter_count > (uint16_t)k_mdl_max_chapters) ||
      (st->page_rec_count > (uint32_t)k_mdl_max_page_recs) ||
      !field_valid(st->series_url, sizeof(st->series_url)) ||
      !field_valid(st->series_title, sizeof(st->series_title)) ||
      !field_valid(st->site_name, sizeof(st->site_name)) ||
      !field_valid(st->site_host, sizeof(st->site_host)) ||
      !field_valid(st->config_path, sizeof(st->config_path)) ||
      !field_valid(st->summary, sizeof(st->summary)) ||
      !field_valid(st->writer, sizeof(st->writer)) ||
      !field_valid(st->artist, sizeof(st->artist)) ||
      !field_valid(st->cover_url, sizeof(st->cover_url)) ||
      !relative_path_valid(st->cover_path, sizeof(st->cover_path)) ||
      !field_valid(st->language, sizeof(st->language)) ||
      ((st->reading_direction != k_mdl_state_read_ltr) &&
       (st->reading_direction != k_mdl_state_read_rtl))) {
    return false;
  }
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    const mdl_chapter_rec_t* chapter = &st->chapters[i];
    if (!field_valid(chapter->chapter_id, sizeof(chapter->chapter_id)) ||
        !field_valid(chapter->source_url, sizeof(chapter->source_url)) ||
        !field_valid(chapter->title, sizeof(chapter->title)) ||
        !chapter_number_valid(chapter->number, chapter->number_known) ||
        (chapter->pages_done > chapter->page_count) ||
        (chapter->complete &&
         ((chapter->page_count == 0U) || (chapter->pages_done != chapter->page_count)))) {
      return false;
    }
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    const mdl_page_rec_t* page = &st->pages[i];
    if (!field_valid(page->rel_path, sizeof(page->rel_path)) ||
        !field_valid(page->etag, sizeof(page->etag)) ||
        !field_valid(page->last_modified, sizeof(page->last_modified))) {
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
  if ((st == nullptr) || !field_valid(summary, sizeof(st->summary)) ||
      !field_valid(writer, sizeof(st->writer)) || !field_valid(artist, sizeof(st->artist)) ||
      !field_valid(cover_url, sizeof(st->cover_url)) ||
      !relative_path_valid(cover_path, sizeof(st->cover_path)) ||
      !field_valid(language, sizeof(st->language)) ||
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
  set_opt(st->series_url, sizeof(st->series_url), url);
  set_opt(st->series_title, sizeof(st->series_title), title);
  set_opt(st->site_name, sizeof(st->site_name), site_name);
  set_opt(st->site_host, sizeof(st->site_host), site_host);
  set_opt(st->config_path, sizeof(st->config_path), config_path);
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
  if (!field_valid(id, k_mdl_chapter_id_max) || !field_valid(url, k_mdl_url_max) ||
      !chapter_number_valid(number, number_known)) {
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
  if ((chapter == nullptr) || !field_valid(title, sizeof(chapter->title)) ||
      !chapter_number_valid(number, number_known)) {
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
                        const char*  last_modified)
{
  if ((st == nullptr) || (rel_path == nullptr)) {
    return false;
  }
  if (!field_valid(rel_path, k_mdl_relpath_max) ||
      !field_valid((etag != nullptr) ? etag : "", k_mdl_etag_max) ||
      !field_valid((last_modified != nullptr) ? last_modified : "", k_mdl_last_mod_max)) {
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
  return true;
}

/* ---- serialisation ------------------------------------------------------- */

/** @brief Write `val` as one line `type<TAB>val`, TAB/newlines mapped to space.
 */
RA8_INTERNAL static bool write_kv(FILE* fp, char type, const char* val)
{
  if ((fputc(type, fp) == EOF) || (fputc('\t', fp) == EOF)) {
    return false;
  }
  for (const char* c = val; *c != '\0'; ++c) {
    const char ch = (char)(((*c == '\t') || (*c == '\n') || (*c == '\r')) ? ' ' : *c);
    if (fputc(ch, fp) == EOF) {
      return false;
    }
  }
  return fputc('\n', fp) != EOF;
}

/** @brief Serialise every chapter and page record to an open stream. */
RA8_INTERNAL static bool write_records(FILE* fp, const mdl_state_t* st)
{
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    const mdl_chapter_rec_t* c = &st->chapters[i];
    if (fprintf(fp,
                "C\t%s\t%d\t%.17g\t%d\t%u\t%u\t%lld\t%s\t%s\n",
                c->chapter_id,
                c->number_known ? 1 : 0,
                c->number,
                c->complete ? 1 : 0,
                (unsigned)c->page_count,
                (unsigned)c->pages_done,
                (long long)c->fetched_at,
                c->source_url,
                c->title) < 0) {
      return false;
    }
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    const mdl_page_rec_t* p = &st->pages[i];
    if (fprintf(fp,
                "P\t%016llx\t%016llx\t%s\t%s\t%s\n",
                (unsigned long long)p->url_hash,
                (unsigned long long)p->content_hash,
                p->rel_path,
                p->etag,
                p->last_modified) < 0) {
      return false;
    }
  }
  return true;
}

/** @brief Flush file bytes and the containing directory before reporting
 * success. */
RA8_INTERNAL static bool sync_parent(const char* path)
{
  char         dir[k_state_line_max];
  const size_t len = strnlen(path, sizeof(dir));
  if ((len == 0U) || (len >= sizeof(dir))) {
    return false;
  }
  memcpy(dir, path, len + 1U);
  char* slash = strrchr(dir, '/');
  if (slash == nullptr) {
    (void)snprintf(dir, sizeof(dir), ".");
  } else if (slash == dir) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  const int fd = open(dir, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const bool ok = fsync(fd) == 0;
  (void)close(fd);
  return ok;
}

ra8_err_t mdl_state_save(const char* path, const mdl_state_t* st)
{
  if ((path == nullptr) || (st == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!state_valid(st)) {
    return k_ra8_err_invalid_state;
  }
  char      tmp[k_state_line_max];
  const int tn = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
  if ((tn < 0) || ((size_t)tn >= sizeof(tmp))) {
    return k_ra8_fail;
  }
  const int fd = mkstemp(tmp);
  if (fd < 0) {
    return k_ra8_fail;
  }
  FILE* fp = fdopen(fd, "w");
  if (fp == nullptr) {
    (void)close(fd);
    (void)remove(tmp);
    return k_ra8_fail;
  }
  bool io_ok = fprintf(fp, "# media_dl library state v%u\n", (unsigned)k_mdl_state_version) >= 0;
  io_ok      = io_ok && (fprintf(fp, "V\t%u\n", (unsigned)k_mdl_state_version) >= 0);
  io_ok      = io_ok && write_kv(fp, 'S', st->series_url);
  io_ok      = io_ok && write_kv(fp, 'T', st->series_title);
  io_ok      = io_ok && write_kv(fp, 'N', st->site_name);
  io_ok      = io_ok && write_kv(fp, 'H', st->site_host);
  io_ok      = io_ok && write_kv(fp, 'G', st->config_path);
  io_ok      = io_ok && write_kv(fp, 'D', st->summary);
  io_ok      = io_ok && write_kv(fp, 'W', st->writer);
  io_ok      = io_ok && write_kv(fp, 'A', st->artist);
  io_ok      = io_ok && write_kv(fp, 'O', st->cover_url);
  io_ok      = io_ok && write_kv(fp, 'K', st->cover_path);
  io_ok      = io_ok && write_kv(fp, 'L', st->language);
  io_ok      = io_ok && (fprintf(fp, "R\t%u\n", (unsigned)st->reading_direction) >= 0);
  io_ok      = io_ok && write_records(fp, st);
  io_ok      = io_ok && (fflush(fp) == 0) && (fsync(fd) == 0) && (ferror(fp) == 0);
  if ((fclose(fp) != 0) || !io_ok) {
    (void)remove(tmp);
    return k_ra8_fail;
  }
  if (rename(tmp, path) != 0) {
    (void)remove(tmp);
    return k_ra8_fail;
  }
  if (!sync_parent(path)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/* ---- parsing ------------------------------------------------------------- */

/** @brief Split `line` in place on TAB into `fld`; return the field count. */
RA8_INTERNAL static size_t split_tabs(char* line, char* fld[], size_t max)
{
  size_t n = 0U;
  fld[n]   = line;
  ++n;
  for (char* c = line; (*c != '\0') && (n < max); ++c) {
    if (*c == '\t') {
      *c     = '\0';
      fld[n] = c + 1;
      ++n;
    }
  }
  if ((n == max) && (strchr(fld[max - 1U], '\t') != nullptr)) {
    return max + 1U;
  }
  return n;
}

/** @brief Parse a complete signed long field, rejecting overflow and tails. */
RA8_INTERNAL static bool parse_long_field(const char* text, long* out)
{
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  errno            = 0;
  char*      end   = nullptr;
  const long value = strtol(text, &end, (int)k_state_dec_base);
  if ((errno != 0) || (end == text) || (*end != '\0')) {
    return false;
  }
  *out = value;
  return true;
}

/**
 * @brief Parse one complete finite double field.
 * @details Rejects empty text, overflow, non-finite values, and trailing bytes.
 * @param[in]  text NUL-terminated decimal field.
 * @param[out] out  Parsed value destination.
 * @return Whether one complete finite value was parsed.
 * @retval true  @p out received the parsed value.
 * @retval false The input was empty, malformed, non-finite, or out of range.
 * @pre @p text is NULL or NUL-terminated.
 * @pre @p out is non-NULL.
 * @post On true, @p out contains a finite value.
 * @post On false, no persisted state is changed.
 * @note Thread-safe except for the C-library thread-local `errno`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_double_field(const char* text, double* out)
{
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  errno              = 0;
  char*        end   = nullptr;
  const double value = strtod(text, &end);
  if ((errno != 0) || (end == text) || (*end != '\0') || !isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

/** @brief Parse a complete uint64 field in `base`, rejecting overflow/tails. */
RA8_INTERNAL static bool parse_u64_field(const char* text, int base, uint64_t* out)
{
  if ((text == nullptr) || (text[0] == '\0') || (text[0] == '-') || (text[0] == '+')) {
    return false;
  }
  errno                          = 0;
  char*                    end   = nullptr;
  const unsigned long long value = strtoull(text, &end, base);
  if ((errno != 0) || (end == text) || (*end != '\0')) {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

/** @brief Parse a complete signed 64-bit epoch field. */
RA8_INTERNAL static bool parse_i64_field(const char* text, int64_t* out)
{
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  errno                 = 0;
  char*           end   = nullptr;
  const long long value = strtoll(text, &end, (int)k_state_dec_base);
  if ((errno != 0) || (end == text) || (*end != '\0')) {
    return false;
  }
  *out = (int64_t)value;
  return true;
}

/** @brief Strip a trailing CR/LF pair from a fgets() line. */
RA8_INTERNAL static void chomp(char* s)
{
  size_t n = strlen(s);
  while ((n > 0U) && ((s[n - 1U] == '\n') || (s[n - 1U] == '\r'))) {
    s[n - 1U] = '\0';
    --n;
  }
}

/**
 * @brief Apply validated common chapter fields.
 * @details Enforces page/completion invariants and chapter-id uniqueness before
 *          appending a fully populated migrated/current record.
 * @param[in,out] st           State receiving the chapter.
 * @param[in]     fld          Split record fields.
 * @param[in]     number       Parsed chapter number.
 * @param[in]     number_known Explicit number-presence flag.
 * @param[in]     done         Parsed completion flag.
 * @param[in]     pages        Parsed page count.
 * @param[in]     ready        Parsed completed-page count.
 * @param[in]     epoch        Parsed fetch epoch.
 * @param[in]     url_col      Source-URL column index.
 * @param[in]     title        Bounded display title.
 * @return Whether the complete record was accepted.
 * @retval true  One valid chapter was appended.
 * @retval false A field/invariant failed or capacity was exhausted.
 * @pre @p st, @p fld, and @p title are non-NULL.
 * @pre @p url_col addresses an element of @p fld.
 * @post On true, chapter count grows by one.
 * @post On false, no duplicate chapter is appended.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_chapter_values(mdl_state_t* st,
                                              char*        fld[],
                                              double       number,
                                              bool         number_known,
                                              uint64_t     done,
                                              uint64_t     pages,
                                              uint64_t     ready,
                                              int64_t      epoch,
                                              size_t       url_col,
                                              const char*  title)
{
  if ((done > 1U) || (pages > UINT16_MAX) || (ready > UINT16_MAX) || (ready > pages) ||
      ((done != 0U) && ((pages == 0U) || (ready != pages))) ||
      (mdl_state_find_chapter(st, fld[k_c_id]) != nullptr)) {
    return false;
  }
  mdl_chapter_rec_t* rec =
    mdl_state_add_chapter_numbered(st, fld[k_c_id], fld[url_col], number, number_known);
  if ((rec == nullptr) || !mdl_state_set_chapter_metadata(rec, title, number, number_known)) {
    return false;
  }
  rec->complete   = done != 0U;
  rec->page_count = (uint16_t)pages;
  rec->pages_done = (uint16_t)ready;
  rec->fetched_at = epoch;
  return true;
}

/**
 * @brief Parse and apply one legacy v1 chapter record.
 * @details Migrates integral nonzero presence semantics into the v2 tuple.
 * @param[in,out] st  State receiving the record.
 * @param[in]     fld Split TAB fields.
 * @param[in]     nf  Number of fields.
 * @return Whether the exact v1 record was accepted.
 * @retval true  A valid migrated chapter was appended.
 * @retval false Field count, numeric syntax, invariants, or capacity failed.
 * @pre @p st and @p fld are non-NULL.
 * @pre @p nf describes accessible entries in @p fld.
 * @post On true, the chapter is represented using v2 in-memory semantics.
 * @post On false, parsing stops without reporting success.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_chapter_v1(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf != (size_t)k_c_fields_v1) {
    return false;
  }
  long     number = 0L;
  uint64_t done   = 0U;
  uint64_t pages  = 0U;
  uint64_t ready  = 0U;
  int64_t  epoch  = 0;
  if (!parse_long_field(fld[k_c_v1_number], &number) ||
      !parse_u64_field(fld[k_c_v1_done], (int)k_state_dec_base, &done) ||
      !parse_u64_field(fld[k_c_v1_pages], (int)k_state_dec_base, &pages) ||
      !parse_u64_field(fld[k_c_v1_ready], (int)k_state_dec_base, &ready) ||
      !parse_i64_field(fld[k_c_v1_epoch], &epoch)) {
    return false;
  }
  return apply_chapter_values(st,
                              fld,
                              (double)number,
                              number != 0L,
                              done,
                              pages,
                              ready,
                              epoch,
                              (size_t)k_c_v1_url,
                              "");
}

/**
 * @brief Parse and apply one current v2 chapter record.
 * @details Requires exact field count, Boolean flags, and a finite double.
 * @param[in,out] st  State receiving the record.
 * @param[in]     fld Split TAB fields.
 * @param[in]     nf  Number of fields.
 * @return Whether the exact v2 record was accepted.
 * @retval true  A valid chapter was appended.
 * @retval false Field count, syntax, invariants, or capacity failed.
 * @pre @p st and @p fld are non-NULL.
 * @pre @p nf describes accessible entries in @p fld.
 * @post On true, explicit presence and fractional number are preserved.
 * @post On false, parsing stops without reporting success.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_chapter_v2(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf != (size_t)k_c_fields_v2) {
    return false;
  }
  uint64_t known  = 0U;
  double   number = 0.0;
  uint64_t done   = 0U;
  uint64_t pages  = 0U;
  uint64_t ready  = 0U;
  int64_t  epoch  = 0;
  if (!parse_u64_field(fld[k_c_known], (int)k_state_dec_base, &known) ||
      !parse_double_field(fld[k_c_number], &number) ||
      !parse_u64_field(fld[k_c_done], (int)k_state_dec_base, &done) ||
      !parse_u64_field(fld[k_c_pages], (int)k_state_dec_base, &pages) ||
      !parse_u64_field(fld[k_c_ready], (int)k_state_dec_base, &ready) ||
      !parse_i64_field(fld[k_c_epoch], &epoch) || (known > 1U)) {
    return false;
  }
  return apply_chapter_values(st,
                              fld,
                              number,
                              known != 0U,
                              done,
                              pages,
                              ready,
                              epoch,
                              (size_t)k_c_url,
                              fld[k_c_title]);
}

/** @brief Apply one `P` record line; false when it is malformed/over-capacity.
 */
RA8_INTERNAL static bool apply_page(mdl_state_t* st, char* fld[], size_t nf)
{
  if ((nf < (size_t)k_p_fields) || (nf > ((size_t)k_p_lastmod + 1U))) {
    return false;
  }
  uint64_t uh = 0U;
  uint64_t ch = 0U;
  if (!parse_u64_field(fld[k_p_urlhash], (int)k_state_hex_base, &uh) ||
      !parse_u64_field(fld[k_p_content], (int)k_state_hex_base, &ch)) {
    return false;
  }
  const char* etag    = (nf > (size_t)k_p_etag) ? fld[k_p_etag] : "";
  const char* lastmod = (nf > (size_t)k_p_lastmod) ? fld[k_p_lastmod] : "";
  return mdl_state_add_page(st, uh, ch, fld[k_p_relpath], etag, lastmod);
}

/**
 * @brief Store one exact bounded key/value record.
 * @details Requires exactly two TAB fields and copies without truncation.
 * @param[out] dst Destination fixed field.
 * @param[in]  cap Destination capacity.
 * @param[in]  fld Split record fields.
 * @param[in]  nf  Number of fields.
 * @return Whether the record was valid and copied.
 * @retval true  The value was copied exactly.
 * @retval false Field count or bounded-field validation failed.
 * @pre @p dst and @p fld are non-NULL.
 * @pre @p cap is greater than zero.
 * @post On true, @p dst equals `fld[1]`.
 * @post On false, @p dst is unchanged.
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_kv(char* dst, size_t cap, char* fld[], size_t nf)
{
  if ((nf != 2U) || !field_valid(fld[1], cap)) {
    return false;
  }
  set_opt(dst, cap, fld[1]);
  return true;
}

/**
 * @brief Apply one already-split state record.
 * @details Dispatches by record type and enforces schema-specific grammar.
 * @param[in,out] st             State receiving the record.
 * @param[in]     fld            Split TAB fields.
 * @param[in]     nf             Number of fields.
 * @param[in,out] schema_version Zero before V; accepted schema afterward.
 * @return Whether the record is valid in sequence and schema.
 * @retval true  The version was accepted or the record was applied.
 * @retval false The record is malformed, unsupported, duplicated, or misplaced.
 * @pre All pointer arguments are non-NULL.
 * @pre @p nf is nonzero and describes accessible @p fld entries.
 * @post On first success, @p schema_version becomes v1 or v2.
 * @post On false, the caller rejects the complete stream.
 * @note Not thread-safe: mutates parser state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
apply_line(mdl_state_t* st, char* fld[], size_t nf, uint16_t* schema_version)
{
  const char type = fld[0][0];
  if ((fld[0][1] != '\0')) {
    return false; /* a record type is exactly one character */
  }
  if (*schema_version == 0U) {
    /* The first record MUST be a version line this build understands. */
    uint64_t version = 0U;
    if ((type != 'V') || (nf != 2U) || !parse_u64_field(fld[1], (int)k_state_dec_base, &version) ||
        ((version != (uint64_t)k_mdl_state_version_v1) &&
         (version != (uint64_t)k_mdl_state_version))) {
      return false;
    }
    *schema_version = (uint16_t)version;
    return true;
  }
  switch (type) {
    case 'V':
      return false; /* duplicate headers make corruption/concatenation visible */
    case 'S':
      return apply_kv(st->series_url, sizeof(st->series_url), fld, nf);
    case 'T':
      return apply_kv(st->series_title, sizeof(st->series_title), fld, nf);
    case 'N':
      return apply_kv(st->site_name, sizeof(st->site_name), fld, nf);
    case 'H':
      return apply_kv(st->site_host, sizeof(st->site_host), fld, nf);
    case 'G':
      return apply_kv(st->config_path, sizeof(st->config_path), fld, nf);
    case 'D':
      return (*schema_version == (uint16_t)k_mdl_state_version) &&
             apply_kv(st->summary, sizeof(st->summary), fld, nf);
    case 'W':
      return (*schema_version == (uint16_t)k_mdl_state_version) &&
             apply_kv(st->writer, sizeof(st->writer), fld, nf);
    case 'A':
      return (*schema_version == (uint16_t)k_mdl_state_version) &&
             apply_kv(st->artist, sizeof(st->artist), fld, nf);
    case 'O':
      return (*schema_version == (uint16_t)k_mdl_state_version) &&
             apply_kv(st->cover_url, sizeof(st->cover_url), fld, nf);
    case 'K':
      return (*schema_version == (uint16_t)k_mdl_state_version) && (nf == 2U) &&
             relative_path_valid(fld[1], sizeof(st->cover_path)) &&
             apply_kv(st->cover_path, sizeof(st->cover_path), fld, nf);
    case 'L':
      return (*schema_version == (uint16_t)k_mdl_state_version) &&
             apply_kv(st->language, sizeof(st->language), fld, nf);
    case 'R': {
      uint64_t direction = 0U;
      if ((*schema_version != (uint16_t)k_mdl_state_version) || (nf != 2U) ||
          !parse_u64_field(fld[1], (int)k_state_dec_base, &direction) ||
          (direction > (uint64_t)k_mdl_state_read_rtl)) {
        return false;
      }
      st->reading_direction = (mdl_state_reading_direction_t)direction;
      return true;
    }
    case 'C':
      return (*schema_version == (uint16_t)k_mdl_state_version_v1) ? apply_chapter_v1(st, fld, nf)
                                                                   : apply_chapter_v2(st, fld, nf);
    case 'P':
      return apply_page(st, fld, nf);
    default:
      return false;
  }
}

/** @brief Parse an open state stream into `st`; false on the first malformed
 * line. */
RA8_INTERNAL static bool parse_stream(FILE* fp, mdl_state_t* st)
{
  char     line[k_state_line_max];
  uint16_t schema_version = 0U;
  while (fgets(line, (int)sizeof(line), fp) != nullptr) {
    if ((strchr(line, '\n') == nullptr) && (strchr(line, '\r') == nullptr) && (feof(fp) == 0)) {
      return false; /* overlong record: do not parse its continuation as another
                       record */
    }
    chomp(line);
    if ((line[0] == '\0') || (line[0] == '#')) {
      continue; /* blank or comment */
    }
    char*        fld[k_state_max_flds];
    const size_t nf = split_tabs(line, fld, (size_t)k_state_max_flds);
    if ((nf > (size_t)k_state_max_flds) || !apply_line(st, fld, nf, &schema_version)) {
      return false;
    }
  }
  return (schema_version != 0U) && state_valid(st) &&
         (ferror(fp) == 0); /* no version or a read error is corrupt */
}

ra8_err_t mdl_state_load(const char* path, mdl_state_t* st)
{
  if ((path == nullptr) || (st == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_state_init(st);
  FILE* fp = fopen(path, "r");
  if (fp == nullptr) {
    if (errno == ENOENT) {
      return k_ra8_ok; /* first run of this series: start empty */
    }
    (void)fprintf(stderr, "media_dl: cannot read state '%s'\n", path);
    return k_ra8_err_invalid_state;
  }
  bool ok = parse_stream(fp, st);
  ok      = (fclose(fp) == 0) && ok;
  if (!ok) {
    mdl_state_init(st);
    (void)fprintf(stderr,
                  "media_dl: state '%s' is corrupt or unsupported; rebuilding "
                  "from scratch (already-downloaded pages are reused by content)\n",
                  path);
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/* ---- coverage ------------------------------------------------------------ */

/**
 * @brief Convert a known integral chapter number for gap reporting.
 * @details Rejects absent, fractional, and values outside the `long` domain.
 * @param[in]  chapter Chapter whose number is inspected.
 * @param[out] out     Integral result destination.
 * @return Whether the chapter has a representable integral number.
 * @retval true  @p out received the exact integral number.
 * @retval false The number is absent, fractional, or out of range.
 * @pre @p chapter and @p out are non-NULL.
 * @pre @p chapter satisfies the persisted finite-number invariant.
 * @post On true, converting @p out back to double reproduces the number.
 * @post @p chapter is unchanged.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool chapter_number_as_long(const mdl_chapter_rec_t* chapter, long* out)
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

/** @brief Number span [lo,hi] of complete integral chapters. */
RA8_INTERNAL static bool complete_span(const mdl_state_t* st, long* lo, long* hi, size_t* n)
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
    if (!chapter_number_as_long(chapter, &num)) {
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

/** @brief True when a complete chapter with parsed number `num` exists. */
RA8_INTERNAL static bool has_complete_number(const mdl_state_t* st, long num)
{
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    long chapter_number = 0L;
    if (st->chapters[i].complete && chapter_number_as_long(&st->chapters[i], &chapter_number) &&
        (chapter_number == num)) {
      return true;
    }
  }
  return false;
}

/** @brief Append the missing-chapter list within [lo,hi] to `buf` (bounded). */
RA8_INTERNAL static void append_gaps(const mdl_state_t* st, long lo, long hi, char* buf, size_t cap)
{
  size_t len    = strlen(buf);
  size_t listed = 0U;
  bool   wrote  = false;
  for (long num = lo; (num <= hi) && (listed < (size_t)k_state_gap_cap); ++num) {
    if (has_complete_number(st, num)) {
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
  if (!complete_span(st, &lo, &hi, &n)) {
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
  append_gaps(st, lo, hi, buf, cap);
}
