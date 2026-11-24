/**
 * @file test_reflow_shaping.c
 * @brief End-to-end C ABI verification of text shaping, UAX #9 bidi,
 *        combining-mark rendering, RTL link hit-testing, and cache
 *        serialisation against a bound RGB888 framebuffer (`RA8FW-1173`).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "reflow.h"
#include "reflow_cache.h"
#include "unity_minimal.h"

/** @brief Test glyph descriptor for building a synthetic SFNT TTF blob. */
typedef struct {
  uint32_t cp;
  uint16_t adv;
  int16_t  x0;
  int16_t  y0;
  int16_t  x1;
  int16_t  y1;
} internal_test_glyph_t;

/** @brief Pixel bounding box scan result over the RGB888 framebuffer. */
typedef struct {
  uint32_t count;
  int32_t  min_x;
  int32_t  max_x;
  int32_t  min_y;
  int32_t  max_y;
} internal_px_box_t;

enum {
  k_sh_vp_w     = 200,
  k_sh_vp_h     = 120,
  k_sh_ttf_cap  = 4096,
  k_sh_fb_bytes = k_sh_vp_w * k_sh_vp_h * 3,
  k_sh_cache_sz = 16384,
};

static uint8_t  s_ttf[k_sh_ttf_cap];
static size_t   s_ttf_len = 0;
static reflow_t s_eng_a;
static reflow_t s_eng_b;
static uint8_t  s_fb_a[k_sh_fb_bytes];
static uint8_t  s_fb_b[k_sh_fb_bytes];
static uint8_t  s_cache_blob[k_sh_cache_sz];

static const internal_test_glyph_t k_test_glyphs[] = {
  { 0U,      500U, 50,  0,    450, 500 }, /* 0: .notdef */
  { ' ',     300U, 0,   0,    0,   0   }, /* 1: space */
  { 'e',     700U, 100, 0,    600, 350 }, /* 2: latin base */
  { 0x0301U, 0U,   0,   0,    200, 140 }, /* 3: combining acute */
  { 0x0300U, 0U,   0,   0,    200, 140 }, /* 4: combining grave */
  { 0x0628U, 500U, 50,  0,    450, 350 }, /* 5: Beh isolated */
  { 0xFE91U, 500U, 50,  0,    450, 350 }, /* 6: Beh initial */
  { 0xFE90U, 500U, 50,  0,    450, 350 }, /* 7: Beh final */
  { 0x0644U, 500U, 50,  0,    450, 550 }, /* 8: Lam */
  { 0x0627U, 400U, 100, 0,    300, 550 }, /* 9: Alef */
  { 0xFEFBU, 700U, 80,  0,    620, 550 }, /* 10: Lam-Alef isolated */
  { 0xFEFCU, 700U, 80,  0,    620, 550 }, /* 11: Lam-Alef final */
  { 0x0915U, 700U, 120, 0,    600, 360 }, /* 12: Devanagari Ka */
  { 0x093FU, 400U, 50,  0,    220, 740 }, /* 13: Devanagari Matra I (taller left bar) */
  { 0x0E01U, 500U, 60,  0,    440, 400 }, /* 14: Thai Ko Kai */
  { 0x0E33U, 500U, 60,  0,    440, 400 }, /* 15: Thai Sara Am */
  { 0x0E4DU, 0U,   120, 520,  260, 620 }, /* 16: Thai Nikhahit */
  { 0x0E32U, 400U, 80,  0,    320, 400 }, /* 17: Thai Sara Aa */
  { 0x0E49U, 0U,   100, 660,  280, 780 }, /* 18: Thai Mai Tho */
};

static void internal_wr_u16(uint8_t* dst, size_t off, uint16_t v)
{
  dst[off]     = (uint8_t)((v >> 8U) & 0xFFU);
  dst[off + 1] = (uint8_t)(v & 0xFFU);
}

static void internal_wr_u32(uint8_t* dst, size_t off, uint32_t v)
{
  internal_wr_u16(dst, off, (uint16_t)((v >> 16U) & 0xFFFFU));
  internal_wr_u16(dst, off + 2U, (uint16_t)(v & 0xFFFFU));
}

static size_t internal_emit_cmap4(uint8_t* out, size_t n)
{
  uint16_t idx[32];
  size_t   cnt = 0U;
  for (size_t i = 0U; i < n; i++) {
    if (k_test_glyphs[i].cp != 0U) {
      idx[cnt++] = (uint16_t)i;
    }
  }
  for (size_t a = 0U; a < cnt; a++) {
    for (size_t b = a + 1U; b < cnt; b++) {
      if (k_test_glyphs[idx[b]].cp < k_test_glyphs[idx[a]].cp) {
        uint16_t tmp = idx[a];
        idx[a]       = idx[b];
        idx[b]       = tmp;
      }
    }
  }
  size_t   seg_cnt    = cnt + 1U;
  uint16_t seg_cnt_x2 = (uint16_t)(seg_cnt * 2U);
  uint16_t sr         = 2U;
  uint16_t es         = 0U;
  while ((uint32_t)sr * 2U <= (uint32_t)seg_cnt_x2) {
    sr = (uint16_t)(sr * 2U);
    es++;
  }
  size_t sub_off = 12U;
  internal_wr_u16(out, 0U, 0U);
  internal_wr_u16(out, 2U, 1U);
  internal_wr_u16(out, 4U, 3U);
  internal_wr_u16(out, 6U, 1U);
  internal_wr_u32(out, 8U, 12U);
  internal_wr_u16(out, sub_off + 0U, 4U);
  internal_wr_u16(out, sub_off + 4U, 0U);
  internal_wr_u16(out, sub_off + 6U, seg_cnt_x2);
  internal_wr_u16(out, sub_off + 8U, sr);
  internal_wr_u16(out, sub_off + 10U, es);
  internal_wr_u16(out, sub_off + 12U, (uint16_t)(seg_cnt_x2 - sr));

  size_t p = sub_off + 14U;
  for (size_t k = 0U; k < cnt; k++) {
    internal_wr_u16(out, p + k * 2U, (uint16_t)k_test_glyphs[idx[k]].cp);
  }
  internal_wr_u16(out, p + cnt * 2U, 0xFFFFU);
  p += seg_cnt * 2U;
  internal_wr_u16(out, p, 0U);
  p += 2U;
  for (size_t k = 0U; k < cnt; k++) {
    internal_wr_u16(out, p + k * 2U, (uint16_t)k_test_glyphs[idx[k]].cp);
  }
  internal_wr_u16(out, p + cnt * 2U, 0xFFFFU);
  p += seg_cnt * 2U;
  for (size_t k = 0U; k < cnt; k++) {
    uint16_t delta = (uint16_t)(idx[k] - (uint16_t)k_test_glyphs[idx[k]].cp);
    internal_wr_u16(out, p + k * 2U, delta);
  }
  internal_wr_u16(out, p + cnt * 2U, 1U);
  p += seg_cnt * 2U;
  memset(out + p, 0, seg_cnt * 2U);
  p += seg_cnt * 2U;
  internal_wr_u16(out, sub_off + 2U, (uint16_t)(p - sub_off));
  return p;
}

static size_t internal_emit_glyf_and_loca(uint8_t* glyf, uint8_t* loca, size_t n)
{
  size_t goff = 0U;
  for (size_t i = 0U; i < n; i++) {
    const internal_test_glyph_t* g = &k_test_glyphs[i];
    internal_wr_u16(loca, i * 2U, (uint16_t)(goff / 2U));
    if (g->x1 <= g->x0 || g->y1 <= g->y0) {
      continue;
    }
    internal_wr_u16(glyf, goff + 0U, 1U);
    internal_wr_u16(glyf, goff + 2U, (uint16_t)g->x0);
    internal_wr_u16(glyf, goff + 4U, (uint16_t)g->y0);
    internal_wr_u16(glyf, goff + 6U, (uint16_t)g->x1);
    internal_wr_u16(glyf, goff + 8U, (uint16_t)g->y1);
    internal_wr_u16(glyf, goff + 10U, 3U);
    internal_wr_u16(glyf, goff + 12U, 0U);
    memset(glyf + goff + 14U, 0x01, 4U);
    internal_wr_u16(glyf, goff + 18U, (uint16_t)g->x0);
    internal_wr_u16(glyf, goff + 20U, 0U);
    internal_wr_u16(glyf, goff + 22U, (uint16_t)(g->x1 - g->x0));
    internal_wr_u16(glyf, goff + 24U, 0U);
    internal_wr_u16(glyf, goff + 26U, (uint16_t)g->y0);
    internal_wr_u16(glyf, goff + 28U, (uint16_t)(g->y1 - g->y0));
    internal_wr_u16(glyf, goff + 30U, 0U);
    internal_wr_u16(glyf, goff + 32U, (uint16_t)(g->y0 - g->y1));
    goff += 34U;
  }
  internal_wr_u16(loca, n * 2U, (uint16_t)(goff / 2U));
  return goff;
}

static size_t internal_pad4(size_t len)
{
  return (len + 3U) & ~3U;
}

static void internal_append_tbl(const char tag[4], const uint8_t* src, size_t len, uint32_t dir_idx, size_t* data_off)
{
  size_t off = *data_off;
  size_t rec = 12U + (size_t)dir_idx * 16U;
  memcpy(s_ttf + rec, tag, 4U);
  internal_wr_u32(s_ttf, rec + 4U, 0U);
  internal_wr_u32(s_ttf, rec + 8U, (uint32_t)off);
  internal_wr_u32(s_ttf, rec + 12U, (uint32_t)len);
  memcpy(s_ttf + off, src, len);
  size_t padded = internal_pad4(len);
  if (padded > len) {
    memset(s_ttf + off + len, 0, padded - len);
  }
  *data_off = off + padded;
}

static void internal_build_test_ttf(void)
{
  uint8_t cmap[512];
  uint8_t glyf[1024];
  uint8_t loca[64];
  uint8_t head[54];
  uint8_t hhea[36];
  uint8_t hmtx[128];
  uint8_t maxp[32];
  size_t  n = sizeof(k_test_glyphs) / sizeof(k_test_glyphs[0]);

  memset(cmap, 0, sizeof(cmap));
  memset(head, 0, sizeof(head));
  memset(hhea, 0, sizeof(hhea));
  memset(hmtx, 0, sizeof(hmtx));
  memset(maxp, 0, sizeof(maxp));

  size_t cmap_len = internal_emit_cmap4(cmap, n);
  size_t glyf_len = internal_emit_glyf_and_loca(glyf, loca, n);
  size_t loca_len = (n + 1U) * 2U;

  internal_wr_u32(head, 0U, 0x00010000U);
  internal_wr_u32(head, 12U, 0x5F0F3CF5U);
  internal_wr_u16(head, 18U, 1000U);
  internal_wr_u16(head, 38U, 1000U);
  internal_wr_u16(head, 40U, 1000U);
  internal_wr_u16(head, 42U, 1000U);
  internal_wr_u16(head, 50U, 0U);

  internal_wr_u32(hhea, 0U, 0x00010000U);
  internal_wr_u16(hhea, 4U, 800U);
  internal_wr_u16(hhea, 6U, (uint16_t)(-200));
  internal_wr_u16(hhea, 8U, 200U);
  internal_wr_u16(hhea, 34U, (uint16_t)n);

  for (size_t i = 0U; i < n; i++) {
    internal_wr_u16(hmtx, i * 4U, k_test_glyphs[i].adv);
    internal_wr_u16(hmtx, i * 4U + 2U, (uint16_t)k_test_glyphs[i].x0);
  }

  internal_wr_u32(maxp, 0U, 0x00010000U);
  internal_wr_u16(maxp, 4U, (uint16_t)n);

  internal_wr_u32(s_ttf, 0U, 0x00010000U);
  internal_wr_u16(s_ttf, 4U, 7U);
  internal_wr_u16(s_ttf, 6U, 64U);
  internal_wr_u16(s_ttf, 8U, 2U);
  internal_wr_u16(s_ttf, 10U, 48U);

  size_t off = 12U + 7U * 16U;
  /* SFNT table directory records must be sorted strictly ascending by tag. */
  internal_append_tbl("cmap", cmap, cmap_len, 0U, &off);
  internal_append_tbl("glyf", glyf, glyf_len, 1U, &off);
  internal_append_tbl("head", head, sizeof(head), 2U, &off);
  internal_append_tbl("hhea", hhea, sizeof(hhea), 3U, &off);
  internal_append_tbl("hmtx", hmtx, n * 4U, 4U, &off);
  internal_append_tbl("loca", loca, loca_len, 5U, &off);
  internal_append_tbl("maxp", maxp, sizeof(maxp), 6U, &off);
  s_ttf_len = off;
}

static internal_px_box_t internal_scan_ink(const uint8_t* fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  internal_px_box_t b = { 0U, k_sh_vp_w, -1, k_sh_vp_h, -1 };
  for (int32_t y = y0; y < y1; y++) {
    for (int32_t x = x0; x < x1; x++) {
      size_t idx = ((size_t)y * k_sh_vp_w + (size_t)x) * 3U;
      /* Background fill is pure white (0xFF, 0xFF, 0xFF); any darker pixel is ink. */
      if (fb[idx] < 0xF0U || fb[idx + 1U] < 0xF0U || fb[idx + 2U] < 0xF0U) {
        b.count++;
        if (x < b.min_x) {
          b.min_x = x;
        }
        if (x > b.max_x) {
          b.max_x = x;
        }
        if (y < b.min_y) {
          b.min_y = y;
        }
        if (y > b.max_y) {
          b.max_y = y;
        }
      }
    }
  }
  return b;
}

static void internal_test_combining_mark_render(void)
{
  const char html[] = "<p>e\xcc\x81\xcc\x80</p>";
  uint32_t   pages  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, reflow_init(k_sh_vp_w, k_sh_vp_h, s_ttf, s_ttf_len, 18U, 0x000000U, 0x0000FFU, &s_eng_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_layout_chapter(&s_eng_a, (const uint8_t*)html, sizeof(html) - 1U, &pages));
  TEST_ASSERT_EQ(1U, pages);

  memset(s_fb_a, 0xFF, sizeof(s_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb_a, k_sh_vp_w, k_sh_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_a, 0U, NULL));

  internal_px_box_t box = internal_scan_ink(s_fb_a, 0, 0, k_sh_vp_w, k_sh_vp_h);
  TEST_ASSERT(box.count > 20U);
  TEST_ASSERT(box.min_x >= 14 && box.max_x <= 32);
  /* Verify non-blank gap row between stacked acute mark and base 'e' top. */
  int32_t has_high_mark = (box.min_y < 24);
  TEST_ASSERT(has_high_mark);
  TEST_ASSERT_EQ(k_ra8_ok, reflow_close(&s_eng_a));
}

static void internal_test_arabic_rtl_link_and_zwnj(void)
{
  const char rtl_html[] =
    "<p dir=\"rtl\"><a href=\"rtl.xhtml\">\xd9\x84\xd8\xa7 </a>\xd8\xa8</p>"
    "<table><tr><td>\xd8\xa8\xd9\x84\xd8\xa7</td></tr></table>";
  const char ctrl_html[] = "<p dir=\"rtl\">\xd8\xa8\xe2\x80\x8c\xd8\xa8\xe2\x80\x8f</p>";
  const char plain_html[] = "<p dir=\"rtl\">\xd8\xa8\xd8\xa8</p>";
  uint32_t   pages        = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, reflow_init(k_sh_vp_w, k_sh_vp_h, s_ttf, s_ttf_len, 18U, 0x000000U, 0x0000FFU, &s_eng_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_layout_chapter(&s_eng_a, (const uint8_t*)rtl_html, sizeof(rtl_html) - 1U, &pages));
  memset(s_fb_a, 0xFF, sizeof(s_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb_a, k_sh_vp_w, k_sh_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_a, 0U, NULL));

  /* Paragraph is right-aligned near right margin (200 - 16 = 184). */
  internal_px_box_t pbox = internal_scan_ink(s_fb_a, 0, 0, k_sh_vp_w, 40);
  TEST_ASSERT(pbox.count > 20U);
  TEST_ASSERT(pbox.max_x > 165);

  /* Link target covers rightmost Lam-Alef cluster in the RTL line. */
  uint32_t hoff = 0U;
  uint32_t hlen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, reflow_hit_test_link(&s_eng_a, 0U, pbox.max_x - 2, pbox.min_y + 3, &hoff, &hlen));
  TEST_ASSERT_EQ(9U, hlen);
  /* Leftmost isolated Beh glyph is outside the link hitbox. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, reflow_hit_test_link(&s_eng_a, 0U, pbox.min_x + 1, pbox.min_y + 3, &hoff, &hlen));

  /* Control characters (ZWNJ/RLM) emit zero tofu glyphs: ink box count stays <= plain 2-glyph run. */
  memset(s_fb_a, 0xFF, sizeof(s_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_layout_chapter(&s_eng_a, (const uint8_t*)plain_html, sizeof(plain_html) - 1U, &pages));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_a, 0U, NULL));
  internal_px_box_t plain_box = internal_scan_ink(s_fb_a, 0, 0, k_sh_vp_w, 60);

  memset(s_fb_a, 0xFF, sizeof(s_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_layout_chapter(&s_eng_a, (const uint8_t*)ctrl_html, sizeof(ctrl_html) - 1U, &pages));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_a, 0U, NULL));
  internal_px_box_t ctrl_box = internal_scan_ink(s_fb_a, 0, 0, k_sh_vp_w, 60);
  TEST_ASSERT_EQ(plain_box.count, ctrl_box.count);
  TEST_ASSERT_EQ(k_ra8_ok, reflow_close(&s_eng_a));
}

static void internal_test_indic_thai_and_cache_roundtrip(void)
{
  const char doc[] = "<p>\xe0\xa4\x95\xe0\xa4\xbf</p><p>\xe0\xb8\x81\xe0\xb9\x89\xe0\xb8\xb3</p>";
  uint32_t   pages = 0U;
  size_t     need  = 0U;
  size_t     wrote = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, reflow_init(k_sh_vp_w, k_sh_vp_h, s_ttf, s_ttf_len, 18U, 0x000000U, 0x0000FFU, &s_eng_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_layout_chapter(&s_eng_a, (const uint8_t*)doc, sizeof(doc) - 1U, &pages));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_cache_size(&s_eng_a, &need));
  TEST_ASSERT(need <= sizeof(s_cache_blob));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_cache_serialize(&s_eng_a, (const uint8_t*)doc, sizeof(doc) - 1U, s_cache_blob, sizeof(s_cache_blob), &wrote));

  memset(s_fb_a, 0xFF, sizeof(s_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb_a, k_sh_vp_w, k_sh_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_a, 0U, NULL));

  /* Line 1 Devanagari 'ki': taller left Matra I appears left of shorter Ka. */
  internal_px_box_t left_matra = internal_scan_ink(s_fb_a, 14, 0, 22, 42);
  internal_px_box_t right_ka   = internal_scan_ink(s_fb_a, 24, 0, 42, 42);
  TEST_ASSERT(left_matra.count > 4U && right_ka.count > 4U);
  TEST_ASSERT(left_matra.min_y < right_ka.min_y);

  /* Restore shaped glyphs/pages into a fresh engine via cache and assert identical pixels. */
  memset(&s_eng_b, 0, sizeof(s_eng_b));
  memset(s_fb_b, 0xFF, sizeof(s_fb_b));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_init(k_sh_vp_w, k_sh_vp_h, s_ttf, s_ttf_len, 18U, 0x000000U, 0x0000FFU, &s_eng_b));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_cache_load(&s_eng_b, (const uint8_t*)doc, sizeof(doc) - 1U, s_cache_blob, wrote));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb_b, k_sh_vp_w, k_sh_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_render_page(&s_eng_b, 0U, NULL));
  TEST_ASSERT_EQ(0, memcmp(s_fb_a, s_fb_b, sizeof(s_fb_a)));

  TEST_ASSERT_EQ(k_ra8_ok, reflow_close(&s_eng_a));
  TEST_ASSERT_EQ(k_ra8_ok, reflow_close(&s_eng_b));
}

/** @brief Entry point executing C ABI shaping, bidi, render, and cache tests. */
int main(void)
{
  internal_build_test_ttf();
  internal_test_combining_mark_render();
  internal_test_arabic_rtl_link_and_zwnj();
  internal_test_indic_thai_and_cache_roundtrip();
  return 0;
}
