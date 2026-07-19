/**
 * @file test_ra8_epaper.c
 * @brief Unit tests for the IT8951 e-paper SPI driver (``ra8_epaper.c``)
 *
 * @details
 * Host-only tests. The simulator mmap models SPI registers as host
 * RAM, so the driver's SPI calls succeed with whatever last-write
 * value the test set. The test plays the app's role in the DI split:
 * it initialises SPI_B channel 0 itself, binds it through the ra8_io
 * SPI-bus facade (``ra8_io_spi_bus_bind_spi_b`` +
 * ``ra8_io_spi_bus_as_ops``), and hands the resulting seam to
 * ``ra8_epaper_init`` -- exercising the exact production wiring.
 * The HRDY busy-poll, the /RESET GPIO pulse, and the LUT-idle poll in
 * ``display_area`` all run for real on host (issues #177 / #238): the
 * two polls are driven through the ``ra8_sim_mmio`` fault seam (the
 * LUT poll keyed on the seam's ctx cookie, the bound bus handle) and
 * the reset pulse drives the RAM-backed PORT block.
 *
 * What we cover:
 *   - ``ra8_epaper_init`` rejects NULL cfg.
 *   - ``ra8_epaper_init`` rejects out-of-range cfg fields.
 *   - ``ra8_epaper_init`` rejects double init.
 *   - Calls before init return ``k_ra8_err_invalid_state``.
 *   - ``ra8_epaper_load_image`` rejects NULL / size mismatch.
 *   - Happy-path init -> load -> display -> sleep round-trip succeeds
 *     and leaves the driver back in the uninit state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_spi_b.h"
#include "ra8_mstp.h"
#include "ra8_port_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_epaper_test_const_t
 * @brief Test fixtures.
 */
typedef enum : uint32_t {
  k_ra8_epaper_test_pclka_hz      = 100000000U,  /**< 100 MHz PCLKA.           */
  k_ra8_epaper_test_baud_hz       = 12000000U,   /**< 12 MHz SPI clock.        */
  k_ra8_epaper_test_panel_w       = 800U,        /**< RA8 epaper test panel w. */
  k_ra8_epaper_test_panel_h       = 600U,        /**< RA8 epaper test panel h. */
  k_ra8_epaper_test_buf_pixels    = 64U,         /**< 8x8 = 64 px.             */
  k_ra8_epaper_test_vcom_mv       = 1530U,       /**< Plausible VCOM (-1.53V). */
  k_ra8_epaper_test_bad_wf        = 200U,        /**< Unknown wf selector.     */
  k_ra8_epaper_test_devinfo_words = 20U,         /**< GET_DEV_INFO word count. */
  k_ra8_epaper_test_buf_lo        = 0x1234U,     /**< Image-buffer base, low.  */
  k_ra8_epaper_test_buf_hi        = 0x0012U,     /**< Image-buffer base, high. */
  k_ra8_epaper_test_buf_base      = 0x00121234U, /**< Reassembled base addr.   */
  k_ra8_epaper_test_lut_w0        = 0x4D36U,     /**< "M6" packed high-first.  */
  k_ra8_epaper_test_lut_w1        = 0x3431U,     /**< "41" packed high-first.  */
  k_ra8_epaper_test_ctrl_word     = 0x0107U,     /**< Two non-printable bytes. */
  k_ra8_epaper_test_a2_m641       = 4U,          /**< A2 mode on the M641 LUT. */
  /**
   * The only VCOM that round-trips on this fixture.
   *
   * ``ra8_epaper_set_vcom`` reads its write back and requires a match
   * before it grants the INV-VCOM-1 permit. The sim SPI is plain mmap'd
   * RAM, so a read returns the last byte written to SPDR -- which for a
   * receive is the driver's own 0xFF dummy. The loopback therefore always
   * reports 0xFFFF, and that is the only value whose readback can match
   * here. It is a fixture artefact, not a plausible bias: the *value*
   * comparison logic is exercised where it can be controlled properly, in
   * ``test_ra8_epaper_cov.c`` (programmable rx byte) and in
   * ``test_ra8_epd_cal.c`` (fully mocked seams). What this file proves is
   * the permit gating end to end.
   */
  k_ra8_epaper_test_vcom_loopback = 0xFFFFU,
} ra8_epaper_test_const_t;

/** @brief Bound SPI_B bus handle -- the seam's ctx and the mmio-seam key. */
static ra8_io_spi_bus_t s_bus;
/** @brief Seam filled from ::s_bus by ``ra8_io_spi_bus_as_ops`` in prep(). */
static ra8_spi_bus_ops_t s_bus_ops;

static ra8_epaper_cfg_t make_cfg(void)
{
  const ra8_epaper_cfg_t cfg = {
    .bus          = s_bus_ops,
    .waveform     = {.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U},
    .reset_pin    = 0U,
    .busy_pin     = 0U,
    .panel_width  = (uint16_t)k_ra8_epaper_test_panel_w,
    .panel_height = (uint16_t)k_ra8_epaper_test_panel_h,
  };
  return cfg;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* The driver state is file-static; a "sleep" call resets it back
   * to uninit. We achieve the same effect between tests via the
   * sleep API; tests that don't init (NULL-arg paths) don't need it.
   */
  (void)ra8_epaper_sleep();
  /* Bring up MSTP so ra8_spi_init can flip the SPI module-stop bit. */
  (void)ra8_mstp_init();
  /* Play the app's role: initialise SPI_B channel 0 in mode 0 and bind
   * it through the ra8_io facade into the driver's injected seam. */
  const ra8_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_epaper_test_baud_hz,
    .pclka_hz  = (uint32_t)k_ra8_epaper_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init(0U, &spi_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_spi_b(&s_bus, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_as_ops(&s_bus, &s_bus_ops));
}

/**
 * @brief Pre-stage SPSR with both ready flags asserted on every SPI channel.
 *
 * @details
 * The host build of ``ra8_spi_b.c`` short-circuits ``internal_wait_spsr``
 * (RA8_SIMULATOR_MODE branch): it returns ``k_ra8_ok`` if the requested
 * flag (SPTEF or SPRF) is asserted, otherwise ``k_ra8_err_hw_timeout``.
 * The simulator backs SPSR with plain mmap'd RAM that is zeroed by
 * ``ra8_sim_mmap_reset``, so a fresh test fixture has no flags
 * asserted. Pre-staging once is sufficient because the driver clears
 * flags via SPSRC (a separate write-1-to-clear register that, in sim,
 * is independent RAM and does not touch SPSR).
 */
static void stage_spsr_ready(void)
{
  const uint32_t both = (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
  for (uint8_t ch = 0U; ch < 2U; ch++) {
    volatile r_spi_regs_t* reg = ra8_spi(ch);
    if (reg != nullptr) {
      reg->SPSR = both;
    }
  }
}

/**
 * @brief Grant the INV-VCOM-1 permit so display commands are allowed.
 *
 * @details
 * ``ra8_epaper_display_area`` refuses every refresh until a VCOM has been
 * programmed and read back unchanged. Tests that are about something else
 * -- the LUT poll, the HRDY wait, waveform resolution -- need the permit
 * in hand first, and must take it before arming any fault seam, since
 * granting it costs bus traffic of its own.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  No fault seam is armed yet.
 * @post ``ra8_epaper_vcom_verified()`` is true.
 * @post Display commands are permitted until the next sleep / init.
 */
static void grant_vcom_permit(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom((uint16_t)k_ra8_epaper_test_vcom_loopback));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified());
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_init(nullptr));
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
  ra8_epaper_cfg_t cfg = make_cfg();

  cfg.bus.xfer8 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg.bus.xfer8 = s_bus_ops.xfer8;

  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg.panel_width = (uint16_t)k_ra8_epaper_test_panel_w;

  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
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
  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 1U, .height = 1U};
  const uint8_t           buf  = 0x80U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_epaper_load_image(&area, &buf, 1U, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_sleep());
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
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  /* Double init -> invalid state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_init(&cfg));

  /* Load + size mismatch. */
  uint8_t                 pixels[k_ra8_epaper_test_buf_pixels] = {};
  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_epaper_load_image(&area, pixels, 1U, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_load_image(nullptr,
                                       pixels,
                                       sizeof(pixels),
                                       k_ra8_epaper_pf_8bpp,
                                       k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_load_image(&area,
                                       nullptr,
                                       sizeof(pixels),
                                       k_ra8_epaper_pf_8bpp,
                                       k_ra8_epaper_endian_little));

  /* Happy load + display + sleep. The display_area LUT-idle poll reads LUTAFSR
   * over SPI; the sim SPI loopback returns the driver's own dummy byte (never
   * zero), so the real bounded poll (issue #177 / T1-01, no longer compiled
   * out) is driven through the ra8_sim_mmio seam keyed on the injected seam's
   * ctx cookie -- the bound bus handle &s_bus. Arm it to report "LUT idle" on
   * the 3rd poll so the real loop iterates twice then succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_load_image(&area,
                                       pixels,
                                       sizeof(pixels),
                                       k_ra8_epaper_pf_8bpp,
                                       k_ra8_epaper_endian_little));
  /* INV-VCOM-1: no refresh without a verified VCOM. Taken before the seam
   * is armed, because granting the permit costs bus traffic of its own. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));
  grant_vcom_permit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_satisfy_after((volatile const void*)&s_bus, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_display_area(nullptr, k_ra8_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());

  /* After sleep we're back to uninit. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));
  TEST_END("test_happy_path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the display_area LUT-idle poll is a
 * single-condition ``status == 0U`` comparison. This case drives the real
 * bounded poll's timeout leg: the ra8_sim_mmio seam keyed on the seam's ctx
 * cookie (the bound bus handle) is armed to never report idle, so the loop runs to its budget
 * and returns ``k_ra8_err_hw_timeout``. Paired with ``test_happy_path`` (which
 * arms satisfy_after for the success leg), gcov sees both legs of the poll.)
 */
static void test_display_area_lut_timeout(void)
{
  TEST_BEGIN("epaper display_area LUT poll timeout (real seam poll)");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  grant_vcom_permit();

  /* Arm the seam so the LUT-idle poll never reports idle: the real bounded loop
   * exhausts its budget and returns k_ra8_err_hw_timeout (SPI transfers still
   * succeed -- only the LUT-idle verdict is forced false). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((volatile const void*)&s_bus));
  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
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
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  grant_vcom_permit();

  /* Arm the HRDY busy-pin input register (the pin port's PCNTR2) so the panel
   * never signals ready: the next command's bounded HRDY wait exhausts its
   * budget and returns the real hardware timeout instead of the fake success
   * the deleted RA8_SIMULATOR_MODE short-circuit used to return. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sim_mmio_fail_wait((volatile const void*)&ra8_port(RA8_PIN_PORT(cfg.busy_pin))->PCNTR2));
  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_display_area(&area, k_ra8_epaper_wf_gc16));

  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper HRDY wait timeout (real seam poll)");
}

/**
 * @brief The /RESET pulse in init drives the reset pin's PORT block.
 *
 * @details
 * The issue-238 migration deleted the ``RA8_SIMULATOR_MODE`` branch
 * that compiled the reset pulse out of the host build; init now runs
 * the real high -> low -> high ``ra8_gpio_write`` sequence against the
 * RAM-backed PORT window (``ra8_delay_ms`` is a host no-op). PCNTR3 is
 * last-write-wins RAM on host, so the observable trace is the final
 * write: the POSR (set) half carrying the reset pin's bit -- the pulse
 * ended with the panel taken OUT of reset. A leftover PORR (clear)
 * pattern would mean the driver parked the panel in reset.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the pulse is straight-line
 * GPIO writes; no `&&` or `||` in the code under test)
 */
static void test_pulse_reset_drives_line(void)
{
  TEST_BEGIN("epaper init pulses /RESET for real on host");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t  cfg      = make_cfg();
  volatile r_port_regs_t* port     = ra8_port(RA8_PIN_PORT(cfg.reset_pin));
  const uint32_t          pin_mask = (uint32_t)(1UL << RA8_PIN_PIN(cfg.reset_pin));

  /* Pre-stage PCNTR3 with the PORR (clear) pattern so the assertion can
   * only pass if init's pulse actually wrote the register. */
  port->PCNTR3 = pin_mask << (uint32_t)k_ra8_pcntr_high_half_shift;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  /* Last write of the high->low->high sequence sets the POSR half. */
  TEST_ASSERT_EQ(pin_mask, port->PCNTR3);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper init pulses /RESET for real on host");
}

/**
 * @test test_mcdc_ra8_epaper
 *
 * @par MC/DC:
 * Decision A libs/ra8_hal/src/ra8_epaper_geom.c@ra8_epaper_validate_cfg:
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
 * Decision B libs/ra8_hal/src/ra8_epaper.c@ra8_epaper_load_image:
 * ``if ((buf_len != expect) || (expect == 0U))`` (2 conditions, ``||``).
 * N+1 = 3:
 * - V1: buf_len=expect, expect>0 -> dec F (accept)
 * - V2: buf_len!=expect          -> dec T (reject)
 * - V3: expect=0 (0x0 area)      -> dec T (reject)
 */
static void test_mcdc_ra8_epaper(void)
{
  TEST_BEGIN("epaper MC/DC: validate_cfg 5-cond + load_image 2-cond");
  prep();
  stage_spsr_ready();
  ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());

  cfg           = make_cfg();
  cfg.bus.xfer8 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg             = make_cfg();
  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg              = make_cfg();
  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg             = make_cfg();
  cfg.panel_width = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  cfg              = make_cfg();
  cfg.panel_height = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));

  prep();
  stage_spsr_ready();
  cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));
  uint8_t                 pixels[k_ra8_epaper_test_buf_pixels] = {};
  const ra8_epaper_area_t area_8x8 = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_load_image(&area_8x8,
                                       pixels,
                                       sizeof(pixels),
                                       k_ra8_epaper_pf_8bpp,
                                       k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_epaper_load_image(&area_8x8, pixels, 1U, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  const ra8_epaper_area_t area_0x0 = {.x = 0U, .y = 0U, .width = 0U, .height = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_epaper_load_image(&area_0x0, pixels, 0U, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  TEST_END("epaper MC/DC: validate_cfg 5-cond + load_image 2-cond");
}

/**
 * @test epaper_bits_per_pixel_and_image_bytes
 *
 * @details
 * Pins the packing arithmetic the PAL and the driver must agree on. The
 * 4 bpp row size is the one that matters commercially: it is exactly half
 * the 8 bpp size, which is where the transfer-time win comes from.
 */
static void test_pixel_format_sizing(void)
{
  TEST_BEGIN("epaper: bits_per_pixel + image_bytes");
  TEST_ASSERT_EQ(1U, ra8_epaper_bits_per_pixel(k_ra8_epaper_pf_1bpp));
  TEST_ASSERT_EQ(2U, ra8_epaper_bits_per_pixel(k_ra8_epaper_pf_2bpp));
  TEST_ASSERT_EQ(4U, ra8_epaper_bits_per_pixel(k_ra8_epaper_pf_4bpp));
  TEST_ASSERT_EQ(8U, ra8_epaper_bits_per_pixel(k_ra8_epaper_pf_8bpp));

  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 64U, .height = 4U};
  size_t                  n    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_8bpp, &n));
  TEST_ASSERT_EQ(256U, n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_4bpp, &n));
  TEST_ASSERT_EQ(128U, n); /* half of 8 bpp -- the throughput win */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_2bpp, &n));
  TEST_ASSERT_EQ(64U, n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_1bpp, &n));
  TEST_ASSERT_EQ(32U, n);

  /* Odd widths round each row up to a whole byte, per row. */
  const ra8_epaper_area_t odd = {.x = 0U, .y = 0U, .width = 3U, .height = 2U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&odd, k_ra8_epaper_pf_4bpp, &n));
  TEST_ASSERT_EQ(4U, n); /* ceil(3*4/8) = 2 bytes per row, 2 rows */

  const ra8_epaper_area_t empty = {.x = 0U, .y = 0U, .width = 0U, .height = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epaper_image_bytes(&empty, k_ra8_epaper_pf_4bpp, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_image_bytes(nullptr, k_ra8_epaper_pf_4bpp, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_4bpp, nullptr));
  TEST_END("epaper: bits_per_pixel + image_bytes");
}

/**
 * @test epaper_area_alignment
 *
 * @par MC/DC:
 * Decision: `return ((area->x % grid) == 0U) && ((area->width % grid) == 0U)`
 * in ``ra8_epaper_area_is_aligned`` (2 conditions, 1 bpp only).
 * - Vector 1: x=32, w=64  -> T&&T = true  (control: both conditions true)
 * - Vector 2: x=17, w=64  -> F&&T = false (varies the x condition only)
 * - Vector 3: x=32, w=3   -> T&&F = false (varies the width condition only)
 * Vectors 1+2 prove ``x`` independently affects the outcome; vectors 1+3
 * prove the same for ``width``. N+1 = 3 vectors for N=2 conditions.
 *
 * @details
 * A misaligned 1 bpp update renders nothing at all on an M641 panel -- the
 * controller accepts the command and the screen stays blank -- so this is
 * a correctness gate, not a quality hint.
 */
static void test_area_alignment(void)
{
  TEST_BEGIN("epaper MC/DC: 1bpp 32px X/width alignment");
  /* V1: both conditions true. */
  const ra8_epaper_area_t ok = {.x = 32U, .y = 0U, .width = 64U, .height = 1U};
  TEST_ASSERT(ra8_epaper_area_is_aligned(&ok, k_ra8_epaper_pf_1bpp));
  /* V2: x misaligned. */
  const ra8_epaper_area_t bad_x = {.x = 17U, .y = 0U, .width = 64U, .height = 1U};
  TEST_ASSERT(!ra8_epaper_area_is_aligned(&bad_x, k_ra8_epaper_pf_1bpp));
  /* V3: width misaligned. */
  const ra8_epaper_area_t bad_w = {.x = 32U, .y = 0U, .width = 3U, .height = 1U};
  TEST_ASSERT(!ra8_epaper_area_is_aligned(&bad_w, k_ra8_epaper_pf_1bpp));

  /* Depths other than 1 bpp are unconstrained. */
  TEST_ASSERT(ra8_epaper_area_is_aligned(&bad_x, k_ra8_epaper_pf_4bpp));
  TEST_ASSERT(ra8_epaper_area_is_aligned(&bad_w, k_ra8_epaper_pf_8bpp));
  /* A NULL area is a guard, not a trap. */
  TEST_ASSERT(!ra8_epaper_area_is_aligned(nullptr, k_ra8_epaper_pf_1bpp));

  /* align_area grows outward so the caller's rectangle stays covered. */
  ra8_epaper_area_t grow = {.x = 17U, .y = 0U, .width = 3U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_align_area(&grow, k_ra8_epaper_pf_1bpp, 1448U));
  TEST_ASSERT_EQ(0U, grow.x);
  TEST_ASSERT_EQ(32U, grow.width);
  TEST_ASSERT(ra8_epaper_area_is_aligned(&grow, k_ra8_epaper_pf_1bpp));

  ra8_epaper_area_t spanning = {.x = 40U, .y = 0U, .width = 40U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_align_area(&spanning, k_ra8_epaper_pf_1bpp, 1448U));
  TEST_ASSERT_EQ(32U, spanning.x);
  TEST_ASSERT_EQ(64U, spanning.width); /* covers 40..80 */

  /* Non-1bpp is a no-op. */
  ra8_epaper_area_t noop = {.x = 17U, .y = 0U, .width = 3U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_align_area(&noop, k_ra8_epaper_pf_4bpp, 1448U));
  TEST_ASSERT_EQ(17U, noop.x);

  /* Refusals. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_align_area(nullptr, k_ra8_epaper_pf_1bpp, 1448U));
  ra8_epaper_area_t any = {.x = 0U, .y = 0U, .width = 32U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_align_area(&any, k_ra8_epaper_pf_1bpp, 0U));
  ra8_epaper_area_t outside = {.x = 1440U, .y = 0U, .width = 32U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_align_area(&outside, k_ra8_epaper_pf_1bpp, 1448U));
  TEST_END("epaper MC/DC: 1bpp 32px X/width alignment");
}

/**
 * @test epaper_waveform_cfg_is_lut_derived
 *
 * @details
 * The defect this pins: A2 was hardcoded to 4, which is right for the
 * M641 LUT (6 inch / 6 inch HD) and wrong for the 7.8 / 9.7 / 10.3 inch
 * panels, where the vendor default is 6.
 */
static void test_waveform_cfg_for_lut(void)
{
  TEST_BEGIN("epaper: waveform modes are LUT-derived, not hardcoded");
  ra8_epaper_waveform_cfg_t wf = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_waveform_cfg_for_lut("M641", &wf));
  TEST_ASSERT_EQ(0U, wf.init);
  TEST_ASSERT_EQ(1U, wf.du);
  TEST_ASSERT_EQ(2U, wf.gc16);
  TEST_ASSERT_EQ(4U, wf.a2);

  /* Build suffixes must still match -- the reported string carries one. */
  wf = (ra8_epaper_waveform_cfg_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_waveform_cfg_for_lut("M641_TFAB1234", &wf));
  TEST_ASSERT_EQ(4U, wf.a2);

  /* Any other LUT gets the vendor's generic numbering. */
  wf = (ra8_epaper_waveform_cfg_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_waveform_cfg_for_lut("M841", &wf));
  TEST_ASSERT_EQ(6U, wf.a2);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_waveform_cfg_for_lut(nullptr, &wf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_waveform_cfg_for_lut("M641", nullptr));
  TEST_END("epaper: waveform modes are LUT-derived, not hardcoded");
}

/**
 * @test epaper_init_rejects_unset_waveform_map
 *
 * @details
 * A zero-initialised waveform map would refresh every mode as INIT. Init
 * refuses it so a board descriptor cannot inherit another panel's
 * numbering by omission.
 */
static void test_init_requires_waveform_map(void)
{
  TEST_BEGIN("epaper: init rejects an unset waveform map");
  prep();
  ra8_epaper_cfg_t cfg = make_cfg();
  cfg.waveform         = (ra8_epaper_waveform_cfg_t){};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));

  cfg             = make_cfg();
  cfg.waveform.a2 = 0U; /* DU / GC16 set, A2 left unset */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));

  cfg             = make_cfg();
  cfg.waveform.a2 = (uint8_t)k_ra8_epaper_test_bad_wf; /* beyond the LUT mode range */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init(&cfg));
  TEST_END("epaper: init rejects an unset waveform map");
}

/**
 * @test epaper_vcom_before_init
 *
 * @details
 * Every VCOM entry point must refuse before the panel has been brought up.
 * The lifecycle guard matters more here than elsewhere: a VCOM write to an
 * unconfigured controller is exactly the unattended-damage case this
 * driver exists to prevent.
 */
static void test_vcom_before_init(void)
{
  TEST_BEGIN("epaper: VCOM + dev_info refuse before init");
  prep();
  uint16_t              mv   = 0U;
  ra8_epaper_dev_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_get_vcom(&mv));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_set_vcom(k_ra8_epaper_test_vcom_mv));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_dev_info(&info));
  TEST_END("epaper: VCOM + dev_info refuse before init");
}

/**
 * @test epaper_vcom_commands
 *
 * @details
 * Drives the VCOM get / set commands against the simulator-backed bus.
 * The RAM-backed SPI window returns whatever the fixture last wrote rather
 * than a real panel's value, so the assertions are on the state-machine
 * guards and on the commands completing, not on a millivolt figure.
 */
static void test_vcom_commands(void)
{
  TEST_BEGIN("epaper: VCOM get / set command legs");
  prep();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  uint16_t mv = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_get_vcom(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_get_vcom(&mv));
  /* A zero VCOM is never programmable -- it is the "controller not ready"
   * signature, and driving a panel at it is the damage case. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_set_vcom(0U));
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified());

  /* A well-formed value whose readback disagrees is refused, and grants no
   * permit. On this fixture the loopback always reports 0xFFFF, so any
   * other value lands here -- which is the readback compare doing its job,
   * not a fixture quirk to work around. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_epaper_set_vcom(k_ra8_epaper_test_vcom_mv));
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified());

  /* The value the loopback does echo back is accepted and grants the permit. */
  grant_vcom_permit();

  ra8_epaper_dev_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_dev_info(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_dev_info(&info));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper: VCOM get / set command legs");
}

/**
 * @test epaper_load_image_depth_and_alignment
 *
 * @details
 * The two load-path defects together: a 1 bpp update off the 32-pixel grid
 * renders nothing on an M641 panel, so it must be refused before any bus
 * traffic; and a 4 bpp area wants exactly half the bytes of the 8 bpp
 * path, so a mis-sized buffer must be refused rather than silently
 * truncating the row.
 */
static void test_load_image_depth_and_alignment(void)
{
  TEST_BEGIN("epaper: load_image depth sizing + 1bpp alignment refusal");
  prep();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  static uint8_t          bitmap[8]  = {};
  const ra8_epaper_area_t misaligned = {.x = 8U, .y = 0U, .width = 64U, .height = 1U};
  const ra8_epaper_area_t aligned    = {.x = 0U, .y = 0U, .width = 64U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_load_image(&misaligned,
                                       bitmap,
                                       sizeof(bitmap),
                                       k_ra8_epaper_pf_1bpp,
                                       k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_load_image(&aligned,
                                       bitmap,
                                       sizeof(bitmap),
                                       k_ra8_epaper_pf_1bpp,
                                       k_ra8_epaper_endian_little));

  /* 4 bpp: half the bytes of the 8 bpp path for the same rectangle. */
  static uint8_t packed4[32] = {};
  static uint8_t packed8[64] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_load_image(&aligned,
                                       packed4,
                                       sizeof(packed4),
                                       k_ra8_epaper_pf_4bpp,
                                       k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_epaper_load_image(&aligned,
                                       packed8,
                                       sizeof(packed8),
                                       k_ra8_epaper_pf_4bpp,
                                       k_ra8_epaper_endian_little));

  /* An unknown waveform selector is refused rather than defaulted. The
   * permit is taken first so this asserts the waveform refusal and not the
   * INV-VCOM-1 one, which is checked earlier and would otherwise mask it. */
  grant_vcom_permit();
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_epaper_display_area(&aligned, (ra8_epaper_waveform_t)k_ra8_epaper_test_bad_wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper: load_image depth sizing + 1bpp alignment refusal");
}

/**
 * @test epaper_decode_dev_info_layout
 *
 * @details
 * Pins the 20-word ``GET_DEV_INFO`` layout as a pure decode, with no
 * controller and no bus. This is the seam that decides which waveform
 * mode number A2 refreshes use: ``lut_version`` feeds
 * ``ra8_epaper_waveform_cfg_for_lut``, so a decode that slipped a word
 * would silently select the wrong A2 mode on the real panel while every
 * bus-level test still passed.
 *
 * Also covers the two argument guards and the short-response refusal, and
 * asserts that a non-printable version byte is dropped to NUL rather than
 * reaching a log as a control character.
 */
static void test_decode_dev_info_layout(void)
{
  TEST_BEGIN("epaper: GET_DEV_INFO decode layout + guards");

  uint16_t words[k_ra8_epaper_test_devinfo_words] = {};
  words[0]                                        = (uint16_t)k_ra8_epaper_test_panel_w;
  words[1]                                        = (uint16_t)k_ra8_epaper_test_panel_h;
  words[2]                                        = (uint16_t)k_ra8_epaper_test_buf_lo;
  words[3]                                        = (uint16_t)k_ra8_epaper_test_buf_hi;
  /* "M641" in the LUT slot (word 12), two chars per word, high byte first. */
  words[12] = (uint16_t)k_ra8_epaper_test_lut_w0;
  words[13] = (uint16_t)k_ra8_epaper_test_lut_w1;
  /* A control byte in the firmware slot must not survive the decode. */
  words[4] = (uint16_t)k_ra8_epaper_test_ctrl_word;

  ra8_epaper_dev_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_decode_dev_info(nullptr, k_ra8_epaper_test_devinfo_words, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words, nullptr));
  /* One word short of the fixed layout is refused rather than decoded from
   * whatever follows the buffer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words - 1U, &info));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words, &info));
  TEST_ASSERT_EQ(k_ra8_epaper_test_panel_w, info.panel_width);
  TEST_ASSERT_EQ(k_ra8_epaper_test_panel_h, info.panel_height);
  /* The base address is assembled from two halves, high word shifted up. */
  TEST_ASSERT_EQ(k_ra8_epaper_test_buf_base, info.image_buf_base);
  TEST_ASSERT_EQ(0, strcmp(info.lut_version, "M641"));
  /* The control byte was dropped to NUL, terminating the string early. */
  TEST_ASSERT_EQ(0, strcmp(info.fw_version, ""));

  /* The decoded LUT string drives the waveform map, which is the whole
   * reason this decode is worth pinning. */
  ra8_epaper_waveform_cfg_t wf = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_waveform_cfg_for_lut(info.lut_version, &wf));
  TEST_ASSERT_EQ(k_ra8_epaper_test_a2_m641, wf.a2);

  TEST_END("epaper: GET_DEV_INFO decode layout + guards");
}

int main(void)
{
  test_init_null_cfg();
  test_init_bad_cfg();
  test_calls_before_init();
  test_happy_path();
  test_display_area_lut_timeout();
  test_wait_ready_hrdy_timeout();
  test_pulse_reset_drives_line();
  test_mcdc_ra8_epaper();
  test_pixel_format_sizing();
  test_area_alignment();
  test_waveform_cfg_for_lut();
  test_init_requires_waveform_map();
  test_vcom_before_init();
  test_vcom_commands();
  test_load_image_depth_and_alignment();
  test_decode_dev_info_layout();
  return 0;
}
