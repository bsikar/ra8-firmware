/**
 * @file test_ra8_epaper_waits.c
 * @brief HRDY wait-timeout arms for the IT8951 e-paper SPI driver
 *        (``ra8_epaper.c``)
 *
 * @details
 * Split out of `test_ra8_epaper.c` to keep each test translation unit under
 * the repository file-size cap. This sibling owns one thing: the six
 * re-raises that fire when a bounded HRDY wait times out part-way through a
 * transfer helper. Each transfer helper brackets its two SPI words with a
 * wait, so a single public call runs several sequential waits on the SAME
 * register (the busy pin port's PCNTR2), and only
 * ::ra8_fake_mmio_fail_nth_wait can reach any one of them individually.
 *
 * The fixture (the SPI_B bring-up, the ra8_io bus binding, the pre-staged
 * SPSR flags) is duplicated from the parent rather than shared through a
 * header, matching the pattern the other split test suites in this tree use:
 * everything here has internal linkage, so each test executable owns a
 * private copy of the driver state it drives.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_spi_b.h"
#include "ra8_mstp.h"
#include "ra8_port_regs.h"
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum epaper_wait_index_t
 * @brief Zero-based index of the HRDY wait-loop each timeout arm targets.
 *
 * @details
 * Every IT8951 transfer helper brackets its two SPI words with an HRDY wait,
 * so one public call runs several sequential bounded waits on the SAME
 * register (the busy pin port's PCNTR2). ::ra8_fake_mmio_fail_nth_wait counts
 * those loops from the moment it is armed and fails exactly one, which is what
 * lets a test reach a specific re-raise instead of only the first. The indices
 * below are the loop ordering of the calls they name:
 *
 * - `ra8_epaper_sleep()`    -> write_cmd(SLEEP): waits 0, 1.
 * - `ra8_epaper_set_vcom()` -> write_cmd(VCOM): waits 0, 1;
 *                              write_data16(SET): waits 2, 3;
 *                              write_data16(mv): waits 4, 5.
 * - `ra8_epaper_get_vcom()` -> write_cmd(VCOM): waits 0, 1;
 *                              write_data16(GET): waits 2, 3;
 *                              read_data16(): waits 4, 5.
 *
 * @invariant Each value must be armed immediately before the call it targets,
 *            because the seam counts every wait-loop on the register from the
 *            moment of arming.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_epaper_wait_cmd_second  = 1U, /**< write_cmd: the wait before the command word.   */
  k_epaper_wait_data_first  = 2U, /**< write_data16: the wait before its preamble.    */
  k_epaper_wait_data_second = 3U, /**< write_data16: the wait before the data word.   */
  k_epaper_wait_read_first  = 4U, /**< read_data16: the wait before its preamble.     */
  k_epaper_wait_read_second = 5U, /**< read_data16: the wait before the dummy + word. */
} epaper_wait_index_t;

/**
 * @enum ra8_epaper_test_const_t
 * @brief Test fixtures.
 */
typedef enum : uint32_t {
  k_ra8_epaper_test_pclka_hz   = 100000000U, /**< 100 MHz PCLKA.           */
  k_ra8_epaper_test_baud_hz    = 12000000U,  /**< 12 MHz SPI clock.        */
  k_ra8_epaper_test_panel_w    = 800U,       /**< RA8 epaper test panel w. */
  k_ra8_epaper_test_panel_h    = 600U,       /**< RA8 epaper test panel h. */
  k_ra8_epaper_test_buf_pixels = 64U,        /**< 8x8 = 64 px.             */
  k_ra8_epaper_test_vcom_mv    = 1530U,      /**< Plausible VCOM (-1.53V). */
  k_ra8_epaper_test_bad_wf     = 200U,       /**< Unknown wf selector.     */
  /**
   * The only VCOM that round-trips on this fixture.
   *
   * ``ra8_epaper_set_vcom`` reads its write back and requires a match
   * before it grants the INV-VCOM-1 permit. The fake SPI is plain mmap'd
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
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
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
 * (RA8_OFF_TARGET branch): it returns ``k_ra8_ok`` if the requested
 * flag (SPTEF or SPRF) is asserted, otherwise ``k_ra8_err_hw_timeout``.
 * The fake backs SPSR with plain mmap'd RAM that is zeroed by
 * ``ra8_fake_mmap_reset``, so a fresh test fixture has no flags
 * asserted. Pre-staging once is sufficient because the driver clears
 * flags via SPSRC (a separate write-1-to-clear register that, off-target,
 * is independent RAM and does not touch SPSR).
 */
static void stage_spsr_ready(void)
{
  const uint32_t both = (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
  for (uint8_t ch = 0U; ch < 2U; ch++) {
    volatile r_spi_regs_t* reg = ra8_spi(ch);
    if (reg != nullptr) {
      /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
      reg->SPSR = both;
    }
  }
}

/**
 * @brief The HRDY key for the configured busy pin.
 *
 * @details
 * ``internal_ra8_epaper_wait_ready`` keys its seam consult on the busy pin
 * port's PCNTR2 input register, so every timeout arm below arms that exact
 * address. Naming it once keeps the five arms from re-deriving it.
 *
 * @param[in] cfg Panel configuration whose ``busy_pin`` selects the port.
 *
 * @return Address of the PCNTR2 register the HRDY wait polls.
 *
 * @pre ``cfg.busy_pin`` names a port the fake mmap has backed.
 * @pre The fake MMIO seam is available (host test build).
 * @post No state is modified.
 * @post The returned pointer stays valid until the next ra8_fake_mmap_reset().
 *
 * @note Not thread-safe; tests are single-threaded.
 * @since 0.1.0
 */
static volatile const void* hrdy_key(const ra8_epaper_cfg_t* cfg)
{
  /* HUM Ch 20.2.2 "PCNTR2/EIDR/PIDR : Port Control Register 2" p 841 */
  return (volatile const void*)&ra8_port(RA8_PIN_PORT(cfg->busy_pin))->PCNTR2;
}

/**
 * @par MC/DC:
 * Decision: the SECOND `if (err != k_ra8_ok)` in
 * `internal_ra8_epaper_write_cmd()` (libs/ra8_hal/src/ra8_epaper.c) -- the one
 * guarding the wait between the command preamble and the command word.
 * 1 condition.
 * - Vector 1: HRDY asserts on the first poll of wait-loop 1 -> err == k_ra8_ok
 *   -> false, and the command word goes out (control; the successful sleep at
 *   the end of this case, and every other command in this file).
 * - Vector 2: wait-loop 1 armed to never satisfy -> err == k_ra8_err_hw_timeout
 *   -> true, and write_cmd re-raises it (this case).
 * N = 1 condition, N+1 = 2 vectors. Both vectors run the SAME call with only
 * the wait outcome differing, so `err` independently affects the decision.
 * Wait-loop 0 succeeds in both, which is what makes this the second re-raise
 * and not the first.
 */
static void test_write_cmd_second_wait_timeout(void)
{
  TEST_BEGIN("epaper write_cmd: second HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  /* Wait-loop 0 of write_cmd(SLEEP) succeeds, wait-loop 1 times out. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait(hrdy_key(&cfg), (uint32_t)k_epaper_wait_cmd_second));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_sleep());

  /* Control: with the seam disarmed the identical call completes. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper write_cmd: second HRDY wait timeout re-raises");
}

/**
 * @par MC/DC:
 * Decision: the FIRST `if (err != k_ra8_ok)` in
 * `internal_ra8_epaper_write_data16()` (libs/ra8_hal/src/ra8_epaper.c).
 * 1 condition.
 * - Vector 1: HRDY asserts -> err == k_ra8_ok -> false, the data preamble is
 *   sent (control; the successful set_vcom at the end of this case).
 * - Vector 2: wait-loop 2 -- the first wait inside write_data16, reached only
 *   after write_cmd(VCOM) has consumed waits 0 and 1 -- armed to never satisfy
 *   -> err == k_ra8_err_hw_timeout -> true, and write_data16 re-raises it
 *   (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the wait outcome, so
 * `err` independently affects the decision. The failure is observable at the
 * API: set_vcom must NOT grant the INV-VCOM-1 permit.
 */
static void test_write_data16_first_wait_timeout(void)
{
  TEST_BEGIN("epaper write_data16: first HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait(hrdy_key(&cfg), (uint32_t)k_epaper_wait_data_first));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_epaper_set_vcom((uint16_t)k_ra8_epaper_test_vcom_loopback));
  /* A VCOM write that never reached the wire must leave the permit revoked. */
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified());

  /* Control: disarmed, the same write completes and does grant the permit. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom((uint16_t)k_ra8_epaper_test_vcom_loopback));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper write_data16: first HRDY wait timeout re-raises");
}

/**
 * @par MC/DC:
 * Decision: the SECOND `if (err != k_ra8_ok)` in
 * `internal_ra8_epaper_write_data16()` (libs/ra8_hal/src/ra8_epaper.c) -- the
 * wait between the data preamble and the data word. 1 condition.
 * - Vector 1: HRDY asserts -> err == k_ra8_ok -> false, the data word is
 *   clocked out (control; the successful set_vcom at the end of this case).
 * - Vector 2: wait-loop 3 armed to never satisfy, with wait-loop 2 allowed to
 *   succeed -> err == k_ra8_err_hw_timeout -> true, and write_data16 re-raises
 *   it (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the wait outcome, so
 * `err` independently affects the decision. Pairing this arm with
 * test_write_data16_first_wait_timeout is what distinguishes the two
 * re-raises: they differ only in WHICH wait-loop was allowed to succeed first.
 */
static void test_write_data16_second_wait_timeout(void)
{
  TEST_BEGIN("epaper write_data16: second HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait(hrdy_key(&cfg), (uint32_t)k_epaper_wait_data_second));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_epaper_set_vcom((uint16_t)k_ra8_epaper_test_vcom_loopback));
  /* A VCOM write that never reached the wire must leave the permit revoked. */
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified());

  /* Control: disarmed, the same write completes and does grant the permit. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom((uint16_t)k_ra8_epaper_test_vcom_loopback));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper write_data16: second HRDY wait timeout re-raises");
}

/**
 * @par MC/DC:
 * Decision: the FIRST `if (err != k_ra8_ok)` in
 * `internal_ra8_epaper_read_data16()` (libs/ra8_hal/src/ra8_epaper.c).
 * 1 condition.
 * - Vector 1: HRDY asserts -> err == k_ra8_ok -> false, the read preamble is
 *   sent (control; the successful get_vcom at the end of this case).
 * - Vector 2: wait-loop 4 -- the first wait inside read_data16, reached only
 *   after write_cmd(VCOM) took waits 0/1 and write_data16(GET) took 2/3 --
 *   armed to never satisfy -> err == k_ra8_err_hw_timeout -> true, and
 *   read_data16 re-raises it (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the wait outcome, so
 * `err` independently affects the decision.
 */
static void test_read_data16_first_wait_timeout(void)
{
  TEST_BEGIN("epaper read_data16: first HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  uint16_t mv = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait(hrdy_key(&cfg), (uint32_t)k_epaper_wait_read_first));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_get_vcom(&mv));
  /* The failed read left the caller's slot untouched. */
  TEST_ASSERT_EQ(0U, mv);

  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_get_vcom(&mv));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper read_data16: first HRDY wait timeout re-raises");
}

/**
 * @par MC/DC:
 * Decision: the SECOND `if (err != k_ra8_ok)` in
 * `internal_ra8_epaper_read_data16()` (libs/ra8_hal/src/ra8_epaper.c) -- the
 * wait between the read preamble and the dummy + data words. 1 condition.
 * - Vector 1: HRDY asserts -> err == k_ra8_ok -> false, the dummy word and the
 *   data word are clocked in (control; the successful get_vcom at the end of
 *   this case).
 * - Vector 2: wait-loop 5 armed to never satisfy, with wait-loop 4 allowed to
 *   succeed -> err == k_ra8_err_hw_timeout -> true, and read_data16 re-raises
 *   it (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the wait outcome, so
 * `err` independently affects the decision. Pairing this arm with
 * test_read_data16_first_wait_timeout is what distinguishes the two re-raises:
 * they differ only in WHICH wait-loop was allowed to succeed first.
 */
static void test_read_data16_second_wait_timeout(void)
{
  TEST_BEGIN("epaper read_data16: second HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));

  uint16_t mv = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait(hrdy_key(&cfg), (uint32_t)k_epaper_wait_read_second));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_get_vcom(&mv));
  TEST_ASSERT_EQ(0U, mv);

  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_get_vcom(&mv));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper read_data16: second HRDY wait timeout re-raises");
}

/**
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` on the post-reset HRDY wait in
 * `ra8_epaper_init()` (libs/ra8_hal/src/ra8_epaper.c). 1 condition.
 * - Vector 1: HRDY asserts after the /RESET pulse -> err == k_ra8_ok -> false,
 *   and init goes on to SYS_RUN + GET_DEV_INFO (control; the successful init
 *   at the end of this case and in every other arm of this file).
 * - Vector 2: the busy pin armed to never satisfy BEFORE init runs -> the very
 *   first HRDY wait in the process times out -> true, and init re-raises
 *   k_ra8_err_hw_timeout (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the wait outcome, so
 * `err` independently affects the decision. This is the earliest of the six
 * re-raises: it fires before any command word has been sent at all.
 */
static void test_init_hrdy_timeout(void)
{
  TEST_BEGIN("epaper init: post-reset HRDY wait timeout re-raises");
  prep();
  stage_spsr_ready();
  const ra8_epaper_cfg_t cfg = make_cfg();

  /* Armed BEFORE init, so the wait that follows the /RESET pulse is the one
     that times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(hrdy_key(&cfg)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_init(&cfg));
  /* A refused init must not leave the panel claiming to be ready. */
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_sleep());

  /* Control: disarmed, the same cfg initialises. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep());
  TEST_END("epaper init: post-reset HRDY wait timeout re-raises");
}

int main(void)
{
  test_write_cmd_second_wait_timeout();
  test_write_data16_first_wait_timeout();
  test_write_data16_second_wait_timeout();
  test_read_data16_first_wait_timeout();
  test_read_data16_second_wait_timeout();
  test_init_hrdy_timeout();
  return 0;
}
