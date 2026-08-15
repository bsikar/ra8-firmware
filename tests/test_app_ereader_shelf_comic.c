/**
 * @file test_app_ereader_shelf_comic.c
 * @brief Host twin of the ereader_shelf comic reader (sh_comic.c, #236).
 *
 * @details
 * Two things are pinned here, so a change to the shelf's comic integration is
 * caught on the host:
 *
 *  1. The page-0 decode DIGEST the app's boot self-check (and its `hil.conf`
 *     banner) commits to. This drives the app's EXACT pipeline over the app's
 *     EXACT baked fixture header -- `ra8_comic_open` over a resident-buffer read
 *     seam, `ra8_comic_page_read` of page 0, then `ra8_img_decode_blit` into a
 *     160x120 RGB565 scratch cleared to 0x202028 -- and FNV-1a-32-hashes the
 *     scratch. The pipeline is a deterministic integer path, so the asserted
 *     numbers ARE the banner fields (`cbz=2:160x106:733D076C`, and the CBR is
 *     byte-identical). This test is the golden the ra8_emulator / silicon run is
 *     compared against.
 *
 *  2. The comic-screen decision logic: the RTL-aware edge mapping, the
 *     page-turn clamp, the tap router, and the SD extension classifier. These
 *     are `static` inside the app TU (which pulls in the app-generated
 *     `library.h`), so -- exactly like test_ereader_shelf_geometry.c mirrors
 *     sh_shelf.c's layout -- their logic is mirrored hermetically here and
 *     exercised with the MC/DC vectors documented per test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/sh_comic_fixture.h"
#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow_image.h"
#include "unity_minimal.h"

/** @enum tsc_dim_t @brief Fixture geometry, budgets, and golden digest. */
typedef enum : uint32_t {
  k_tsc_fb_w        = 160U,        /**< Self-check scratch width, pixels.  */
  k_tsc_fb_h        = 120U,        /**< Self-check scratch height, pixels. */
  k_tsc_probe_bg    = 0x202028U,   /**< Scratch clear colour (app match).  */
  k_tsc_page_cap    = 8U,          /**< Page-index capacity.               */
  k_tsc_name_cap    = 256U,        /**< Name-arena bytes.                  */
  k_tsc_pagebuf     = 16U * 1024U, /**< Encoded page-image scratch.        */
  k_tsc_arena       = 64U * 1024U, /**< stb_image decode arena.            */
  k_tsc_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.            */
  k_tsc_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                   */
  k_tsc_golden_crc  = 0x733D076CU, /**< Page-0 decode digest (both files). */
  k_tsc_golden_w    = 160U,        /**< Fitted page width.                 */
  k_tsc_golden_h    = 106U,        /**< Fitted page height.                */
  k_tsc_golden_page = 2U,          /**< Pages in each fixture archive.     */
} tsc_dim_t;

/** @brief RGB565 self-check scratch (the app's off-screen decode target twin). */
static uint16_t s_fb[(size_t)k_tsc_fb_h * (size_t)k_tsc_fb_w];
/** @brief Page-index / name-arena / page / decode buffers (app twins). */
static ra8_comic_page_t s_pages[k_tsc_page_cap];
static char             s_names[k_tsc_name_cap];
static uint8_t          s_pagebuf[k_tsc_pagebuf];
static uint8_t          s_arena[k_tsc_arena];

/** @struct tsc_src_t @brief Resident-buffer backing for the comic read seam. */
typedef struct {
  const uint8_t* d; /**< Archive bytes.  */
  size_t         n; /**< Archive length. */
} tsc_src_t;

/** @brief ::ra8_comic_read_fn over a resident buffer (the app's blob seam twin). */
static size_t tsc_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  const tsc_src_t* s = (const tsc_src_t*)ctx;
  if (off >= (uint64_t)s->n) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->n - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s->d[off], k);
  return k;
}

/** @brief FNV-1a-32 over the scratch framebuffer bytes. */
static uint32_t tsc_fnv(void)
{
  const uint8_t* p        = (const uint8_t*)s_fb;
  const size_t   fb_bytes = sizeof s_fb;
  uint32_t       h        = (uint32_t)k_tsc_fnv_offset;
  for (size_t i = 0U; i < fb_bytes; ++i) {
    h = (h ^ (uint32_t)p[i]) * (uint32_t)k_tsc_fnv_prime;
  }
  return h;
}

/** @brief Open @p data, decode page 0 into the scratch, return (pages,w,h,crc). */
static void
tsc_probe(const uint8_t* data, size_t len, uint32_t* pages, int32_t* w, int32_t* h, uint32_t* crc)
{
  ra8_comic_t c   = {};
  tsc_src_t   src = {.d = data, .n = len};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tsc_read,
                                &src,
                                (uint64_t)len,
                                s_pages,
                                (uint32_t)k_tsc_page_cap,
                                s_names,
                                (uint32_t)sizeof s_names));
  *pages     = ra8_comic_page_count(&c);
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&c, 0U, s_pagebuf, sizeof s_pagebuf, &got));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_tsc_fb_w, (uint16_t)k_tsc_fb_h, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_tsc_probe_bg));
  ra8_img_arena_t arena = {.base = s_arena, .cap = sizeof s_arena, .offset = 0U, .live = 0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena,
                                     s_pagebuf,
                                     got,
                                     0,
                                     0,
                                     (int32_t)k_tsc_fb_w,
                                     (int32_t)k_tsc_fb_h,
                                     w,
                                     h));
  *crc = tsc_fnv();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
}

/**
 * @test test_comic_page0_digest_matches_golden
 * @brief The CBZ + CBR page-0 decode reproduce the app self-check golden.
 * @details Both baked fixtures carry the same first page, so both digests equal
 *          the value the shelf `hil.conf` banner pins -- proving the integrated
 *          ra8_comic + ra8_img_decode_blit path renders both containers alike.
 *
 * @par MC/DC:
 * (no compound decision is varied by this case -- it runs the integrated CBZ/CBR
 * page-0 decode pipeline once per container and pins the deterministic golden
 * digest (pages, fitted w/h, FNV-1a). The comic-screen decision logic's MC/DC
 * vectors live in test_comic_edge_dir_rtl_mcdc, test_comic_turn_clamp_mcdc,
 * test_comic_tap_mcdc and test_comic_classify_mcdc.)
 */
static void test_comic_page0_digest_matches_golden(void)
{
  TEST_BEGIN("ereader_shelf comic: CBZ+CBR page-0 decode == 733D076C golden");
  uint32_t zp = 0U;
  int32_t  zw = 0;
  int32_t  zh = 0;
  uint32_t zc = 0U;
  tsc_probe(k_comic_cbz, (size_t)k_comic_cbz_len, &zp, &zw, &zh, &zc);
  TEST_ASSERT_EQ(k_tsc_golden_page, zp);
  TEST_ASSERT_EQ(k_tsc_golden_w, zw);
  TEST_ASSERT_EQ(k_tsc_golden_h, zh);
  TEST_ASSERT_EQ(k_tsc_golden_crc, zc);

  uint32_t rp = 0U;
  int32_t  rw = 0;
  int32_t  rh = 0;
  uint32_t rc = 0U;
  tsc_probe(k_comic_cbr, (size_t)k_comic_cbr_len, &rp, &rw, &rh, &rc);
  TEST_ASSERT_EQ(k_tsc_golden_page, rp);
  TEST_ASSERT_EQ(k_tsc_golden_crc, rc);
  TEST_ASSERT_EQ(zc, rc); /* CBZ and CBR share the page bytes */
  TEST_END("ereader_shelf comic: CBZ+CBR page-0 decode == 733D076C golden");
}

/* ---- Mirrored comic-screen decision logic (hermetic; mirrors sh_comic.c) --- */

/** @brief Mirror of sh_comic_edge_dir(): RTL-aware screen-edge -> page delta. */
static int32_t m_edge_dir(int32_t x, int32_t fb_w, bool rtl)
{
  const int32_t third = fb_w / 3;
  int32_t       base  = 0;
  if (x < third) {
    base = -1;
  } else if (x >= (fb_w - third)) {
    base = 1;
  }
  return rtl ? -base : base;
}

/** @brief Mirror of sh_comic_turn(): clamped page step in reading order. */
static bool m_turn(uint32_t* page, uint32_t count, int32_t dir)
{
  if (count == 0U) {
    return false;
  }
  if ((dir > 0) && ((*page + 1U) < count)) {
    *page += 1U;
    return true;
  }
  if ((dir < 0) && (*page > 0U)) {
    *page -= 1U;
    return true;
  }
  return false;
}

/**
 * @test test_comic_edge_dir_rtl_mcdc
 * @brief RTL mirrors the LTR edge-tap zones; the centre band is neutral.
 * @par MC/DC:
 * Decision cascade in sh_comic_edge_dir: `x < third` then `x >= fb_w - third`,
 * followed by the RTL negation `rtl ? -base : base`.
 *  - x=0 (left), LTR      -> -1     (x<third true)
 *  - x=fb_w-1 (right), LTR -> +1    (x<third false, x>=fb_w-third true)
 *  - x=fb_w/2 (centre)    ->  0     (both false: neutral band)
 *  - x=0 (left), RTL      -> +1     (RTL negates the left edge)
 *  - x=fb_w-1 (right), RTL -> -1    (RTL negates the right edge)
 * The left/right rows vary each relational independently; the centre row proves
 * the neutral fall-through; the RTL rows prove the negation branch flips sign.
 */
static void test_comic_edge_dir_rtl_mcdc(void)
{
  TEST_BEGIN("sh_comic_edge_dir: RTL-mirrored edge zones + neutral centre");
  const int32_t w = (int32_t)k_tsc_fb_w * 8; /* any width; third = w/3 */
  TEST_ASSERT_EQ(-1, m_edge_dir(0, w, false));
  TEST_ASSERT_EQ(1, m_edge_dir(w - 1, w, false));
  TEST_ASSERT_EQ(0, m_edge_dir(w / 2, w, false));
  TEST_ASSERT_EQ(1, m_edge_dir(0, w, true));
  TEST_ASSERT_EQ(-1, m_edge_dir(w - 1, w, true));
  TEST_ASSERT_EQ(0, m_edge_dir(w / 2, w, true)); /* centre is neutral either way */
  TEST_END("sh_comic_edge_dir: RTL-mirrored edge zones + neutral centre");
}

/**
 * @test test_comic_turn_clamp_mcdc
 * @brief The page turn advances / retreats one page and clamps at both ends.
 * @par MC/DC:
 * Decision 1 `(dir > 0) && ((page + 1) < count)` (advance):
 *  - dir=1, page=0, count=2 -> true  (both conditions true: advance to 1)
 *  - dir=0, page=0, count=2 -> false (varies dir>0 only)
 *  - dir=1, page=1, count=2 -> false (varies page+1<count only: at last page)
 * Decision 2 `(dir < 0) && (page > 0)` (retreat):
 *  - dir=-1, page=1 -> true  (both true: retreat to 0)
 *  - dir=0,  page=1 -> false (varies dir<0 only)
 *  - dir=-1, page=0 -> false (varies page>0 only: at first page)
 * Each pair shares the true-vector and flips exactly one condition, so each
 * condition is shown to independently drive the outcome (N+1 = 3 vectors each).
 */
static void test_comic_turn_clamp_mcdc(void)
{
  TEST_BEGIN("sh_comic_turn: clamped one-page step, both decisions MC/DC");
  uint32_t page = 0U;
  TEST_ASSERT(m_turn(&page, 2U, 1)); /* D1 true */
  TEST_ASSERT_EQ(1, page);
  page = 0U;
  TEST_ASSERT(!m_turn(&page, 2U, 0)); /* D1 dir false */
  TEST_ASSERT_EQ(0, page);
  page = 1U;
  TEST_ASSERT(!m_turn(&page, 2U, 1)); /* D1 bound false (last page) */
  TEST_ASSERT_EQ(1, page);

  page = 1U;
  TEST_ASSERT(m_turn(&page, 2U, -1)); /* D2 true */
  TEST_ASSERT_EQ(0, page);
  page = 1U;
  TEST_ASSERT(!m_turn(&page, 2U, 0)); /* D2 dir false */
  TEST_ASSERT_EQ(1, page);
  page = 0U;
  TEST_ASSERT(!m_turn(&page, 2U, -1)); /* D2 bound false (first page) */
  TEST_ASSERT_EQ(0, page);

  page = 0U;
  TEST_ASSERT(!m_turn(&page, 0U, 1)); /* empty archive: no-op */
  TEST_END("sh_comic_turn: clamped one-page step, both decisions MC/DC");
}

/** @brief Mirror of sh_comic_tap()'s non-header branch: `(d != 0) && turn(d)`. */
static bool m_tap_edge(uint32_t* page, uint32_t count, int32_t x, int32_t fb_w, bool rtl)
{
  const int32_t d = m_edge_dir(x, fb_w, rtl);
  return (d != 0) && m_turn(page, count, d);
}

/**
 * @test test_comic_tap_mcdc
 * @brief A page-zone tap turns only when it maps to a real move.
 * @par MC/DC:
 * Decision `(d != 0) && sh_comic_turn(d)` in sh_comic_tap:
 *  - x=right edge, page=0, count=2 -> true  (d=+1, turn advances: both true)
 *  - x=centre,     page=0, count=2 -> false (d==0: short-circuits, varies d!=0)
 *  - x=right edge, page=1, count=2 -> false (d=+1 but at last page: varies turn)
 * Vectors 1+2 vary `d != 0`; vectors 1+3 vary the turn result.
 */
static void test_comic_tap_mcdc(void)
{
  TEST_BEGIN("sh_comic_tap: edge tap turns iff it maps to a real move");
  const int32_t w    = (int32_t)k_tsc_fb_w * 8;
  uint32_t      page = 0U;
  TEST_ASSERT(m_tap_edge(&page, 2U, w - 1, w, false)); /* both true */
  TEST_ASSERT_EQ(1, page);
  page = 0U;
  TEST_ASSERT(!m_tap_edge(&page, 2U, w / 2, w, false)); /* d==0 short-circuit */
  TEST_ASSERT_EQ(0, page);
  page = 1U;
  TEST_ASSERT(!m_tap_edge(&page, 2U, w - 1, w, false)); /* turn false at end */
  TEST_ASSERT_EQ(1, page);
  TEST_END("sh_comic_tap: edge tap turns iff it maps to a real move");
}

/** @brief Mirror of sh_sd's 4-char extension suffix test. */
static bool m_has_ext(const char* name, const char* ext)
{
  const size_t n = strlen(name);
  return (n > 4U) && (strcmp(&name[n - 4U], ext) == 0);
}

/**
 * @test test_comic_classify_mcdc
 * @brief `.CBZ` / `.CBR` names classify as comics; others do not.
 * @par MC/DC:
 * Decision `(n > 4) && (strcmp(tail, ext) == 0)` in sh_sd_has_ext:
 *  - "MANGA01.CBZ" vs ".CBZ" -> true  (long enough AND tail matches)
 *  - ".CBZ"        vs ".CBZ" -> false (n>4 false: name is only the ext)
 *  - "MANGA01.EPB" vs ".CBZ" -> false (tail-match false)
 * The full classifier maps .CBZ and .CBR to comics and rejects .RBK/.EPB.
 */
static void test_comic_classify_mcdc(void)
{
  TEST_BEGIN("sh_sd classify: .CBZ/.CBR are comics, others are not");
  TEST_ASSERT(m_has_ext("MANGA01.CBZ", ".CBZ"));  /* both true  */
  TEST_ASSERT(!m_has_ext(".CBZ", ".CBZ"));        /* n>4 false  */
  TEST_ASSERT(!m_has_ext("MANGA01.EPB", ".CBZ")); /* tail false */
  TEST_ASSERT(m_has_ext("VOL1.CBR", ".CBR"));
  TEST_ASSERT(!m_has_ext("BOOK01.RBK", ".CBZ"));
  TEST_ASSERT(!m_has_ext("BOOK01.RBK", ".CBR"));
  TEST_END("sh_sd classify: .CBZ/.CBR are comics, others are not");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_comic_page0_digest_matches_golden();
  test_comic_edge_dir_rtl_mcdc();
  test_comic_turn_clamp_mcdc();
  test_comic_tap_mcdc();
  test_comic_classify_mcdc();
  return 0;
}
