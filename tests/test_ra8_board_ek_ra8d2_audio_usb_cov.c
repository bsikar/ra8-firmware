/**
 * @file test_ra8_board_ek_ra8d2_audio_usb_cov.c
 * @brief Coverage-boosting unit tests for the EK-RA8D2 audio + USB BSP layer
 *
 * @details
 * Companion to test_ra8_board_ek_ra8d2.c.  The primary board test only
 * exercises the argument-rejection guards of
 * ``ra8_board_ek_ra8d2_audio_usb.c`` (bad sample rate / depth / channels,
 * null buffers, not-initialized states) and the single U15-timeout leg.
 * This file drives the register-level happy paths and the error-return
 * legs that only become reachable once the simulated peripheral state is
 * pre-seeded so each state-machine step advances:
 *
 *   Audio CODEC (internal_audio_bits_to_word / route_pins / build_ssie_cfg
 *   / ra8_board_audio_init / ra8_board_audio_play_sample_block):
 *     - Call ra8_board_audio_init with every supported bit depth
 *       (8/16/18/20/22/24/32) so each SSIE DWL/SWL switch arm executes,
 *       and with both mono and stereo so both ternary arms of the SSIE
 *       config builder run.  With a clean pin-validator the six DA7212
 *       pins route cleanly and ra8_ssie_init succeeds in RA8_SIMULATOR_MODE,
 *       so the success tail (s_audio_initialized = true) is covered.
 *     - Pre-claim the BCLK pin so internal_audio_route_pins returns a
 *       gpio-conflict from its first ra8_pfs_route_peripheral, covering the
 *       loop error-return leg and ra8_board_audio_init's route-error tail.
 *     - After a successful init, play a 32-bit-aligned PCM block: the SSIE
 *       transmit FIFO count (SSIFSR.TDC) is zero after reset so every word
 *       is written and ra8_board_audio_play_sample_block returns k_ra8_ok.
 *     - Pre-seed SSIFSR.TDC to the full FIFO depth so ra8_ssie_write_buffer
 *       writes zero words; the short-write guard then returns hw_timeout.
 *
 *   U15 PI4IOE5V6408 I/O expander (internal_io_expander_* + the four
 *   public SW4-override entry points):
 *     - Pre-claim SCL1 (P512) so internal_io_expander_bus_recover fails at
 *       its scl output-init and internal_io_expander_apply propagates the
 *       error.
 *     - Pre-claim SDA1 (P511) so bus-recover's scl output-init succeeds but
 *       the sda input-init fails, exercising the scl-release + error-return
 *       leg.
 *     - Pre-seed the P511 input latch (port 5 PCNTR2 bit 11) high so the
 *       bus-recovery pulse loop observes SDA released on the first pass and
 *       takes the early break.
 *     - Pre-claim P109 so internal_io_expander_enable_pullups fails at its
 *       first pull-up drive and apply propagates the error.
 *     - Pre-seed RIIC channel 1 ICSR2 (TDRE + TEND) so all three U15
 *       register writes ACK; internal_io_expander_program_u15 completes and
 *       apply reaches its success tail.
 *     - Call the host-mode / project-SW4 / octo-SPI entry points so their
 *       one-line delegations to internal_io_expander_apply run.
 *
 *   USB-HS bring-up (internal_usbhs_role_select_device /
 *   internal_usbhs_clock_and_mstp / ra8_board_usbhs_device_init /
 *   ra8_board_usbhs_host_init):
 *     - Pre-claim PD07 so the role-select GPIO drive fails and
 *       ra8_board_usbhs_device_init returns at its role-select guard.
 *     - Pre-seed OSCSF so ra8_cgc_usbhs_pll_enable's oscillator-stable and
 *       PLL2 waits resolve; internal_usbhs_clock_and_mstp then reaches its
 *       ra8_mstp_enable tail and the device / host init reach the underlying
 *       ra8_usb_*_init calls.
 *
 * No source line in ra8_board_ek_ra8d2_audio_usb.c is marked GCOVR_EXCL by
 * this work.  A small set of error-return legs remain genuinely
 * undrivable from the host (they depend on a HAL sub-call that cannot
 * fail in RA8_SIMULATOR_MODE with the fixed board constants); they are
 * documented in the accompanying task notes rather than excluded, because
 * the file clears the 90% line-coverage bar without them and the shared
 * source is left untouched.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_i2c_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_ssie_regs.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum board_ek_ra8d2_audio_usb_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready. */
} board_ek_ra8d2_audio_usb_cov_uint8_const_t;

/* -------------------------------------------------------------------------
 * Externally-linked bisect probes owned by the module under test.  They are
 * file-scope volatiles (non-static) so a JLink session can read them on the
 * target; here they let a test assert that a state-machine step executed
 * without depending on the final return value of a HAL sub-call that may
 * differ between the host simulator and silicon.
 * -------------------------------------------------------------------------
 */

/** @brief Step probe advanced across internal_usbhs_clock_and_mstp / device-init. */
extern volatile uint32_t s_usbhs_probe;

/* -------------------------------------------------------------------------
 * Local sentinels (tests are exempt from the magic-number gate; named here
 * for readability).
 * -------------------------------------------------------------------------
 */

/**
 * @brief Local test sentinels for the audio + U15 coverage cases.
 */
typedef enum : uint32_t {
  k_test_audio_rate_hz             = 48000U, /**< Nominal 48 kHz sample rate.          */
  k_test_pcm_word_cnt              = 4U,     /**< 32-bit PCM words in the play buffer. */
  k_test_pcm_len                   = 8U,     /**< int16 samples (2 per 32-bit word).   */
  k_test_usbhs_probe_post_dev_init = 5U,     /**< s_usbhs_probe post device-init.      */
} test_audio_usb_sentinel_t;

/**
 * @brief Reset all simulated peripheral state and pin ownership.
 *
 * @details
 * Zeroes every hardware register window (ra8_sim_mmap_reset) and frees all
 * claimed pins (ra8_pin_validator_reset).  Called at the start of each test
 * that issues pin claims or register pre-seeds so ordering cannot create a
 * false conflict.  Note: it does NOT reset the module's static
 * s_audio_initialized flag, which persists across tests in one binary.
 *
 * @pre None.
 * @post Register windows cleared; pin-validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * 1. internal_audio_bits_to_word -- every DWL/SWL switch arm
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_board_audio_init succeeds for every supported bit depth.
 *
 * @details
 * The primary board test only feeds an unsupported depth (12) to hit the
 * switch default.  This test feeds each supported depth so every non-default
 * arm of internal_audio_bits_to_word executes and returns k_ra8_ok, after
 * which the six DA7212 pins route cleanly (clean pin validator) and
 * ra8_ssie_init succeeds in RA8_SIMULATOR_MODE (SSIRST/FIFO-reset polls read
 * back clear immediately).  The success tail sets s_audio_initialized.
 * A mono call and stereo calls together cover both arms of the SSIE-config
 * builder's channel ternary.
 *
 * @par MC/DC:
 * The only compound decision on this path is
 * ``channels != mono && channels != stereo`` (ra8_board_audio_init).  Every
 * call here passes a valid channel count, so both conditions are false and
 * the guard is not taken -- the all-valid control vector.  The varying
 * vectors that prove independence are supplied by test_audio_init_validates
 * in test_ra8_board_ek_ra8d2.c and the source carries an mcdc-deactivated
 * annotation for that guard; no new vector is introduced here.
 *
 * @pre Clean simulated state before each sub-call.
 * @post s_audio_initialized is true; SSIE0 config registers written.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_audio_init_all_bit_depths(void)
{
  TEST_BEGIN("audio_init succeeds for every supported bit depth");
  static const uint8_t s_depths[] = {8U, 16U, 18U, 20U, 22U, 24U, 32U};
  for (uint32_t i = 0U; i < sizeof(s_depths) / sizeof(s_depths[0]); ++i) {
    reset_state();
    /* Alternate mono/stereo so both arms of the channel ternary run. */
    const uint8_t   channels = ((i % 2U) == 0U) ? 2U : 1U;
    const ra8_err_t err      = ra8_board_audio_init(k_test_audio_rate_hz, s_depths[i], channels);
    TEST_ASSERT_EQ(k_ra8_ok, err);
  }
  TEST_END("audio_init succeeds for every supported bit depth");
}

/* -------------------------------------------------------------------------
 * 2. internal_audio_route_pins -- first-route conflict propagation
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_board_audio_init propagates a routing pin conflict.
 *
 * @details
 * Pre-claiming the BCLK pin (P403) makes the first ra8_pfs_route_peripheral
 * inside internal_audio_route_pins return k_ra8_err_gpio_conflict, exercising
 * the loop's error-return leg and ra8_board_audio_init's route-error tail.
 * A valid sample rate, depth and channel count are supplied so control
 * reaches the routing step rather than an earlier guard.
 *
 * @par MC/DC:
 * The route loop's decision ``if (err != k_ra8_ok)`` is a single condition.
 * This test supplies the taken vector (err != ok); the not-taken vector is
 * supplied by test_audio_init_all_bit_depths above.  N+1 = 2 vectors.
 *
 * @pre Clean pin-validator state, then BCLK pre-claimed.
 * @post BCLK remains claimed by the test owner; init returned early.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_audio_route_pin_conflict(void)
{
  TEST_BEGIN("audio_init propagates a route pin conflict");
  reset_state();
  const ra8_port_pin_t bclk = (ra8_port_pin_t)k_ra8_board_audio_pin_bclk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(bclk, "test.audio.bclk"));
  const ra8_err_t err = ra8_board_audio_init(k_test_audio_rate_hz, 16U, 2U);
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("audio_init propagates a route pin conflict");
}

/* -------------------------------------------------------------------------
 * 3. ra8_board_audio_play_sample_block -- full-write success path
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify a play block fully written returns k_ra8_ok.
 *
 * @details
 * A successful ra8_board_audio_init sets s_audio_initialized.  The SSIE
 * transmit-FIFO count (SSIFSR.TDC) is zero after reset, so ra8_ssie_write_buffer
 * writes every 32-bit word and reports words_written == words; the short-write
 * guard is not taken and the function returns k_ra8_ok.  The PCM buffer is a
 * uint32_t array so the int16 stream the API takes is naturally 32-bit
 * aligned for the FIFO reinterpret.
 *
 * @par MC/DC:
 * The length guard ``len == 0U || (len % 2) != 0U`` is a compound decision;
 * this test passes a non-zero even length so both conditions are false
 * (control vector).  The taken vectors are supplied by
 * test_audio_play_sample_block_validates in test_ra8_board_ek_ra8d2.c.  The
 * short-write decision ``if (written != words)`` is single-condition and its
 * not-taken vector is supplied here (taken vector: test 4 below).
 *
 * @pre A successful audio init has set s_audio_initialized.
 * @post SSIE0 transmit FIFO fed with k_test_pcm_word_cnt words.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_audio_play_full_write(void)
{
  TEST_BEGIN("audio_play_sample_block writes a full block and returns ok");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_audio_init(k_test_audio_rate_hz, 16U, 2U));

  static uint32_t s_pcm_words[k_test_pcm_word_cnt] = {};
  const int16_t*  buf                              = (const int16_t*)s_pcm_words;
  const ra8_err_t err = ra8_board_audio_play_sample_block(buf, (uint32_t)k_test_pcm_len);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("audio_play_sample_block writes a full block and returns ok");
}

/* -------------------------------------------------------------------------
 * 4. ra8_board_audio_play_sample_block -- short-write timeout leg
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify a play block that cannot drain the FIFO returns hw_timeout.
 *
 * @details
 * With s_audio_initialized set, SSIFSR.TDC is pre-seeded to the full FIFO
 * depth (32).  ra8_ssie_write_buffer reads TDC >= depth on the first sample
 * and breaks immediately, so words_written stays zero and is less than the
 * requested word count; ra8_board_audio_play_sample_block then takes the
 * short-write guard and returns k_ra8_err_hw_timeout.
 *
 * @par MC/DC:
 * See test 3 for the ``if (written != words)`` table; this test supplies the
 * taken vector (written != words).
 *
 * @pre A successful audio init has set s_audio_initialized.
 * @post SSIFSR.TDC pre-seeded full for SSIE channel 0.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_audio_play_short_write(void)
{
  TEST_BEGIN("audio_play_sample_block short write returns hw_timeout");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_audio_init(k_test_audio_rate_hz, 16U, 2U));

  /* Fill the transmit-FIFO count so ra8_ssie_write_buffer writes zero words. */
  volatile r_ssie_regs_t* ssie = ra8_ssie((uint8_t)k_ra8_board_audio_ssie_channel);
  ssie->SSIFSR = (uint32_t)((uint32_t)k_ra8_ssie_fifo_depth << (uint32_t)k_ra8_ssie_shift_tdc);

  static uint32_t s_pcm_words[k_test_pcm_word_cnt] = {};
  const int16_t*  buf                              = (const int16_t*)s_pcm_words;
  const ra8_err_t err = ra8_board_audio_play_sample_block(buf, (uint32_t)k_test_pcm_len);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("audio_play_sample_block short write returns hw_timeout");
}

/* -------------------------------------------------------------------------
 * 5. internal_io_expander_bus_recover -- scl output-init conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify apply propagates a bus-recover SCL claim conflict.
 *
 * @details
 * Pre-claiming SCL1 (P512) makes internal_io_expander_bus_recover fail at its
 * ra8_gpio_output_init(scl) call, so it returns the conflict without releasing
 * anything, and internal_io_expander_apply propagates it out of
 * ra8_board_io_expander_set_usbhs_device_mode.
 *
 * @par MC/DC:
 * Both decisions on the path (bus-recover's ``if (err != k_ra8_ok)`` and
 * apply's ``if (err != k_ra8_ok)``) are single-condition; this test supplies
 * the taken vector.  The not-taken vectors are supplied by the successful
 * apply tests below and the primary board test.
 *
 * @pre Clean pin-validator state, then SCL1 pre-claimed.
 * @post SCL1 remains claimed by the test owner.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_bus_recover_scl_conflict(void)
{
  TEST_BEGIN("io_expander apply propagates bus-recover SCL conflict");
  reset_state();
  const ra8_port_pin_t scl = RA8_PIN(k_ra8_port_5, k_ra8_pin_12);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(scl, "test.ioexp.scl"));
  const ra8_err_t err = ra8_board_io_expander_set_usbhs_device_mode();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("io_expander apply propagates bus-recover SCL conflict");
}

/* -------------------------------------------------------------------------
 * 6. internal_io_expander_bus_recover -- sda input-init conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify bus-recover releases SCL and returns on an SDA conflict.
 *
 * @details
 * Pre-claiming only SDA1 (P511) lets bus-recover's ra8_gpio_output_init(scl)
 * succeed (SCL1 free) but its ra8_gpio_input_init(sda) fails; the recover
 * routine then releases the SCL claim it just took and returns the conflict,
 * which apply propagates.
 *
 * @par MC/DC:
 * The two ``if (err != k_ra8_ok)`` decisions crossed here are single-condition;
 * this test supplies the taken vector for the sda-input-init leg.
 *
 * @pre Clean pin-validator state, then SDA1 pre-claimed.
 * @post SDA1 remains claimed by the test owner; SCL1 released by recover.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_bus_recover_sda_conflict(void)
{
  TEST_BEGIN("io_expander bus-recover releases SCL on SDA conflict");
  reset_state();
  const ra8_port_pin_t sda = RA8_PIN(k_ra8_port_5, k_ra8_pin_11);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(sda, "test.ioexp.sda"));
  const ra8_err_t err = ra8_board_io_expander_set_usbhs_device_mode();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("io_expander bus-recover releases SCL on SDA conflict");
}

/* -------------------------------------------------------------------------
 * 7. internal_io_expander_bus_recover -- SDA-released early break
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify bus-recover breaks its pulse loop when SDA reads high.
 *
 * @details
 * Pre-seeding the P511 input latch (port 5 PCNTR2 bit 11) high makes the
 * first ra8_gpio_read(sda) in the bus-recovery pulse loop observe SDA released,
 * taking the loop's early break instead of running the nine clock pulses.
 * gpio input-init writes only PFS, so the pre-seeded input latch survives.
 * The remainder of apply still NACKs at the first U15 write (RIIC ICSR2 not
 * pre-seeded), so the call returns hw_timeout.
 *
 * @par MC/DC:
 * The loop guards ``if (rd == k_ra8_ok)`` and ``if (sda_lvl == high)`` are
 * nested single-condition checks; this test supplies the both-true vector
 * (read ok, sda high) that reaches the break.  The both-continue vector
 * (sda low) is supplied by the primary board test.
 *
 * @pre Clean simulated state, then P511 input latch high.
 * @post Bus recovered via the early break; U15 write NACKs.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_bus_recover_sda_high(void)
{
  TEST_BEGIN("io_expander bus-recover breaks on SDA already high");
  reset_state();
  /* PCNTR2 low half is PIDR; bit 11 == P511 input level. */
  volatile r_port_regs_t* p5 = ra8_port(k_ra8_port_5);
  p5->PCNTR2                 = (uint32_t)(1UL << (uint32_t)k_ra8_pin_11);
  const ra8_err_t err        = ra8_board_io_expander_set_usbhs_device_mode();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("io_expander bus-recover breaks on SDA already high");
}

/* -------------------------------------------------------------------------
 * 8. internal_io_expander_enable_pullups -- first pull-up conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify apply propagates a pull-up-enable claim conflict.
 *
 * @details
 * Pre-claiming P109 lets bus-recover run to completion (it uses and releases
 * P512/P511) but makes internal_io_expander_enable_pullups fail at its first
 * ra8_gpio_output_init(pullup_a); apply propagates the conflict.
 *
 * @par MC/DC:
 * enable_pullups' ``if (err != k_ra8_ok)`` and apply's guard are
 * single-condition; this test supplies the taken vector for the first
 * pull-up drive.
 *
 * @pre Clean pin-validator state, then P109 pre-claimed.
 * @post P109 remains claimed by the test owner.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_pullup_conflict(void)
{
  TEST_BEGIN("io_expander apply propagates pull-up-enable conflict");
  reset_state();
  const ra8_port_pin_t pullup_a = RA8_PIN(k_ra8_port_1, k_ra8_pin_9);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(pullup_a, "test.ioexp.pullup"));
  const ra8_err_t err = ra8_board_io_expander_set_usbhs_device_mode();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("io_expander apply propagates pull-up-enable conflict");
}

/* -------------------------------------------------------------------------
 * 9. internal_io_expander_program_u15 / apply -- full success path
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify the full U15 bring-up succeeds when RIIC ACKs every write.
 *
 * @details
 * Pre-seeding RIIC channel 1 ICSR2 with TDRE + TEND makes each of the three
 * U15 register writes (OUTPUT, HIZ, IODIR) satisfy internal_i2c_send_address
 * / drain / finish without a timeout, since ra8_i2c_init never touches ICSR2
 * and internal_i2c_clear_status preserves TDRE/TEND between writes.
 * internal_io_expander_program_u15 then advances through all three writes and
 * internal_io_expander_apply reaches its success tail, so
 * ra8_board_io_expander_set_usbhs_device_mode returns k_ra8_ok.
 *
 * @par MC/DC:
 * The program-u15 and apply decisions crossed here are single-condition
 * ``if (err != k_ra8_ok)`` checks; this test supplies the all-not-taken
 * (success) vectors.  The taken vectors are supplied by the conflict / NACK
 * tests above and the primary board test.
 *
 * @pre Clean simulated state, then RIIC1 ICSR2 = TDRE | TEND.
 * @post U15 output/hiz/iodir writes ACK; apply returns success.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_apply_success(void)
{
  TEST_BEGIN("io_expander apply succeeds when RIIC ACKs every write");
  reset_state();
  /* TDRE + TEND pre-armed so every U15 write completes without a timeout. */
  volatile r_i2c_regs_t* riic1 = ra8_i2c_regs((uint8_t)1U);
  riic1->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend);
  const ra8_err_t err = ra8_board_io_expander_set_usbhs_device_mode();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("io_expander apply succeeds when RIIC ACKs every write");
}

/* -------------------------------------------------------------------------
 * 10. Public SW4-override entry points -- delegation lines
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify the host / project / octo-SPI SW4 entry points delegate.
 *
 * @details
 * Each of ra8_board_io_expander_set_usbhs_host_mode,
 * ra8_board_io_expander_apply_project_sw4_defaults and
 * ra8_board_io_expander_set_octospi_active is a one-line delegation to
 * internal_io_expander_apply with a different SW4 output byte.  A clean state
 * is restored before each so bus-recover and the pull-up drive succeed, and
 * with RIIC left un-armed the first U15 write NACKs, so each returns
 * hw_timeout.  Resetting between calls prevents the retained P109/P311 claims
 * from turning the next call into a pull-up conflict.
 *
 * @par MC/DC:
 * (no compound decisions in the delegation lines themselves; the apply
 * decisions are covered by tests 5-9)
 *
 * @pre Clean simulated state before each entry point.
 * @post Each entry point exercised for its delegation line.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_io_expander_entry_points(void)
{
  TEST_BEGIN("io_expander host/project/octospi entry points delegate");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_io_expander_set_usbhs_host_mode());
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_io_expander_apply_project_sw4_defaults());
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_io_expander_set_octospi_active());
  TEST_END("io_expander host/project/octospi entry points delegate");
}

/* -------------------------------------------------------------------------
 * 11. internal_usbhs_role_select_device -- PD07 claim conflict
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify device init returns early when the PD07 role pin is claimed.
 *
 * @details
 * Pre-claiming PD07 (port 13 pin 7) makes internal_usbhs_role_select_device's
 * ra8_gpio_output_init fail, so ra8_board_usbhs_device_init returns the
 * conflict at its role-select guard, before the U15 override or the CGC/MSTP
 * bring-up.
 *
 * @par MC/DC:
 * The role-select guard ``if (pd07_err != k_ra8_ok)`` is single-condition;
 * this test supplies the taken vector.  The not-taken vector is supplied by
 * the device-init success test below.
 *
 * @pre Clean pin-validator state, then PD07 pre-claimed.
 * @post PD07 remains claimed by the test owner.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_usbhs_device_role_pin_conflict(void)
{
  TEST_BEGIN("usbhs_device_init returns on PD07 role-pin conflict");
  reset_state();
  const ra8_port_pin_t pd07 = RA8_PIN(k_ra8_port_13, k_ra8_pin_7);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(pd07, "test.usbhs.pd07"));
  const ra8_err_t err = ra8_board_usbhs_device_init();
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("usbhs_device_init returns on PD07 role-pin conflict");
}

/* -------------------------------------------------------------------------
 * 12. internal_usbhs_clock_and_mstp / device init -- full bring-up
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify device init reaches the USB device-init call after clocking.
 *
 * @details
 * Pre-seeding OSCSF (all stabilization flags) makes
 * ra8_cgc_usbhs_pll_enable's main-oscillator, PLL2 and HOCO waits resolve, so
 * internal_usbhs_clock_and_mstp advances past ra8_cgc_usbhs_pll_enable to its
 * ra8_mstp_enable tail and ra8_board_usbhs_device_init reaches
 * ra8_usb_device_init.  The assertion checks the module's s_usbhs_probe reached
 * its post-device-init value, which proves the pre / post probe writes around
 * ra8_usb_device_init executed regardless of that call's final code (so the
 * check does not depend on host-vs-silicon USB behaviour).
 *
 * @par MC/DC:
 * The bring-up guards (``if (err != k_ra8_ok)`` after pll-enable and after
 * clock-and-mstp) are single-condition; this test supplies the not-taken
 * (success) vectors.  The taken vectors are supplied by the primary board
 * test (which runs with OSCSF clear so pll-enable times out).
 *
 * @pre Clean simulated state, then OSCSF pre-seeded.
 * @post s_usbhs_probe == post-device-init step value.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_usbhs_device_init_full_bringup(void)
{
  TEST_BEGIN("usbhs_device_init reaches ra8_usb_device_init after clocking");
  reset_state();
  *ra8_sys_oscsf()    = (uint8_t)k_sys_oscsf_all_ready;
  const ra8_err_t err = ra8_board_usbhs_device_init();
  (void)err;
  TEST_ASSERT_EQ(k_test_usbhs_probe_post_dev_init, s_usbhs_probe);
  TEST_END("usbhs_device_init reaches ra8_usb_device_init after clocking");
}

/* -------------------------------------------------------------------------
 * 13. internal_usbhs_clock_and_mstp / host init -- full bring-up
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify host init reaches ra8_usb_host_init after clocking.
 *
 * @details
 * With OSCSF pre-seeded, internal_usbhs_clock_and_mstp succeeds and
 * ra8_board_usbhs_host_init reaches ra8_usb_host_init, whose HS bring-up skips
 * the real PHY PLL-lock poll in RA8_SIMULATOR_MODE and returns k_ra8_ok, so the
 * board host-init returns k_ra8_ok.
 *
 * @par MC/DC:
 * The bring-up guard ``if (err != k_ra8_ok)`` after clock-and-mstp is
 * single-condition; this test supplies the not-taken (success) vector.
 *
 * @pre Clean simulated state, then OSCSF pre-seeded.
 * @post ra8_usb_host_init reached; board host-init returned k_ra8_ok.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_usbhs_host_init_full_bringup(void)
{
  TEST_BEGIN("usbhs_host_init reaches ra8_usb_host_init after clocking");
  reset_state();
  *ra8_sys_oscsf()    = (uint8_t)k_sys_oscsf_all_ready;
  const ra8_err_t err = ra8_board_usbhs_host_init();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("usbhs_host_init reaches ra8_usb_host_init after clocking");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs every coverage-boosting test in sequence.  Returns 0 on success; any
 * TEST_ASSERT failure calls exit(1) before this function returns.  The audio
 * success tests run before the play tests so s_audio_initialized is set; each
 * test otherwise resets the simulated state so ordering cannot create a false
 * conflict.
 *
 * @return 0 on success.
 *
 * @pre ra8_sim_mmap register window allocated by the test framework.
 * @post Targeted source lines are instrumented with gcov data.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_audio_init_all_bit_depths();
  test_audio_route_pin_conflict();
  test_audio_play_full_write();
  test_audio_play_short_write();
  test_io_expander_bus_recover_scl_conflict();
  test_io_expander_bus_recover_sda_conflict();
  test_io_expander_bus_recover_sda_high();
  test_io_expander_pullup_conflict();
  test_io_expander_apply_success();
  test_io_expander_entry_points();
  test_usbhs_device_role_pin_conflict();
  test_usbhs_device_init_full_bringup();
  test_usbhs_host_init_full_bringup();
  (void)fprintf(stderr, "[OK ] test_ra8_board_ek_ra8d2_audio_usb_cov.c\n");
  return 0;
}
