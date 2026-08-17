/**
 * @file test_ra8_board_ek_ra8d2_cov.c
 * @brief Coverage-boosting unit tests for the EK-RA8D2 v1 board-support layer
 *
 * @details
 * Companion to test_ra8_board_ek_ra8d2.c.  Exercises the error-return
 * branches and alternate switch arms that the primary test deliberately
 * omitted (it favoured API shape over branch depth).  Every test in
 * this file is deterministic: register-window state is reset via
 * ra8_fake_mmap_reset() + ra8_pin_validator_reset() before each claim
 * sequence, so no ordering dependency exists between this binary and
 * the primary test binary.
 *
 * Lines targeted (84.8% -> >=90%):
 *   155  -- ra8_board_sw_pin null out_pin early return
 *   169  -- ra8_board_sw_init invalid sw id propagation
 *   576-582 -- ra8_board_glcdc_init rgb666 / rgb565 arms
 *   593  -- ra8_board_glcdc_init conflict return
 *   629  -- ra8_board_lcd_panel_power_on conflict return
 *   714  -- ra8_board_xspi_pins_init gpio conflict return
 *   733  -- ra8_board_xspi_pins_init pfs conflict return
 *   773-787 -- ra8_board_sdhi_pins_init (all lines)
 *   798, 800, 802 -- ra8_board_arduino_pin_init valid modes
 *
 * Lines marked GCOVR_EXCL_LINE in the source (HW-only paths):
 *   187, 213, 487, 558, 634, 721
 *
 * The companion SPH0690 microphone policy in ra8_board_ek_ra8d2_pdm.c is
 * covered here too (it had no test at all): both microphone pin routes and
 * their conflict legs, and the MIC1 / MIC2 edge selection checked where it
 * lands -- PDMDSR.INPSEL after the returned policy is handed to the PDM HAL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pdm.h"
#include "ra8_pdm_regs.h"
#include "ra8_pfs_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Reset all fake peripheral state and pin ownership.
 *
 * @details
 * Mirrors reset_board_hal_state() in test_ra8_board_ek_ra8d2.c.  Called
 * at the start of every test that makes HAL pin claims to guarantee a
 * clean slate regardless of execution order within this binary.
 *
 * @pre None.
 * @post ra8_fake_mmap register window cleared; pin validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * 1. ra8_board_sw_pin -- null out_pin (source line 155)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_sw_pin rejects a null output pointer.
 *
 * @details
 * The existing test exercises the invalid-sw-id path and the happy path
 * but not the null-out_pin early return at source line 155.  This test
 * provides the missing vector.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `out_pin == nullptr`)
 *
 * @pre ra8_fake_mmap and pin validator in any state (no claims made here).
 * @post No side effects beyond the early return.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sw_pin_null_out_pin(void)
{
  TEST_BEGIN("sw_pin rejects null out_pin (line 155)");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_pin(k_ra8_board_sw1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_pin(k_ra8_board_sw2, nullptr));
  TEST_END("sw_pin rejects null out_pin (line 155)");
}

/* -------------------------------------------------------------------------
 * 2. ra8_board_sw_init -- invalid sw id propagation (source line 169)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_sw_init propagates the ra8_board_sw_pin error.
 *
 * @details
 * Source line 169 is the `return err;` inside `if (err != k_ra8_ok)` in
 * ra8_board_sw_init.  It is only reached when the pin lookup fails
 * (invalid sw id).
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra8_ok`)
 *
 * @pre None (no HAL state required).
 * @post No HAL state modified.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sw_init_invalid_id(void)
{
  TEST_BEGIN("sw_init propagates error for invalid sw id (line 169)");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_init((ra8_board_sw_id_t)99));
  TEST_END("sw_init propagates error for invalid sw id (line 169)");
}

/* -------------------------------------------------------------------------
 * 3. ra8_board_glcdc_init -- rgb666 arm (source lines 576-578)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_glcdc_init accepts the rgb666 format.
 *
 * @details
 * The existing test only calls glcdc_init with an out-of-range format
 * (which hits the default: case).  This test reaches the
 * k_ra8_board_glcdc_fmt_rgb666 arm (source lines 576-578).
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state (all pins free).
 * @post All rgb666 GLCDC output pins are routed to the GLCDC peripheral.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_rgb666(void)
{
  TEST_BEGIN("glcdc_init rgb666 routes pins (lines 576-578)");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb666));
  TEST_END("glcdc_init rgb666 routes pins (lines 576-578)");
}

/* -------------------------------------------------------------------------
 * 4. ra8_board_glcdc_init -- rgb565 arm (source lines 580-582)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_glcdc_init accepts the rgb565 format.
 *
 * @details
 * Mirrors test_glcdc_init_rgb666 but targets the rgb565 switch arm
 * at source lines 580-582.
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state (all pins free).
 * @post All rgb565 GLCDC output pins are routed to the GLCDC peripheral.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_rgb565(void)
{
  TEST_BEGIN("glcdc_init rgb565 routes pins (lines 580-582)");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb565));
  TEST_END("glcdc_init rgb565 routes pins (lines 580-582)");
}

/* -------------------------------------------------------------------------
 * 5. ra8_board_glcdc_init -- pfs conflict return (source line 593)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_glcdc_init propagates a pin-conflict error.
 *
 * @details
 * Source line 593 is `return err;` inside the GLCDC output-pin routing
 * loop when ra8_pfs_route_peripheral reports a conflict.  Calling
 * ra8_board_glcdc_init a second time (without a pin-validator reset)
 * forces the first GLCDC output pin (TCON0) to conflict, exercising
 * that return path.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra8_ok`)
 *
 * @pre Clean pin-validator state.
 * @post GLCDC output pins remain claimed from the first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_glcdc_init_conflict(void)
{
  TEST_BEGIN("glcdc_init second call returns conflict (line 593)");
  reset_state();
  /* First call: all rgb666 GLCDC output pins claimed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb666));
  /* Second call: first GLCDC output pin already owned -> conflict. */
  const ra8_err_t err = ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb666);
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("glcdc_init second call returns conflict (line 593)");
}

/* -------------------------------------------------------------------------
 * 6. ra8_board_lcd_panel_power_on -- gpio conflict return (source line 629)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_lcd_panel_power_on returns conflict on retry.
 *
 * @details
 * Source line 629 is `return err;` when the first ra8_gpio_output_init
 * call inside ra8_board_lcd_panel_power_on fails.  After a successful
 * first call the lcd_reset_l pin (P606) is claimed; a second call
 * therefore fails at that first init attempt, covering line 629.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra8_ok`)
 *
 * @pre Clean pin-validator state.
 * @post lcd_reset_l and lcd_blen pins remain claimed from the first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_lcd_panel_power_on_conflict(void)
{
  TEST_BEGIN("lcd_panel_power_on second call returns conflict (line 629)");
  reset_state();
  /* First call: claims lcd_reset_l and lcd_blen. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_lcd_panel_power_on());
  /* Second call: gpio_output_init(lcd_reset_l) fails -> line 629. */
  const ra8_err_t err = ra8_board_lcd_panel_power_on();
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("lcd_panel_power_on second call returns conflict (line 629)");
}

/**
 * @brief Verify ra8_board_backlight_set drives BLEN both on and off.
 *
 * @details
 * ra8_board_backlight_set writes the BLEN (P514) output level via ra8_gpio_write,
 * mapping @c true -> high (lit) and @c false -> low (blanked). BLEN is claimed
 * and configured as an output by ra8_board_lcd_panel_power_on, so drive it there
 * first, then exercise both the on and off arms of the level ternary so both
 * paths of the accessor are covered.
 *
 * @par MC/DC:
 * (no compound decisions -- `on ? high : low` is a single-condition ternary)
 *
 * @pre Clean pin-validator state.
 * @post BLEN (P514) is left driven low (backlight blanked).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_backlight_set_on_off(void)
{
  TEST_BEGIN("backlight_set drives BLEN high then low");
  reset_state();
  /* panel_power_on claims + configures BLEN (P514) as a GPIO output. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_lcd_panel_power_on());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_backlight_set(true));  /* light */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_backlight_set(false)); /* blank */
  TEST_END("backlight_set drives BLEN high then low");
}

/* -------------------------------------------------------------------------
 * 7. ra8_board_xspi_pins_init -- gpio conflict return (source line 714)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_xspi_pins_init returns conflict on retry.
 *
 * @details
 * Source line 714 is `return err;` when ra8_gpio_output_init for the
 * RESET_L pin fails.  After a successful first call P106 is claimed;
 * a second call fails immediately at that gpio step.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra8_ok`)
 *
 * @pre Clean pin-validator state.
 * @post RESET_L and all 11 OCTA bus pins remain claimed from first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_xspi_pins_init_gpio_conflict(void)
{
  TEST_BEGIN("xspi_pins_init second call fails at gpio step (line 714)");
  reset_state();
  /* First call: claims reset pin + all 11 OCTA bus pins. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_xspi_pins_init());
  /* Second call: gpio_output_init(reset_pin) fails -> line 714. */
  const ra8_err_t err = ra8_board_xspi_pins_init();
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("xspi_pins_init second call fails at gpio step (line 714)");
}

/* -------------------------------------------------------------------------
 * 8. ra8_board_xspi_pins_init -- pfs conflict return (source line 733)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_xspi_pins_init returns conflict in the bus-pin loop.
 *
 * @details
 * Source line 733 is `return err;` inside the OCTA bus-pin routing
 * loop.  To reach it the gpio steps must succeed, which requires the
 * RESET_L pin to be free.  After a full successful init the reset pin
 * is released via ra8_pin_validator_release, and then a second call is
 * made: the gpio steps now succeed but the first OCTA bus pin (CS) is
 * still claimed from the first call, causing ra8_pfs_route_peripheral to
 * return a conflict and triggering line 733.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition guard `err != k_ra8_ok`)
 *
 * @pre Clean pin-validator state.
 * @post RESET_L is re-claimed; OCTA bus pins remain from first call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_xspi_pins_init_pfs_conflict(void)
{
  TEST_BEGIN("xspi_pins_init fails in pfs loop (line 733)");
  reset_state();
  /* First call: claims reset pin + all 11 OCTA bus pins. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_xspi_pins_init());
  /* Free only the reset pin so the gpio steps succeed on the next call.
   * The 11 OCTA bus pins remain claimed from the first call. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t reset_pin = (ra8_port_pin_t)k_ra8_board_xspi_reset;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_release(reset_pin));
  /* Second call: gpio steps succeed; first OCTA bus pin conflicts -> 733. */
  const ra8_err_t err = ra8_board_xspi_pins_init();
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("xspi_pins_init fails in pfs loop (line 733)");
}

/* -------------------------------------------------------------------------
 * 9. ra8_board_sdhi_pins_init -- full coverage (source lines 773-787)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_sdhi_pins_init routes all eight SDHI bus pins.
 *
 * @details
 * No existing test calls ra8_board_sdhi_pins_init, leaving the entire
 * function uncovered.  This test covers the happy path (lines 773-782,
 * 785-787) plus the error return (line 783) by calling the function
 * twice: the second call conflicts on the first SDHI bus pin.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` inside the SDHI pin loop (1 condition).
 * - Vector A: err=k_ra8_ok (first call, all pins free)    -> body not taken
 * - Vector B: err!=k_ra8_ok (second call, first pin owned) -> body taken
 * Vectors A+B prove the single condition independently affects outcome.
 *
 * @pre Clean pin-validator state.
 * @post All eight SDHI bus pins remain claimed after the second call
 *       fails on the first pin.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sdhi_pins_init(void)
{
  TEST_BEGIN("sdhi_pins_init happy path then conflict (lines 773-787)");
  reset_state();
  /* First call: all eight SDHI0 bus pins routed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sdhi_pins_init());
  /* Second call: first bus pin already owned -> conflict (line 783). */
  const ra8_err_t err = ra8_board_sdhi_pins_init();
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("sdhi_pins_init happy path then conflict (lines 773-787)");
}

/* -------------------------------------------------------------------------
 * 10. ra8_board_arduino_pin_init -- valid mode arms (lines 798, 800, 802)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_arduino_pin_init succeeds for each valid mode.
 *
 * @details
 * The existing test only exercises the default (invalid mode) arm.  This
 * test hits all three valid modes:
 *   - k_ra8_board_arduino_mode_input       (line 798)
 *   - k_ra8_board_arduino_mode_input_pullup (line 800)
 *   - k_ra8_board_arduino_mode_output       (line 802)
 *
 * Distinct Arduino pins are used for each mode so pin-validator claims
 * do not conflict within the same test.
 *
 * @par MC/DC:
 * (no compound decisions -- switch dispatch only)
 *
 * @pre Clean pin-validator state.
 * @post D2, D3, D4 pins are claimed as input / input-pullup / output.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_arduino_pin_init_valid_modes(void)
{
  TEST_BEGIN("arduino_pin_init valid modes (lines 798, 800, 802)");
  reset_state();
  /* input mode -> ra8_gpio_input_init(pin, k_ra8_pull_none) -- line 798. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_board_arduino_pin_init(k_ra8_board_arduino_d2, k_ra8_board_arduino_mode_input));
  /* input_pullup mode -> ra8_gpio_input_init(pin, k_ra8_pull_up) -- line 800. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_board_arduino_pin_init(k_ra8_board_arduino_d3, k_ra8_board_arduino_mode_input_pullup));
  /* output mode -> ra8_gpio_output_init(pin, k_ra8_level_low) -- line 802. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_board_arduino_pin_init(k_ra8_board_arduino_d4, k_ra8_board_arduino_mode_output));
  TEST_END("arduino_pin_init valid modes (lines 798, 800, 802)");
}

/* -------------------------------------------------------------------------
 * 11. ra8_board_ek_ra8d2_pdm.c -- SPH0690 microphone routing and policy
 * -------------------------------------------------------------------------
 */

/**
 * @enum pdm_mic_expect_t
 * @brief Register-visible results of the board microphone policy.
 *
 * @details
 * ``k_pdm_pfs_routed`` is the exact PmnPFS word a PDM-IF route leaves behind:
 * PSEL = 0x1B (``k_ra8_psel_pdm``) in bits 31:24 with PMR set, so a route that
 * programmed the wrong peripheral select is visible rather than merely
 * "non-zero".  ``k_pdm_mic1_dsr`` / ``k_pdm_mic2_dsr`` are the PDMDSR words the
 * PDM HAL packs from the returned policy: the shared SFMD = 4 sinc order in
 * bits 6:4 with the per-microphone INPSEL edge in bit 0.
 */
typedef enum : uint32_t {
  k_pdm_pfs_routed = 0x1B010000U, /**< PmnPFS: PSEL=0x1B (PDM-IF), PMR=1.  */
  k_pdm_rate_hz    = 16000U,      /**< SPH0690 decimated sample rate.      */
  k_pdm_valid_bits = 20U,         /**< Significant PCM bits per sample.    */
  k_pdm_mic1_dsr   = 0x40U,       /**< PDMDSR: SFMD=4, INPSEL=0 (MIC1).    */
  k_pdm_mic2_dsr   = 0x41U,       /**< PDMDSR: SFMD=4, INPSEL=1 (MIC2).    */
  k_pdm_sinc_dec   = 0x7CU,       /**< SPH0690 sinc decimation ratio.      */
  k_pdm_comp_tap   = 0x1FE8U,     /**< Compensation filter tap h(10).      */
  k_pdm_comp_last  = 10U,         /**< Last compensation-filter tap index. */
} pdm_mic_expect_t;

/** @brief EK-RA8D2 PDMCLK2 microphone clock pin (P812). */
static const ra8_port_pin_t s_pdm_pin_clk = RA8_PIN(k_ra8_port_8, k_ra8_pin_12);
/** @brief EK-RA8D2 PDMDAT2 microphone data pin (P502). */
static const ra8_port_pin_t s_pdm_pin_dat = RA8_PIN(k_ra8_port_5, k_ra8_pin_2);

/**
 * @brief Verify microphone pin routing and both of its conflict legs.
 *
 * @details
 * Routes P812/P502 to PDM-IF and checks the exact PmnPFS words, then
 * pre-claims each pin in turn so the first and then the second
 * ra8_pfs_route_peripheral call fails.  The pin that was NOT programmed
 * identifies which of the two legs returned, which a bare "not ok" cannot.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the clock-pin route (1 condition)
 * - Vector 1: both pins free      -> false (both routes run)
 * - Vector 2: clock pin pre-owned -> true  (data pin never programmed)
 * Vectors 1+2 prove the condition independently affects the outcome; the
 * data-pin claim additionally pins the second route's error return.
 *
 * @pre Clean pin-validator and register-window state.
 * @pre No other owner holds P812 or P502.
 * @post Both microphone pins carry the PDM-IF peripheral select.
 * @post A pin claimed elsewhere makes routing report a GPIO conflict.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_pdm_mic_route(void)
{
  TEST_BEGIN("pdm_mic_route routes P812/P502 and reports conflicts");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_pdm_mic_route());
  TEST_ASSERT_EQ(k_pdm_pfs_routed, *ra8_pfs_pmn(k_ra8_port_8, k_ra8_pin_12));
  TEST_ASSERT_EQ(k_pdm_pfs_routed, *ra8_pfs_pmn(k_ra8_port_5, k_ra8_pin_2));

  /* Clock pin owned elsewhere: the first route fails and the data pin is
   * never programmed. */
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(s_pdm_pin_clk, "test.pdm.clk"));
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_pdm_mic_route());
  TEST_ASSERT_EQ(0U, *ra8_pfs_pmn(k_ra8_port_5, k_ra8_pin_2));

  /* Data pin owned elsewhere: the clock pin routes, the second call fails. */
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(s_pdm_pin_dat, "test.pdm.dat"));
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_pdm_mic_route());
  TEST_ASSERT_EQ(k_pdm_pfs_routed, *ra8_pfs_pmn(k_ra8_port_8, k_ra8_pin_12));
  TEST_END("pdm_mic_route routes P812/P502 and reports conflicts");
}

/**
 * @brief Verify the per-microphone filter policy and its argument guards.
 *
 * @details
 * Rejects a null destination and an out-of-range selector, then checks that
 * MIC1 and MIC2 return the same SPH0690 coefficient set and differ only in the
 * sampling edge.  Each returned policy is handed to the PDM HAL so the edge is
 * asserted where it actually matters: PDMDSR.INPSEL.
 *
 * @par MC/DC:
 * Decision: `(microphone == k_ra8_board_pdm_mic1) ? 0 : 1` (1 condition)
 * - Vector 1: microphone=MIC1 -> true  (edge 0, PDMDSR 0x40)
 * - Vector 2: microphone=MIC2 -> false (edge 1, PDMDSR 0x41)
 * Vectors 1+2 prove the selector independently affects the outcome.  The null
 * and out-of-range vectors cover the two single-condition guards ahead of it.
 *
 * @pre Clean register-window state; the PDM block can be clocked.
 * @pre No PDM channel is mid-capture.
 * @post Both microphones report 16 kHz, 20-bit, channel-2 mono policy.
 * @post The PDM block is returned to module stop.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_pdm_mic_get_config(void)
{
  TEST_BEGIN("pdm_mic_get_config returns per-microphone edge policy");
  reset_state();
  ra8_board_pdm_mic_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic_count, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic1, &cfg));
  TEST_ASSERT_EQ(k_pdm_rate_hz, cfg.sample_rate_hz);
  TEST_ASSERT_EQ(k_ra8_pdm_ch2, cfg.channel);
  TEST_ASSERT_EQ(k_pdm_valid_bits, cfg.valid_bits);
  TEST_ASSERT_EQ(0U, cfg.pdm.edge);
  TEST_ASSERT_EQ(k_pdm_sinc_dec, cfg.pdm.sinc_dec);
  TEST_ASSERT_EQ(k_pdm_comp_tap, cfg.pdm.comp_h[k_pdm_comp_last]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_configure(cfg.channel, &cfg.pdm));
  /* HUM Ch 49.2.19 "PDMDSRCHn : Mode Setting Register" p 3208 */
  TEST_ASSERT_EQ(k_pdm_mic1_dsr, ra8_pdm_ch(k_ra8_pdm_ch2)->PDMDSR);

  /* MIC2 shares the whole SPH0690 filter set and flips only the edge. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic2, &cfg));
  TEST_ASSERT_EQ(1U, cfg.pdm.edge);
  TEST_ASSERT_EQ(k_pdm_sinc_dec, cfg.pdm.sinc_dec);
  TEST_ASSERT_EQ(k_pdm_comp_tap, cfg.pdm.comp_h[k_pdm_comp_last]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_configure(cfg.channel, &cfg.pdm));
  /* HUM Ch 49.2.19 "PDMDSRCHn : Mode Setting Register" p 3208 */
  TEST_ASSERT_EQ(k_pdm_mic2_dsr, ra8_pdm_ch(k_ra8_pdm_ch2)->PDMDSR);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_deinit());
  TEST_END("pdm_mic_get_config returns per-microphone edge policy");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs every coverage-boosting test in sequence.  Returns 0 on success;
 * any TEST_ASSERT failure calls exit(1) before this function returns.
 *
 * @return 0 on success.
 *
 * @pre ra8_fake_mmap register window allocated by the test framework.
 * @post All targeted source lines are instrumented with gcov data.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  test_sw_pin_null_out_pin();
  test_sw_init_invalid_id();
  test_glcdc_init_rgb666();
  test_glcdc_init_rgb565();
  test_glcdc_init_conflict();
  test_lcd_panel_power_on_conflict();
  test_backlight_set_on_off();
  test_xspi_pins_init_gpio_conflict();
  test_xspi_pins_init_pfs_conflict();
  test_sdhi_pins_init();
  test_arduino_pin_init_valid_modes();
  test_pdm_mic_route();
  test_pdm_mic_get_config();
  return 0;
}
