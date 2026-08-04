/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_book.c
 * @brief Format-agnostic book backend: ra8_book (.rabook) + ra8_epub (.epub).
 *
 * @details
 * The shelf/cover/TOC/reader screens never touch a parser directly -- they go
 * through this backend, which dispatches on the open book's ::sh_book_fmt_t:
 *
 *   - `.rabook`: the pre-parsed ra8_book blob, demand-paged through the chunked
 *     RBKC reader bound by sh_paged.c (#204/#205 -- always paged, never a
 *     whole-book inflate). Chapter text, TOC labels, metadata strings and the
 *     4bpp cover are all copied out of the page cache via ra8_book_src_read /
 *     ra8_book_chapter_text_src.
 *   - `.epub`: parsed on-device by ra8_epub (SD only), STREAMED straight off
 *     the card (#230 -- the source file stays held open in sh_sd.c and every
 *     ZIP entry is seek+read on demand; no whole-file buffer, no book-size
 *     ceiling). Chapters arrive as XHTML, which this module strips to the same
 *     plain text the reader word-wraps; the cover is a compressed JPEG/PNG
 *     decoded via ra8_img_decode_blit.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_epub.h"
#include "ra8_gfx.h"
#include "ra8_reflow_image.h"
#include "sh_app.h"

/** @enum sh_book_const_t @brief Backend buffer + format constants. */
typedef enum : uint32_t {
  k_sh_xhtml_cap = 256U * 1024U, /**< One chapter's XHTML scratch.  */
  k_sh_cover_cap = 512U * 1024U, /**< Compressed cover image bytes. */
  k_sh_arena_cap = 64U * 1024U,  /**< stb_image decode bump arena.  */
  k_sh_col_white = 0xFFFFFFU,    /**< Thumbnail letterbox fill.     */
  k_sh_luma_r    = 77U,          /**< Rec.601 red weight (/256).    */
  k_sh_luma_g    = 150U,         /**< Rec.601 green weight.         */
  k_sh_luma_b    = 29U,          /**< Rec.601 blue weight.          */
  k_sh_luma_sh   = 8U,           /**< Rec.601 divide shift (/256).  */
  k_sh_r5_sh     = 11U,          /**< RGB565 red shift.             */
  k_sh_g6_sh     = 5U,           /**< RGB565 green shift.           */
  k_sh_r5_mask   = 0x1FU,        /**< 5-bit channel mask.           */
  k_sh_g6_mask   = 0x3FU,        /**< 6-bit channel mask.           */
  k_sh_5to8      = 3U,           /**< 5-bit -> 8-bit shift.         */
  k_sh_6to8      = 2U,           /**< 6-bit -> 8-bit shift.         */
} sh_book_const_t;

static ra8_epub_book_t s_epub; /**< The open EPUB (in_use when active). */

[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_xhtml[k_sh_xhtml_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_cover[k_sh_cover_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_arena[k_sh_arena_cap];
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static uint16_t s_thumb_rgb[k_sh_thumb_w * k_sh_thumb_h];

/* ---- XHTML -> plain text -------------------------------------------------- */

/** @brief Is @p tag (length @p n) a block element that should break the line? */
static bool sh_xhtml_is_block(const char* tag, size_t n)
{
  static const char* const k_block[] = {
    "p",          "br",      "div",    "h1",     "h2",     "h3",        "h4",  "h5",
    "h6",         "li",      "ul",     "ol",     "tr",     "hr",        "pre", "section",
    "blockquote", "article", "header", "footer", "figure", "figcaption"};
  for (size_t i = 0U; i < (sizeof(k_block) / sizeof(k_block[0])); ++i) {
    if ((strlen(k_block[i]) == n) && (strncmp(tag, k_block[i], n) == 0)) {
      return true;
    }
  }
  return false;
}

/** @brief Is @p tag an element whose text content must be suppressed entirely? */
static bool sh_xhtml_is_suppress(const char* tag, size_t n)
{
  static const char* const k_skip[] = {"head", "title", "style", "script"};
  for (size_t i = 0U; i < (sizeof(k_skip) / sizeof(k_skip[0])); ++i) {
    if ((strlen(k_skip[i]) == n) && (strncmp(tag, k_skip[i], n) == 0)) {
      return true;
    }
  }
  return false;
}

/** @enum sh_u8enc_t @brief UTF-8 encode boundaries / lead-byte prefixes. */
typedef enum : uint32_t {
  k_u8_1max  = 0x80U,  /**< Code points below: 1 byte.   */
  k_u8_2max  = 0x800U, /**< Below: 2 bytes.              */
  k_u8_cont  = 0x80U,  /**< Continuation prefix.         */
  k_u8_2lead = 0xC0U,  /**< 2-byte lead prefix.          */
  k_u8_3lead = 0xE0U,  /**< 3-byte lead prefix.          */
  k_u8_data  = 0x3FU,  /**< Continuation payload mask.   */
  k_u8_sh6   = 6U,     /**< One continuation shift.      */
  k_u8_sh12  = 12U,    /**< Two continuation shift.      */
  k_ent_dec  = 10U,    /**< Decimal numeric-entity base. */
  k_ent_hex  = 16U,    /**< Hex numeric-entity base.     */
  k_ent_a10  = 10U,    /**< Hex 'a'/'A' digit value.     */
} sh_u8enc_t;

/** @brief Append code point @p cp to @p out as UTF-8 (the reader folds to ASCII). */
static void sh_emit_cp(char* out, size_t cap, size_t* pos, uint32_t cp)
{
  if ((cp < k_u8_1max) && (*pos < cap)) {
    out[(*pos)++] = (char)cp;
  } else if ((cp < k_u8_2max) && (*pos + 1U < cap)) {
    out[(*pos)++] = (char)(k_u8_2lead | (cp >> k_u8_sh6));
    out[(*pos)++] = (char)(k_u8_cont | (cp & k_u8_data));
  } else if (*pos + 2U < cap) {
    out[(*pos)++] = (char)(k_u8_3lead | (cp >> k_u8_sh12));
    out[(*pos)++] = (char)(k_u8_cont | ((cp >> k_u8_sh6) & k_u8_data));
    out[(*pos)++] = (char)(k_u8_cont | (cp & k_u8_data));
  }
}

/** @brief Value of hex/decimal digit @p c in base @p base, or UINT32_MAX if invalid. */
static uint32_t sh_digit(char c, uint32_t base)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint32_t)(c - '0');
  }
  if ((base == k_ent_hex) && (c >= 'a') && (c <= 'f')) {
    return (uint32_t)(c - 'a') + k_ent_a10;
  }
  if ((base == k_ent_hex) && (c >= 'A') && (c <= 'F')) {
    return (uint32_t)(c - 'A') + k_ent_a10;
  }
  return UINT32_MAX;
}

/** @brief Parse a numeric entity body `#[x]NNN;` at @p s; sets @p cp + @p consumed. */
static bool sh_parse_numeric(const char* s, size_t n, size_t* consumed, uint32_t* cp)
{
  if ((n < 4U) || (s[1] != '#')) {
    return false;
  }
  const bool     hex  = (s[2] == 'x') || (s[2] == 'X');
  const uint32_t base = hex ? k_ent_hex : k_ent_dec;
  size_t         j    = hex ? 3U : 2U;
  uint32_t       v    = 0U;
  size_t         dig  = 0U;
  while ((j < n) && (s[j] != ';')) {
    const uint32_t d = sh_digit(s[j], base);
    if (d == UINT32_MAX) {
      return false;
    }
    v = (v * base) + d;
    ++j;
    ++dig;
  }
  if ((dig == 0U) || (j >= n) || (s[j] != ';')) {
    return false;
  }
  *cp       = v;
  *consumed = j + 1U;
  return true;
}

/** @brief Decode the &..; entity at @p s; sets @p cp + @p consumed; false if invalid. */
static bool sh_decode_entity(const char* s, size_t n, size_t* consumed, uint32_t* cp)
{
  static const struct {
    const char* name; /**< Name. */
    uint32_t    cp;   /**< Cp.   */
  } k_named[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
  for (size_t i = 0U; i < (sizeof(k_named) / sizeof(k_named[0])); ++i) {
    const size_t ln = strlen(k_named[i].name);
    if ((n >= ln) && (strncmp(s, k_named[i].name, ln) == 0)) {
      *cp       = k_named[i].cp;
      *consumed = ln;
      return true;
    }
  }
  return sh_parse_numeric(s, n, consumed, cp);
}

/**
 * @struct sh_strip_t
 * @brief Mutable state threaded through the XHTML strip.
 */
typedef struct {
  size_t pos;      /**< Bytes written to out.                  */
  bool   at_break; /**< Suppress a leading space (post-break). */
  int    suppress; /**< >0 inside head/title/style/script.     */
} sh_strip_t;

/** @brief Emit a paragraph break: trim trailing spaces, collapse runs of '\n'. */
static void sh_strip_break(char* out, sh_strip_t* st)
{
  while ((st->pos > 0U) && (out[st->pos - 1U] == ' ')) {
    st->pos--;
  }
  st->at_break = true;
  if ((st->pos > 0U) && (out[st->pos - 1U] != '\n')) {
    out[st->pos++] = '\n';
  }
}

/** @brief Emit one decoded code point as collapsed body text. */
static void sh_strip_text(char* out, size_t cap, sh_strip_t* st, uint32_t cp)
{
  const bool ws = (cp == ' ') || (cp == '\t') || (cp == '\n') || (cp == '\r');
  if (ws) {
    if (!st->at_break && (st->pos < cap)) {
      out[st->pos++] = ' ';
      st->at_break   = true;
    }
    return;
  }
  sh_emit_cp(out, cap, &st->pos, cp);
  st->at_break = false;
}

/** @brief Handle one `<...>` tag: break/suppress bookkeeping; returns index past `>`. */
static size_t sh_strip_tag(const uint8_t* x, size_t n, size_t i, char* out, sh_strip_t* st)
{
  size_t     j     = i + 1U;
  const bool close = (j < n) && (x[j] == '/');
  if (close) {
    ++j;
  }
  const size_t ts = j;
  while ((j < n) && (((x[j] >= 'a') && (x[j] <= 'z')) || ((x[j] >= 'A') && (x[j] <= 'Z')) ||
                     ((x[j] >= '0') && (x[j] <= '9')))) {
    ++j;
  }
  const size_t tn = j - ts;
  if (sh_xhtml_is_suppress((const char*)&x[ts], tn)) {
    st->suppress += close ? -1 : 1;
    if (st->suppress < 0) {
      st->suppress = 0;
    }
  } else if ((st->suppress == 0) && sh_xhtml_is_block((const char*)&x[ts], tn)) {
    sh_strip_break(out, st);
  }
  while ((i < n) && (x[i] != '>')) {
    ++i;
  }
  return (i < n) ? (i + 1U) : n;
}

/** @brief Strip XHTML @p x[0,n) to collapsed plain text in @p out; returns length. */
static size_t sh_strip_xhtml(const uint8_t* x, size_t n, char* out, size_t cap)
{
  sh_strip_t st  = {.pos = 0U, .at_break = true, .suppress = 0};
  size_t     i   = 0U;
  uint32_t   itr = 0U;
  while ((i < n) && (st.pos < cap) && (itr < (uint32_t)k_sh_xhtml_cap)) {
    ++itr;
    if (x[i] == '<') {
      i = sh_strip_tag(x, n, i, out, &st);
      continue;
    }
    if (st.suppress > 0) {
      ++i;
      continue;
    }
    if (x[i] == '&') {
      uint32_t cp  = 0U;
      size_t   adv = 0U;
      if (sh_decode_entity((const char*)&x[i], n - i, &adv, &cp)) {
        sh_strip_text(out, cap, &st, cp);
        i += adv;
        continue;
      }
    }
    sh_strip_text(out, cap, &st, x[i]);
    ++i;
  }
  return st.pos;
}

/* ---- cover decode --------------------------------------------------------- */

/** @brief Rec.601 luma of an RGB565 pixel (0..255). */
static uint8_t sh_luma565(uint16_t px)
{
  const uint32_t r = (((uint32_t)px >> k_sh_r5_sh) & k_sh_r5_mask) << k_sh_5to8;
  const uint32_t g = (((uint32_t)px >> k_sh_g6_sh) & k_sh_g6_mask) << k_sh_6to8;
  const uint32_t b = ((uint32_t)px & k_sh_r5_mask) << k_sh_5to8;
  return (uint8_t)(((r * k_sh_luma_r) + (g * k_sh_luma_g) + (b * k_sh_luma_b)) >> k_sh_luma_sh);
}

/** @brief Decode the open EPUB's cover into gray8 thumbnail slot @p idx. */
static bool sh_epub_thumb(uint16_t idx)
{
  size_t got = 0U;
  if ((ra8_epub_get_cover_image(&s_epub, s_cover, sizeof s_cover, &got) != k_ra8_ok) ||
      (got == 0U)) {
    return false;
  }
  /* Decode into an off-screen RGB565 scratch by rebinding ra8_gfx, read back to
   * gray8, then rebind to the live framebuffer. */
  (void)ra8_gfx_init(s_thumb_rgb,
                     (uint16_t)k_sh_thumb_w,
                     (uint16_t)k_sh_thumb_h,
                     k_ra8_gfx_format_rgb565);
  (void)ra8_gfx_clear((uint32_t)k_sh_col_white);
  ra8_img_arena_t arena = {.base = s_arena, .cap = sizeof s_arena, .offset = 0U, .live = 0U};
  int32_t         w     = 0;
  int32_t         h     = 0;
  const ra8_err_t err   = ra8_img_decode_blit(&arena,
                                              s_cover,
                                              got,
                                              0,
                                              0,
                                              (int32_t)k_sh_thumb_w,
                                              (int32_t)k_sh_thumb_h,
                                              &w,
                                              &h);
  if (err == k_ra8_ok) {
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        g_sh.thumb[idx][(y * w) + x] = sh_luma565(s_thumb_rgb[(y * (int32_t)k_sh_thumb_w) + x]);
      }
    }
    g_sh.thumb_w[idx] = (uint16_t)w;
    g_sh.thumb_h[idx] = (uint16_t)h;
  }
  (void)
    ra8_gfx_init(sh_fb_pixels(), (uint16_t)k_sh_fb_w, (uint16_t)k_sh_fb_h, k_ra8_gfx_format_rgb565);
  return err == k_ra8_ok;
}

bool sh_book_decode_thumb(uint16_t idx)
{
  g_sh.thumb_w[idx] = 0U;
  g_sh.thumb_h[idx] = 0U;
  if (g_sh.open_fmt == k_sh_fmt_epub) {
    return sh_epub_thumb(idx);
  }
  if (g_sh.book_src.vm != nullptr) {
    sh_decode_cover(idx, &g_sh.book_src);
  }
  return g_sh.thumb_w[idx] > 0U;
}

void sh_book_cover_fullscreen(int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (g_sh.open_fmt == k_sh_fmt_epub) {
    size_t got = 0U;
    if ((ra8_epub_get_cover_image(&s_epub, s_cover, sizeof s_cover, &got) == k_ra8_ok) &&
        (got > 0U)) {
      ra8_img_arena_t arena = {.base = s_arena, .cap = sizeof s_arena, .offset = 0U, .live = 0U};
      int32_t         dw    = 0;
      int32_t         dh    = 0;
      (void)ra8_img_decode_blit(&arena, s_cover, got, x, y, w, h, &dw, &dh);
    }
    return;
  }
  if (g_sh.book_src.vm == nullptr) {
    return;
  }
  const uint32_t cover = g_sh.book_src.hdr.cover_image_index;
  if (cover != k_ra8_book_nil) {
    (void)sh_image_blit_cover(&g_sh.book_src, cover, x, y, w, h, nullptr, nullptr);
  }
}

/* ---- metadata / chapters / TOC ------------------------------------------- */

/** @brief Copy @p src into @p dst (cap bytes) NUL-terminated. */
static void sh_copy_str(char* dst, size_t cap, const char* src)
{
  (void)strncpy(dst, (src != nullptr) ? src : "", cap - 1U);
  dst[cap - 1U] = '\0';
}

/**
 * @brief Copy the string-pool entry at pool offset @p off out of the paged
 *        source into @p out (cap bytes, always NUL-terminated).
 * @details Reads at most `cap - 1` bytes (clamped to the pool end) through
 *          ra8_book_src_read, then terminates. Pool strings are themselves
 *          NUL-terminated, so a shorter string keeps its own terminator and a
 *          longer one truncates. On any fault @p out is the empty string.
 */
static void sh_src_string(const ra8_book_src_t* src, uint32_t off, char* out, size_t cap)
{
  out[0] = '\0';
  if (cap < 2U) {
    return;
  }
  const uint64_t pool_end = (uint64_t)src->hdr.string_off + src->hdr.string_size;
  const uint64_t abs      = (uint64_t)src->hdr.string_off + off;
  if (abs >= pool_end) {
    return;
  }
  uint64_t n = pool_end - abs;
  if (n > (uint64_t)(cap - 1U)) {
    n = (uint64_t)(cap - 1U);
  }
  if (ra8_book_src_read(src, (uint32_t)abs, out, (uint32_t)n) != k_ra8_ok) {
    out[0] = '\0';
    return;
  }
  out[n] = '\0';
}

/** @brief Populate SD entry @p idx title/author/cover from the just-opened book. */
static void sh_book_capture_meta(uint16_t idx)
{
  sh_entry_t* e = &g_sh.entry[idx];
  if (g_sh.open_fmt == k_sh_fmt_epub) {
    ra8_epub_metadata_t m = {};
    if (ra8_epub_get_metadata(&s_epub, &m) == k_ra8_ok) {
      sh_copy_str(e->title, sizeof e->title, m.title);
      sh_copy_str(e->author, sizeof e->author, m.author);
    }
  } else if (g_sh.book_src.vm != nullptr) {
    sh_src_string(&g_sh.book_src, g_sh.book_src.hdr.title_off, e->title, sizeof e->title);
    sh_src_string(&g_sh.book_src, g_sh.book_src.hdr.author_off, e->author, sizeof e->author);
  }
  (void)sh_book_decode_thumb(idx);
}

/** @brief Bind shelf entry @p e's RBKC container as the demand-paged source. */
static bool sh_book_open_rabook(const sh_entry_t* e)
{
  if (e->from_sd) {
    uint32_t len = 0U;
    if (!sh_sd_book_open(e->sd_name, &len)) {
      return false;
    }
    return sh_paged_open(sh_sd_book_read, nullptr, (uint64_t)len);
  }
  return sh_paged_open_mram(e->blob, e->blob_len);
}

bool sh_book_open(uint16_t idx)
{
  if (idx >= g_sh.book_count) {
    return false;
  }
  if (s_epub.in_use != 0U) {
    sh_sd_close_epub(&s_epub);
  }
  sh_paged_close();
  sh_comic_close(); /* release any open comic before the next book binds */
  sh_entry_t* const e = &g_sh.entry[idx];

  if (sh_fmt_is_comic(e->fmt)) {
    /* CBZ / CBR route through sh_comic.c (image pages), not the text screens. */
    if (!sh_comic_open(idx)) {
      return false;
    }
    g_sh.open_fmt      = e->fmt;
    g_sh.chapter_count = 0U; /* comics paginate by image page, not chapters */
    return true;
  }

  if (e->fmt == k_sh_fmt_epub) {
    if (!sh_sd_open_epub(e->sd_name, &s_epub)) {
      return false;
    }
    uint16_t n = 0U;
    if (ra8_epub_get_chapter_count(&s_epub, &n) != k_ra8_ok) {
      sh_sd_close_epub(&s_epub);
      return false;
    }
    g_sh.open_fmt      = k_sh_fmt_epub;
    g_sh.chapter_count = n;
  } else {
    /* Always demand-paged through the chunked container -- no resident/paged
     * size threshold, one code path for every book size (#204/#205). */
    if (!sh_book_open_rabook(e)) {
      sh_paged_close();
      return false;
    }
    g_sh.open_fmt      = k_sh_fmt_rabook;
    g_sh.chapter_count = g_sh.book_src.hdr.chapter_count;
  }
  if (e->from_sd && (g_sh.thumb_w[idx] == 0U)) {
    sh_book_capture_meta(idx);
  }
  return true;
}

ra8_err_t sh_book_chapter_text(uint32_t chapter, char* out, size_t cap, size_t* out_len)
{
  if (g_sh.open_fmt == k_sh_fmt_epub) {
    size_t          got = 0U;
    const ra8_err_t err =
      ra8_epub_load_chapter(&s_epub, (uint16_t)chapter, s_xhtml, sizeof s_xhtml, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    *out_len = sh_strip_xhtml(s_xhtml, got, out, cap);
    return k_ra8_ok;
  }
  return ra8_book_chapter_text_src(&g_sh.book_src, chapter, out, cap, out_len);
}

/** @brief Reverse-lookup the EPUB TOC label that targets spine @p chapter. */
static bool sh_epub_label(uint32_t chapter, char* out, size_t cap)
{
  uint16_t tc = 0U;
  if (ra8_epub_get_toc_count(&s_epub, &tc) != k_ra8_ok) {
    return false;
  }
  for (uint16_t i = 0U; i < tc; ++i) {
    uint16_t mapped = UINT16_MAX;
    if ((ra8_epub_toc_entry_to_chapter(&s_epub, i, &mapped) == k_ra8_ok) &&
        ((uint32_t)mapped == chapter)) {
      ra8_epub_toc_entry_t entry = {};
      if ((ra8_epub_get_toc_entry(&s_epub, i, &entry) == k_ra8_ok) && (entry.title[0] != '\0')) {
        sh_copy_str(out, cap, entry.title);
        return true;
      }
    }
  }
  return false;
}

/** @brief Paged rabook TOC label: read the chapter record, then its string. */
static bool sh_rabook_label(uint32_t chapter, char* out, size_t cap)
{
  const ra8_book_src_t* src = &g_sh.book_src;
  if (src->vm == nullptr) {
    return false;
  }
  if (chapter >= src->hdr.chapter_count) {
    return false;
  }
  ra8_book_chapter_t ch = {};
  const uint64_t     off =
    (uint64_t)src->hdr.chapter_off + ((uint64_t)chapter * sizeof(ra8_book_chapter_t));
  if (ra8_book_src_read(src, (uint32_t)off, &ch, (uint32_t)sizeof ch) != k_ra8_ok) {
    return false;
  }
  sh_src_string(src, ch.title_off, out, cap);
  return out[0] != '\0';
}

void sh_book_chapter_label(uint32_t chapter, char* out, size_t cap)
{
  if (g_sh.open_fmt == k_sh_fmt_rabook) {
    if (sh_rabook_label(chapter, out, cap)) {
      return;
    }
  } else if (sh_epub_label(chapter, out, cap)) {
    return;
  }
  size_t p = 0U;
  out[p++] = 'C';
  out[p++] = 'h';
  out[p++] = '.';
  out[p++] = ' ';
  p        = sh_fmt_uint(out, p, chapter + 1U);
  out[p]   = '\0';
}
