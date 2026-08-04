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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_ceu.h"
#include "ra8_ceu_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
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

int32_t main(void)
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
  (void)fprintf(stderr, "[OK ] test_ra8_ceu_config.c\n");
  return 0;
}
