/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_state.c
 * @brief Persistent per-series library state: parse, serialise (atomic), query.
 */
#include "mdl_state.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Parser/serialiser limits and radices. */
typedef enum : uint16_t {
  k_state_line_max = 1200, /**< Max serialised line length.       */
  k_state_max_flds = 8,    /**< Max TAB fields split from a line. */
  k_state_hex_base = 16,   /**< Radix for the hex hash fields.    */
  k_state_dec_base = 10,   /**< Radix for the decimal fields.     */
  k_state_gap_cap  = 12,   /**< Max missing chapters listed.      */
} mdl_state_parse_t;

/** @brief Field index of each column on a `C` (chapter) record line. */
typedef enum : uint8_t {
  k_c_id     = 1, /**< Chapter identifier.       */
  k_c_number = 2, /**< Parsed chapter number.    */
  k_c_done   = 3, /**< Complete flag (0/1).      */
  k_c_pages  = 4, /**< Total page count.         */
  k_c_ready  = 5, /**< Pages fetched + verified. */
  k_c_epoch  = 6, /**< Fetch time (epoch s).     */
  k_c_url    = 7, /**< Source URL.               */
  k_c_fields = 8, /**< Fields a `C` line needs.  */
} mdl_c_col_t;

/** @brief Field index of each column on a `P` (page) record line. */
typedef enum : uint8_t {
  k_p_urlhash = 1, /**< Source-URL hash (hex).   */
  k_p_content = 2, /**< Content hash (hex).      */
  k_p_relpath = 3, /**< Path under the series.   */
  k_p_fields  = 4, /**< Fields a `P` line needs. */
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
  if ((st == nullptr) || (id == nullptr) || (url == nullptr)) {
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
  rec->number = number;
  st->chapter_count += 1U;
  return rec;
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
                        const char*  rel_path)
{
  if ((st == nullptr) || (rel_path == nullptr)) {
    return false;
  }
  if (st->page_rec_count >= (uint32_t)k_mdl_max_page_recs) {
    return false;
  }
  mdl_page_rec_t* rec = &st->pages[st->page_rec_count];
  rec->url_hash       = url_hash;
  rec->content_hash   = content_hash;
  (void)snprintf(rec->rel_path, sizeof(rec->rel_path), "%s", rel_path);
  st->page_rec_count += 1U;
  return true;
}

/* ---- serialisation ------------------------------------------------------- */

/** @brief Write `val` as one line `type<TAB>val`, TAB/newlines mapped to space. */
RA8_INTERNAL static void write_kv(FILE* fp, char type, const char* val)
{
  (void)fputc(type, fp);
  (void)fputc('\t', fp);
  for (const char* c = val; *c != '\0'; ++c) {
    const char ch = (char)(((*c == '\t') || (*c == '\n') || (*c == '\r')) ? ' ' : *c);
    (void)fputc(ch, fp);
  }
  (void)fputc('\n', fp);
}

/** @brief Serialise every chapter and page record to an open stream. */
RA8_INTERNAL static void write_records(FILE* fp, const mdl_state_t* st)
{
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    const mdl_chapter_rec_t* c = &st->chapters[i];
    (void)fprintf(fp,
                  "C\t%s\t%ld\t%d\t%u\t%u\t%lld\t%s\n",
                  c->chapter_id,
                  c->number,
                  c->complete ? 1 : 0,
                  (unsigned)c->page_count,
                  (unsigned)c->pages_done,
                  (long long)c->fetched_at,
                  c->source_url);
  }
  for (uint32_t i = 0U; i < st->page_rec_count; ++i) {
    const mdl_page_rec_t* p = &st->pages[i];
    (void)fprintf(fp,
                  "P\t%016llx\t%016llx\t%s\n",
                  (unsigned long long)p->url_hash,
                  (unsigned long long)p->content_hash,
                  p->rel_path);
  }
}

ra8_err_t mdl_state_save(const char* path, const mdl_state_t* st)
{
  if ((path == nullptr) || (st == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  char      tmp[k_state_line_max];
  const int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if ((tn < 0) || ((size_t)tn >= sizeof(tmp))) {
    return k_ra8_fail;
  }
  FILE* fp = fopen(tmp, "w");
  if (fp == nullptr) {
    return k_ra8_fail;
  }
  (void)fprintf(fp, "# media_dl library state v%u\n", (unsigned)k_mdl_state_version);
  (void)fprintf(fp, "V\t%u\n", (unsigned)k_mdl_state_version);
  write_kv(fp, 'S', st->series_url);
  write_kv(fp, 'T', st->series_title);
  write_kv(fp, 'N', st->site_name);
  write_kv(fp, 'H', st->site_host);
  write_kv(fp, 'G', st->config_path);
  write_records(fp, st);
  const bool io_ok = (ferror(fp) == 0);
  if ((fclose(fp) != 0) || !io_ok) {
    (void)remove(tmp);
    return k_ra8_fail;
  }
  if (rename(tmp, path) != 0) {
    (void)remove(tmp);
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
  return n;
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

/** @brief Apply one `C` record line; false when it is malformed/over-capacity. */
RA8_INTERNAL static bool apply_chapter(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf < (size_t)k_c_fields) {
    return false;
  }
  const long         num = strtol(fld[k_c_number], nullptr, (int)k_state_dec_base);
  mdl_chapter_rec_t* rec = mdl_state_add_chapter(st, fld[k_c_id], fld[k_c_url], num);
  if (rec == nullptr) {
    return false;
  }
  rec->complete   = (strtol(fld[k_c_done], nullptr, (int)k_state_dec_base) != 0);
  rec->page_count = (uint16_t)strtoul(fld[k_c_pages], nullptr, (int)k_state_dec_base);
  rec->pages_done = (uint16_t)strtoul(fld[k_c_ready], nullptr, (int)k_state_dec_base);
  rec->fetched_at = (int64_t)strtoll(fld[k_c_epoch], nullptr, (int)k_state_dec_base);
  return true;
}

/** @brief Apply one `P` record line; false when it is malformed/over-capacity. */
RA8_INTERNAL static bool apply_page(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf < (size_t)k_p_fields) {
    return false;
  }
  const uint64_t uh = (uint64_t)strtoull(fld[k_p_urlhash], nullptr, (int)k_state_hex_base);
  const uint64_t ch = (uint64_t)strtoull(fld[k_p_content], nullptr, (int)k_state_hex_base);
  return mdl_state_add_page(st, uh, ch, fld[k_p_relpath]);
}

/** @brief Apply one already-split record line; false when it is malformed. */
RA8_INTERNAL static bool apply_line(mdl_state_t* st, char* fld[], size_t nf, bool* seen_v)
{
  const char type = fld[0][0];
  if ((fld[0][1] != '\0')) {
    return false; /* a record type is exactly one character */
  }
  if (!*seen_v) {
    /* The first record MUST be a version line this build understands. */
    if ((type != 'V') || (nf < 2U) ||
        (strtoul(fld[1], nullptr, (int)k_state_dec_base) != (unsigned long)k_mdl_state_version)) {
      return false;
    }
    *seen_v = true;
    return true;
  }
  switch (type) {
    case 'V':
      return true; /* a duplicate version line is harmless */
    case 'S':
      set_opt(st->series_url, sizeof(st->series_url), (nf > 1U) ? fld[1] : "");
      return true;
    case 'T':
      set_opt(st->series_title, sizeof(st->series_title), (nf > 1U) ? fld[1] : "");
      return true;
    case 'N':
      set_opt(st->site_name, sizeof(st->site_name), (nf > 1U) ? fld[1] : "");
      return true;
    case 'H':
      set_opt(st->site_host, sizeof(st->site_host), (nf > 1U) ? fld[1] : "");
      return true;
    case 'G':
      set_opt(st->config_path, sizeof(st->config_path), (nf > 1U) ? fld[1] : "");
      return true;
    case 'C':
      return apply_chapter(st, fld, nf);
    case 'P':
      return apply_page(st, fld, nf);
    default:
      return false;
  }
}

/** @brief Parse an open state stream into `st`; false on the first malformed line. */
RA8_INTERNAL static bool parse_stream(FILE* fp, mdl_state_t* st)
{
  char line[k_state_line_max];
  bool seen_v = false;
  while (fgets(line, (int)sizeof(line), fp) != nullptr) {
    chomp(line);
    if ((line[0] == '\0') || (line[0] == '#')) {
      continue; /* blank or comment */
    }
    char*        fld[k_state_max_flds];
    const size_t nf = split_tabs(line, fld, (size_t)k_state_max_flds);
    if (!apply_line(st, fld, nf, &seen_v)) {
      return false;
    }
  }
  return seen_v; /* a file with no version record is corrupt */
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
  const bool ok = parse_stream(fp, st);
  (void)fclose(fp);
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

/** @brief Number span [lo,hi] of complete chapters; false when there are none. */
RA8_INTERNAL static bool complete_span(const mdl_state_t* st, long* lo, long* hi, size_t* n)
{
  bool any = false;
  *n       = 0U;
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    if (!st->chapters[i].complete) {
      continue;
    }
    const long num = st->chapters[i].number;
    if (!any) {
      *lo = num;
      *hi = num;
      any = true;
    } else {
      *lo = (num < *lo) ? num : *lo;
      *hi = (num > *hi) ? num : *hi;
    }
    *n += 1U;
  }
  return any;
}

/** @brief True when a complete chapter with parsed number `num` exists. */
RA8_INTERNAL static bool has_complete_number(const mdl_state_t* st, long num)
{
  for (uint16_t i = 0U; i < st->chapter_count; ++i) {
    if (st->chapters[i].complete && (st->chapters[i].number == num)) {
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
    (void)snprintf(buf, cap, "no chapters complete");
    return;
  }
  (void)snprintf(buf, cap, "%zu chapter(s) complete, %ld..%ld", n, lo, hi);
  append_gaps(st, lo, hi, buf, cap);
}
