/**
 * @file test_ra8_reflow_cache.c
 * @brief Host unit tests for the import-time pagination cache
 *        (libs/ra8_reflow/src/ra8_reflow_cache.c, #79).
 *
 * @details
 * Proves the serialise -> load round-trip restores an identical layout
 * (so `ra8_reflow_layout_chapter()` can be skipped), and that every
 * invalidation / corruption path is rejected with the documented code:
 * a changed key (font size, content) is reported stale, a flipped body
 * byte fails the checksum, a wrong magic / truncated blob is rejected,
 * and the NULL / not-initialised guards hold. The compound NULL / arg
 * guards in the three public functions carry dedicated `test_mcdc_*`
 * cases with N+1 vectors.
 *
 * The layout is populated synthetically -- the cache serialises whatever
 * is in `engine->glyphs[]` / `pages[]`, independent of how it got there
 * -- so the test is hermetic (no font rasterisation, no filesystem).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_cache.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_t_vp_w     = 320U, /**< Test viewport width.  */
  k_t_vp_h     = 240U, /**< Test viewport height. */
  k_t_font_px  = 16U,  /**< Baseline body font.   */
  k_t_font_px2 = 18U,  /**< Changed body font.    */
} t_dim_t;

typedef enum : uint32_t {
  k_t_body_color = 0x000000FFU, /**< Body colour key.       */
  k_t_link_color = 0x0000FFFFU, /**< Link colour key.       */
  k_t_glyphs     = 40U,         /**< Synthetic glyph count. */
  k_t_pages      = 3U,          /**< Synthetic page count.  */
  k_t_per_page   = 10U,         /**< Glyphs per synth page. */
  k_t_cp_span    = 26U,         /**< Code-point cycle.      */
  k_t_style_span = 4U,          /**< Style cycle.           */
  k_t_fpx_span   = 3U,          /**< Per-glyph font cycle.  */
  k_t_x_step     = 9U,          /**< Synthetic x stride.    */
  k_t_y_step     = 13U,         /**< Synthetic y stride.    */
  k_t_x_bias     = 4U,          /**< Negative-x bias.       */
  k_t_font_seed  = 7U,          /**< Font-byte generator.   */
  k_t_flip       = 0xFFU,       /**< Byte-flip corruptor.   */
} t_const_t;

typedef enum : size_t {
  k_t_font_len = 64U,   /**< Synthetic font blob length. */
  k_t_blob_cap = 4096U, /**< Serialisation buffer cap.   */
} t_size_t;

static uint8_t            s_font[k_t_font_len];
static const uint8_t      s_content[] = "<p>The quick brown fox jumps over the lazy dog.</p>";
static ra8_reflow_t       s_engine;
static uint8_t            s_blob[k_t_blob_cap];
static ra8_reflow_glyph_t s_glyph_save[k_t_glyphs];
static ra8_reflow_page_t  s_page_save[k_t_pages];

static const size_t s_content_len = sizeof(s_content);

/** @brief Initialise the shared engine at a given body font size. */
static ra8_err_t init_engine(ra8_reflow_t* eng, uint16_t font_px)
{
  return ra8_reflow_init((uint16_t)k_t_vp_w,
                         (uint16_t)k_t_vp_h,
                         s_font,
                         (size_t)k_t_font_len,
                         font_px,
                         (uint32_t)k_t_body_color,
                         (uint32_t)k_t_link_color,
                         eng);
}

/** @brief Fill the engine with a deterministic synthetic layout. */
static void build_layout(ra8_reflow_t* eng)
{
  eng->glyph_count = (uint32_t)k_t_glyphs;
  for (uint32_t i = 0U; i < (uint32_t)k_t_glyphs; ++i) {
    eng->glyphs[i].x        = (int32_t)(i * (uint32_t)k_t_x_step) - (int32_t)k_t_x_bias;
    eng->glyphs[i].y        = (int32_t)(i * (uint32_t)k_t_y_step);
    eng->glyphs[i].cp       = (int32_t)('A' + (i % (uint32_t)k_t_cp_span));
    eng->glyphs[i].color    = (uint32_t)k_t_body_color + i;
    eng->glyphs[i].font_px  = (uint16_t)((uint32_t)k_t_font_px + (i % (uint32_t)k_t_fpx_span));
    eng->glyphs[i].style    = (uint8_t)(i % (uint32_t)k_t_style_span);
    eng->glyphs[i].reserved = 0U;
  }
  eng->page_count = (uint32_t)k_t_pages;
  for (uint32_t p = 0U; p < (uint32_t)k_t_pages; ++p) {
    eng->pages[p].glyph_first = p * (uint32_t)k_t_per_page;
    eng->pages[p].glyph_count = (uint32_t)k_t_per_page;
  }
}

/** @brief Serialise the current layout into s_blob; return byte count. */
static size_t serialize_ok(void)
{
  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_cache_serialize(&s_engine,
                                            s_content,
                                            s_content_len,
                                            s_blob,
                                            (size_t)k_t_blob_cap,
                                            &n));
  return n;
}

/**
 * @test test_size_matches_serialize
 * @brief `ra8_reflow_cache_size()` equals the serialised byte count and
 *        the closed-form header + records formula.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the size contract;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_size_matches_serialize(void)
{
  TEST_BEGIN("reflow_cache: size == serialized length");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);

  size_t want = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_size(&s_engine, &want));
  const size_t formula = (size_t)k_ra8_reflow_cache_header_bytes +
                         ((size_t)k_t_glyphs * (size_t)k_ra8_reflow_cache_glyph_bytes) +
                         ((size_t)k_t_pages * (size_t)k_ra8_reflow_cache_page_bytes);
  TEST_ASSERT_EQ(formula, want);
  TEST_ASSERT_EQ(want, serialize_ok());
  TEST_END("reflow_cache: size == serialized length");
}

/**
 * @test test_roundtrip_restores_layout
 * @brief serialise -> wipe -> load reproduces every glyph and page and
 *        repoints the cached chapter buffer.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the round-trip
 * contract; the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_roundtrip_restores_layout(void)
{
  TEST_BEGIN("reflow_cache: round-trip restores layout");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  memcpy(s_glyph_save, s_engine.glyphs, sizeof(s_glyph_save));
  memcpy(s_page_save, s_engine.pages, sizeof(s_page_save));
  const size_t n = serialize_ok();

  /* Wipe the laid-out state so a successful load must rebuild it. */
  memset(s_engine.glyphs, 0, sizeof(s_glyph_save));
  memset(s_engine.pages, 0, sizeof(s_page_save));
  s_engine.glyph_count = 0U;
  s_engine.page_count  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  TEST_ASSERT_EQ(k_t_glyphs, s_engine.glyph_count);
  TEST_ASSERT_EQ(k_t_pages, s_engine.page_count);
  for (uint32_t i = 0U; i < (uint32_t)k_t_glyphs; ++i) {
    TEST_ASSERT_EQ(s_glyph_save[i].x, s_engine.glyphs[i].x);
    TEST_ASSERT_EQ(s_glyph_save[i].y, s_engine.glyphs[i].y);
    TEST_ASSERT_EQ(s_glyph_save[i].cp, s_engine.glyphs[i].cp);
    TEST_ASSERT_EQ(s_glyph_save[i].color, s_engine.glyphs[i].color);
    TEST_ASSERT_EQ(s_glyph_save[i].font_px, s_engine.glyphs[i].font_px);
    TEST_ASSERT_EQ(s_glyph_save[i].style, s_engine.glyphs[i].style);
  }
  for (uint32_t p = 0U; p < (uint32_t)k_t_pages; ++p) {
    TEST_ASSERT_EQ(s_page_save[p].glyph_first, s_engine.pages[p].glyph_first);
    TEST_ASSERT_EQ(s_page_save[p].glyph_count, s_engine.pages[p].glyph_count);
  }
  TEST_ASSERT(s_engine.xhtml_buf == s_content);
  TEST_ASSERT_EQ(s_content_len, s_engine.xhtml_len);
  TEST_END("reflow_cache: round-trip restores layout");
}

/**
 * @test test_empty_layout_roundtrip
 * @brief A zero-glyph / zero-page layout serialises to just the header
 *        and loads back cleanly.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the empty-layout edge
 * case; the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_empty_layout_roundtrip(void)
{
  TEST_BEGIN("reflow_cache: empty layout round-trip");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  const size_t n = serialize_ok();
  TEST_ASSERT_EQ(k_ra8_reflow_cache_header_bytes, n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  TEST_ASSERT_EQ(0U, s_engine.glyph_count);
  TEST_ASSERT_EQ(0U, s_engine.page_count);
  TEST_END("reflow_cache: empty layout round-trip");
}

/**
 * @test test_stale_on_font_change
 * @brief A blob made at one font size is rejected (stale) by an engine
 *        re-initialised at a different size.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the stale-key path;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_stale_on_font_change(void)
{
  TEST_BEGIN("reflow_cache: stale on font_px change");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px2));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  TEST_END("reflow_cache: stale on font_px change");
}

/**
 * @test test_stale_on_content_change
 * @brief Loading with different chapter bytes is reported stale.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the stale-content
 * path; the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_stale_on_content_change(void)
{
  TEST_BEGIN("reflow_cache: stale on content change");
  static const uint8_t other[] = "<p>A completely different chapter body here.</p>";
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_reflow_cache_load(&s_engine, other, sizeof(other), s_blob, n));
  TEST_END("reflow_cache: stale on content change");
}

/**
 * @test test_corrupt_body_checksum
 * @brief Flipping a payload byte fails the body checksum.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the checksum path;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_corrupt_body_checksum(void)
{
  TEST_BEGIN("reflow_cache: corrupt body -> checksum mismatch");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  s_blob[(size_t)k_ra8_reflow_cache_header_bytes + (size_t)k_t_glyphs] ^= (uint8_t)k_t_flip;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  TEST_END("reflow_cache: corrupt body -> checksum mismatch");
}

/**
 * @test test_bad_magic
 * @brief A blob whose magic does not match is not recognised.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the magic check;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_bad_magic(void)
{
  TEST_BEGIN("reflow_cache: bad magic -> not_found");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  s_blob[0] ^= (uint8_t)k_t_flip;
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  TEST_END("reflow_cache: bad magic -> not_found");
}

/**
 * @test test_truncated_blob
 * @brief A short header and a short body are both rejected as invalid size.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the length checks;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_truncated_blob(void)
{
  TEST_BEGIN("reflow_cache: truncated -> invalid_size");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_reflow_cache_load(&s_engine,
                                       s_content,
                                       s_content_len,
                                       s_blob,
                                       (size_t)k_ra8_reflow_cache_header_bytes - 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n - 1U));
  TEST_END("reflow_cache: truncated -> invalid_size");
}

/**
 * @test test_cap_too_small
 * @brief Serialising into an undersized buffer is rejected.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the capacity check;
 * the `&&` / `||` guards have dedicated `test_mcdc_*` cases.)
 */
static void test_cap_too_small(void)
{
  TEST_BEGIN("reflow_cache: serialize cap too small");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  size_t need = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_size(&s_engine, &need));
  size_t n = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_reflow_cache_serialize(&s_engine, s_content, s_content_len, s_blob, need - 1U, &n));
  TEST_END("reflow_cache: serialize cap too small");
}

/**
 * @test test_mcdc_cache_size
 *
 * @par MC/DC:
 * Decision in libs/ra8_reflow/src/ra8_reflow_cache.c@ra8_reflow_cache_size:
 * D1 null guard `(engine == nullptr) || (out_bytes == nullptr)`:
 * - engine=NULL               -> C1=T       -> null_ptr (C1 flips).
 * - engine ok, out_bytes=NULL -> C1=F, C2=T -> null_ptr (C2 flips).
 * - engine ok, out_bytes ok   -> C1=F, C2=F -> ok (baseline).
 * C1+baseline isolate C1; C2+baseline isolate C2. N+1 = 3.
 */
static void test_mcdc_cache_size(void)
{
  TEST_BEGIN("reflow_cache: ra8_reflow_cache_size MC/DC");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  size_t sz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_cache_size(nullptr, &sz));       /* C1=T      */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_cache_size(&s_engine, nullptr)); /* C2=T      */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_size(&s_engine, &sz));               /* C1=F,C2=F */
  /* in_use==0 simple guard (not compound). */
  ra8_reflow_t cold = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_reflow_cache_size(&cold, &sz));
  TEST_END("reflow_cache: ra8_reflow_cache_size MC/DC");
}

/**
 * @test test_mcdc_cache_serialize
 *
 * @par MC/DC:
 * Decisions in libs/ra8_reflow/src/ra8_reflow_cache.c@ra8_reflow_cache_serialize:
 * D1 null guard `(engine==nullptr) || (out_buf==nullptr) || (out_len==nullptr)`:
 * - engine=NULL  -> C1=T            -> null_ptr (C1 flips).
 * - out_buf=NULL -> C1=F,C2=T       -> null_ptr (C2 flips).
 * - out_len=NULL -> C1=F,C2=F,C3=T  -> null_ptr (C3 flips).
 * - all non-NULL -> C1=F,C2=F,C3=F  -> proceeds (baseline). N+1 = 4.
 * D2 content guard `(content==nullptr) && (content_len != 0)`:
 * - content=NULL, len!=0 -> A=T,B=T -> invalid_arg.
 * - content ok,   len!=0 -> A=F     -> proceeds (A flips; the D1 baseline).
 * - content=NULL, len==0 -> A=T,B=F -> proceeds (B flips). N+1 = 3.
 */
static void test_mcdc_cache_serialize(void)
{
  TEST_BEGIN("reflow_cache: ra8_reflow_cache_serialize MC/DC");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t cap = (size_t)k_t_blob_cap;
  size_t       n   = 0U;
  /* D1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_cache_serialize(nullptr, s_content, s_content_len, s_blob, cap, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_cache_serialize(&s_engine, s_content, s_content_len, nullptr, cap, &n));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_reflow_cache_serialize(&s_engine, s_content, s_content_len, s_blob, cap, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_cache_serialize(&s_engine, s_content, s_content_len, s_blob, cap, &n));
  /* D2 (A=F is the D1 baseline above). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_cache_serialize(&s_engine, nullptr, s_content_len, s_blob, cap, &n));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_serialize(&s_engine, nullptr, 0U, s_blob, cap, &n));
  TEST_END("reflow_cache: ra8_reflow_cache_serialize MC/DC");
}

/**
 * @test test_mcdc_cache_load
 *
 * @par MC/DC:
 * Decisions in libs/ra8_reflow/src/ra8_reflow_cache.c@ra8_reflow_cache_load:
 * D1 null guard `(engine==nullptr) || (buf==nullptr)`:
 * - engine=NULL         -> C1=T      -> null_ptr (C1 flips).
 * - engine ok, buf=NULL -> C1=F,C2=T -> null_ptr (C2 flips).
 * - engine ok, buf ok   -> C1=F,C2=F -> proceeds -> ok (baseline). N+1 = 3.
 * D2 content guard `(content==nullptr) && (content_len != 0)`:
 * - content=NULL, len!=0 -> A=T,B=T -> invalid_arg.
 * - content ok,   len!=0 -> A=F     -> proceeds -> ok (A flips; D1 baseline).
 * - content=NULL, len==0 -> A=T,B=F -> proceeds -> stale key (B flips). N+1 = 3.
 */
static void test_mcdc_cache_load(void)
{
  TEST_BEGIN("reflow_cache: ra8_reflow_cache_load MC/DC");
  TEST_ASSERT_EQ(k_ra8_ok, init_engine(&s_engine, (uint16_t)k_t_font_px));
  build_layout(&s_engine);
  const size_t n = serialize_ok();
  /* D1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_cache_load(nullptr, s_content, s_content_len, s_blob, n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_cache_load(&s_engine, s_content, s_content_len, nullptr, n));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_cache_load(&s_engine, s_content, s_content_len, s_blob, n));
  /* D2 (A=F is the D1 baseline above). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_cache_load(&s_engine, nullptr, s_content_len, s_blob, n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_reflow_cache_load(&s_engine, nullptr, 0U, s_blob, n));
  /* in_use==0 simple guard (not compound). */
  ra8_reflow_t cold = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_reflow_cache_load(&cold, s_content, s_content_len, s_blob, n));
  TEST_END("reflow_cache: ra8_reflow_cache_load MC/DC");
}

int32_t main(void)
{
  for (size_t i = 0U; i < (size_t)k_t_font_len; ++i) {
    s_font[i] = (uint8_t)((i * (size_t)k_t_font_seed) + 1U);
  }
  test_size_matches_serialize();
  test_roundtrip_restores_layout();
  test_empty_layout_roundtrip();
  test_stale_on_font_change();
  test_stale_on_content_change();
  test_corrupt_body_checksum();
  test_bad_magic();
  test_truncated_blob();
  test_cap_too_small();
  test_mcdc_cache_size();
  test_mcdc_cache_serialize();
  test_mcdc_cache_load();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_cache.c\n");
  return 0;
}
