/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_rmac_phy_cov.c
 * @brief Line-coverage top-up for ra8_rmac_phy.c
 *
 * @details
 * Sibling to test_ra8_rmac_phy.c. That test covers the happy paths and
 * the compound decisions under MC/DC; this file drives the remaining
 * error-return and speed-decode legs of the off-chip PHY driver so
 * gcovr merges its .gcda with the sibling target for the same source.
 *
 * The driver is fully mock-driven: every register touch flows through
 * the injected ra8_rmac_phy_io_t read/write function pointers, so each
 * uncovered line is reached by pre-seeding a targeted MDIO register
 * value or a per-register callback failure. No MCU register block is
 * touched, so no GCOVR_EXCL markers are required.
 *
 * Uncovered lines targeted (libs/ra8_hal/src/ra8_rmac_phy.c):
 *   146  BMCR.RESET write callback fails
 *   152  reset-poll read callback fails
 *   188  ANAR (reg 4) write callback fails
 *   196  1000T-CTRL (reg 9) write callback fails
 *   272,273  open() rolls opened=false and returns on advertise failure
 *   449  BMSR (reg 1) read callback fails
 *   468,469,470  MSR reports 1000BASE-T half-duplex
 *   472  MSR reports no gigabit -> fall through to LPA decode
 *   478  LPA reports 100BASE-TX full-duplex
 *   482  LPA reports 10BASE-T full-duplex
 *   483,484,485  LPA reports 10BASE-T half-duplex
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_rmac_phy.h"
#include "unity_minimal.h"

/**
 * @enum t_phy_cov_t
 * @brief IEEE 802.3 Clause 22 advertisement words and the BMCR reset bit.
 */
typedef enum : uint16_t {
  k_t_bmcr_reset      = 0x8000U, /**< BMCR bit 15: software reset, self-clearing. */
  k_t_advertise_local = 0x01E1U, /**< Advertised ability: 10/100, both duplexes,
                                      with the 802.3 selector field.            */
  k_t_advertise_gbit  = 0x0300U, /**< Advertised 1000BASE-T ability, both duplexes. */
} t_phy_cov_t;

typedef enum : uint8_t {
  k_test_phy_addr  = 1U,  /**< Test PHY address.    */
  k_test_reg_count = 32U, /**< Test register count. */
} test_rmac_cov_const_t;

/* Clause-22 register indices and bit masks used by the fixtures. */
typedef enum : uint8_t {
  k_reg_control      = 0U,  /**< Register control.       */
  k_reg_status       = 1U,  /**< Register status.        */
  k_reg_an_advert    = 4U,  /**< Register an advert.     */
  k_reg_an_partner   = 5U,  /**< Register an partner.    */
  k_reg_1000t_ctrl   = 9U,  /**< Register 1000t control. */
  k_reg_1000t_status = 10U, /**< Register 1000t status.  */
} test_rmac_cov_reg_t;

typedef enum : uint16_t {
  k_bmsr_link_up     = 0x0004U, /**< Bmsr link up.     */
  k_bmsr_an_complete = 0x0020U, /**< Bmsr an complete. */
  k_msr_1000full     = 0x0800U, /**< Msr 1000full.     */
  k_msr_1000half     = 0x0400U, /**< Msr 1000half.     */
  k_lpa_100full      = 0x0100U, /**< Lpa 100full.      */
  k_lpa_10full       = 0x0040U, /**< Lpa 10full.       */
  k_lpa_10half       = 0x0020U, /**< Lpa 10half.       */
} test_rmac_cov_bits_t;

/**
 * @brief Backing store for the mocked MDIO bus.
 *
 * @details
 * ``regs`` mirrors the 32-entry Clause-22 register file. The failure
 * flags let a case fail a read or a write of exactly one register so
 * an individual error-return leg can be driven in isolation.
 */
typedef struct {
  uint16_t regs[k_test_reg_count]; /**< Mocked Clause-22 register file.          */
  uint16_t reset_reads_remaining;  /**< reg-0 reads before RESET auto-clears.    */
  bool     read_fail_en;           /**< When set, fail reads of read_fail_reg.   */
  uint8_t  read_fail_reg;          /**< Register whose read returns hw_error.    */
  bool     write_fail_en;          /**< When set, fail writes of write_fail_reg. */
  uint8_t  write_fail_reg;         /**< Register whose write returns hw_error.   */
} test_rmac_cov_io_t;

static test_rmac_cov_io_t s_io = {};

/**
 * @brief Mocked MDIO read: optionally fail one register, else self-clear RESET.
 *
 * @details
 * Reads of register 0 model the BMCR.RESET self-clear: while
 * ``reset_reads_remaining`` is non-zero it counts down (RESET still
 * asserted); once zero it clears bit 15 so the driver's poll loop
 * observes reset completion.
 *
 * @param[in]  ctx Points to the ::test_rmac_cov_io_t backing store.
 * @param[in]  phy Unused PHY address.
 * @param[in]  reg Clause-22 register index.
 * @param[out] out Receives the register value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok        Read serviced.
 * @retval k_ra8_err_hw_error Injected read failure for ``reg``.
 *
 * @pre ``ctx`` and ``out`` are non-NULL.
 * @pre ``reg`` < ::k_test_reg_count.
 * @post ``*out`` holds the register value on success.
 * @post Register file is unchanged except the modeled RESET self-clear.
 *
 * @note Test-only mock; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_rmac_cov_io_t* st = (test_rmac_cov_io_t*)ctx;
  if (st->read_fail_en) {
    if (reg == st->read_fail_reg) {
      return k_ra8_err_hw_error;
    }
  }
  if (reg == (uint8_t)k_reg_control) {
    if (st->reset_reads_remaining > 0U) {
      st->reset_reads_remaining = (uint16_t)(st->reset_reads_remaining - 1U);
    } else {
      st->regs[k_reg_control] = (uint16_t)(st->regs[k_reg_control] & (uint16_t)~k_t_bmcr_reset);
    }
  }
  *out = st->regs[reg];
  return k_ra8_ok;
}

/**
 * @brief Mocked MDIO write: optionally fail one register, else store the value.
 *
 * @param[in] ctx  Points to the ::test_rmac_cov_io_t backing store.
 * @param[in] phy  Unused PHY address.
 * @param[in] reg  Clause-22 register index.
 * @param[in] data Value to store.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok        Write serviced.
 * @retval k_ra8_err_hw_error Injected write failure for ``reg``.
 *
 * @pre ``ctx`` is non-NULL.
 * @pre ``reg`` < ::k_test_reg_count.
 * @post ``regs[reg]`` reflects ``data`` on success.
 * @post Register file is unchanged on injected failure.
 *
 * @note Test-only mock; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_rmac_cov_io_t* st = (test_rmac_cov_io_t*)ctx;
  if (st->write_fail_en) {
    if (reg == st->write_fail_reg) {
      return k_ra8_err_hw_error;
    }
  }
  st->regs[reg] = data;
  return k_ra8_ok;
}

/**
 * @brief Reset the fake + driver + mock between cases.
 *
 * @details
 * ``reset_reads_remaining`` stays 0 so the first reg-0 poll read clears
 * RESET, letting reset-and-advertise complete on the happy paths.
 *
 * @pre None.
 * @pre None.
 * @post Driver is closed and the mock is zeroed.
 * @post ``ra8_fake_mmap`` window is reset.
 *
 * @note Test-only helper.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_rmac_phy_close();
  (void)memset(&s_io, 0, sizeof(s_io));
}

/**
 * @brief Build a default valid configuration bound to the mock bus.
 *
 * @return A ::ra8_rmac_phy_cfg_t advertising 10/100 + 1000BASE-T.
 *
 * @pre None.
 * @pre None.
 * @post Returned config has non-NULL read/write callbacks.
 * @post ``gbit_advertise`` is non-zero (1000T control written on open).
 *
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_rmac_phy_cfg_t make_cfg(void)
{
  ra8_rmac_phy_cfg_t cfg = {};
  cfg.io.read            = bus_read;
  cfg.io.write           = bus_write;
  cfg.io.ctx             = &s_io;
  cfg.lsi_type           = k_ra8_rmac_phy_lsi_ksz8091rnb;
  cfg.phy_address        = (uint8_t)k_test_phy_addr;
  cfg.reset_poll_max     = 4U;
  cfg.local_advertise    = k_t_advertise_local;
  cfg.gbit_advertise     = k_t_advertise_gbit;
  return cfg;
}

/**
 * @test rmac_phy_cov open returns the write error when BMCR.RESET write fails
 *
 * @par MC/DC:
 * No compound decision in the touched leg. Drives the single condition
 * ``if (err != k_ra8_ok)`` after the BMCR.RESET write in
 * internal_reset_and_wait() to its true arm (line 146) by failing the
 * reg-0 write in the mock.
 */
static void test_reset_write_fail(void)
{
  TEST_BEGIN("open propagates BMCR.RESET write failure");
  prep();
  s_io.write_fail_en     = true;
  s_io.write_fail_reg    = (uint8_t)k_reg_control;
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rmac_phy_open(&cfg));
  /* opened rolled back -> a re-open must not report already-exists. */
  s_io.write_fail_en = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  TEST_END("open propagates BMCR.RESET write failure");
}

/**
 * @test rmac_phy_cov open returns the read error when the reset poll read fails
 *
 * @par MC/DC:
 * No compound decision in the touched leg. Drives the single condition
 * ``if (err != k_ra8_ok)`` inside the reset-poll loop to its true arm
 * (line 152) by failing the reg-0 read in the mock after the RESET
 * write succeeds.
 */
static void test_reset_read_fail(void)
{
  TEST_BEGIN("open propagates reset-poll read failure");
  prep();
  s_io.read_fail_en      = true;
  s_io.read_fail_reg     = (uint8_t)k_reg_control;
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rmac_phy_open(&cfg));
  TEST_END("open propagates reset-poll read failure");
}

/**
 * @test rmac_phy_cov open returns the write error when the ANAR write fails
 *
 * @par MC/DC:
 * No compound decision in the touched leg. Drives the single condition
 * ``if (err != k_ra8_ok)`` after the ANAR write in
 * internal_program_advertise() to true (line 188), which in turn drives
 * open()'s rollback ``s_state.opened = false; return err`` (lines
 * 272-273) by failing the reg-4 write.
 */
static void test_advert_anar_fail(void)
{
  TEST_BEGIN("open propagates ANAR write failure");
  prep();
  s_io.write_fail_en     = true;
  s_io.write_fail_reg    = (uint8_t)k_reg_an_advert;
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rmac_phy_open(&cfg));
  /* Rollback: re-open (no failure) must succeed, proving opened==false. */
  s_io.write_fail_en = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  TEST_END("open propagates ANAR write failure");
}

/**
 * @test rmac_phy_cov open returns the write error when the 1000T write fails
 *
 * @par MC/DC:
 * Touches two single conditions, each forced (no compound decision).
 * ``if (s_state.gbit_advertise != 0U)`` is true (gbit=0x0300), then
 * ``if (err != k_ra8_ok)`` after the 1000T-CTRL write is true (line 196)
 * because the mock fails the reg-9 write.
 */
static void test_advert_gbit_fail(void)
{
  TEST_BEGIN("open propagates 1000T-CTRL write failure");
  prep();
  s_io.write_fail_en     = true;
  s_io.write_fail_reg    = (uint8_t)k_reg_1000t_ctrl;
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rmac_phy_open(&cfg));
  TEST_END("open propagates 1000T-CTRL write failure");
}

/**
 * @test rmac_phy_cov link_status returns the read error when BMSR read fails
 *
 * @par MC/DC:
 * No compound decision in the touched leg. Drives the single condition
 * ``if (err != k_ra8_ok)`` after the BMSR read to true (line 449). The
 * mock is armed to fail the reg-1 read only after open() has completed,
 * so open's reg-0 reset reads are unaffected.
 */
static void test_link_bmsr_read_fail(void)
{
  TEST_BEGIN("link_status propagates BMSR read failure");
  prep();
  const ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  s_io.read_fail_en      = true;
  s_io.read_fail_reg     = (uint8_t)k_reg_status;
  ra8_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rmac_phy_link_status_get(&lk));
  TEST_END("link_status propagates BMSR read failure");
}

/**
 * @test rmac_phy_cov link_status resolves 1000BASE-T half-duplex from the MSR
 *
 * @par MC/DC:
 * The compound decision ``if (link_up && auto_neg_done)`` is held true
 * (BMSR = LINK | AN_COMPLETE) to reach the speed-decode leg under test;
 * full MC/DC for that decision lives in test_ra8_rmac_phy.c. The
 * ``speed_ok`` call for 1000FULL returns false (MSR bit clear) and the
 * ``speed_ok`` call for 1000HALF returns true, taking lines 468-470.
 */
static void test_link_1000half(void)
{
  TEST_BEGIN("link_status resolves 1000Base-T half-duplex from MSR");
  prep();
  const ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  s_io.regs[k_reg_status]       = (uint16_t)(k_bmsr_link_up | k_bmsr_an_complete);
  s_io.regs[k_reg_1000t_status] = (uint16_t)k_msr_1000half;
  ra8_rmac_phy_link_t lk        = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(1, lk.link_up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_1000h, lk.speed);
  TEST_END("link_status resolves 1000Base-T half-duplex from MSR");
}

/**
 * @test rmac_phy_cov link_status falls through the MSR to LPA 100BASE-TX full
 *
 * @par MC/DC:
 * The compound decision ``if (link_up && auto_neg_done)`` is held true
 * to reach the speed-decode leg. With gbit advertised but the MSR
 * reporting neither 1000FULL nor 1000HALF, control falls out of the
 * gbit block (line 472) into the LPA decode, where the first single
 * condition ``lpa & 100FULL`` is true (line 478).
 */
static void test_link_100full_fallthrough(void)
{
  TEST_BEGIN("link_status falls through MSR to LPA 100Base-TX full");
  prep();
  const ra8_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  s_io.regs[k_reg_status]       = (uint16_t)(k_bmsr_link_up | k_bmsr_an_complete);
  s_io.regs[k_reg_1000t_status] = 0x0000U; /* no gigabit -> fall to LPA. */
  s_io.regs[k_reg_an_partner]   = (uint16_t)k_lpa_100full;
  ra8_rmac_phy_link_t lk        = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_100f, lk.speed);
  TEST_ASSERT_EQ(k_lpa_100full, lk.partner_ability);
  TEST_END("link_status falls through MSR to LPA 100Base-TX full");
}

/**
 * @test rmac_phy_cov link_status resolves LPA 10BASE-T full-duplex
 *
 * @par MC/DC:
 * The compound decision ``if (link_up && auto_neg_done)`` is held true.
 * With gbit advertise cleared, the 1000T block is skipped and the LPA
 * decode runs; the 100FULL/100HALF single conditions are false and the
 * 10FULL single condition is true (line 482).
 */
static void test_link_10full(void)
{
  TEST_BEGIN("link_status resolves LPA 10Base-T full-duplex");
  prep();
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  cfg.gbit_advertise     = 0U; /* skip 1000T block -> LPA decode. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  s_io.regs[k_reg_status]     = (uint16_t)(k_bmsr_link_up | k_bmsr_an_complete);
  s_io.regs[k_reg_an_partner] = (uint16_t)k_lpa_10full;
  ra8_rmac_phy_link_t lk      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_10f, lk.speed);
  TEST_END("link_status resolves LPA 10Base-T full-duplex");
}

/**
 * @test rmac_phy_cov link_status resolves LPA 10BASE-T half-duplex
 *
 * @par MC/DC:
 * The compound decision ``if (link_up && auto_neg_done)`` is held true.
 * With gbit advertise cleared, the LPA decode runs; the 100FULL /
 * 100HALF / 10FULL single conditions are all false and the final
 * 10HALF condition is true (lines 483-485).
 */
static void test_link_10half(void)
{
  TEST_BEGIN("link_status resolves LPA 10Base-T half-duplex");
  prep();
  ra8_rmac_phy_cfg_t cfg = make_cfg();
  cfg.gbit_advertise     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  s_io.regs[k_reg_status]     = (uint16_t)(k_bmsr_link_up | k_bmsr_an_complete);
  s_io.regs[k_reg_an_partner] = (uint16_t)k_lpa_10half;
  ra8_rmac_phy_link_t lk      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_10h, lk.speed);
  TEST_END("link_status resolves LPA 10Base-T half-duplex");
}

int32_t main(void)
{
  test_reset_write_fail();
  test_reset_read_fail();
  test_advert_anar_fail();
  test_advert_gbit_fail();
  test_link_bmsr_read_fail();
  test_link_1000half();
  test_link_100full_fallthrough();
  test_link_10full();
  test_link_10half();
  (void)fprintf(stderr, "[OK ] test_ra8_rmac_phy_cov.c\n");
  return 0;
}
