/**
 * @file test_ra_ceu.c
 * @brief Unit tests for ra_ceu.c (Capture Engine Unit driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_ceu_regs.h"
#include "ra_ceu.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_ceu_width  = 1280U,
  k_test_ceu_height = 720U,
  k_test_ceu_stride = 2560U,
} test_ceu_dim_t;

typedef enum : uint32_t {
  k_test_ceu_ints = (uint32_t)((uint32_t)k_ra_ceu_evt_cpe | (uint32_t)k_ra_ceu_evt_vd),
} test_ceu_ints_t;

typedef enum : uintptr_t {
  /* Picked so the lower 3 bits are zero (8-byte aligned). SDRAM head
   * range mapped by `ra_sim_mmap`. */
  k_test_ceu_buffer_addr = 0x68000040UL,
  k_test_ceu_buffer_b    = 0x68001000UL,
  k_test_ceu_buffer_c    = 0x68002000UL,
  k_test_ceu_buffer_d    = 0x68003000UL,
  k_test_ceu_unaligned   = 0x68000043UL, /* lower 3 bits != 0. */
  k_test_ceu_dma_src     = 0x68010000UL,
  k_test_ceu_dma_dst     = 0x68020000UL,
} test_ceu_buf_t;

typedef enum : uint32_t {
  k_test_ceu_dma_bytes = 2048U,
} test_ceu_dma_t;

static uint32_t s_ceu_cb_count;
static uint32_t s_ceu_cb_last_mask;
static void*    s_ceu_cb_last_ctx;

static void stub_ceu_cb(void* ctx, uint32_t mask)
{
  ++s_ceu_cb_count;
  s_ceu_cb_last_mask = mask;
  s_ceu_cb_last_ctx  = ctx;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_ceu_cb_count     = 0U;
  s_ceu_cb_last_mask = 0U;
  s_ceu_cb_last_ctx  = nullptr;
  /* Detach any leftover callback from prior test cases. */
  (void)ra_ceu_attach_handler(nullptr, nullptr);
}

static ra_ceu_config_t make_cfg(void)
{
  const ra_ceu_config_t cfg = {
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
    .capture_format  = k_ra_ceu_fmt_image_capture,
    .capture_mode    = k_ra_ceu_capture_single,
    .data_bus        = k_ra_ceu_bus_8_bit,
    .hsync_polarity  = k_ra_ceu_pol_high_active,
    .vsync_polarity  = k_ra_ceu_pol_high_active,
    .field_polarity  = k_ra_ceu_pol_high_active,
    .input_order     = k_ra_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra_ceu_output_ycbcr_422,
    .burst_mode      = k_ra_ceu_burst_64,
    .first_field     = k_ra_ceu_field_immediate,
    .edge            = {.data  = k_ra_ceu_edge_rising,
                        .hsync = k_ra_ceu_edge_rising,
                        .vsync = k_ra_ceu_edge_rising,
                        .field = k_ra_ceu_edge_rising},
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

static void test_init_happy(void)
{
  TEST_BEGIN("ceu init happy");
  prep();

  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));

  /* CDWDR == stride, CMCYR low half == width, CEIER == interrupts. */
  TEST_ASSERT_EQ((int32_t)k_test_ceu_stride, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdwdr));
  const uint32_t cmcyr = *ra_ceu_reg32(k_ra_ceu_off_cmcyr);
  TEST_ASSERT_EQ((int32_t)k_test_ceu_width, (int32_t)(cmcyr & 0xFFFFU));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_height, (int32_t)(cmcyr >> 16U));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_ints, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_ceier));
  /* CFLCR / CFWCR / CDOCR all near-zero -> filters / firewall off,
   * output format cleared bit kept. */
  TEST_ASSERT_EQ(0, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cflcr));
  TEST_ASSERT_EQ(0, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cfwcr));
  /* CDS bit set for 4:2:2 output. */
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_cdocr) & (uint32_t)k_ra_ceu_cdocr_mask_cds) != 0U);

  TEST_END("ceu init happy");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("ceu init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_init(nullptr));
  TEST_END("ceu init null cfg");
}

static void test_init_continuous_non_image_rejected(void)
{
  TEST_BEGIN("ceu init continuous-only-image rule");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.capture_mode    = k_ra_ceu_capture_continuous;
  cfg.capture_format  = k_ra_ceu_fmt_data_synchronous;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ceu_init(&cfg));
  TEST_END("ceu init continuous-only-image rule");
}

static void test_init_data_enable_format(void)
{
  TEST_BEGIN("ceu init data-enable format");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.capture_format  = k_ra_ceu_fmt_data_enable;
  cfg.image_area_size = 4096U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));

  /* CAMCR.JPG should reflect the data-enable encoding. */
  const uint32_t camcr = *ra_ceu_reg32(k_ra_ceu_off_camcr);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ceu_fmt_data_enable,
    (int32_t)((camcr & (uint32_t)k_ra_ceu_camcr_mask_jpg) >> k_ra_ceu_camcr_shift_jpg));
  TEST_END("ceu init data-enable format");
}

static void test_init_continuous_capture(void)
{
  TEST_BEGIN("ceu init continuous mode");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.capture_mode    = k_ra_ceu_capture_continuous;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  const uint32_t capcr = *ra_ceu_reg32(k_ra_ceu_off_capcr);
  TEST_ASSERT((capcr & (uint32_t)k_ra_ceu_capcr_mask_ctncp) != 0U);
  TEST_END("ceu init continuous mode");
}

static void test_init_interlace_one_field(void)
{
  TEST_BEGIN("ceu init interlace one-field");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.interlace       = true;
  cfg.one_field_only  = true;
  cfg.first_field     = k_ra_ceu_field_top;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  const uint32_t caifr = *ra_ceu_reg32(k_ra_ceu_off_caifr);
  TEST_ASSERT((caifr & (uint32_t)k_ra_ceu_caifr_mask_ifs) != 0U);
  TEST_ASSERT((caifr & (uint32_t)k_ra_ceu_caifr_mask_cim) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ceu_field_top, (int32_t)(caifr & (uint32_t)k_ra_ceu_caifr_mask_fci));
  TEST_END("ceu init interlace one-field");
}

static void test_init_byte_swap_full(void)
{
  TEST_BEGIN("ceu init byte swap all");
  prep();
  ra_ceu_config_t cfg       = make_cfg();
  cfg.byte_swap.swap_8_bit  = true;
  cfg.byte_swap.swap_16_bit = true;
  cfg.byte_swap.swap_32_bit = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  const uint32_t cdocr = *ra_ceu_reg32(k_ra_ceu_off_cdocr);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cobs) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cows) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cols) != 0U);
  TEST_END("ceu init byte swap all");
}

static void test_init_scale_down(void)
{
  TEST_BEGIN("ceu init scale-down + clip");
  prep();
  ra_ceu_config_t cfg     = make_cfg();
  cfg.scale.h_mantissa    = 2U;
  cfg.scale.h_fraction    = 0x800U;
  cfg.scale.v_mantissa    = 1U;
  cfg.scale.v_fraction    = 0x400U;
  cfg.scale.h_output_clip = 320U;
  cfg.scale.v_output_clip = 240U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  const uint32_t cflcr = *ra_ceu_reg32(k_ra_ceu_off_cflcr);
  TEST_ASSERT((cflcr & (uint32_t)k_ra_ceu_cflcr_mask_hmant) != 0U);
  TEST_ASSERT((cflcr & (uint32_t)k_ra_ceu_cflcr_mask_vmant) != 0U);
  const uint32_t cfszr = *ra_ceu_reg32(k_ra_ceu_off_cfszr);
  TEST_ASSERT((cfszr & (uint32_t)k_ra_ceu_cfszr_mask_hfclp) == 320U);
  TEST_END("ceu init scale-down + clip");
}

static void test_init_low_pass_and_burst(void)
{
  TEST_BEGIN("ceu init LPF + burst + frame-drop");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.low_pass_filter = true;
  cfg.burst_mode      = k_ra_ceu_burst_256;
  cfg.frame_drop      = 4U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_clfcr) & (uint32_t)k_ra_ceu_clfcr_mask_lpf) != 0U);
  const uint32_t capcr = *ra_ceu_reg32(k_ra_ceu_off_capcr);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ceu_burst_256,
    (int32_t)((capcr & (uint32_t)k_ra_ceu_capcr_mask_mtcm) >> k_ra_ceu_capcr_shift_mtcm));
  TEST_ASSERT_EQ(
    4,
    (int32_t)((capcr & (uint32_t)k_ra_ceu_capcr_mask_fdrp) >> k_ra_ceu_capcr_shift_fdrp));
  TEST_END("ceu init LPF + burst + frame-drop");
}

static void test_capture_start_happy(void)
{
  TEST_BEGIN("ceu capture_start happy");
  prep();

  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  /* CDAYR / CDACR receive the buffer, CAPSR.CE bit gets set. */
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_addr, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdayr));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_addr, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdacr));
  const uint32_t capsr = *ra_ceu_reg32(k_ra_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra_ceu_capsr_mask_ce) != 0U);

  TEST_END("ceu capture_start happy");
}

static void test_capture_start_null(void)
{
  TEST_BEGIN("ceu capture_start null buffer");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_capture_start(nullptr));
  TEST_END("ceu capture_start null buffer");
}

static void test_capture_start_unaligned(void)
{
  TEST_BEGIN("ceu capture_start unaligned");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_unaligned));
  TEST_END("ceu capture_start unaligned");
}

static void test_capture_start_busy(void)
{
  TEST_BEGIN("ceu capture_start busy");
  prep();

  /* Force the busy path: pretend a capture is in flight by setting
   * CSTSR.CPTON in the simulated mmap. */
  *ra_ceu_reg32(k_ra_ceu_off_cstsr) = (uint32_t)k_ra_ceu_cstsr_mask_cpton;
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  /* Clear CSTSR but force CAPSR.CPKIL -> still busy. */
  *ra_ceu_reg32(k_ra_ceu_off_cstsr) = 0U;
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = (uint32_t)k_ra_ceu_capsr_mask_cpkil;
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));

  TEST_END("ceu capture_start busy");
}

static void test_capture_start_ex_full_bundle(void)
{
  TEST_BEGIN("ceu capture_start_ex full bundle");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));

  const ra_ceu_buffers_t bufs = {
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_capture_start_ex(&bufs));

  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_addr, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdayr));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_b, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdacr));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_c, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdbyr));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_d, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cdbcr));
  /* CBDSR aligned-down to 8. */
  TEST_ASSERT_EQ(1024, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cbdsr));

  TEST_END("ceu capture_start_ex full bundle");
}

static void test_capture_start_ex_misaligned(void)
{
  TEST_BEGIN("ceu capture_start_ex misaligned");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  const ra_ceu_buffers_t bufs = {
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ceu_capture_start_ex(&bufs));
  TEST_END("ceu capture_start_ex misaligned");
}

static void test_capture_start_ex_null(void)
{
  TEST_BEGIN("ceu capture_start_ex null");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_capture_start_ex(nullptr));
  TEST_END("ceu capture_start_ex null");
}

static void test_capture_stop(void)
{
  TEST_BEGIN("ceu capture_stop");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_capture_stop());
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_capsr) & (uint32_t)k_ra_ceu_capsr_mask_ce) == 0U);
  TEST_END("ceu capture_stop");
}

static void test_data_enable_arms_firewall(void)
{
  TEST_BEGIN("ceu data-enable arms firewall");
  prep();
  ra_ceu_config_t cfg = make_cfg();
  cfg.capture_format  = k_ra_ceu_fmt_data_enable;
  cfg.image_area_size = 4096U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ceu_capture_start((uint8_t*)(uintptr_t)k_test_ceu_buffer_addr));
  const uint32_t cfwcr = *ra_ceu_reg32(k_ra_ceu_off_cfwcr);
  TEST_ASSERT((cfwcr & (uint32_t)k_ra_ceu_cfwcr_mask_fwe) != 0U);
  TEST_END("ceu data-enable arms firewall");
}

static void test_status_get_clear(void)
{
  TEST_BEGIN("ceu status get + clear");
  prep();

  /* Seed CETCR with a couple of event flags. */
  const uint32_t seed = (uint32_t)((uint32_t)k_ra_ceu_evt_cpe | (uint32_t)k_ra_ceu_evt_vd);
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) = seed;

  uint32_t snapshot = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_get_status(&snapshot));
  TEST_ASSERT_EQ((int32_t)seed, (int32_t)snapshot);

  /* Clear only the CPE bit; VD must remain. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_clear_status((uint32_t)k_ra_ceu_evt_cpe));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_get_status(&snapshot));
  TEST_ASSERT_EQ((int32_t)k_ra_ceu_evt_vd, (int32_t)snapshot);

  /* NULL out_mask. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_get_status(nullptr));

  TEST_END("ceu status get + clear");
}

static void test_status_snapshot(void)
{
  TEST_BEGIN("ceu status_snapshot");
  prep();
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) = (uint32_t)k_ra_ceu_evt_cpe;
  *ra_ceu_reg32(k_ra_ceu_off_cstsr) =
    (uint32_t)k_ra_ceu_cstsr_mask_cpton | (uint32_t)k_ra_ceu_cstsr_mask_cpfld;
  *ra_ceu_reg32(k_ra_ceu_off_cdssr) = 0x200U;

  ra_ceu_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_status_snapshot(&st));
  TEST_ASSERT(st.capturing);
  TEST_ASSERT(st.top_field);
  TEST_ASSERT_EQ(0x200, (int32_t)st.data_size);
  TEST_ASSERT_EQ((int32_t)k_ra_ceu_evt_cpe, (int32_t)st.events);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_status_snapshot(nullptr));
  TEST_END("ceu status_snapshot");
}

static void test_data_size_get(void)
{
  TEST_BEGIN("ceu data_size_get");
  prep();
  *ra_ceu_reg32(k_ra_ceu_off_cdssr) = 0xCAFE0000UL;
  uint32_t bytes                    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_data_size_get(&bytes));
  TEST_ASSERT_EQ((int32_t)0xCAFE0000UL, (int32_t)bytes);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_data_size_get(nullptr));
  TEST_END("ceu data_size_get");
}

static void test_interrupts_set(void)
{
  TEST_BEGIN("ceu interrupts_set");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ceu_interrupts_set((uint32_t)k_ra_ceu_evt_mask_all_errors));
  TEST_ASSERT_EQ((int32_t)k_ra_ceu_evt_mask_all_errors, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_ceier));
  TEST_END("ceu interrupts_set");
}

static void test_attach_dispatch(void)
{
  TEST_BEGIN("ceu attach + dispatch");
  prep();

  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));

  void* const cookie = (void*)(uintptr_t)0xDEADBEEFUL;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_attach_handler(stub_ceu_cb, cookie));

  /* Seed CETCR with CPE + VD; only CPE | VD are in CEIER (test_ceu_ints).
   * The dispatched mask must equal the AND of CETCR and CEIER. */
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) =
    (uint32_t)((uint32_t)k_ra_ceu_evt_cpe | (uint32_t)k_ra_ceu_evt_vd |
               (uint32_t)k_ra_ceu_evt_firewall);
  ra_ceu_dispatch();

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ceu_cb_count);
  TEST_ASSERT(s_ceu_cb_last_ctx == cookie);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ceu_evt_cpe | (uint32_t)k_ra_ceu_evt_vd),
                 (int32_t)s_ceu_cb_last_mask);

  /* After dispatch, the observed bits are cleared (~pending was written). */
  uint32_t cetcr_after = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_get_status(&cetcr_after));
  TEST_ASSERT_EQ(0, (int32_t)cetcr_after);

  TEST_END("ceu attach + dispatch");
}

static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("ceu dispatch no handler");
  prep();

  /* Calling dispatch with no callback registered must not crash. */
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) = (uint32_t)k_ra_ceu_evt_cpe;
  ra_ceu_dispatch();
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_ceu_cb_count);

  TEST_END("ceu dispatch no handler");
}

static void test_power_transition(void)
{
  TEST_BEGIN("ceu power transition");
  prep();

  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_exit_stop());

  /* enter_stop sets CAPSR.CPKIL to abort any in-flight capture. */
  const uint32_t capsr = *ra_ceu_reg32(k_ra_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra_ceu_capsr_mask_cpkil) != 0U);

  TEST_END("ceu power transition");
}

static void test_reset(void)
{
  TEST_BEGIN("ceu reset");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_reset());
  /* On the simulator, internal_wait_idle clears these bits. */
  TEST_ASSERT_EQ(
    0,
    (int32_t)(*ra_ceu_reg32(k_ra_ceu_off_cstsr) & (uint32_t)k_ra_ceu_cstsr_mask_cpton));
  TEST_END("ceu reset");
}

static void test_plane_b_program(void)
{
  TEST_BEGIN("ceu plane_b_program");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  /* Set Plane A address as a baseline. */
  *ra_ceu_reg32_plane(k_ra_ceu_off_cdayr, k_ra_ceu_plane_a_off) = (uint32_t)k_test_ceu_buffer_addr;
  const ra_ceu_buffers_t bufs                                   = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_ceu_buffer_b,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_plane_b_program(&bufs));
  /* Plane B CDAYR should hold the override. */
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_b,
                 (int32_t)*ra_ceu_reg32_plane(k_ra_ceu_off_cdayr, k_ra_ceu_plane_b_off));
  /* CRCNTR should have RC + RS + RVS set. */
  const uint32_t crcntr = *ra_ceu_reg32(k_ra_ceu_off_crcntr);
  TEST_ASSERT((crcntr & (uint32_t)k_ra_ceu_crcntr_mask_rvs) != 0U);
  TEST_END("ceu plane_b_program");
}

static void test_plane_b_misaligned(void)
{
  TEST_BEGIN("ceu plane_b misaligned");
  prep();
  const ra_ceu_buffers_t bufs = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_ceu_unaligned,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ceu_plane_b_program(&bufs));
  TEST_END("ceu plane_b misaligned");
}

static void test_plane_b_null_keeps_mirror(void)
{
  TEST_BEGIN("ceu plane_b null mirror");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  /* Stamp a non-zero Plane A and verify mirror copies it. */
  *ra_ceu_reg32_plane(k_ra_ceu_off_cdayr, k_ra_ceu_plane_a_off) = (uint32_t)k_test_ceu_buffer_c;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_plane_b_program(nullptr));
  TEST_ASSERT_EQ((int32_t)k_test_ceu_buffer_c,
                 (int32_t)*ra_ceu_reg32_plane(k_ra_ceu_off_cdayr, k_ra_ceu_plane_b_off));
  TEST_END("ceu plane_b null mirror");
}

static void test_plane_swap_force(void)
{
  TEST_BEGIN("ceu plane_swap_force");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_plane_swap_force());
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_crcmpr) & (uint32_t)k_ra_ceu_crcmpr_mask_ra) != 0U);
  TEST_END("ceu plane_swap_force");
}

static void test_firewall_set(void)
{
  TEST_BEGIN("ceu firewall_set");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_firewall_set(true, 0x68001FFFUL));
  const uint32_t cfwcr = *ra_ceu_reg32(k_ra_ceu_off_cfwcr);
  TEST_ASSERT((cfwcr & (uint32_t)k_ra_ceu_cfwcr_mask_fwe) != 0U);
  /* Disable. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_firewall_set(false, 0U));
  TEST_ASSERT_EQ(0, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cfwcr));
  TEST_END("ceu firewall_set");
}

static void test_byte_swap_set(void)
{
  TEST_BEGIN("ceu byte_swap_set");
  prep();
  /* Seed CDOCR with extra bits to verify only the swap fields move. */
  *ra_ceu_reg32(k_ra_ceu_off_cdocr) = (uint32_t)k_ra_ceu_cdocr_mask_cds;
  const ra_ceu_byte_swap_t sw = {.swap_8_bit = true, .swap_16_bit = false, .swap_32_bit = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_byte_swap_set(&sw));
  const uint32_t cdocr = *ra_ceu_reg32(k_ra_ceu_off_cdocr);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cobs) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cows) == 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cols) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra_ceu_cdocr_mask_cds) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ceu_byte_swap_set(nullptr));
  TEST_END("ceu byte_swap_set");
}

static void test_bundle_size_set(void)
{
  TEST_BEGIN("ceu bundle_size_set");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_bundle_size_set(0x1234U));
  /* Lower 3 bits masked off -> 0x1230. */
  TEST_ASSERT_EQ(0x1230, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_cbdsr));
  TEST_END("ceu bundle_size_set");
}

static void test_low_pass_set(void)
{
  TEST_BEGIN("ceu low_pass_set");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_low_pass_set(true));
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_clfcr) & (uint32_t)k_ra_ceu_clfcr_mask_lpf) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_low_pass_set(false));
  TEST_ASSERT_EQ(0, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_clfcr));
  TEST_END("ceu low_pass_set");
}

static void test_capture_mode_set(void)
{
  TEST_BEGIN("ceu capture_mode_set");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_capture_mode_set(k_ra_ceu_capture_continuous));
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_capcr) & (uint32_t)k_ra_ceu_capcr_mask_ctncp) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_capture_mode_set(k_ra_ceu_capture_single));
  TEST_ASSERT((*ra_ceu_reg32(k_ra_ceu_off_capcr) & (uint32_t)k_ra_ceu_capcr_mask_ctncp) == 0U);
  TEST_END("ceu capture_mode_set");
}

static void test_frame_drop_set(void)
{
  TEST_BEGIN("ceu frame_drop_set");
  prep();
  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_frame_drop_set(7U));
  const uint32_t fdrp_val =
    (*ra_ceu_reg32(k_ra_ceu_off_capcr) & (uint32_t)k_ra_ceu_capcr_mask_fdrp) >>
    k_ra_ceu_capcr_shift_fdrp;
  TEST_ASSERT_EQ(7, (int32_t)fdrp_val);
  TEST_END("ceu frame_drop_set");
}

static void test_dma_pump(void)
{
  TEST_BEGIN("ceu dma_pump");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ceu_dma_pump(0U,
                                          (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                          (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                          (uint32_t)k_test_ceu_dma_bytes));
  TEST_END("ceu dma_pump");
}

static void test_dma_pump_bad_args(void)
{
  TEST_BEGIN("ceu dma_pump bad args");
  prep();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_ceu_dma_pump(0U, nullptr, (uint8_t*)(uintptr_t)k_test_ceu_dma_dst, 64U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_ceu_dma_pump(0U, (const uint8_t*)(uintptr_t)k_test_ceu_dma_src, nullptr, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ceu_dma_pump(0U,
                                          (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                          (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                          0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ceu_dma_pump(0U,
                                          (const uint8_t*)(uintptr_t)k_test_ceu_dma_src,
                                          (uint8_t*)(uintptr_t)k_test_ceu_dma_dst,
                                          7U));
  TEST_END("ceu dma_pump bad args");
}

static void test_deinit(void)
{
  TEST_BEGIN("ceu deinit");
  prep();

  const ra_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ceu_deinit());

  /* CEIER masked, CAPSR.CPKIL asserted. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_ceu_reg32(k_ra_ceu_off_ceier));
  const uint32_t capsr = *ra_ceu_reg32(k_ra_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra_ceu_capsr_mask_cpkil) != 0U);

  TEST_END("ceu deinit");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_continuous_non_image_rejected();
  test_init_data_enable_format();
  test_init_continuous_capture();
  test_init_interlace_one_field();
  test_init_byte_swap_full();
  test_init_scale_down();
  test_init_low_pass_and_burst();
  test_capture_start_happy();
  test_capture_start_null();
  test_capture_start_unaligned();
  test_capture_start_busy();
  test_capture_start_ex_full_bundle();
  test_capture_start_ex_misaligned();
  test_capture_start_ex_null();
  test_capture_stop();
  test_data_enable_arms_firewall();
  test_status_get_clear();
  test_status_snapshot();
  test_data_size_get();
  test_interrupts_set();
  test_attach_dispatch();
  test_dispatch_no_handler();
  test_power_transition();
  test_reset();
  test_plane_b_program();
  test_plane_b_misaligned();
  test_plane_b_null_keeps_mirror();
  test_plane_swap_force();
  test_firewall_set();
  test_byte_swap_set();
  test_bundle_size_set();
  test_low_pass_set();
  test_capture_mode_set();
  test_frame_drop_set();
  test_dma_pump();
  test_dma_pump_bad_args();
  test_deinit();
  (void)fprintf(stderr, "[OK  ] test_ra_ceu.c\n");
  return 0;
}
