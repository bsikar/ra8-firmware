/**
 * @file test_ra8_glcdc.c
 * @brief Unit tests for the GLCDC driver (ra8_glcdc.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_glcdc_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"
/**
 * @enum glcdc_probe_t
 * @brief Values planted in GLCDC registers to prove a read or write reaches them.
 *
 * @details
 * No two are equal, so a driver that read a neighbouring register -- or
 * returned a cached first result -- fails a specific assertion rather than
 * matching by coincidence.
 */
typedef enum : uint32_t {
  k_glcdc_probe_cfg       = 0xAAU,       /**< Written to SYS_CFG.               */
  k_glcdc_probe_stat_a    = 0xCAFEU,     /**< Written to SYS_STAT.              */
  k_glcdc_probe_stat_wide = 0xDEADBEEFU, /**< A full-width SYS_STAT value, so a
                                              truncated field is visible.       */
} glcdc_probe_t;

typedef enum : uint32_t {
  k_test_glcdc_fb_addr  = 0x68000000UL, /**< Framebuffer at SDRAM base. */
  k_test_glcdc_fb2_addr = 0x68800000UL, /**< Layer-2 framebuffer base.  */
} test_glcdc_fb_t;

typedef enum : uint16_t {
  k_test_glcdc_width  = 1024U, /**< Test GLCDC width.    */
  k_test_glcdc_height = 600U,  /**< Test GLCDC height.   */
  k_test_layer2_w     = 320U,  /**< Test layer2 w.       */
  k_test_layer2_h     = 240U,  /**< Test layer2 h.       */
  k_test_layer2_x     = 64U,   /**< Test layer2 x.       */
  k_test_layer2_y     = 32U,   /**< Test layer2 y.       */
  k_test_layer2_strd  = 640U,  /**< 320 * 2bpp (RGB565). */
} test_glcdc_dim_t;

/* ER-TFT070-6 raw panel timing handed to ra8_glcdc_init (the values that used
 * to be hardcoded in the driver). The expected register layouts below are
 * derived from these, so this fixture pins the driver's output value-for-value. */
typedef enum : uint16_t {
  k_test_glcdc_h_front = 160U, /**< Test GLCDC h front. */
  k_test_glcdc_h_back  = 160U, /**< Test GLCDC h back.  */
  k_test_glcdc_h_sync  = 4U,   /**< Test GLCDC h sync.  */
  k_test_glcdc_v_front = 12U,  /**< Test GLCDC v front. */
  k_test_glcdc_v_back  = 23U,  /**< Test GLCDC v back.  */
  k_test_glcdc_v_sync  = 3U,   /**< Test GLCDC v sync.  */
} test_glcdc_timing_t;

static const ra8_glcdc_timing_t k_test_glcdc_timing = {
  .h_active = (uint16_t)k_test_glcdc_width,
  .h_front  = (uint16_t)k_test_glcdc_h_front,
  .h_back   = (uint16_t)k_test_glcdc_h_back,
  .h_sync   = (uint16_t)k_test_glcdc_h_sync,
  .v_active = (uint16_t)k_test_glcdc_height,
  .v_front  = (uint16_t)k_test_glcdc_v_front,
  .v_back   = (uint16_t)k_test_glcdc_v_back,
  .v_sync   = (uint16_t)k_test_glcdc_v_sync,
};

typedef enum : uint8_t {
  k_test_alpha_half = 0x80U, /**< Test alpha half. */
  k_test_layer1     = 0U,    /**< Test layer1.     */
  k_test_layer2     = 1U,    /**< Test layer2.     */
  k_test_clut_small = 4U,    /**< Test CLUT small. */
} test_glcdc_byte_t;

/**
 * @enum test_glcdc_sysc_addr_t
 * @brief Raw SYSC addresses of the graphics power-on / LCDCLK registers.
 *
 * @details
 * Mirrors the file-static address enum in ra8_glcdc.c so the test can
 * observe the end state of internal_graphics_power_on (which now runs
 * on the host through the sim-mmap peripheral window). HUM Ch 11.2.1
 * "PRCR : Protect Register" p 440 documents PRCR; PDCTRGD / LCDCKCR /
 * LCDCKDIVCR / HOCOCR live in the same SYSC block.
 */
typedef enum : uintptr_t {
  k_test_glcdc_addr_prcr       = 0x4001E3FAUL, /**< PRCR protect register. */
  k_test_glcdc_addr_pdctrgd    = 0x4001E110UL, /**< Graphics power domain. */
  k_test_glcdc_addr_lcdckdivcr = 0x4001E05EUL, /**< LCDCLK divider.        */
  k_test_glcdc_addr_lcdckcr    = 0x4001E05FUL, /**< LCDCLK source select.  */
  k_test_glcdc_addr_hococr     = 0x4001E036UL, /**< HOCO control register. */
} test_glcdc_sysc_addr_t;

/**
 * @enum test_glcdc_sysc_val_t
 * @brief Expected end-state values of the power-on sequence registers.
 */
typedef enum : uint8_t {
  k_test_glcdc_lcdck_sel_pll1r = 0x08U, /**< LCDCKSEL[3:0] = PLL1R.        */
  k_test_glcdc_lcdckdiv4       = 0x02U, /**< LCDCKDIVCR = /4.              */
  k_test_glcdc_hococr_run      = 0x00U, /**< HOCOCR.HCSTP = 0 (running).   */
  k_test_glcdc_pdctrgd_on      = 0x00U, /**< PDCTRGD = 0 (domain powered). */
  k_test_glcdc_pdctrgd_gated   = 0x80U, /**< PDPGSF set = domain gated.    */
} test_glcdc_sysc_val_t;

/**
 * @enum test_glcdc_prcr_val_t
 * @brief Expected PRCR end state (relocked, key 0xA5).
 */
typedef enum : uint16_t {
  k_test_glcdc_prcr_relock = 0xA500U, /**< PRCR relock image. */
} test_glcdc_prcr_val_t;

typedef enum : uint32_t {
  k_test_bgc_color = 0xFF112233UL, /**< Test bgc color. */
  k_test_clut_e0   = 0xAA000001UL, /**< Test CLUT e0.   */
  k_test_clut_e1   = 0xAA000002UL, /**< Test CLUT e1.   */
  k_test_clut_e2   = 0xAA000003UL, /**< Test CLUT e2.   */
  k_test_clut_e3   = 0xAA000004UL, /**< Test CLUT e3.   */
  k_test_clut_dist = 0xDEADBEEFUL, /**< Test CLUT dist. */
} test_glcdc_word_t;

/* Packed register layouts the driver writes for the supplied ER-TFT070-6
 * timing (1024x600, h_back=160, v_back=23).  Per HUM Ch 63 BG_HSIZE/BG_VSIZE
 * carry `(back+1) << 16 | active`, GR1_LINE carries `(h-1) << 16 | (line_bytes/64 - 1)`,
 * and GR1_FMT carries `format << 28`. */
typedef enum : uint32_t {
  k_test_pgeb1_h_back_plus_1   = 161U, /**< 160 + sync_pos_min(1).      */
  k_test_pgeb1_v_back_plus_1   = 24U,  /**< 23  + sync_pos_min(1).      */
  k_test_glcdc_shift_high      = 16U,  /**< Test GLCDC shift high.      */
  k_test_glcdc_shift_flm6_fmt  = 28U,  /**< Test GLCDC shift flm6 fmt.  */
  k_test_glcdc_axi_burst_bytes = 64U,  /**< Test GLCDC axi burst bytes. */
  k_test_glcdc_bpp_rgb565      = 2U,   /**< Test GLCDC bpp rgb565.      */
  k_test_exp_bg_hsize          = ((uint32_t)k_test_pgeb1_h_back_plus_1 << 16) |
                                 (uint32_t)k_test_glcdc_width, /**< Test exp bg hsize. */
  k_test_exp_bg_vsize          = ((uint32_t)k_test_pgeb1_v_back_plus_1 << 16) |
                                 (uint32_t)k_test_glcdc_height, /**< Test exp bg vsize. */
  k_test_exp_gr1_fmt           = ((uint32_t)k_ra8_glcdc_fmt_rgb565 << 28), /**< Test exp gr1 fmt. */
  k_test_exp_gr1_line_bytes    = (uint32_t)k_test_glcdc_width *
                                 (uint32_t)k_test_glcdc_bpp_rgb565, /**< Test exp gr1 line bytes. */
  k_test_exp_gr1_line = (((uint32_t)k_test_glcdc_height - 1U) << 16) |
                        ((k_test_exp_gr1_line_bytes / (uint32_t)k_test_glcdc_axi_burst_bytes) -
                         1U), /**< Test exp gr1 line. */
} test_glcdc_packed_t;

/**
 * @enum test_glcdc_l2_packed_t
 * @brief Expected register images for ra8_glcdc_layer2_show and
 *        ra8_glcdc_layer2_chroma_key_enable.
 *
 * @details
 * layer2_show is driven with fb_addr=k_test_glcdc_fb2_addr(0x68800000),
 * panel_x=64, panel_y=32, fb_w=320, fb_h=240 (RGB565).
 * line_bytes = 320*2 = 640, datanum = 640/64-1 = 9, lnnum = 240-1 = 239.
 * chroma_key_enable is driven with key_rgb888 = 0x00FF0000 (red).
 */
typedef enum : uint32_t {
  /* ra8_glcdc_layer2_show expected images. */
  k_test_l2s_fmt  = (2UL << 28),          /**< FLM6.FORMAT[30:28] = RGB565.       */
  k_test_l2s_flm3 = (640UL << 16),        /**< line_bytes(640) in FLM3 high half. */
  k_test_l2s_line = (239UL << 16) | 9UL,  /**< (h-1)<<16 | (line_bytes/64 - 1).   */
  k_test_l2s_size = (64UL << 16) | 320UL, /**< AB3: (panel_x << 16) | fb_w.       */
  k_test_l2s_ab2  = (32UL << 16) | 240UL, /**< AB2: (panel_y << 16) | fb_h.       */
  k_test_l2s_ab7  = (0xFFUL << 16),       /**< AB7: fully-opaque constant alpha.  */
  k_test_l2s_ab1  = 3UL,                  /**< AB1.DISPSEL = ON_LOWER (3).        */
  /* ra8_glcdc_layer2_chroma_key_enable expected images -- key=0x00FF0000. */
  k_test_ckey_rgb       = 0x00FF0000UL, /**< Red chroma-key input colour.       */
  k_test_ckey_ab8       = 0xFFFF0000UL, /**< AB8: 0xFF opaque-byte | red key.   */
  k_test_ckey_ab7       = 0x01FF0001UL, /**< AB7: arcdef_op | ckon enable bits. */
  k_test_ckey_ab1_arcon = 0x00001000UL, /**< ARCON bit at AB1[12].              */
} test_glcdc_l2_packed_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_path(void)
{
  TEST_BEGIN("ra8_glcdc_init happy path");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();

  const ra8_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
    .width_px         = (uint16_t)k_test_glcdc_width,
    .height_px        = (uint16_t)k_test_glcdc_height,
    .format           = k_ra8_glcdc_fmt_rgb565,
    .timing           = k_test_glcdc_timing,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));

  /* HUM Ch 63: BG_HSIZE/BG_VSIZE carry `(back+1) << 16 | active`,
   * GR1_FMT carries `format << 28`, GR1_LINE carries
   * `(h-1) << 16 | (line_bytes/burst - 1)`. */
  TEST_ASSERT_EQ(k_test_exp_bg_hsize, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_hsize));
  TEST_ASSERT_EQ(k_test_exp_bg_vsize, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_vsize));
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_bgc));
  TEST_ASSERT_EQ(k_test_exp_gr1_fmt, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_fmt));
  TEST_ASSERT_EQ(k_test_glcdc_fb_addr, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_saddr));
  TEST_ASSERT_EQ(k_test_exp_gr1_line, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_line));

  /* The graphics power-on sequence now runs on host too: HOCO started,
   * PDCTRGD driven to "powered", LCDCLK routed to PLL1R / 4 and PRCR
   * relocked (HUM Ch 11.2.1 "PRCR : Protect Register" p 440). */
  TEST_ASSERT_EQ(k_test_glcdc_hococr_run, *(volatile uint8_t*)k_test_glcdc_addr_hococr);
  TEST_ASSERT_EQ(k_test_glcdc_pdctrgd_on, *(volatile uint8_t*)k_test_glcdc_addr_pdctrgd);
  TEST_ASSERT_EQ(k_test_glcdc_lcdck_sel_pll1r, *(volatile uint8_t*)k_test_glcdc_addr_lcdckcr);
  TEST_ASSERT_EQ(k_test_glcdc_lcdckdiv4, *(volatile uint8_t*)k_test_glcdc_addr_lcdckdivcr);
  TEST_ASSERT_EQ(k_test_glcdc_prcr_relock, *(volatile uint16_t*)k_test_glcdc_addr_prcr);

  TEST_END("ra8_glcdc_init happy path");
}

/**
 * @test test_init_power_on_wait_legs
 *
 * @par MC/DC:
 * (no compound decisions in this test -- drives the retry and
 * full-budget legs of the four best-effort power-on/LCDCLK status
 * polls in ra8_glcdc.c. The polls break out on the seam decision; on
 * exhaustion the sequence continues by design, so init still returns
 * k_ra8_ok and the register end state is identical.)
 */
static void test_init_power_on_wait_legs(void)
{
  TEST_BEGIN("ra8_glcdc_init power-on wait retry/exhaustion legs");

  const ra8_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
    .width_px         = (uint16_t)k_test_glcdc_width,
    .height_px        = (uint16_t)k_test_glcdc_height,
    .format           = k_ra8_glcdc_fmt_rgb565,
    .timing           = k_test_glcdc_timing,
  };

  /* Exhaustion leg: both PDCTRGD polls run to their full budget (the
   * sequence is best-effort and must still complete + relock PRCR). */
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  *(volatile uint8_t*)k_test_glcdc_addr_pdctrgd = (uint8_t)k_test_glcdc_pdctrgd_gated;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sim_mmio_fail_wait((const volatile void*)(uintptr_t)k_test_glcdc_addr_pdctrgd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));
  TEST_ASSERT_EQ(k_test_glcdc_pdctrgd_on, *(volatile uint8_t*)k_test_glcdc_addr_pdctrgd);
  TEST_ASSERT_EQ(k_test_glcdc_prcr_relock, *(volatile uint16_t*)k_test_glcdc_addr_prcr);

  /* Retry leg: the LCDCKCR SRDY polls converge on their 2nd poll. */
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sim_mmio_satisfy_after((const volatile void*)(uintptr_t)k_test_glcdc_addr_lcdckcr, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));
  TEST_ASSERT_EQ(k_test_glcdc_lcdck_sel_pll1r, *(volatile uint8_t*)k_test_glcdc_addr_lcdckcr);

  /* Exhaustion leg on LCDCKCR: both SRDY polls burn their budget. */
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sim_mmio_fail_wait((const volatile void*)(uintptr_t)k_test_glcdc_addr_lcdckcr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));
  TEST_ASSERT_EQ(k_test_glcdc_prcr_relock, *(volatile uint16_t*)k_test_glcdc_addr_prcr);

  TEST_END("ra8_glcdc_init power-on wait retry/exhaustion legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg_rejected(void)
{
  TEST_BEGIN("ra8_glcdc_init rejects NULL cfg");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glcdc_init(nullptr));
  TEST_END("ra8_glcdc_init rejects NULL cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_enable(void)
{
  TEST_BEGIN("ra8_glcdc_start enables engine");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_start(true));
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg));
  /* ra8_glcdc_start sequences VEN(bit8) then EN(bit0) into BG_EN per FSP
   * R_GLCDC_Start, leaving 0x101 = (VEN|EN). */
  TEST_ASSERT_EQ(0x101U, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en));
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_en));
  TEST_END("ra8_glcdc_start enables engine");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_disable(void)
{
  TEST_BEGIN("ra8_glcdc_start disables engine");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* Prime with something non-zero first. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg) = k_glcdc_probe_cfg;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_start(false));
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg));
  /* Disable path: SWRST(bit16) is held high so the controller stays
   * out of operating reset while EN/VEN drop -- BG_EN reads 0x10000. */
  TEST_ASSERT_EQ(0x10000U, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en));
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_en));
  TEST_END("ra8_glcdc_start disables engine");
}

/* ---- lifecycle + IRQ + power ---- */

static uint32_t s_glcdc_cb_count;
static uint32_t s_glcdc_cb_last_mask;

static void stub_glcdc_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_glcdc_cb_count;
  s_glcdc_cb_last_mask = mask;
}

static void prep_w61(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  s_glcdc_cb_count     = 0U;
  s_glcdc_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("glcdc deinit");
  prep_w61();
  const ra8_glcdc_config_t cfg = {.framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
                                  .width_px         = (uint16_t)k_test_glcdc_width,
                                  .height_px        = (uint16_t)k_test_glcdc_height,
                                  .format           = k_ra8_glcdc_fmt_rgb565,
                                  .timing           = k_test_glcdc_timing};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_deinit());
  TEST_END("glcdc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("glcdc status read + clear");
  prep_w61();
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_stat) = k_glcdc_probe_stat_wide;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_get_status(&mask));
  TEST_ASSERT_EQ(0xDEADBEEFU, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glcdc_get_status(nullptr));
  TEST_END("glcdc status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("glcdc attach + dispatch");
  prep_w61();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_attach_handler(stub_glcdc_cb, (void*)(uintptr_t)0x77U));
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_stat) = k_glcdc_probe_stat_a;
  ra8_glcdc_dispatch();
  TEST_ASSERT_EQ(1, s_glcdc_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_glcdc_cb_last_mask);
  TEST_END("glcdc attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("glcdc power transition");
  prep_w61();
  const ra8_glcdc_config_t cfg = {.framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
                                  .width_px         = (uint16_t)k_test_glcdc_width,
                                  .height_px        = (uint16_t)k_test_glcdc_height,
                                  .format           = k_ra8_glcdc_fmt_rgb565,
                                  .timing           = k_test_glcdc_timing};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_exit_stop());
  TEST_END("glcdc power transition");
}

/* ---- Layer-2 / blending / output stage / CLUT ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_layer2_happy(void)
{
  TEST_BEGIN("glcdc set_layer2 happy path");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  const ra8_glcdc_layer2_cfg_t cfg = {
    .framebuffer_addr  = (uint32_t)k_test_glcdc_fb2_addr,
    .line_stride_bytes = (uint32_t)k_test_layer2_strd,
    .width_px          = (uint16_t)k_test_layer2_w,
    .height_px         = (uint16_t)k_test_layer2_h,
    .pos_x             = (uint16_t)k_test_layer2_x,
    .pos_y             = (uint16_t)k_test_layer2_y,
    .format            = k_ra8_glcdc_fmt_rgb565,
    .alpha             = (uint8_t)k_test_alpha_half,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_layer2(&cfg));

  /* Format -> FLM6. */
  TEST_ASSERT_EQ(k_ra8_glcdc_fmt_rgb565, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_fmt));
  /* Base address -> FLM2. */
  TEST_ASSERT_EQ(k_test_glcdc_fb2_addr, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_saddr));
  /* Line stride -> FLM3 high half. */
  TEST_ASSERT_EQ(((uint32_t)k_test_layer2_strd << 16U), *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flm3));
  /* FLM5 = (h<<16) | w. */
  TEST_ASSERT_EQ((((uint32_t)k_test_layer2_h << 16U) | (uint32_t)k_test_layer2_w),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_line));
  /* AB3 (size) = (pos_x<<16) | w. */
  TEST_ASSERT_EQ((((uint32_t)k_test_layer2_x << 16U) | (uint32_t)k_test_layer2_w),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_size));
  /* AB2 = (pos_y<<16) | h. */
  TEST_ASSERT_EQ((((uint32_t)k_test_layer2_y << 16U) | (uint32_t)k_test_layer2_h),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab2));
  /* AB5 = (pos_x<<16) | w. */
  TEST_ASSERT_EQ((((uint32_t)k_test_layer2_x << 16U) | (uint32_t)k_test_layer2_w),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab5));
  /* AB4 = (pos_y<<16) | h. */
  TEST_ASSERT_EQ((((uint32_t)k_test_layer2_y << 16U) | (uint32_t)k_test_layer2_h),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab4));
  /* AB7 = alpha << 16. */
  TEST_ASSERT_EQ(((uint32_t)k_test_alpha_half << 16U), *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7));
  /* AB1 = DISPSEL=01 (lower-layer blend). */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1));
  /* FLMRD enabled. */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flmrd));
  /* VEN bit set. */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en));

  TEST_END("glcdc set_layer2 happy path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_layer2_null_cfg(void)
{
  TEST_BEGIN("glcdc set_layer2 rejects NULL");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glcdc_set_layer2(nullptr));
  TEST_END("glcdc set_layer2 rejects NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions -- ra8_glcdc_layer2_show contains no `&&`
 * or `||`; exercises the full register-image written for RGB565
 * layer-2 at a non-zero panel position)
 */
static void test_layer2_show_happy(void)
{
  TEST_BEGIN("glcdc layer2_show writes correct register images");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_layer2_show((uintptr_t)k_test_glcdc_fb2_addr,
                                       (uint16_t)k_test_layer2_x,
                                       (uint16_t)k_test_layer2_y,
                                       (uint16_t)k_test_layer2_w,
                                       (uint16_t)k_test_layer2_h));
  /* FLM6.FORMAT[30:28] = 2 (RGB565). */
  TEST_ASSERT_EQ(k_test_l2s_fmt, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_fmt));
  /* FLM2 = framebuffer base address. */
  TEST_ASSERT_EQ(k_test_glcdc_fb2_addr, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_saddr));
  /* FLM3[31:16] = line_bytes (640). */
  TEST_ASSERT_EQ(k_test_l2s_flm3, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flm3));
  /* FLM5 = (lnnum << 16) | datanum. */
  TEST_ASSERT_EQ(k_test_l2s_line, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_line));
  /* AB3: (panel_x << 16) | fb_w. */
  TEST_ASSERT_EQ(k_test_l2s_size, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_size));
  /* AB2: (panel_y << 16) | fb_h. */
  TEST_ASSERT_EQ(k_test_l2s_ab2, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab2));
  /* AB4 mirrors AB2 (alpha-rect V). */
  TEST_ASSERT_EQ(k_test_l2s_ab2, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab4));
  /* AB5 mirrors AB3 (alpha-rect H). */
  TEST_ASSERT_EQ(k_test_l2s_size, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab5));
  /* AB7 = fully-opaque constant alpha at ARCDEF bits. */
  TEST_ASSERT_EQ(k_test_l2s_ab7, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7));
  /* AB1 = DISPSEL = ON_LOWER (3). */
  TEST_ASSERT_EQ(k_test_l2s_ab1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1));
  /* FLMRD enabled (AXI fetch on). */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flmrd));
  /* VEN bit triggers register-update at next vsync. */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en));
  TEST_END("glcdc layer2_show writes correct register images");
}

/**
 * @par MC/DC:
 * (no compound decisions -- ra8_glcdc_layer2_chroma_key_enable has
 * no `&&` or `||`; tests AB8/AB9/AB7/AB1 images when AB1 starts at 0)
 */
static void test_layer2_chroma_key_enable_fresh_ab1(void)
{
  TEST_BEGIN("glcdc layer2_chroma_key_enable: fresh AB1 gets ARCON only");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* Red key; the driver forces an 0xFF alpha byte in AB8. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_layer2_chroma_key_enable((uint32_t)k_test_ckey_rgb));
  /* AB8 = 0xFF opaque-alpha-byte | masked 24-bit key. */
  TEST_ASSERT_EQ(k_test_ckey_ab8, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab8));
  /* AB9 = 0: transparent replacement colour. */
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab9));
  /* AB7 = arcdef_op | ckon enable bits. */
  TEST_ASSERT_EQ(k_test_ckey_ab7, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7));
  /* AB1 = ARCON only (AB1 was zero on entry). */
  TEST_ASSERT_EQ(k_test_ckey_ab1_arcon, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1));
  /* VEN bit set to commit at next vsync. */
  TEST_ASSERT_EQ(1, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en));
  TEST_END("glcdc layer2_chroma_key_enable: fresh AB1 gets ARCON only");
}

/**
 * @par MC/DC:
 * (no compound decisions; verifies that the AB1 read-modify-write
 * preserves pre-existing DISPSEL bits when ORing in ARCON)
 */
static void test_layer2_chroma_key_enable_preserves_ab1(void)
{
  TEST_BEGIN("glcdc layer2_chroma_key_enable: AB1 OR-in preserves DISPSEL");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* Pre-load AB1 with DISPSEL=ON_LOWER (3) as layer2_show leaves it. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1) = (uint32_t)k_test_l2s_ab1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_layer2_chroma_key_enable((uint32_t)k_test_ckey_rgb));
  /* AB1 must be ON_LOWER(3) | ARCON(0x1000) = 0x1003. */
  TEST_ASSERT_EQ(k_test_l2s_ab1 | (uint32_t)k_test_ckey_ab1_arcon,
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1));
  TEST_END("glcdc layer2_chroma_key_enable: AB1 OR-in preserves DISPSEL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_blend_alpha(void)
{
  TEST_BEGIN("glcdc set_blend k_ra8_blend_alpha");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_blend(k_ra8_blend_alpha, (uint8_t)k_test_alpha_half));
  /* AB1 should hold DISPSEL=2 (above) | ARCON bit (1 << 12) = 0x1002. */
  TEST_ASSERT_EQ(0x1002U, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1));
  /* AB7 = alpha << 16. */
  TEST_ASSERT_EQ(((uint32_t)k_test_alpha_half << 16U), *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab7));
  TEST_END("glcdc set_blend k_ra8_blend_alpha");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_blend_normal(void)
{
  TEST_BEGIN("glcdc set_blend k_ra8_blend_normal");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_blend(k_ra8_blend_normal, 0xFFU));
  /* DISPSEL=2 with no ARCON. */
  TEST_ASSERT_EQ(2U, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1));
  TEST_END("glcdc set_blend k_ra8_blend_normal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_blend_overwrite(void)
{
  TEST_BEGIN("glcdc set_blend k_ra8_blend_overwrite");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_blend(k_ra8_blend_overwrite, 0U));
  TEST_ASSERT_EQ(1U, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1));
  TEST_END("glcdc set_blend k_ra8_blend_overwrite");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_blend_invalid_mode(void)
{
  TEST_BEGIN("glcdc set_blend rejects invalid mode");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_glcdc_set_blend((ra8_glcdc_blend_mode_t)0xFFU, 0U));
  TEST_END("glcdc set_blend rejects invalid mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_background_color(void)
{
  TEST_BEGIN("glcdc set_background_color");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_background_color((uint32_t)k_test_bgc_color));
  TEST_ASSERT_EQ(k_test_bgc_color, *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_bgc));
  TEST_END("glcdc set_background_color");
}

/* ---- CLUT double-buffering ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clut_swap_now_false(void)
{
  TEST_BEGIN("glcdc clut double_buffered swap_now=false");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* CLUTINT.SEL starts at 0 -> active plane is 0 -> writes go to plane 1. */
  /* Stamp distinct content into the CURRENTLY active plane (plane 0)
   * so we can prove we did NOT overwrite it. */
  volatile uint32_t* const active0 =
    (volatile uint32_t*)(k_ra8_glcdc_base_addr + (uint32_t)k_ra8_glcdc_off_gr1_clut0);
  for (uint8_t i = 0U; i < (uint8_t)k_test_clut_small; ++i) {
    active0[i] = (uint32_t)k_test_clut_dist;
  }

  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    src,
                                                    (uint32_t)k_test_clut_small,
                                                    false));

  /* Active plane (CLUT0) untouched. */
  TEST_ASSERT_EQ(k_test_clut_dist, active0[0]);
  TEST_ASSERT_EQ(k_test_clut_dist, active0[3]);
  /* Inactive plane (CLUT1) has the new entries. */
  volatile uint32_t* const inactive1 =
    (volatile uint32_t*)(k_ra8_glcdc_base_addr + (uint32_t)k_ra8_glcdc_off_gr1_clut1);
  TEST_ASSERT_EQ(k_test_clut_e0, inactive1[0]);
  TEST_ASSERT_EQ(k_test_clut_e3, inactive1[3]);
  /* CLUTINT.SEL untouched (still 0). */
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_clutint));
  TEST_END("glcdc clut double_buffered swap_now=false");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clut_swap_now_true(void)
{
  TEST_BEGIN("glcdc clut double_buffered swap_now=true");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    src,
                                                    (uint32_t)k_test_clut_small,
                                                    true));
  /* CLUTINT.SEL bit (16) flipped to 1 -- triggers swap on next vsync. */
  TEST_ASSERT_EQ(0x10000U, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_clutint));
  /* Verify the second swap toggles SEL back to 0. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    src,
                                                    (uint32_t)k_test_clut_small,
                                                    true));
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_clutint));
  TEST_END("glcdc clut double_buffered swap_now=true");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clut_layer2(void)
{
  TEST_BEGIN("glcdc clut double_buffered layer 2");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer2,
                                                    src,
                                                    (uint32_t)k_test_clut_small,
                                                    false));
  /* New entries live in GR2_CLUT1 (the inactive plane). */
  volatile uint32_t* const inactive1 =
    (volatile uint32_t*)(k_ra8_glcdc_base_addr + (uint32_t)k_ra8_glcdc_off_gr2_clut1);
  TEST_ASSERT_EQ(k_test_clut_e0, inactive1[0]);
  TEST_ASSERT_EQ(k_test_clut_e3, inactive1[3]);
  TEST_END("glcdc clut double_buffered layer 2");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clut_null_rejected(void)
{
  TEST_BEGIN("glcdc clut rejects NULL src");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    nullptr,
                                                    (uint32_t)k_test_clut_small,
                                                    false));
  TEST_END("glcdc clut rejects NULL src");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clut_invalid_args(void)
{
  TEST_BEGIN("glcdc clut rejects bad layer / size");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  const uint32_t src[k_test_clut_small] = {0U, 0U, 0U, 0U};
  /* Layer >= 2 is invalid. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_clut_double_buffered(2U, src, (uint32_t)k_test_clut_small, false));
  /* 0 entries is invalid. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1, src, 0U, false));
  /* > 256 entries is invalid. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1, src, 257U, false));
  TEST_END("glcdc clut rejects bad layer / size");
}

/* ---- Output stage ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dithering_modes(void)
{
  TEST_BEGIN("glcdc set_dithering modes");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_dithering(k_ra8_dither_off));
  TEST_ASSERT_EQ(0, *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_dithering(k_ra8_dither_truncate));
  TEST_ASSERT_EQ((2U << 8U), *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_dithering(k_ra8_dither_2x2));
  TEST_ASSERT_EQ((3U << 8U), *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_glcdc_set_dithering((ra8_glcdc_dither_mode_t)0xFFU));
  TEST_END("glcdc set_dithering modes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_brightness(void)
{
  TEST_BEGIN("glcdc set_brightness");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_brightness(0x11U, 0x22U, 0x33U));
  TEST_ASSERT_EQ(0x22U, *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright1));
  TEST_ASSERT_EQ(((0x33U << 16U) | 0x11U), *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright2));
  TEST_END("glcdc set_brightness");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_contrast(void)
{
  TEST_BEGIN("glcdc set_contrast");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_set_contrast(0x40U, 0x80U, 0xC0U));
  TEST_ASSERT_EQ(((0x80U << 16U) | (0xC0U << 8U) | 0x40U),
                 *ra8_glcdc_reg32(k_ra8_glcdc_off_out_contrast));
  TEST_END("glcdc set_contrast");
}

/* ===========================================================================
 * MC/DC vector coverage for compound boolean decisions in ra8_glcdc.c.
 * Per docs/MCDC_GAPS.csv there is one decision (line 261, 2 conditions).
 * ===========================================================================
 */

/**
 * @enum test_glcdc_mcdc_t
 * @brief Numeric vectors for the MC/DC tests below.
 */
typedef enum : uint32_t {
  k_test_glcdc_mcdc_clut_zero      = 0U,   /**< Below valid range.        */
  k_test_glcdc_mcdc_clut_in_range  = 4U,   /**< Within 1..256.            */
  k_test_glcdc_mcdc_clut_too_large = 257U, /**< Above k_clut_entries=256. */
} test_glcdc_mcdc_t;

/**
 * @test test_mcdc_set_clut_double_buffered_entries
 *
 * @par MC/DC:
 * Decision: `if ((entries == 0U) || (entries > (uint32_t)k_ra8_glcdc_clut_entries))`
 * (2 conditions, libs/ra8_hal/src/ra8_glcdc.c line 261)
 *
 * - V1: entries=4   -> C1=(4==0)=F, C2=(4>256)=F -> decision F: ok
 * - V2: entries=0   -> C1=T short-circuits        -> decision T: invalid_arg
 * - V3: entries=257 -> C1=F, C2=T                 -> decision T: invalid_arg
 * V1+V2 vary C1; V1+V3 vary C2 (C1 held F). N+1=3 vectors for N=2.
 */
static void test_mcdc_set_clut_double_buffered_entries(void)
{
  TEST_BEGIN("glcdc MC/DC set_clut_double_buffered entries range");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  static const uint32_t clut[(uint32_t)k_test_glcdc_mcdc_clut_in_range] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  /* V1: in-range. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    clut,
                                                    (uint32_t)k_test_glcdc_mcdc_clut_in_range,
                                                    false));
  /* V2: zero entries. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    clut,
                                                    (uint32_t)k_test_glcdc_mcdc_clut_zero,
                                                    false));
  /* V3: above max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                    clut,
                                                    (uint32_t)k_test_glcdc_mcdc_clut_too_large,
                                                    false));
  TEST_END("glcdc MC/DC set_clut_double_buffered entries range");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_happy_path,
  test_init_power_on_wait_legs,
  test_init_null_cfg_rejected,
  test_start_enable,
  test_start_disable,
  test_deinit,
  test_status_read_and_clear,
  test_attach_and_dispatch,
  test_power_transition,
  test_set_layer2_happy,
  test_set_layer2_null_cfg,
  test_layer2_show_happy,
  test_layer2_chroma_key_enable_fresh_ab1,
  test_layer2_chroma_key_enable_preserves_ab1,
  test_set_blend_alpha,
  test_set_blend_normal,
  test_set_blend_overwrite,
  test_set_blend_invalid_mode,
  test_set_background_color,
  test_clut_swap_now_false,
  test_clut_swap_now_true,
  test_clut_layer2,
  test_clut_null_rejected,
  test_clut_invalid_args,
  test_set_dithering_modes,
  test_set_brightness,
  test_set_contrast,
  test_mcdc_set_clut_double_buffered_entries,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_glcdc.c\n");
  return 0;
}
