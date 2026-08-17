/**
 * @file test_ra8_ceu_config.c
 * @brief Unit tests for the CEU status, dispatch and runtime setters
 *
 * @details
 * Split sibling of the original test_ra8_ceu.c suite covering the
 * status and runtime-configuration surface of ra8_ceu.c (Capture
 * Engine Unit): status get / clear / snapshot, data size, interrupt
 * enables, IRQ dispatch fan-out incl. the no-handler leg, power
 * transition, reset, the bounded wait-idle timeout, plane-B
 * programming incl. alignment + mirror guards, plane swap, firewall,
 * byte swap, bundle size, low-pass, capture mode and frame drop.
 *
 * Sibling suite: test_ra8_ceu_capture.c (init + capture / DMA
 * datapath + MC/DC vectors).
 *
 * It also covers libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_camera.c,
 * the EK-RA8D2 wiring the CEU capture path sits on: the J35 XCLK divisor
 * bounds and pin routing, the U15 SW4-6 override, the eleven parallel
 * DVP pin claims, the CAM_RST pulse, and the SCCB register transport.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_ceu.h"
#include "ra8_ceu_regs.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_gpio_constants.h"
#include "ra8_gpt_regs.h"
#include "ra8_i2c.h"
#include "ra8_i2c_regs.h"
#include "ra8_mstp.h"
#include "ra8_pfs_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum ceu_config_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_ceu_probe_cdssr_small = 0x200U, /**< A CDSSR value with a single field set. */
} ceu_config_fixture_t;

/**
 * @enum ceu_config_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_ceu_probe_cdssr_wide =
    0xCAFE0000UL, /**< A full-width CDSSR value proving no field is truncated. */
} ceu_config_fixture2_t;

typedef enum : uint16_t {
  k_test_ceu_width  = 1280U, /**< Test CEU width.  */
  k_test_ceu_height = 720U,  /**< Test CEU height. */
  k_test_ceu_stride = 2560U, /**< Test CEU stride. */
} test_ceu_dim_t;

typedef enum : uint32_t {
  k_test_ceu_ints =
    (uint32_t)((uint32_t)k_ra8_ceu_evt_cpe | (uint32_t)k_ra8_ceu_evt_vd), /**< Test ceu ints. */
} test_ceu_ints_t;

typedef enum : uintptr_t {
  /* Picked so the lower 3 bits are zero (8-byte aligned). SDRAM head
   * range mapped by `ra8_fake_mmap`. */
  k_test_ceu_buffer_addr = 0x68000040UL, /**< Test CEU buffer address. */
  k_test_ceu_buffer_b    = 0x68001000UL, /**< Test CEU buffer b.       */
  k_test_ceu_buffer_c    = 0x68002000UL, /**< Test CEU buffer c.       */
  k_test_ceu_buffer_d    = 0x68003000UL, /**< Test CEU buffer d.       */
  k_test_ceu_unaligned   = 0x68000043UL, /**< Lower 3 bits != 0.       */
  k_test_ceu_dma_src     = 0x68010000UL, /**< Test CEU DMA src.        */
  k_test_ceu_dma_dst     = 0x68020000UL, /**< Test CEU DMA dst.        */
} test_ceu_buf_t;

typedef enum : uint32_t {
  k_test_ceu_dma_bytes = 2048U, /**< Test CEU DMA bytes. */
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
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
  s_ceu_cb_count     = 0U;
  s_ceu_cb_last_mask = 0U;
  s_ceu_cb_last_ctx  = nullptr;
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
static void test_status_get_clear(void)
{
  TEST_BEGIN("ceu status get + clear");
  prep();

  /* Seed CETCR with a couple of event flags. */
  const uint32_t seed = (uint32_t)((uint32_t)k_ra8_ceu_evt_cpe | (uint32_t)k_ra8_ceu_evt_vd);
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = seed;

  uint32_t snapshot = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_get_status(&snapshot));
  TEST_ASSERT_EQ(seed, snapshot);

  /* Clear only the CPE bit; VD must remain. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_clear_status((uint32_t)k_ra8_ceu_evt_cpe));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_get_status(&snapshot));
  TEST_ASSERT_EQ(k_ra8_ceu_evt_vd, snapshot);

  /* NULL out_mask. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_get_status(nullptr));

  TEST_END("ceu status get + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_snapshot(void)
{
  TEST_BEGIN("ceu status_snapshot");
  prep();
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_cpe;
  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) =
    (uint32_t)k_ra8_ceu_cstsr_mask_cpton | (uint32_t)k_ra8_ceu_cstsr_mask_cpfld;
  *ra8_ceu_reg32(k_ra8_ceu_off_cdssr) = k_ceu_probe_cdssr_small;

  ra8_ceu_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_status_snapshot(&st));
  TEST_ASSERT(st.capturing);
  TEST_ASSERT(st.top_field);
  TEST_ASSERT_EQ(0x200, st.data_size);
  TEST_ASSERT_EQ(k_ra8_ceu_evt_cpe, st.events);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_status_snapshot(nullptr));
  TEST_END("ceu status_snapshot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_data_size_get(void)
{
  TEST_BEGIN("ceu data_size_get");
  prep();
  *ra8_ceu_reg32(k_ra8_ceu_off_cdssr) = k_ceu_probe_cdssr_wide;
  uint32_t bytes                      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_data_size_get(&bytes));
  TEST_ASSERT_EQ(0xCAFE0000UL, bytes);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_data_size_get(nullptr));
  TEST_END("ceu data_size_get");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_interrupts_set(void)
{
  TEST_BEGIN("ceu interrupts_set");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_interrupts_set((uint32_t)k_ra8_ceu_evt_mask_all_errors));
  TEST_ASSERT_EQ(k_ra8_ceu_evt_mask_all_errors, *ra8_ceu_reg32(k_ra8_ceu_off_ceier));
  TEST_END("ceu interrupts_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dispatch(void)
{
  TEST_BEGIN("ceu attach + dispatch");
  prep();

  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  void* const cookie = (void*)(uintptr_t)0xDEADBEEFUL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_attach_handler(stub_ceu_cb, cookie));

  /* Seed CETCR with CPE + VD; only CPE | VD are in CEIER (test_ceu_ints).
   * The dispatched mask must equal the AND of CETCR and CEIER. */
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) =
    (uint32_t)((uint32_t)k_ra8_ceu_evt_cpe | (uint32_t)k_ra8_ceu_evt_vd |
               (uint32_t)k_ra8_ceu_evt_firewall);
  ra8_ceu_dispatch();

  TEST_ASSERT_EQ(1, s_ceu_cb_count);
  TEST_ASSERT(s_ceu_cb_last_ctx == cookie);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ceu_evt_cpe | (uint32_t)k_ra8_ceu_evt_vd), s_ceu_cb_last_mask);

  /* After dispatch, the observed bits are cleared (~pending was written). */
  uint32_t cetcr_after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_get_status(&cetcr_after));
  TEST_ASSERT_EQ(0, cetcr_after);

  TEST_END("ceu attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("ceu dispatch no handler");
  prep();

  /* Calling dispatch with no callback registered must not crash. */
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_cpe;
  ra8_ceu_dispatch();
  TEST_ASSERT_EQ(0, s_ceu_cb_count);

  TEST_END("ceu dispatch no handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("ceu power transition");
  prep();

  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_exit_stop());

  /* enter_stop sets CAPSR.CPKIL to abort any in-flight capture. */
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  TEST_ASSERT((capsr & (uint32_t)k_ra8_ceu_capsr_mask_cpkil) != 0U);

  TEST_END("ceu power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset(void)
{
  TEST_BEGIN("ceu reset");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_reset());
  /* CSTSR is never written by the driver on the host: the CPTON bit
   * stays at its reset value of 0 while the seam-decided idle wait
   * converges on its first poll. */
  TEST_ASSERT_EQ(0, (*ra8_ceu_reg32(k_ra8_ceu_off_cstsr) & (uint32_t)k_ra8_ceu_cstsr_mask_cpton));
  TEST_END("ceu reset");
}

/**
 * @test test_wait_idle_timeout_and_mcdc
 *
 * @par MC/DC:
 * Decision in internal_wait_idle:
 * ``((cstsr & CPTON) == 0U) && ((capsr & CPKIL) == 0U)`` (2 conditions).
 * The seam owns the loop EXIT, so each staged vector below is evaluated
 * on every poll while the armed fault drives the loop to its budget:
 * - V1: CPTON=1, CPKIL=0 -> C1=F short-circuits the AND   (dec F).
 * - V2: CPTON=0, CPKIL=1 -> C1=T, C2=F                    (dec F).
 * - V3: CPTON=0, CPKIL=0 -> C1=T, C2=T                    (dec T;
 *       driven by every unarmed prep()+ra8_ceu_init in this binary).
 * V1+V3 prove CPTON independently affects the outcome; V2+V3 prove the
 * same for CPKIL. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_wait_idle_timeout_and_mcdc(void)
{
  TEST_BEGIN("ceu wait_idle timeout + MC/DC vectors");

  /* V1: CPTON stuck -> ra8_ceu_reset propagates hw_timeout. */
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) = (uint32_t)k_ra8_ceu_cstsr_mask_cpton;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_wait((const volatile void*)ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ceu_reset());
  ra8_fake_mmio_reset();

  /* V2: CPTON clear but CPKIL stuck (ra8_ceu_reset itself asserts
   * CPKIL before the wait) -> hw_timeout through the second condition. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_wait((const volatile void*)ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ceu_reset());
  ra8_fake_mmio_reset();

  /* Retry leg: the engine goes idle on the 2nd poll. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_satisfy_after((const volatile void*)ra8_ceu_reg32(k_ra8_ceu_off_cstsr), 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_reset());
  ra8_fake_mmio_reset();

  /* Init-path leg: the wait-idle failure propagates out of init too. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_wait((const volatile void*)ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ceu_init(&cfg));

  TEST_END("ceu wait_idle timeout + MC/DC vectors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_plane_b_program(void)
{
  TEST_BEGIN("ceu plane_b_program");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  /* Set Plane A address as a baseline. */
  *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_a_off) =
    (uint32_t)k_test_ceu_buffer_addr;
  const ra8_ceu_buffers_t bufs = {
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_plane_b_program(&bufs));
  /* Plane B CDAYR should hold the override. */
  TEST_ASSERT_EQ(k_test_ceu_buffer_b,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_b_off));
  /* CRCNTR should have RC + RS + RVS set. */
  const uint32_t crcntr = *ra8_ceu_reg32(k_ra8_ceu_off_crcntr);
  TEST_ASSERT((crcntr & (uint32_t)k_ra8_ceu_crcntr_mask_rvs) != 0U);
  TEST_END("ceu plane_b_program");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_plane_b_misaligned(void)
{
  TEST_BEGIN("ceu plane_b misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu plane_b misaligned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_plane_b_null_keeps_mirror(void)
{
  TEST_BEGIN("ceu plane_b null mirror");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  /* Stamp a non-zero Plane A and verify mirror copies it. */
  *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_a_off) = (uint32_t)k_test_ceu_buffer_c;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_plane_b_program(nullptr));
  TEST_ASSERT_EQ(k_test_ceu_buffer_c,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_b_off));
  TEST_END("ceu plane_b null mirror");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_plane_swap_force(void)
{
  TEST_BEGIN("ceu plane_swap_force");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_plane_swap_force());
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_crcmpr) & (uint32_t)k_ra8_ceu_crcmpr_mask_ra) != 0U);
  TEST_END("ceu plane_swap_force");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_firewall_set(void)
{
  TEST_BEGIN("ceu firewall_set");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_firewall_set(true, 0x68001FFFUL));
  const uint32_t cfwcr = *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr);
  TEST_ASSERT((cfwcr & (uint32_t)k_ra8_ceu_cfwcr_mask_fwe) != 0U);
  /* Disable. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_firewall_set(false, 0U));
  TEST_ASSERT_EQ(0, *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr));
  TEST_END("ceu firewall_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_byte_swap_set(void)
{
  TEST_BEGIN("ceu byte_swap_set");
  prep();
  /* Seed CDOCR with extra bits to verify only the swap fields move. */
  *ra8_ceu_reg32(k_ra8_ceu_off_cdocr) = (uint32_t)k_ra8_ceu_cdocr_mask_cds;
  const ra8_ceu_byte_swap_t sw = {.swap_8_bit = true, .swap_16_bit = false, .swap_32_bit = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_byte_swap_set(&sw));
  const uint32_t cdocr = *ra8_ceu_reg32(k_ra8_ceu_off_cdocr);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cobs) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cows) == 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cols) != 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cds) != 0U);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ceu_byte_swap_set(nullptr));
  TEST_END("ceu byte_swap_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bundle_size_set(void)
{
  TEST_BEGIN("ceu bundle_size_set");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_bundle_size_set(0x1234U));
  /* Lower 3 bits masked off -> 0x1230. */
  TEST_ASSERT_EQ(0x1230, *ra8_ceu_reg32(k_ra8_ceu_off_cbdsr));
  TEST_END("ceu bundle_size_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_low_pass_set(void)
{
  TEST_BEGIN("ceu low_pass_set");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_low_pass_set(true));
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_clfcr) & (uint32_t)k_ra8_ceu_clfcr_mask_lpf) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_low_pass_set(false));
  TEST_ASSERT_EQ(0, *ra8_ceu_reg32(k_ra8_ceu_off_clfcr));
  TEST_END("ceu low_pass_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_mode_set(void)
{
  TEST_BEGIN("ceu capture_mode_set");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_mode_set(k_ra8_ceu_capture_continuous));
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_capcr) & (uint32_t)k_ra8_ceu_capcr_mask_ctncp) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_mode_set(k_ra8_ceu_capture_single));
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_capcr) & (uint32_t)k_ra8_ceu_capcr_mask_ctncp) == 0U);
  TEST_END("ceu capture_mode_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_frame_drop_set(void)
{
  TEST_BEGIN("ceu frame_drop_set");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_frame_drop_set(7U));
  const uint32_t fdrp_val =
    (*ra8_ceu_reg32(k_ra8_ceu_off_capcr) & (uint32_t)k_ra8_ceu_capcr_mask_fdrp) >>
    k_ra8_ceu_capcr_shift_fdrp;
  TEST_ASSERT_EQ(7, fdrp_val);
  TEST_END("ceu frame_drop_set");
}

/* ---------------------------------------------------------------------------
 * ra8_board_ek_ra8d2_camera.c -- the EK-RA8D2 wiring under the CEU
 * --------------------------------------------------------------------------- */

/**
 * @enum test_board_cam_fixture_t
 * @brief Board constants the camera BSP encodes, restated so a change is caught.
 * @details These mirror private constants in the BSP: the U15 IODIR mask that
 *          overrides SW4-6, the GPT channel driving XCLK, and the SCCB target
 *          and register the transport cases address.
 * @invariant k_board_cam_sw46_mask has exactly one bit set.
 * @invariant k_board_cam_addr fits seven bits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_board_cam_sw46_mask   = 0x20U, /**< U15 IODIR bit that overrides SW4-6.   */
  k_board_cam_gpt_channel = 12U,   /**< GPT channel wired to CAM_XCLK.        */
  k_board_cam_addr        = 0x3CU, /**< 7-bit SCCB address used by the case.  */
  k_board_cam_reg_hi      = 0x30U, /**< High byte of the addressed register.  */
  k_board_cam_reg_lo      = 0x0AU, /**< Low byte of the addressed register.   */
  k_board_cam_value       = 0x5AU, /**< Value written to that register.       */
  k_board_cam_rx_byte     = 0xC3U, /**< Byte staged in ICDRR for the read.    */
  k_board_cam_trace_cap   = 8U,    /**< Capacity of the SCCB byte trace.      */
  k_board_cam_xclk_div    = 4U,    /**< PCLKD divisor the XCLK case asks for. */
} test_board_cam_fixture_t;

/**
 * @enum test_board_cam_reg_t
 * @brief The 16-bit sensor register the SCCB cases address.
 * @details Split into its two transmitted bytes above so the big-endian
 *          address encoding the BSP performs can be asserted byte by byte.
 * @invariant k_board_cam_reg == ((k_board_cam_reg_hi << 8) | k_board_cam_reg_lo).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_board_cam_reg = 0x300AU, /**< Addressed sensor register. */
} test_board_cam_reg_t;

/**
 * @enum test_board_cam_clock_t
 * @brief PCLKB frequency handed to the RIIC bit-rate calculation.
 * @details Any real frequency works; 50 MHz matches the value the RIIC unit
 *          tests use, so the divider lands on the same programmed bit rate.
 * @invariant k_test_board_pclkb_hz is non-zero.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_board_pclkb_hz = 50000000U, /**< 50 MHz PCLKB. */
} test_board_cam_clock_t;

/** @brief Distinct consecutive ICDRT values observed on the camera SCCB bus. */
static uint8_t s_board_cam_trace[k_board_cam_trace_cap];

/** @brief Number of entries recorded in ::s_board_cam_trace. */
static uint8_t s_board_cam_trace_len;

/**
 * @brief Record each new byte the driver stages in the SCCB transmit register.
 * @details Runs inline on the driver's own poll thread once per bounded status
 *          poll, before the byte for that poll is written, so consecutive equal
 *          samples are folded away and the trace is the transmitted sequence.
 * @pre RIIC channel 1 registers are mapped.
 * @pre The trace was cleared for the case being run.
 * @post A byte differing from the previous sample is appended, bounded by capacity.
 * @post No register is modified.
 * @note Test-only and not thread-safe; the suite is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_board_cam_trace_hook(void)
{
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  if (reg == nullptr) {
    return;
  }
  if (s_board_cam_trace_len >= (uint8_t)k_board_cam_trace_cap) {
    return;
  }
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  const uint8_t byte = reg->ICDRT;
  if ((s_board_cam_trace_len != 0U) && (s_board_cam_trace[s_board_cam_trace_len - 1U] == byte)) {
    return;
  }
  s_board_cam_trace[s_board_cam_trace_len] = byte;
  s_board_cam_trace_len += 1U;
}

/**
 * @brief Restore every hosted service the camera BSP touches. @details Zeroes the register window, disarms the MMIO fault seam and the SCCB trace hook, frees every pin claim and reinitialises the module-stop model, so a case cannot inherit a claim or a latched status flag. @pre May be called at any point; no prerequisites. @pre No board operation is in flight. @post No pin is claimed and no MMIO wait is armed. @post The SCCB byte trace is empty. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void internal_board_cam_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
  s_board_cam_trace_len = 0U;
}

/**
 * @brief Bring RIIC1 up and stage the flags every SCCB byte waits on. @details ra8_i2c_init leaves ICSR2 alone and the per-transfer status clear preserves TDRE, TEND and RDRF, so staging them once lets a whole address-plus-data sequence complete without a bounded-wait expiry. @pre The register window was reset for this case. @pre The module-stop model is initialized. @post RIIC1 is enabled at the standard bit rate. @post ICSR2 reports transmit-empty, transmit-end and receive-full. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void internal_board_cam_bus_up(void)
{
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_ra8_i2c_speed_standard,
    .pclkb_hz = (uint32_t)k_test_board_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_board_camera_i2c_channel, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
}

/**
 * @brief Reject unrepresentable XCLK divisors and forward a routing conflict. @details A zero request is refused before the clock is read; a request whose PCLKD divisor falls outside the GPT period range is refused after it; a CAM_XCLK pad already owned by another driver stops the sequence at the routing step; and with the pad free the GPT carries the computed saw-PWM period and half-period duty. @pre The module-stop model is initialized. @pre CGC reports a non-zero PCLKD frequency. @post A completed start leaves CAM_XCLK claimed by the board layer. @post GPT12 holds the exact period and duty the divisor implies. @par MC/DC: Decision: the XCLK divisor range guard (2 conditions, below-GPT-minimum OR above-GPT-maximum). Vector 1: request = PCLKD (divisor 1) drives below-minimum true; Vector 2: request = 1 Hz drives above-maximum true; Vector 3: the board divisor drives both false and the start completes. Pairs 1+3 and 2+3 show each bound's independent influence. The zero-request and routing-conflict guards are single-condition; this test takes both directions of each. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_board_camera_xclk_bounds_and_routing(void)
{
  TEST_BEGIN("board camera: XCLK divisor bounds and routing");
  internal_board_cam_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_camera_xclk_start(0U));

  uint32_t pclkd_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_pclkd, &pclkd_hz));
  TEST_ASSERT(pclkd_hz != 0U);
  /* Divisor 1 is below the two-count GPT minimum; 1 Hz is above the maximum. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_camera_xclk_start(pclkd_hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_camera_xclk_start(1U));

  const ra8_port_pin_t xclk = (ra8_port_pin_t)k_ra8_board_cam_xclk;
  const uint32_t       freq = pclkd_hz / (uint32_t)k_board_cam_xclk_div;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(xclk, "test.camera.xclk"));
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_camera_xclk_start(freq));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_release(xclk));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_xclk_start(freq));
  TEST_ASSERT(ra8_pin_validator_is_claimed(xclk));
  const uint32_t                       period = pclkd_hz / freq;
  volatile const r_gpt_channel_regs_t* gpt    = ra8_gpt((uint8_t)k_board_cam_gpt_channel);
  TEST_ASSERT_NOT_NULL(gpt);
  /* HUM Ch 22.2.21 "GTPR : General PWM Timer Cycle Setting Register" p 938 */
  TEST_ASSERT_EQ(period - 1U, gpt->GTPR);
  /* HUM Ch 22.2.20 "GTCCRk : General PWM Timer Compare Capture Register k (k = A to F)" p 938 */
  TEST_ASSERT_EQ(period / 2U, gpt->GTCCR[0]);
  TEST_END("board camera: XCLK divisor bounds and routing");
}

/**
 * @brief Drive the U15 SW4-6 override that selects the parallel camera path. @details With RIIC1 un-armed the first expander register write never completes and the bounded wait expiry is forwarded unchanged; with the transmit flags staged the three-register sequence completes and the final write leaves the SW4-6 override mask in the transmit register. @pre The register window was reset for this case. @pre RIIC1 is reachable through the fake register window. @post The forwarded expander failure keeps its own error code. @post A completed override wrote exactly the SW4-6 mask as the direction byte. @par MC/DC: Single-condition decisions only: the bounded-wait expiry forward (taken with RIIC1 un-armed, not taken with the transmit flags staged) runs in both directions here. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_board_camera_select_parallel(void)
{
  TEST_BEGIN("board camera: SW4-6 override selects the parallel path");
  internal_board_cam_prep();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_camera_select_parallel());

  internal_board_cam_prep();
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_select_parallel());
  /* IODIR is the last of the three expander writes and carries the mask. */
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  TEST_ASSERT_EQ(k_board_cam_sw46_mask, reg->ICDRT);
  TEST_END("board camera: SW4-6 override selects the parallel path");
}

/**
 * @brief Route the eleven parallel DVP pads to the CEU. @details The first walk claims and muxes every pad in board order; the second stops at the first pad it can no longer claim, which is what leaves partial routing behind on a conflict. @pre No camera pad is claimed. @pre The register window was reset for this case. @post Every listed pad is claimed and carries the CEU peripheral function. @post A repeated walk is refused with a pin-ownership conflict. @par MC/DC: Single-condition decisions only: the per-pad claim guard runs in both directions -- every pad free on the first walk, the first pad refused on the repeated walk. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_board_camera_routes_parallel_pins(void)
{
  TEST_BEGIN("board camera: parallel pins route to the CEU");
  internal_board_cam_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_route_parallel_pins());
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_cam_d0));
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_cam_d7));
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_cam_vsync));
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_cam_hsync));
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_cam_pclk));

  const ra8_port_pin_t d0 = (ra8_port_pin_t)k_ra8_board_cam_d0;
  const uint32_t       expected_pfs =
    (uint32_t)k_ra8_pfs_mask_pmr | ((uint32_t)k_ra8_psel_ceu << (uint32_t)k_ra8_pfs_bit_psel0);
  TEST_ASSERT_EQ(expected_pfs, *ra8_pfs_pmn(RA8_PIN_PORT(d0), RA8_PIN_PIN(d0)));

  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_camera_route_parallel_pins());
  TEST_END("board camera: parallel pins route to the CEU");
}

/**
 * @brief Pulse CAM_RST and leave the sensor released. @details The pad is claimed as an output driven low, held, then released high, so the port set/reset register ends holding the CAM_RST set bit; a pad already owned by another driver stops the sequence at its output-init step. @pre No camera pad is claimed. @pre The register window was reset for this case. @post CAM_RST is claimed as a board-owned output and driven high. @post A conflicting claim is forwarded as a pin-ownership error. @par MC/DC: Single-condition decisions only: the CAM_RST output-claim guard runs in both directions -- free pad completes the pulse, the repeated call is refused with the ownership conflict. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_board_camera_reset_pulses_pin(void)
{
  TEST_BEGIN("board camera: reset pulses CAM_RST and releases it");
  internal_board_cam_prep();
  const ra8_port_pin_t rst = (ra8_port_pin_t)k_ra8_board_cam_rst;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_reset());
  TEST_ASSERT(ra8_pin_validator_is_claimed(rst));
  /* PCNTR3 low half is POSR: the release wrote the CAM_RST set bit last. */
  volatile const r_port_regs_t* port = ra8_port(RA8_PIN_PORT(rst));
  TEST_ASSERT_NOT_NULL(port);
  /* HUM Ch 20.2 "PCNTR3/PORR/POSR : Port Control Register 3" p 842 */
  TEST_ASSERT_EQ((1UL << (uint32_t)RA8_PIN_PIN(rst)), port->PCNTR3);

  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_camera_reset());
  TEST_END("board camera: reset pulses CAM_RST and releases it");
}

/**
 * @brief Encode SCCB register transfers as big-endian address plus data. @details The write case must put the peripheral address byte, then the register high byte, then its low byte on the bus and leave the value as the final transmitted byte; the read case must repeat the same two address bytes and hand back the received byte; a null destination is refused before any bus traffic. The delay adapter is exercised alongside them because it shares the transport callback signature. @pre RIIC1 is up with its transmit and receive flags staged. @pre The SCCB byte trace is empty. @post Both transfers report success and the traced bytes match the encoding. @post The delay adapter leaves the millisecond tick source unchanged. @par MC/DC: Single-condition decisions only: the null-destination guard (taken before any bus traffic, not taken on the completed read) runs in both directions; the write and read encodings are value checks, not branches. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_board_camera_sccb_transfers(void)
{
  TEST_BEGIN("board camera: SCCB register transfers");
  internal_board_cam_prep();
  uint8_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_board_camera_sccb_read_reg(nullptr,
                                                (uint8_t)k_board_cam_addr,
                                                (uint16_t)k_board_cam_reg,
                                                nullptr));

  internal_board_cam_bus_up();
  ra8_fake_mmio_set_poll_hook(internal_board_cam_trace_hook);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_sccb_write_reg(nullptr,
                                                 (uint8_t)k_board_cam_addr,
                                                 (uint16_t)k_board_cam_reg,
                                                 (uint8_t)k_board_cam_value));
  ra8_fake_mmio_set_poll_hook(nullptr);
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  TEST_ASSERT(s_board_cam_trace_len >= 4U);
  TEST_ASSERT_EQ(k_board_cam_addr << 1U, s_board_cam_trace[1]);
  TEST_ASSERT_EQ(k_board_cam_reg_hi, s_board_cam_trace[2]);
  TEST_ASSERT_EQ(k_board_cam_reg_lo, s_board_cam_trace[3]);
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  TEST_ASSERT_EQ(k_board_cam_value, reg->ICDRT);

  internal_board_cam_prep();
  internal_board_cam_bus_up();
  volatile r_i2c_regs_t* rx = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  rx->ICDRR = (uint8_t)k_board_cam_rx_byte;
  ra8_fake_mmio_set_poll_hook(internal_board_cam_trace_hook);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_sccb_read_reg(nullptr,
                                                (uint8_t)k_board_cam_addr,
                                                (uint16_t)k_board_cam_reg,
                                                &value));
  ra8_fake_mmio_set_poll_hook(nullptr);
  TEST_ASSERT_EQ(k_board_cam_rx_byte, value);
  TEST_ASSERT(s_board_cam_trace_len >= 4U);
  TEST_ASSERT_EQ(k_board_cam_addr << 1U, s_board_cam_trace[1]);
  TEST_ASSERT_EQ(k_board_cam_reg_hi, s_board_cam_trace[2]);
  TEST_ASSERT_EQ(k_board_cam_reg_lo, s_board_cam_trace[3]);

  /* The delay adapter drives the platform delay, which off-target does not
   * advance the tick source and touches no board state. */
  const uint32_t before = ra8_time_ms();
  ra8_board_camera_delay_ms(nullptr, (uint32_t)k_board_cam_value);
  TEST_ASSERT_EQ(before, ra8_time_ms());
  TEST_END("board camera: SCCB register transfers");
}

int main(void)
{
  test_status_get_clear();
  test_status_snapshot();
  test_data_size_get();
  test_interrupts_set();
  test_attach_dispatch();
  test_dispatch_no_handler();
  test_power_transition();
  test_reset();
  test_wait_idle_timeout_and_mcdc();
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
  test_board_camera_xclk_bounds_and_routing();
  test_board_camera_select_parallel();
  test_board_camera_routes_parallel_pins();
  test_board_camera_reset_pulses_pin();
  test_board_camera_sccb_transfers();
  return 0;
}
