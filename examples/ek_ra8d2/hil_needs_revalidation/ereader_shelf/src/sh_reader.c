/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_reader.c
 * @brief Full-book reader: per-chapter text extraction, word-wrap, pagination.
 *
 * @details
 * Reads the open book chapter by chapter. For the current chapter it pulls the
 * plain text (ra8_book_chapter_text), folds typographic Unicode the bitmap font
 * cannot draw to ASCII, greedily word-wraps to the panel width, and paginates.
 * Page turns advance within a chapter and cross chapter boundaries in either
 * direction, so the whole book reads continuously -- not just one chapter.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "sh_app.h"

/**
 * @struct sh_fold_t
 * @brief One typographic-Unicode -> ASCII substitution.
 */
typedef struct {
  uint32_t    cp;  /**< Source Unicode code point.             */
  const char* rep; /**< ASCII replacement ("" drops the char). */
} sh_fold_t;

/** @brief Punctuation the ASCII bitmap font cannot draw, folded to ASCII. */
static const sh_fold_t k_sh_fold[] = {
  {0x2014U, "--"}, {0x2013U, "-"},  {0x2012U, "-"},  {0x2010U, "-"},   {0x2011U, "-"},
  {0x2018U, "'"},  {0x2019U, "'"},  {0x201AU, "'"},  {0x2032U, "'"},   {0x201CU, "\""},
  {0x201DU, "\""}, {0x201EU, "\""}, {0x2033U, "\""}, {0x2026U, "..."}, {0x00A0U, " "},
  {0x2007U, " "},  {0x2009U, " "},  {0x202FU, " "},  {0x200AU, " "},   {0x2002U, " "},
  {0x2003U, " "},
};

/** @enum sh_u8_t @brief UTF-8 lead/continuation masks. */
typedef enum : uint32_t {
  k_sh_u8_ascii = 0x80U,   /**< Below this: bare ASCII.    */
  k_sh_u8_lead2 = 0xC0U,   /**< 2-byte lead prefix.        */
  k_sh_u8_lead3 = 0xE0U,   /**< 3-byte lead prefix.        */
  k_sh_u8_lead4 = 0xF0U,   /**< 4-byte lead prefix.        */
  k_sh_u8_mask2 = 0xE0U,   /**< 2-byte lead mask.          */
  k_sh_u8_mask3 = 0xF0U,   /**< 3-byte lead mask.          */
  k_sh_u8_mask4 = 0xF8U,   /**< 4-byte lead mask.          */
  k_sh_u8_data2 = 0x1FU,   /**< 2-byte payload bits.       */
  k_sh_u8_data3 = 0x0FU,   /**< 3-byte payload bits.       */
  k_sh_u8_cont  = 0x3FU,   /**< Continuation payload bits. */
  k_sh_u8_repl  = 0xFFFDU, /**< U+FFFD replacement.        */
} sh_u8_t;

/** @enum sh_u8_shift_t @brief Continuation-byte shifts. */
typedef enum : uint8_t {
  k_sh_u8_sh6  = 6U,  /**< One continuation byte.  */
  k_sh_u8_sh12 = 12U, /**< Two continuation bytes. */
} sh_u8_shift_t;

/** @brief Decode one UTF-8 sequence at @p s; sets @p cp, returns bytes consumed. */
static size_t sh_utf8_decode(const uint8_t* s, size_t n, uint32_t* cp)
{
  const uint8_t b = s[0];
  if (b < (uint8_t)k_sh_u8_ascii) {
    *cp = b;
    return 1U;
  }
  if ((((uint32_t)b & k_sh_u8_mask2) == k_sh_u8_lead2) && (n >= 2U)) {
    *cp = (((uint32_t)b & k_sh_u8_data2) << k_sh_u8_sh6) | ((uint32_t)s[1] & k_sh_u8_cont);
    return 2U;
  }
  if ((((uint32_t)b & k_sh_u8_mask3) == k_sh_u8_lead3) && (n >= 3U)) {
    *cp = (((uint32_t)b & k_sh_u8_data3) << k_sh_u8_sh12) |
          (((uint32_t)s[1] & k_sh_u8_cont) << k_sh_u8_sh6) | ((uint32_t)s[2] & k_sh_u8_cont);
    return 3U;
  }
  *cp = k_sh_u8_repl;
  return ((((uint32_t)b & k_sh_u8_mask4) == k_sh_u8_lead4) && (n >= 4U)) ? 4U : 1U;
}

/** @brief ASCII replacement for code point @p cp; "" to drop it. */
static const char* sh_fold_lookup(uint32_t cp)
{
  for (size_t i = 0U; i < (sizeof(k_sh_fold) / sizeof(k_sh_fold[0])); ++i) {
    if (k_sh_fold[i].cp == cp) {
      return k_sh_fold[i].rep;
    }
  }
  return "";
}

/** @brief Fold ::sh_state_t::text in place from UTF-8 to ASCII. */
static void sh_ascii_fold(void)
{
  size_t r = 0U;
  size_t w = 0U;
  while (r < g_sh.text_len) {
    uint32_t     cp  = 0U;
    const size_t adv = sh_utf8_decode((const uint8_t*)&g_sh.text[r], g_sh.text_len - r, &cp);
    r += adv;
    if (cp < k_sh_u8_ascii) {
      g_sh.text[w++] = (char)cp;
      continue;
    }
    for (const char* p = sh_fold_lookup(cp); *p != '\0'; ++p) {
      g_sh.text[w++] = *p; /* every rep is no longer than its UTF-8 source: w <= r */
    }
  }
  g_sh.text_len = w;
}

/** @brief Skip the next word in the text at @p i; returns the index past it. */
static size_t sh_skip_word(size_t i)
{
  while ((i < g_sh.text_len) && (g_sh.text[i] != ' ') && (g_sh.text[i] != '\n')) {
    ++i;
  }
  return i;
}

/** @brief Wrap one line from @p i; store [*ls,*le); return the next index. */
static size_t sh_wrap_line(size_t i, int32_t chars, size_t* ls, size_t* le)
{
  while ((i < g_sh.text_len) && (g_sh.text[i] == ' ')) {
    ++i;
  }
  *ls         = i;
  *le         = i;
  int32_t col = 0;
  while (i < g_sh.text_len) {
    if (g_sh.text[i] == '\n') {
      ++i;
      break;
    }
    const size_t  ws   = i;
    const size_t  we   = sh_skip_word(i);
    const int32_t need = (col > 0 ? col + 1 : 0) + (int32_t)(we - ws);
    if ((col > 0) && (need > chars)) {
      i = ws;
      break;
    }
    i   = ((we < g_sh.text_len) && (g_sh.text[we] == ' ')) ? (we + 1U) : we;
    *le = we;
    col = need;
  }
  return i;
}

/** @brief Greedy word-wrap ::sh_state_t::text into ::sh_state_t::lines. */
static void sh_reader_wrap(int32_t chars)
{
  g_sh.line_count = 0U;
  size_t i        = 0U;
  while ((i < g_sh.text_len) && (g_sh.line_count < (uint32_t)k_sh_max_lines)) {
    size_t ls = 0U;
    size_t le = 0U;
    i         = sh_wrap_line(i, chars, &ls, &le);
    if (ls >= g_sh.text_len) {
      break;
    }
    g_sh.lines[g_sh.line_count].off = (uint32_t)ls;
    g_sh.lines[g_sh.line_count].len = (uint16_t)(le - ls);
    g_sh.line_count++;
  }
}

/** @brief Reader lines that fit on one page. */
static uint32_t sh_lines_per_page(void)
{
  return (uint32_t)(((int32_t)k_sh_fb_h - (int32_t)k_sh_bar_h) / (int32_t)k_sh_line_h);
}

void sh_reader_load_chapter(uint32_t chapter)
{
  g_sh.chapter  = chapter;
  g_sh.page     = 0U;
  g_sh.text_len = 0U;
  size_t tl     = 0U;
  if (sh_book_chapter_text(chapter, g_sh.text, sizeof g_sh.text, &tl) == k_ra8_ok) {
    g_sh.text_len = tl;
  }
  sh_ascii_fold();
  sh_reader_wrap(sh_cells((int32_t)k_sh_fb_w - (2 * (int32_t)k_sh_pad)));
  const uint32_t lpp   = sh_lines_per_page();
  uint32_t       pages = (lpp == 0U) ? 1U : ((g_sh.line_count + lpp - 1U) / lpp);
  g_sh.chap_pages      = (pages == 0U) ? 1U : pages;
}

/** @enum sh_content_const_t @brief Front-matter skip threshold. */
typedef enum : uint32_t {
  k_sh_content_min = 1200U, /**< Min chars to treat a chapter as readable prose. */
} sh_content_const_t;

uint32_t sh_reader_first_content(void)
{
  for (uint32_t c = 0U; c < g_sh.chapter_count; ++c) {
    size_t tl = 0U;
    if ((sh_book_chapter_text(c, g_sh.text, sizeof g_sh.text, &tl) == k_ra8_ok) &&
        (tl > (size_t)k_sh_content_min)) {
      return c;
    }
  }
  return 0U;
}

void sh_reader_render(void)
{
  (void)ra8_gfx_clear((uint32_t)k_sh_col_card);
  char   right[k_sh_linebuf];
  size_t rp   = 0U;
  right[rp++] = 'C';
  right[rp++] = 'h';
  right[rp++] = ' ';
  rp          = sh_fmt_uint(right, rp, g_sh.chapter + 1U);
  right[rp++] = '/';
  rp          = sh_fmt_uint(right, rp, g_sh.chapter_count);
  right[rp++] = ' ';
  right[rp++] = ' ';
  rp          = sh_fmt_uint(right, rp, g_sh.page + 1U);
  right[rp++] = '/';
  rp          = sh_fmt_uint(right, rp, g_sh.chap_pages);
  right[rp]   = '\0';
  sh_titlebar("< Contents", right);

  const uint32_t lpp   = sh_lines_per_page();
  const uint32_t first = g_sh.page * lpp;
  char           line[k_sh_linebuf];
  for (uint32_t r = 0U; r < lpp; ++r) {
    const uint32_t li = first + r;
    if (li >= g_sh.line_count) {
      break;
    }
    uint16_t len = g_sh.lines[li].len;
    if (len >= (uint16_t)sizeof line) {
      len = (uint16_t)sizeof line - 1U;
    }
    memcpy(line, &g_sh.text[g_sh.lines[li].off], len);
    line[len] = '\0';
    const int32_t y =
      (int32_t)k_sh_bar_h + (int32_t)k_sh_card_pad + ((int32_t)r * (int32_t)k_sh_line_h);
    (void)ra8_gfx_text_out((int32_t)k_sh_pad,
                           y,
                           line,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_sh_col_ink,
                           (uint32_t)k_sh_col_card);
  }
}

bool sh_reader_turn(int32_t dir)
{
  if (dir > 0) {
    if ((g_sh.page + 1U) < g_sh.chap_pages) {
      g_sh.page++;
      return true;
    }
    if ((g_sh.chapter + 1U) < g_sh.chapter_count) {
      sh_reader_load_chapter(g_sh.chapter + 1U);
      return true;
    }
    return false;
  }
  if (dir < 0) {
    if (g_sh.page > 0U) {
      g_sh.page--;
      return true;
    }
    if (g_sh.chapter > 0U) {
      sh_reader_load_chapter(g_sh.chapter - 1U);
      g_sh.page = (g_sh.chap_pages > 0U) ? (g_sh.chap_pages - 1U) : 0U;
      return true;
    }
    return false;
  }
  return false;
}

void sh_reader_prefetch_adjacent(void)
{
  if (g_sh.open_fmt != k_sh_fmt_rabook) {
    return; /* EPUB reads through a different backend -- no ra8_vmem cache to warm. */
  }
  if (g_sh.book_src.vm == nullptr) {
    return; /* resident/unbound source -- nothing to page in. */
  }
  const uint32_t ch = g_sh.chapter;
  if ((ch + 1U) < g_sh.chapter_count) {
    (void)ra8_book_src_prefetch_chapter(&g_sh.book_src, ch + 1U); /* warm N+1 */
  }
  if (ch > 0U) {
    (void)ra8_book_src_prefetch_chapter(&g_sh.book_src, ch - 1U); /* warm N-1 */
  }
}
