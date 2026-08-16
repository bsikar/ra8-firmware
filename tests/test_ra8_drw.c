/**
 * @file test_ra8_drw.c
 * @brief Unit tests for ra8_drw.c (2D Drawing Engine driver)
 *
 * @details
 * This sibling owns the lifecycle, status + IRQ paths, power transitions,
 * software reset, cache flush, gradient + pattern + blend + colour-key,
 * and HWREVISION readback tests. The texture / CLUT, fill rect, textured
 * rect blit, line, triangle, display-list trigger, performance-counter,
 * and MC/DC vector tests live in test_ra8_drw_render.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_drw.h"
#include "ra8_drw_internal.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum drw_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_drw_probe_irqctl = 0xABCDUL, /**< Planted in IRQCTL to prove the read reaches the register. */
} drw_fixture_t;

/**
 * @enum drw_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_drw_probe_perfcount2 =
    0xCAFEBABEUL, /**< Planted in PERFCOUNT2, unlike PERFCOUNT1, so the two are never confused. */
  /** Planted in PERFCOUNT1. */
  k_drw_probe_perfcount1 = 0xDEADBEEFUL,
} drw_fixture2_t;

/**
 * @enum ra8_drw_test_const_t
 * @brief Test-side bound constants used by the cases below.
 */
typedef enum : uint32_t {
  k_ra8_drw_test_fb_addr_lo   = 0x68000000UL, /**< SDRAM head, mmap-backed.   */
  k_ra8_drw_test_tex_addr     = 0x68100000UL, /**< Texture base.              */
  k_ra8_drw_test_dlist_addr   = 0x68200000UL, /**< Display list base.         */
  k_ra8_drw_test_pitch_px     = 1024U,        /**< 1024-px wide framebuffer.  */
  k_ra8_drw_test_rect_w       = 16U,          /**< 16-pixel-wide test rect.   */
  k_ra8_drw_test_rect_h       = 8U,           /**< 8-pixel-tall test rect.    */
  k_ra8_drw_test_rect_x       = 4U,           /**< Test rect origin X.        */
  k_ra8_drw_test_rect_y       = 2U,           /**< Test rect origin Y.        */
  k_ra8_drw_test_rect_color   = 0xFFAA5500UL, /**< ARGB orange.               */
  k_ra8_drw_test_color2       = 0x80112233UL, /**< Secondary ARGB stop.       */
  k_ra8_drw_test_status_seed  = 0x000000FFUL, /**< Seed STATUS for read test. */
  k_ra8_drw_test_too_big_dim  = 2048U,        /**< Exceeds HUM 1024 max.      */
  k_ra8_drw_test_zero_dim     = 0U,           /**< Below min.                 */
  k_ra8_drw_test_subpixel     = 16U,          /**< 1 px == 16 sub-pixels.     */
  k_ra8_drw_test_cb_ctx_val   = 0xCAFEU,      /**< Cookie for cb context.     */
  k_ra8_drw_test_color_key    = 0x0000FF00UL, /**< Pure-green colour key.     */
  k_ra8_drw_test_clut_count   = 8U,           /**< CLUT entries to upload.    */
  k_ra8_drw_test_pattern      = 0xA5U,        /**< Pattern bitmap byte.       */
  k_ra8_drw_test_perf_budget  = 256U,         /**< wait_idle poll budget.     */
  k_ra8_drw_test_clut_cap     = 256U,         /**< CLUT max entries.          */
  k_ra8_drw_test_clut_off     = 0x10U,        /**< CLUT offset value.         */
  k_ra8_drw_test_global_alpha = 0x80U,        /**< 50%-ish alpha test value.  */
} ra8_drw_test_const_t;

/**
 * @brief Reset hardware fake, ref-count table, and callback latches.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a default config that points at the SDRAM mmap region.
 */
static ra8_drw_config_t make_cfg(void)
{
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)k_ra8_drw_test_fb_addr_lo,
    .pitch_px               = (uint16_t)k_ra8_drw_test_pitch_px,
    .format                 = k_ra8_drw_writefmt_argb8888,
    .enable_caches          = true,
    .enable_buffered_writes = true,
  };
  return cfg;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_happy(void)
{
  TEST_BEGIN("drw init happy");
  prep();

  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  TEST_ASSERT_EQ(cfg.framebuffer_addr, *ra8_drw_reg32(k_ra8_drw_off_origin));
  TEST_ASSERT_EQ(cfg.pitch_px, *ra8_drw_reg32(k_ra8_drw_off_pitch));
  /* CONTROL2 carries the WRITEFORMAT bits for ARGB8888 (code 2) PLUS the two
   * surface defaults whose reset values are wrong for a solid fill, both
   * established on an EK-RA8D2:
   *   WRITEALPHA = 01 (source alpha). At its reset 00 the framebuffer alpha
   *   comes from COLOR2, which is 0 for a fill, so an opaque 0xFF00FF00 fill
   *   read back 0x0000FF00 on silicon.
   *   BDI. At reset both blend factors are 1, i.e. dst = src + dst, so a fill
   *   ADDED to the framebuffer: 0xFF00FF00 over 0x00000010 gave 0xFF00FF10. */
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  const uint32_t expected_ctl2 =
    ((uint32_t)k_ra8_drw_writefmt_argb8888 << k_ra8_drw_control2_writeformat_pos) |
    ((uint32_t)k_ra8_drw_writealpha_pixel_cov << k_ra8_drw_control2_writealpha_pos) |
    (uint32_t)k_ra8_drw_control2_bdi;
  TEST_ASSERT_EQ(expected_ctl2, ctl2);
  /* CACHECTL should enable both FB and texture caches. */
  TEST_ASSERT_EQ((k_ra8_drw_cachectl_cenablefx | k_ra8_drw_cachectl_cenabletx),
                 *ra8_drw_reg32(k_ra8_drw_off_cachectl));
  /* DBWER bit must be set when buffered writes are enabled. */
  TEST_ASSERT_EQ(k_ra8_drw_dbwer_bwe, *ra8_drw_reg32(k_ra8_drw_off_dbwer));

  TEST_END("drw init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("drw init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_init(nullptr));
  TEST_END("drw init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_caches_off_dbwer_off(void)
{
  TEST_BEGIN("drw init caches off, DBWER off");
  prep();
  ra8_drw_config_t cfg       = make_cfg();
  cfg.enable_caches          = false;
  cfg.enable_buffered_writes = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_cachectl));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_dbwer));
  TEST_END("drw init caches off, DBWER off");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_irq_and_callback(void)
{
  TEST_BEGIN("drw deinit clears IRQs + callback");
  prep();

  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));
  /* Pretend a prior IRQ snuck through. */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_drw_probe_irqctl;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_deinit());
  TEST_ASSERT_EQ(k_ra8_drw_irqctl_all_clr, *ra8_drw_reg32(k_ra8_drw_off_irqctl));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_cachectl));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_dbwer));
  TEST_END("drw deinit clears IRQs + callback");
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_status_reads_seed(void)
{
  TEST_BEGIN("drw get_status reads seed");
  prep();

  *ra8_drw_reg32(k_ra8_drw_off_status) = (uint32_t)k_ra8_drw_test_status_seed;

  uint32_t status = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_get_status(&status));
  TEST_ASSERT_EQ(k_ra8_drw_test_status_seed, status);
  TEST_END("drw get_status reads seed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_null_out(void)
{
  TEST_BEGIN("drw get_status null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_get_status(nullptr));
  TEST_END("drw get_status null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_hwrevision(void)
{
  TEST_BEGIN("drw get_hwrevision");
  prep();
  *ra8_drw_reg32(k_ra8_drw_off_hwrevision) = (uint32_t)k_ra8_drw_hwrev_reset_value;
  uint32_t rev                             = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_get_hwrevision(&rev));
  TEST_ASSERT_EQ(k_ra8_drw_hwrev_reset_value, rev);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_get_hwrevision(nullptr));
  TEST_END("drw get_hwrevision");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_writes_irqctl(void)
{
  TEST_BEGIN("drw clear_status writes IRQCTL");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_clear_status((uint32_t)k_ra8_drw_irqctl_dlistirqclr));
  TEST_ASSERT_EQ(k_ra8_drw_irqctl_dlistirqclr, *ra8_drw_reg32(k_ra8_drw_off_irqctl));
  TEST_END("drw clear_status writes IRQCTL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq_enables_combines_w1c(void)
{
  TEST_BEGIN("drw set_irq_enables ORs W1C");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_irq_enables((uint32_t)k_ra8_drw_irqctl_all_en));
  const uint32_t expected = (uint32_t)k_ra8_drw_irqctl_all_en | (uint32_t)k_ra8_drw_irqctl_all_clr;
  TEST_ASSERT_EQ(expected, *ra8_drw_reg32(k_ra8_drw_off_irqctl));
  TEST_END("drw set_irq_enables ORs W1C");
}

static uint32_t s_drw_cb_count;
static uint32_t s_drw_cb_last_status;
static void*    s_drw_cb_last_ctx;

static void stub_drw_cb(void* ctx, uint32_t status)
{
  ++s_drw_cb_count;
  s_drw_cb_last_status = status;
  s_drw_cb_last_ctx    = ctx;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("drw attach + dispatch");
  prep();
  s_drw_cb_count       = 0U;
  s_drw_cb_last_status = 0U;
  s_drw_cb_last_ctx    = nullptr;

  void* const ctx = (void*)(uintptr_t)k_ra8_drw_test_cb_ctx_val;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_attach_handler(stub_drw_cb, ctx));

  /* Seed STATUS so the dispatcher snapshots a non-zero value. */
  *ra8_drw_reg32(k_ra8_drw_off_status) = (uint32_t)k_ra8_drw_status_dlistirq;

  ra8_drw_dispatch();
  TEST_ASSERT_EQ(1, s_drw_cb_count);
  TEST_ASSERT_EQ(k_ra8_drw_status_dlistirq, s_drw_cb_last_status);
  TEST_ASSERT(s_drw_cb_last_ctx == ctx);
  TEST_ASSERT_EQ(k_ra8_drw_irqctl_all_clr, *ra8_drw_reg32(k_ra8_drw_off_irqctl));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_attach_handler(nullptr, nullptr));
  ra8_drw_dispatch();
  TEST_ASSERT_EQ(1, s_drw_cb_count);
  TEST_END("drw attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wait_idle_paths(void)
{
  TEST_BEGIN("drw wait_idle happy + timeout + zero budget");
  prep();
  /* Idle case: STATUS busy bits all 0. */
  *ra8_drw_reg32(k_ra8_drw_off_status) = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_wait_idle((uint32_t)k_ra8_drw_test_perf_budget));

  /* Timeout case: BUSYENUM stuck. */
  *ra8_drw_reg32(k_ra8_drw_off_status) = (uint32_t)k_ra8_drw_status_busyenum;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_drw_wait_idle(2U));

  /* Zero budget rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_wait_idle(0U));
  TEST_END("drw wait_idle happy + timeout + zero budget");
}

/* =============================================================================
 * Power transitions / reset / cache
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_power_transition(void)
{
  TEST_BEGIN("drw power transition");
  prep();

  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_exit_stop());
  TEST_END("drw power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_clears_perfcount(void)
{
  TEST_BEGIN("drw reset clears perf counters");
  prep();
  *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = k_drw_probe_perfcount1;
  *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = k_drw_probe_perfcount2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_reset());
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_perfcount1));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_perfcount2));
  TEST_END("drw reset clears perf counters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_cache_flush(void)
{
  TEST_BEGIN("drw cache_flush sets bits");
  prep();
  /* Pre-set enable bits to verify they survive the flush write. */
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = (uint32_t)k_ra8_drw_cachectl_all_en;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_cache_flush(true, true));
  const uint32_t expected =
    (uint32_t)k_ra8_drw_cachectl_all_en | (uint32_t)k_ra8_drw_cachectl_all_flush;
  TEST_ASSERT_EQ(expected, *ra8_drw_reg32(k_ra8_drw_off_cachectl));

  /* No-op call still returns OK. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_cache_flush(false, false));
  TEST_END("drw cache_flush sets bits");
}

/* =============================================================================
 * Surface / blend / colour
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_gradient(void)
{
  TEST_BEGIN("drw set_gradient writes COLOR1+COLOR2");
  prep();
  const ra8_drw_gradient_t g = {
    .color1_argb8888 = (uint32_t)k_ra8_drw_test_rect_color,
    .color2_argb8888 = (uint32_t)k_ra8_drw_test_color2,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_gradient(&g));
  TEST_ASSERT_EQ(k_ra8_drw_test_rect_color, *ra8_drw_reg32(k_ra8_drw_off_color1));
  TEST_ASSERT_EQ(k_ra8_drw_test_color2, *ra8_drw_reg32(k_ra8_drw_off_color2));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_set_gradient(nullptr));
  TEST_END("drw set_gradient writes COLOR1+COLOR2");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_pattern_and_enable(void)
{
  TEST_BEGIN("drw set_pattern + enable bits");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_pattern((uint8_t)k_ra8_drw_test_pattern));
  TEST_ASSERT_EQ(k_ra8_drw_test_pattern, *ra8_drw_reg32(k_ra8_drw_off_pattern));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_pattern_enable(true, true));
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_patternenable) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_patternsourcel5) != 0UL);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_pattern_enable(false, false));
  const uint32_t ctl2b = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2b & (uint32_t)k_ra8_drw_control2_patternenable) == 0UL);
  TEST_ASSERT((ctl2b & (uint32_t)k_ra8_drw_control2_patternsourcel5) == 0UL);
  TEST_END("drw set_pattern + enable bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_blend(void)
{
  TEST_BEGIN("drw set_blend full source-over");
  prep();
  /* Pre-load COLOR1 with RGB THROUGH THE DRIVER so the alpha-only update in
   * set_blend has a known base. The DRW register file is write-only on
   * silicon (HUM 62.2.x), so set_blend preserves RGB from the driver's
   * software shadow -- a raw MMIO poke would bypass that shadow and assert
   * a readback behaviour the real hardware does not have. */
  const ra8_drw_gradient_t pre = {
    .color1_argb8888 = 0x00112233UL,
    .color2_argb8888 = 0UL,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_gradient(&pre));
  const ra8_drw_blend_t b = {
    .use_alpha_channel = true,
    .src_factor        = true,
    .dst_factor        = true,
    .src_invert        = false,
    .dst_invert        = false,
    .src_factor_alpha  = true,
    .dst_factor_alpha  = true,
    .src_invert_alpha  = false,
    .dst_invert_alpha  = true,
    .use_color2_dst    = true,
    .global_alpha      = (uint8_t)k_ra8_drw_test_global_alpha,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_blend(&b));
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_useacb) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_bsf) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_bdf) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_bdia) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_bc2) != 0UL);

  /* COLOR1 alpha byte updated, RGB preserved. */
  const uint32_t color1   = *ra8_drw_reg32(k_ra8_drw_off_color1);
  const uint32_t expected = ((uint32_t)k_ra8_drw_test_global_alpha << 24U) | 0x00112233UL;
  TEST_ASSERT_EQ(expected, color1);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_set_blend(nullptr));
  TEST_END("drw set_blend full source-over");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_color_key(void)
{
  TEST_BEGIN("drw set_color_key writes COLKEY + bit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_color_key((uint32_t)k_ra8_drw_test_color_key, true));
  TEST_ASSERT_EQ(k_ra8_drw_test_color_key, *ra8_drw_reg32(k_ra8_drw_off_colkey));
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_colkeyenable) != 0UL);

  /* Disable: bit should clear. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_color_key(0UL, false));
  const uint32_t ctl2b = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2b & (uint32_t)k_ra8_drw_control2_colkeyenable) == 0UL);
  TEST_END("drw set_color_key writes COLKEY + bit");
}
int main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_caches_off_dbwer_off();
  test_deinit_clears_irq_and_callback();
  test_get_status_reads_seed();
  test_get_status_null_out();
  test_get_hwrevision();
  test_clear_status_writes_irqctl();
  test_set_irq_enables_combines_w1c();
  test_attach_and_dispatch();
  test_wait_idle_paths();
  test_power_transition();
  test_reset_clears_perfcount();
  test_cache_flush();
  test_set_gradient();
  test_set_pattern_and_enable();
  test_set_blend();
  test_set_color_key();
  return 0;
}
