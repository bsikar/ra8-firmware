/**
 * @file test_ra_epaper.c
 * @brief Unit tests for the IT8951 e-paper SPI driver (``ra_epaper.c``)
 *
 * @details
 * Host-only tests. The simulator mmap models SPI registers as host
 * RAM, so the driver's SPI calls succeed with whatever last-write
 * value the test set. The test plays the app's role in the DI split:
 * it initialises SPI_B channel 0 itself, binds it through the ra_io
 * SPI-bus facade (``ra_io_spi_bus_bind_spi_b`` +
 * ``ra_io_spi_bus_as_ops``), and hands the resulting seam to
 * ``ra_epaper_init`` -- exercising the exact production wiring.
 * ``RA_SIMULATOR_MODE`` short-circuits the HRDY busy-poll inside the
 * driver; the LUT-idle poll in ``display_area`` runs for real on host
 * (issue #177 / T1-01) and is driven through the ``ra_sim_mmio`` fault
 * seam keyed on the seam's ctx cookie (the bound bus handle).
 *
 * What we cover:
 *   - ``ra_epaper_init`` rejects NULL cfg.
 *   - ``ra_epaper_init`` rejects out-of-range cfg fields.
 *   - ``ra_epaper_init`` rejects double init.
 *   - Calls before init return ``k_ra_err_invalid_state``.
 *   - ``ra_epaper_load_image`` rejects NULL / size mismatch.
 *   - Happy-path init -> load -> display -> sleep round-trip succeeds
 *     and leaves the driver back in the uninit state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_port_regs.h"
#include "ra8d2_spi_regs.h"
#include "ra_epaper.h"
#include "ra_err.h"
#include "ra_io_spi_bus.h"
#include "ra_io_spi_bus_spi_b.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_sim_mmio.h"
#include "ra_spi.h"
#include "ra_spi_bus_ops.h"
#include "unity_minimal.h"

/**
 * @enum ra_epaper_test_const_t
 * @brief Test fixtures.
 */
typedef enum : uint32_t {
  k_ra_epaper_test_pclka_hz   = 100000000U, /**< 100 MHz PCLKA.    */
  k_ra_epaper_test_baud_hz    = 12000000U,  /**< 12 MHz SPI clock. */
  k_ra_epaper_test_panel_w    = 800U,
  k_ra_epaper_test_panel_h    = 600U,
  k_ra_epaper_test_buf_pixels = 64U, /**< 8x8 = 64 px. */
} ra_epaper_test_const_t;

/** @brief Bound SPI_B bus handle -- the seam's ctx and the mmio-seam key. */
static ra_io_spi_bus_t s_bus;
/** @brief Seam filled from ::s_bus by ``ra_io_spi_bus_as_ops`` in prep(). */
static ra_spi_bus_ops_t s_bus_ops;

static ra_epaper_cfg_t make_cfg(void)
{
  const ra_epaper_cfg_t cfg = {
    .bus          = s_bus_ops,
    .reset_pin    = 0U,
    .busy_pin     = 0U,
    .panel_width  = (uint16_t)k_ra_epaper_test_panel_w,
    .panel_height = (uint16_t)k_ra_epaper_test_panel_h,
  };
  return cfg;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  ra_sim_mmio_reset();
  /* The driver state is file-static; a "sleep" call resets it back
   * to uninit. We achieve the same effect between tests via the
   * sleep API; tests that don't init (NULL-arg paths) don't need it.
   */
  (void)ra_epaper_sleep();
  /* Bring up MSTP so ra_spi_init can flip the SPI module-stop bit. */
  (void)ra_mstp_init();
  /* Play the app's role: initialise SPI_B channel 0 in mode 0 and bind
   * it through the ra_io facade into the driver's injected seam. */
  const ra_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra_epaper_test_baud_hz,
    .pclka_hz  = (uint32_t)k_ra_epaper_test_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_spi_init(0U, &spi_cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_spi_bus_bind_spi_b(&s_bus, 0U));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_spi_bus_as_ops(&s_bus, &s_bus_ops));
}

/**
 * @brief Pre-stage SPSR with both ready flags asserted on every SPI channel.
 *
 * @details
 * The host build of ``ra_spi_b.c`` short-circuits ``internal_wait_spsr``
 * (RA_SIMULATOR_MODE branch): it returns ``k_ra_ok`` if the requested
 * flag (SPTEF or SPRF) is asserted, otherwise ``k_ra_err_hw_timeout``.
 * The simulator backs SPSR with plain mmap'd RAM that is zeroed by
 * ``ra_sim_mmap_reset``, so a fresh test fixture has no flags
 * asserted. Pre-staging once is sufficient because the driver clears
 * flags via SPSRC (a separate write-1-to-clear register that, in sim,
 * is independent RAM and does not touch SPSR).
 */
static void stage_spsr_ready(void)
{
  const uint32_t both = (uint32_t)k_ra_spsr_mask_sptef | (uint32_t)k_ra_spsr_mask_sprf;
  for (uint8_t ch = 0U; ch < 2U; ch++) {
    volatile r_spi_regs_t* reg = ra_spi(ch);
    if (reg != nullptr) {
      reg->SPSR = both;
    }
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("test_init_null_cfg");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_init(nullptr));
  TEST_END("test_init_null_cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_cfg(void)
{
  TEST_BEGIN("test_init_bad_cfg");
  prep();
  ra_epaper_cfg_t cfg = make_cfg();

  cfg.bus.xfer8 = nullptr;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg.bus.xfer8 = s_bus_ops.xfer8;

  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg.panel_width = (uint16_t)k_ra_epaper_test_panel_w;

  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  TEST_END("test_init_bad_cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_calls_before_init(void)
{
  TEST_BEGIN("test_calls_before_init");
  prep();
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 1U, .height = 1U};
  const uint8_t          buf  = 0x80U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_epaper_load_image(&area, &buf, 1U, k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_sleep());
  TEST_END("test_calls_before_init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_happy_path(void)
{
  TEST_BEGIN("test_happy_path");
  prep();
  stage_spsr_ready();
  const ra_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));

  /* Double init -> invalid state. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_init(&cfg));

  /* Load + size mismatch. */
  uint8_t                pixels[k_ra_epaper_test_buf_pixels] = {};
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_epaper_load_image(&area, pixels, 1U, k_ra_epaper_endian_little));

  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epaper_load_image(nullptr, pixels, sizeof(pixels), k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epaper_load_image(&area, nullptr, sizeof(pixels), k_ra_epaper_endian_little));

  /* Happy load + display + sleep. The display_area LUT-idle poll reads LUTAFSR
   * over SPI; the sim SPI loopback returns the driver's own dummy byte (never
   * zero), so the real bounded poll (issue #177 / T1-01, no longer compiled
   * out) is driven through the ra_sim_mmio seam keyed on the injected seam's
   * ctx cookie -- the bound bus handle &s_bus. Arm it to report "LUT idle" on
   * the 3rd poll so the real loop iterates twice then succeeds. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_epaper_load_image(&area, pixels, sizeof(pixels), k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_ok, ra_sim_mmio_satisfy_after((volatile const void*)&s_bus, 2U));
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_display_area(nullptr, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep());

  /* After sleep we're back to uninit. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_END("test_happy_path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the display_area LUT-idle poll is a
 * single-condition ``status == 0U`` comparison. This case drives the real
 * bounded poll's timeout leg: the ra_sim_mmio seam keyed on the seam's ctx
 * cookie (the bound bus handle) is armed to never report idle, so the loop runs to its budget
 * and returns ``k_ra_err_hw_timeout``. Paired with ``test_happy_path`` (which
 * arms satisfy_after for the success leg), gcov sees both legs of the poll.)
 */
static void test_display_area_lut_timeout(void)
{
  TEST_BEGIN("epaper display_area LUT poll timeout (real seam poll)");
  prep();
  stage_spsr_ready();
  const ra_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));

  /* Arm the seam so the LUT-idle poll never reports idle: the real bounded loop
   * exhausts its budget and returns k_ra_err_hw_timeout (SPI transfers still
   * succeed -- only the LUT-idle verdict is forced false). */
  TEST_ASSERT_EQ(k_ra_ok, ra_sim_mmio_fail_wait((volatile const void*)&s_bus));
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));

  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep());
  TEST_END("epaper display_area LUT poll timeout (real seam poll)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the HRDY panel-ready wait's loop-exit
 * is a single-condition seam consult, not a compound boolean)
 */
static void test_wait_ready_hrdy_timeout(void)
{
  TEST_BEGIN("epaper HRDY wait timeout (real seam poll)");
  prep();
  stage_spsr_ready();
  const ra_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));

  /* Arm the HRDY busy-pin input register (the pin port's PCNTR2) so the panel
   * never signals ready: the next command's bounded HRDY wait exhausts its
   * budget and returns the real hardware timeout instead of the fake success
   * the deleted RA_SIMULATOR_MODE short-circuit used to return. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sim_mmio_fail_wait(
                   (volatile const void*)&ra_port(RA_PIN_PORT(cfg.busy_pin))->PCNTR2));
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));

  ra_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep());
  TEST_END("epaper HRDY wait timeout (real seam poll)");
}

/**
 * @test test_mcdc_ra_epaper
 *
 * @par MC/DC:
 * Decision A libs/ra_hal/src/ra_epaper.c@internal_ra_epaper_validate_cfg:
 * ``if ((bus.xfer8 == NULL) || (panel_w == 0) || (panel_h == 0) ||
 *      (panel_w > MAX) || (panel_h > MAX))``
 * (5 conditions, ``||`` short-circuit chain).
 *
 * Per DO-178C 6.4.4.3 a 5-condition decision requires N+1 = 6 vectors.
 * Representative-subset rationale: each Boolean Ck is exercised
 * independently against an otherwise-valid baseline (so each Ck flips
 * while every other Ci is fixed-false), giving the required pair-wise
 * independence proof:
 * - V0: all Ci=F                -> dec F (accept)
 * - V1: bus.xfer8=NULL          -> dec T (reject)
 * - V2: panel_width=0           -> dec T (reject)
 * - V3: panel_height=0          -> dec T (reject)
 * - V4: panel_width>MAX         -> dec T (reject)
 * - V5: panel_height>MAX        -> dec T (reject)
 *
 * Decision B libs/ra_hal/src/ra_epaper.c@ra_epaper_load_image:
 * ``if ((buf_len != expect) || (expect == 0U))`` (2 conditions, ``||``).
 * N+1 = 3:
 * - V1: buf_len=expect, expect>0 -> dec F (accept)
 * - V2: buf_len!=expect          -> dec T (reject)
 * - V3: expect=0 (0x0 area)      -> dec T (reject)
 */
static void test_mcdc_ra_epaper(void)
{
  TEST_BEGIN("epaper MC/DC: validate_cfg 5-cond + load_image 2-cond");
  prep();
  stage_spsr_ready();
  ra_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep());

  cfg           = make_cfg();
  cfg.bus.xfer8 = nullptr;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg             = make_cfg();
  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg              = make_cfg();
  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg             = make_cfg();
  cfg.panel_width = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg              = make_cfg();
  cfg.panel_height = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));

  prep();
  stage_spsr_ready();
  cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));
  uint8_t                pixels[k_ra_epaper_test_buf_pixels] = {};
  const ra_epaper_area_t area_8x8 = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_epaper_load_image(&area_8x8, pixels, sizeof(pixels), k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_epaper_load_image(&area_8x8, pixels, 1U, k_ra_epaper_endian_little));
  const ra_epaper_area_t area_0x0 = {.x = 0U, .y = 0U, .width = 0U, .height = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_epaper_load_image(&area_0x0, pixels, 0U, k_ra_epaper_endian_little));
  TEST_END("epaper MC/DC: validate_cfg 5-cond + load_image 2-cond");
}

int main(void)
{
  test_init_null_cfg();
  test_init_bad_cfg();
  test_calls_before_init();
  test_happy_path();
  test_display_area_lut_timeout();
  test_wait_ready_hrdy_timeout();
  test_mcdc_ra_epaper();
  return 0;
}
