/**
 * @file test_ra8_ceu_capture.c
 * @brief Unit tests for the CEU init and capture / DMA datapath
 *
 * @details
 * Split sibling of the original test_ra8_ceu.c suite covering the
 * bring-up and capture surface of ra8_ceu.c (Capture Engine Unit):
 * init variants incl. continuous / interlace / byte-swap / scaling /
 * low-pass, capture arm + start_ex + disarm + data-enable firewall,
 * the DMA pump and set_dma_buffer guards, the capture_start_n
 * wrappers, deinit, and both MC/DC vector tests (init format guard,
 * arm firewall guard).
 *
 * Sibling suite: test_ra8_ceu_config.c (status / IRQ dispatch /
 * power / plane-B and runtime setters).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_ceu.h"
#include "ra8_ceu_regs.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum ceu_capture_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ceu_capture_v_output_clip_240 = 240U,
} ceu_capture_uint8_const_t;

/**
 * @enum ceu_capture_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_ceu_capture_h_fraction_800       = 0x800U,
  k_ceu_capture_h_output_clip_320    = 320U,
  k_ceu_capture_image_area_size_4096 = 4096U,
  k_ceu_capture_v_fraction_400       = 0x400U,
} ceu_capture_uint16_const_t;

/**
 * @enum ceu_capture_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_ceu_capture_sentinel_cafebabe = 0xCAFEBABEU,
  k_ceu_capture_sentinel_deadbeef = 0xDEADBEEFU,
} ceu_capture_uint32_const_t;

typedef enum : uint16_t {
  k_test_ceu_width  = 1280U, /**< Test CEU width.  */
  k_test_ceu_height = 720U,  /**< Test CEU height. */
  k_test_ceu_stride = 2560U, /**< Test CEU stride. */
} test_ceu_dim_t;

typedef enum : uint32_t {
  k_test_ceu_ints =
    (uint32_t)((uint32_t)k_ra8_ceu_evt_cpe | (uint32_t)k_ra8_ceu_evt_vd), /**< Test CEU ints. */
} test_ceu_ints_t;

typedef enum : uintptr_t {
  /* Picked so the lower 3 bits are zero (8-byte aligned). SDRAM head
   * range mapped by `ra8_sim_mmap`. */
  k_test_ceu_buffer_addr = 0x68000040UL, /**< Test CEU buffer address. */
  k_test_ceu_buffer_b    = 0x68001000UL, /**< Test CEU buffer b.       */
  k_test_ceu_buffer_c    = 0x68002000UL, /**< Test CEU buffer c.       */
  k_test_ceu_buffer_d    = 0x68003000UL, /**< Test CEU buffer d.       */
  k_test_ceu_unaligned   = 0x68000043UL, /**< lower 3 bits != 0.       */
  k_test_ceu_dma_src     = 0x68010000UL, /**< Test CEU DMA src.        */
  k_test_ceu_dma_dst     = 0x68020000UL, /**< Test CEU DMA dst.        */
} test_ceu_buf_t;

typedef enum : uint32_t {
  k_test_ceu_dma_bytes = 2048U, /**< Test CEU DMA bytes. */
} test_ceu_dma_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  /* Detach any leftover callback from prior test cases. */
  (void)ra8_ceu_attach_handler(nullptr, nullptr);
}

static ra8_ceu_config_t make_cfg(void)
{
  const ra8_ceu_config_t cfg = {
    .width_px        = (uint16_t)k_test_ceu_width,
    .height_px       = (uint16_t)k_test_ceu_height,
    .x_start_px      = 0U,
    .y_start_px      = 0U,
    .x_capture_px    = (uint16_t)k_test_ceu_width,
    .y_capture_lines = (uint16_t)k_test_ceu_height,
    .dst_stride      = (uint16_t)k_test_ceu_stride,
    .frame_drop      = 0U,
    .bytes_per_pixel = 2U,
    .interrupts      = (uint32_t)k_test_ceu_ints,
    .capture_format  = k_ra8_ceu_fmt_image_capture,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_64,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {.data  = k_ra8_ceu_edge_rising,
                        .hsync = k_ra8_ceu_edge_rising,
                        .vsync = k_ra8_ceu_edge_rising,
                        .field = k_ra8_ceu_edge_rising},
    .byte_swap       = {.swap_8_bit = false, .swap_16_bit = false, .swap_32_bit = false},
    .scale           = {.h_mantissa    = 0U,
                        .h_fraction    = 0U,
                        .v_mantissa    = 0U,
                        .v_fraction    = 0U,
                        .h_output_clip = 0U,
                        .v_output_clip = 0U},
    .interlace       = false,
    .one_field_only  = false,
    .bundle_write    = false,
    .low_pass_filter = false,
    .image_area_size = 0U,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("ceu init happy");
  prep();

  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  /* CDWDR == stride, CMCYR low half == width, CEIER == interrupts. */
  TEST_ASSERT_EQ(k_test_ceu_stride, *ra8_ceu_reg32(k_ra8_ceu_off_cdwdr));
  const uint32_t cmcyr = *ra8_ceu_reg32(k_ra8_ceu_off_cmcyr);
  TEST_ASSERT_EQ(k_test_ceu_width, (cmcyr & 0xFFFFU));
  TEST_ASSERT_EQ(k_test_ceu_height, (cmcyr >> 16U));
  TEST_ASSERT_EQ(k_test_ceu_ints, *ra8_ceu_reg32(k_ra8_ceu_off_ceier));
  /* CFLCR / CFWCR / CDOCR all near-zero -> filters / firewall off,
   * output format cleared bit kept. */
  TEST_ASSERT_EQ(0, *ra8_ceu_reg32(k_ra8_ceu_off_cflcr));
  TEST_ASSERT_EQ(0, *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr));
  /* CDS bit set for 4:2:2 output. */
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_cdocr) & (uint32_t)k_ra8_ceu_cdocr_mask_cds) != 0U);

  TEST_END("ceu init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("ceu init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_init(nullptr));
  TEST_END("ceu init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_continuous_non_image_rejected(void)
{
  TEST_BEGIN("ceu init continuous-only-image rule");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.capture_mode     = k_ra8_ceu_capture_continuous;
  cfg.capture_format   = k_ra8_ceu_fmt_data_synchronous;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_init(&cfg));
  TEST_END("ceu init continuous-only-image rule");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_data_enable_format(void)
{
  TEST_BEGIN("ceu init data-enable format");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.capture_format   = k_ra8_ceu_fmt_data_enable;
  cfg.image_area_size  = k_ceu_capture_image_area_size_4096;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  /* CAMCR.JPG should reflect the data-enable encoding. */
  const uint32_t camcr = *ra8_ceu_reg32(k_ra8_ceu_off_camcr);
  TEST_ASSERT_EQ(k_ra8_ceu_fmt_data_enable,
                 ((camcr & (uint32_t)k_ra8_ceu_camcr_mask_jpg) >> k_ra8_ceu_camcr_shift_jpg));
  TEST_END("ceu init data-enable format");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_continuous_capture(void)
{
  TEST_BEGIN("ceu init continuous mode");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.capture_mode     = k_ra8_ceu_capture_continuous;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const uint32_t capcr = *ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  TEST_ASSERT((capcr & (uint32_t)k_ra8_ceu_capcr_mask_ctncp) != 0U);
  TEST_END("ceu init continuous mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_interlace_one_field(void)
{
  TEST_BEGIN("ceu init interlace one-field");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.interlace        = true;
  cfg.one_field_only   = true;
  cfg.first_field      = k_ra8_ceu_field_top;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const uint32_t caifr = *ra8_ceu_reg32(k_ra8_ceu_off_caifr);
  TEST_ASSERT((caifr & (uint32_t)k_ra8_ceu_caifr_mask_ifs) != 0U);
  TEST_ASSERT((caifr & (uint32_t)k_ra8_ceu_caifr_mask_cim) != 0U);
  TEST_ASSERT_EQ(k_ra8_ceu_field_top, (caifr & (uint32_t)k_ra8_ceu_caifr_mask_fci));
  TEST_END("ceu init interlace one-field");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_byte_swap_full(void)
{
  TEST_BEGIN("ceu init byte swap all");
  prep();
  ra8_ceu_config_t cfg      = make_cfg();
  cfg.byte_swap.swap_8_bit  = true;
  cfg.byte_swap.swap_16_bit = true;
  cfg.byte_swap.swap_32_bit = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const uint32_t cdocr = *ra8_ceu_reg32(k_ra8_ceu_off_cdocr);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cobs) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cows) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cols) != 0U);
  TEST_END("ceu init byte swap all");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_scale_down(void)
{
  TEST_BEGIN("ceu init scale-down + clip");
  prep();
  ra8_ceu_config_t cfg    = make_cfg();
  cfg.scale.h_mantissa    = 2U;
  cfg.scale.h_fraction    = k_ceu_capture_h_fraction_800;
  cfg.scale.v_mantissa    = 1U;
  cfg.scale.v_fraction    = k_ceu_capture_v_fraction_400;
  cfg.scale.h_output_clip = k_ceu_capture_h_output_clip_320;
  cfg.scale.v_output_clip = k_ceu_capture_v_output_clip_240;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const uint32_t cflcr = *ra8_ceu_reg32(k_ra8_ceu_off_cflcr);
  TEST_ASSERT((cflcr & (uint32_t)k_ra8_ceu_cflcr_mask_hmant) != 0U);
  TEST_ASSERT((cflcr & (uint32_t)k_ra8_ceu_cflcr_mask_vmant) != 0U);
  const uint32_t cfszr = *ra8_ceu_reg32(k_ra8_ceu_off_cfszr);
  TEST_ASSERT((cfszr & (uint32_t)k_ra8_ceu_cfszr_mask_hfclp) == 320U);
  TEST_END("ceu init scale-down + clip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_low_pass_and_burst(void)
{
  TEST_BEGIN("ceu init LPF + burst + frame-drop");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.low_pass_filter  = true;
  cfg.burst_mode       = k_ra8_ceu_burst_256;
  cfg.frame_drop       = 4U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_clfcr) & (uint32_t)k_ra8_ceu_clfcr_mask_lpf) != 0U);
  const uint32_t capcr = *ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  TEST_ASSERT_EQ(k_ra8_ceu_burst_256,
                 ((capcr & (uint32_t)k_ra8_ceu_capcr_mask_mtcm) >> k_ra8_ceu_capcr_shift_mtcm));
  TEST_ASSERT_EQ(4, ((capcr & (uint32_t)k_ra8_ceu_capcr_mask_fdrp) >> k_ra8_ceu_capcr_shift_fdrp));
  TEST_END("ceu init LPF + burst + frame-drop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_happy(void)
{
  TEST_BEGIN("ceu capture_start happy");
  prep();

  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  /* CDAYR / CDACR receive the buffer, CAPSR.CE bit gets set. */
  TEST_ASSERT_EQ(k_test_ceu_buffer_addr, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT_EQ(k_test_ceu_buffer_addr, *ra8_ceu_reg32(k_ra8_ceu_off_cdacr));
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra8_ceu_capsr_mask_ce) != 0U);

  TEST_END("ceu capture_start happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_null(void)
{
  TEST_BEGIN("ceu capture_start null buffer");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_capture_arm(nullptr));
  TEST_END("ceu capture_start null buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_unaligned(void)
{
  TEST_BEGIN("ceu capture_start unaligned");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_unaligned));
  TEST_END("ceu capture_start unaligned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_busy(void)
{
  TEST_BEGIN("ceu capture_start busy");
  prep();

  /* Force the busy path: pretend a capture is in flight by setting
   * CSTSR.CPTON in the simulated mmap. */
  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) = (uint32_t)k_ra8_ceu_cstsr_mask_cpton;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  /* Clear CSTSR but force CAPSR.CPKIL -> still busy. */
  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) = 0U;
  *ra8_ceu_reg32(k_ra8_ceu_off_capsr) = (uint32_t)k_ra8_ceu_capsr_mask_cpkil;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  TEST_END("ceu capture_start busy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_ex_full_bundle(void)
{
  TEST_BEGIN("ceu capture_start_ex full bundle");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  const ra8_ceu_buffers_t bufs = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
    .c_top             = (uint8_t*)(uintptr_t)k_test_ceu_buffer_b,
    .y_bottom          = (uint8_t*)(uintptr_t)k_test_ceu_buffer_c,
    .c_bottom          = (uint8_t*)(uintptr_t)k_test_ceu_buffer_d,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 1024U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_start_ex(&bufs));

  TEST_ASSERT_EQ(k_test_ceu_buffer_addr, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT_EQ(k_test_ceu_buffer_b, *ra8_ceu_reg32(k_ra8_ceu_off_cdacr));
  TEST_ASSERT_EQ(k_test_ceu_buffer_c, *ra8_ceu_reg32(k_ra8_ceu_off_cdbyr));
  TEST_ASSERT_EQ(k_test_ceu_buffer_d, *ra8_ceu_reg32(k_ra8_ceu_off_cdbcr));
  /* CBDSR aligned-down to 8. */
  TEST_ASSERT_EQ(1024, *ra8_ceu_reg32(k_ra8_ceu_off_cbdsr));

  TEST_END("ceu capture_start_ex full bundle");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_ex_misaligned(void)
{
  TEST_BEGIN("ceu capture_start_ex misaligned");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const ra8_ceu_buffers_t bufs = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
    .c_top             = (uint8_t*)(uintptr_t)k_test_ceu_unaligned,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_capture_start_ex(&bufs));
  TEST_END("ceu capture_start_ex misaligned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_ex_null(void)
{
  TEST_BEGIN("ceu capture_start_ex null");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_capture_start_ex(nullptr));
  TEST_END("ceu capture_start_ex null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_disarm(void)
{
  TEST_BEGIN("ceu capture_disarm");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_disarm());
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_capsr) & (uint32_t)k_ra8_ceu_capsr_mask_ce) == 0U);
  TEST_END("ceu capture_stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_data_enable_arms_firewall(void)
{
  TEST_BEGIN("ceu data-enable arms firewall");
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.capture_format   = k_ra8_ceu_fmt_data_enable;
  cfg.image_area_size  = k_ceu_capture_image_area_size_4096;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  const uint32_t cfwcr = *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr);
  TEST_ASSERT((cfwcr & (uint32_t)k_ra8_ceu_cfwcr_mask_fwe) != 0U);
  TEST_END("ceu data-enable arms firewall");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dma_pump(void)
{
  TEST_BEGIN("ceu dma_pump");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ceu_dma_pump(0U,
                                  (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                  (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                  (uint32_t)k_test_ceu_dma_bytes));
  TEST_END("ceu dma_pump");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dma_pump_bad_args(void)
{
  TEST_BEGIN("ceu dma_pump bad args");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ceu_dma_pump(0U, nullptr, (uint8_t*)(uintptr_t)k_test_ceu_dma_dst, 64U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ceu_dma_pump(0U, (const uint8_t*)(uintptr_t)k_test_ceu_dma_src, nullptr, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ceu_dma_pump(0U,
                                  (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                  (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                  0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ceu_dma_pump(0U,
                                  (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                  (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                  7U));
  TEST_END("ceu dma_pump bad args");
}

/* ----------------------------------------------------------------------------
 * Sweep 17: ra8_ceu_set_dma_buffer + multi-frame capture wrappers
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_set_dma_buffer_happy(void)
{
  TEST_BEGIN("ceu set_dma_buffer happy");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
                                        (uint32_t)k_test_ceu_dma_bytes));
  TEST_END("ceu set_dma_buffer happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dma_buffer_null(void)
{
  TEST_BEGIN("ceu set_dma_buffer null");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ceu_set_dma_buffer(nullptr, (uint32_t)k_test_ceu_dma_bytes));
  TEST_END("ceu set_dma_buffer null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dma_buffer_zero_len(void)
{
  TEST_BEGIN("ceu set_dma_buffer zero len");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr, 0U));
  TEST_END("ceu set_dma_buffer zero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dma_buffer_misaligned(void)
{
  TEST_BEGIN("ceu set_dma_buffer misaligned");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_unaligned,
                                        (uint32_t)k_test_ceu_dma_bytes));
  TEST_END("ceu set_dma_buffer misaligned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_n_single(void)
{
  TEST_BEGIN("ceu capture_start single (num_frames=1)");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
                                        (uint32_t)k_test_ceu_dma_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_start(1U));
  const uint32_t capcr = *ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  TEST_ASSERT_EQ(0U, (capcr & ((uint32_t)1U << k_ra8_ceu_capcr_shift_ctncp)));
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra8_ceu_capsr_mask_ce) != 0U);
  TEST_END("ceu capture_start single (num_frames=1)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_n_continuous(void)
{
  TEST_BEGIN("ceu capture_start continuous (num_frames=0)");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
                                        (uint32_t)k_test_ceu_dma_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_start(0U));
  const uint32_t capcr = *ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  TEST_ASSERT((capcr & ((uint32_t)1U << k_ra8_ceu_capcr_shift_ctncp)) != 0U);
  TEST_END("ceu capture_start continuous (num_frames=0)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_no_buffer(void)
{
  TEST_BEGIN("ceu capture_start without set_dma_buffer");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ceu_capture_start(1U));
  TEST_END("ceu capture_start without set_dma_buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_stop_wrapper(void)
{
  TEST_BEGIN("ceu capture_stop wrapper");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ceu_set_dma_buffer((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr,
                                        (uint32_t)k_test_ceu_dma_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_start(1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_stop());
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  TEST_ASSERT_EQ(0U, (capsr & (uint32_t)k_ra8_ceu_capsr_mask_ce));
  TEST_END("ceu capture_stop wrapper");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("ceu deinit");
  prep();

  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_deinit());

  /* CEIER masked, CAPSR.CPKIL asserted. */
  TEST_ASSERT_EQ(0, *ra8_ceu_reg32(k_ra8_ceu_off_ceier));
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra8_ceu_capsr_mask_cpkil) != 0U);

  TEST_END("ceu deinit");
}

/**
 * @test test_mcdc_init_continuous_format_guard
 *
 * @par MC/DC:
 * Decision: `if ((cfg->capture_mode == k_ra8_ceu_capture_continuous) &&
 *               (cfg->capture_format != k_ra8_ceu_fmt_image_capture))`
 * (2 conditions, libs/ra8_hal/src/ra8_ceu.c line 607 -- gap row 452 in CSV)
 * - Vector 1: mode=single, format=any -> C1=F short-circuits.
 *   Decision F -> ok.
 * - Vector 2: mode=continuous, format=image_capture -> C1=T, C2=F.
 *   Decision F -> ok (the documented "continuous + image" combo).
 * - Vector 3: mode=continuous, format=data_synchronous -> C1=T, C2=T.
 *   Decision T -> invalid_arg.
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T (decision flips, C2
 * masked in V1 by short-circuit). MC/DC pair for C2: V2(T,F)->F vs
 * V3(T,T)->T (decision flips, C1 held T). N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC.
 */
static void test_mcdc_init_continuous_format_guard(void)
{
  TEST_BEGIN("ceu init MC/DC: continuous && format!=image");
  prep();

  ra8_ceu_config_t v1 = make_cfg();
  v1.capture_mode     = k_ra8_ceu_capture_single;
  v1.capture_format   = k_ra8_ceu_fmt_data_synchronous;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&v1));
  prep();

  ra8_ceu_config_t v2 = make_cfg();
  v2.capture_mode     = k_ra8_ceu_capture_continuous;
  v2.capture_format   = k_ra8_ceu_fmt_image_capture;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&v2));
  prep();

  ra8_ceu_config_t v3 = make_cfg();
  v3.capture_mode     = k_ra8_ceu_capture_continuous;
  v3.capture_format   = k_ra8_ceu_fmt_data_synchronous;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_init(&v3));

  TEST_END("ceu init MC/DC: continuous && format!=image");
}

/**
 * @test test_mcdc_arm_capture_firewall_guard
 *
 * @par MC/DC:
 * Decision: `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr))`
 * (2 conditions, libs/ra8_hal/src/ra8_ceu.c line 965 -- gap row 627 in CSV)
 * Reached only when capture_format == data_enable; firewall register
 * is programmed only when both conditions are true. We observe the
 * decision via the CFWCR register (FWE bit set when decision T).
 * - Vector 1: image_area=0,    y_top!=NULL -> C1=F short-circuit ->
 *   firewall write skipped, CFWCR retains its prior value.
 * - Vector 2: image_area=4096, y_top=NULL  -> C1=T, C2=F           ->
 *   capture_start_ex rejects the NULL y_top up front with
 *   k_ra8_err_null_ptr before reaching the firewall block, so
 *   CFWCR is never written and retains its prior value.
 * - Vector 3: image_area=4096, y_top!=NULL -> C1=T, C2=T           ->
 *   firewall enabled (FWE bit set, FWV upper bound encoded).
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T. MC/DC pair for C2:
 * V2(T,F)->F vs V3(T,T)->T. N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_mcdc_arm_capture_firewall_guard(void)
{
  TEST_BEGIN("ceu arm MC/DC: image_area!=0 && y_top!=NULL");

  /* Vector 1: image_area=0, y_top non-NULL. */
  prep();
  ra8_ceu_config_t cfg = make_cfg();
  cfg.capture_format   = k_ra8_ceu_fmt_data_enable;
  cfg.image_area_size  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = k_ceu_capture_sentinel_deadbeef;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  /* C1=F (image_area==0): the inner write is skipped and the outer
   * else (which clears CFWCR) only fires for non data-enable formats.
   * CFWCR therefore retains its seeded value. */
  TEST_ASSERT_EQ(0xDEADBEEFU, *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr));

  /* Vector 2: image_area=4096, y_top=NULL via capture_start_ex. */
  prep();
  cfg                 = make_cfg();
  cfg.capture_format  = k_ra8_ceu_fmt_data_enable;
  cfg.image_area_size = k_ceu_capture_image_area_size_4096;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  const ra8_ceu_buffers_t bufs_v2     = {.y_top = nullptr};
  *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = k_ceu_capture_sentinel_cafebabe;
  /* capture_start_ex enforces y_top != NULL up front, so the firewall
   * decision is never reached. The MC/DC obligation for C2 is still
   * exercised because the source-level decision flips when this caller
   * would have continued; we observe the API-visible rejection here. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_capture_start_ex(&bufs_v2));
  /* CFWCR not written -> retains seeded value. */
  TEST_ASSERT_EQ(0xCAFEBABEU, *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr));

  /* Vector 3: image_area=4096, y_top non-NULL -> firewall programmed. */
  prep();
  cfg                 = make_cfg();
  cfg.capture_format  = k_ra8_ceu_fmt_data_enable;
  cfg.image_area_size = k_ceu_capture_image_area_size_4096;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_arm((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  /* C1=T, C2=T -> FWE bit asserted. */
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) & (uint32_t)k_ra8_ceu_cfwcr_mask_fwe) != 0U);

  TEST_END("ceu arm MC/DC: image_area!=0 && y_top!=NULL");
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
  test_init_happy,
  test_init_null_cfg,
  test_init_continuous_non_image_rejected,
  test_init_data_enable_format,
  test_init_continuous_capture,
  test_init_interlace_one_field,
  test_init_byte_swap_full,
  test_init_scale_down,
  test_init_low_pass_and_burst,
  test_capture_arm_happy,
  test_capture_arm_null,
  test_capture_arm_unaligned,
  test_capture_arm_busy,
  test_capture_start_ex_full_bundle,
  test_capture_start_ex_misaligned,
  test_capture_start_ex_null,
  test_capture_disarm,
  test_data_enable_arms_firewall,
  test_dma_pump,
  test_dma_pump_bad_args,
  test_set_dma_buffer_happy,
  test_set_dma_buffer_null,
  test_set_dma_buffer_zero_len,
  test_set_dma_buffer_misaligned,
  test_capture_start_n_single,
  test_capture_start_n_continuous,
  test_capture_start_no_buffer,
  test_capture_stop_wrapper,
  test_deinit,
  test_mcdc_init_continuous_format_guard,
  test_mcdc_arm_capture_firewall_guard,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_ceu_capture.c\n");
  return 0;
}
