/**
 * @file test_ra8_ether_phy.c
 * @brief Unit tests for ra8_ether_phy.c (generic MDIO PHY driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ether_phy.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ether_phy_fixture_t
 * @brief The peripheral registers and field values this fixture drives, plus the physical quantities the configuration declares.
 */
typedef enum : uint8_t {
  k_phy_reset_wait_us =
    100U, /**< Reset settling time the configuration requests, in microseconds. */
  k_phy_reg_link_partner = 5, /**< MII register 5: the link-partner ability register. */
} ether_phy_fixture_t;

/**
 * @enum ether_phy_fixture2_t
 * @brief The peripheral registers and field values this fixture drives.
 */
typedef enum : uint16_t {
  k_phy_ability_100_full = 0x0100U, /**< An ability word advertising 100BASE-TX full duplex. */
  k_phy_bmcr_reset =
    0x8000U, /**< BMCR bit 15, the PHY reset bit the driver must clear when reset completes. */
} ether_phy_fixture2_t;

typedef enum : uint8_t {
  k_test_phy_addr      = 1U,  /**< Test PHY address.      */
  k_test_reg_count     = 32U, /**< Test register count.   */
  k_test_phy_addr_high = 31U, /**< Test PHY address high. */
} test_ephy_const_t;

typedef struct {
  uint16_t regs[k_test_reg_count]; /**< Registers.             */
  uint16_t reset_reads_remaining;  /**< Reset reads remaining. */
  uint8_t  fail_next_read;         /**< Fail next read.        */
  uint8_t  fail_next_write;        /**< Fail next write.       */
} test_io_state_t;

static test_io_state_t s_io = {};

static ra8_err_t bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_io_state_t* st = (test_io_state_t*)ctx;
  if (st->fail_next_read != 0U) {
    st->fail_next_read = 0U;
    return k_ra8_err_hw_error;
  }
  if (reg >= k_test_reg_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Auto-clear BMCR.RESET after a couple polls. */
  if (reg == 0U) {
    if (st->reset_reads_remaining > 0U) {
      st->reset_reads_remaining = (uint16_t)(st->reset_reads_remaining - 1U);
    } else {
      st->regs[0] = (uint16_t)(st->regs[0] & (uint16_t)~k_phy_bmcr_reset);
    }
  }
  *out = st->regs[reg];
  return k_ra8_ok;
}

static ra8_err_t bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_io_state_t* st = (test_io_state_t*)ctx;
  if (st->fail_next_write != 0U) {
    st->fail_next_write = 0U;
    return k_ra8_err_hw_error;
  }
  if (reg >= k_test_reg_count) {
    return k_ra8_err_invalid_arg;
  }
  st->regs[reg] = data;
  return k_ra8_ok;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_ether_phy_close();
  (void)memset(&s_io, 0, sizeof(s_io));
  s_io.reset_reads_remaining = 1U; /* clears on second poll */
}

static ra8_ether_phy_cfg_t make_cfg(void)
{
  ra8_ether_phy_cfg_t cfg = {};
  cfg.io.read             = bus_read;
  cfg.io.write            = bus_write;
  cfg.io.ctx              = &s_io;
  cfg.phy_address         = (uint8_t)k_test_phy_addr;
  cfg.mii_type            = k_ra8_ether_phy_rmii;
  cfg.reset_wait_us       = k_phy_reset_wait_us;
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg + NULL callbacks");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ether_phy_open(nullptr));
  ra8_ether_phy_cfg_t cfg = make_cfg();
  cfg.io.read             = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ether_phy_open(&cfg));
  cfg          = make_cfg();
  cfg.io.write = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ether_phy_open(&cfg));
  TEST_END("open rejects NULL cfg + NULL callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_bad_addr(void)
{
  TEST_BEGIN("open rejects out-of-range PHY address");
  prep();
  ra8_ether_phy_cfg_t cfg = make_cfg();
  cfg.phy_address         = (uint8_t)(k_test_phy_addr_high + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ether_phy_open(&cfg));
  TEST_END("open rejects out-of-range PHY address");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_lifecycle(void)
{
  TEST_BEGIN("open / re-open / close");
  prep();
  const ra8_ether_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ether_phy_close());
  TEST_END("open / re-open / close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mdio_io(void)
{
  TEST_BEGIN("mdio read / write round-trip");
  prep();
  const ra8_ether_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ether_phy_mdio_write((uint8_t)k_test_reg_count, 0xABCDU));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_mdio_write(5U, 0xBEEFU));

  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ether_phy_mdio_read(5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ether_phy_mdio_read((uint8_t)k_test_reg_count, &v));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_mdio_read(5U, &v));
  TEST_ASSERT_EQ(0xBEEF, v);
  TEST_END("mdio read / write round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_status(void)
{
  TEST_BEGIN("link_status_get reflects BMSR + LPA");
  prep();
  const ra8_ether_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  /* Stage BMSR = link up + AN complete; LPA = 100full. */
  s_io.regs[1]                      = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[k_phy_reg_link_partner] = k_phy_ability_100_full;

  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ether_phy_link_status_get(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(1, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_100f, link.speed);
  TEST_END("link_status_get reflects BMSR + LPA");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_autoneg_start(void)
{
  TEST_BEGIN("auto_negotiate_start writes BMCR");
  prep();
  const ra8_ether_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_auto_negotiate_start());
  TEST_ASSERT_EQ(0x1200, s_io.regs[0]);
  TEST_END("auto_negotiate_start writes BMCR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_not_initialized(void)
{
  TEST_BEGIN("ops fail with not_initialized when closed");
  prep();
  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ether_phy_mdio_read(0U, &v));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ether_phy_mdio_write(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ether_phy_auto_negotiate_start());
  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ether_phy_link_status_get(&link));
  TEST_END("ops fail with not_initialized when closed");
}

/**
 * @test test_mcdc_link_status_resolved_speed_guard
 *
 * @par MC/DC:
 * Decision: ``if ((out->link_up != 0U) && (out->auto_neg_done != 0U))``
 * (2 conditions, libs/ra8_hal/src/ra8_ether_phy.c line 164)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: BMSR=0 (link_up=0)
 *        -> C1=F shorts. Decision F (speed remains no_link, LPA not read).
 *  - V2: BMSR=link_up only (an_complete=0)
 *        -> C1=T, C2=F. Decision F (speed remains no_link).
 *  - V3: BMSR=link_up | an_complete, LPA=100full
 *        -> C1=T, C2=T. Decision T (speed resolves to 100full from LPA).
 *
 * Independence:
 *  - V1 vs V3 vary C1 with C2 also flipping (masked under short-circuit
 *    F-side); the decision flips F->T -- valid masking-MC/DC pair.
 *  - V2 vs V3 vary C2 with C1 held T; the decision flips F->T.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_link_status_resolved_speed_guard(void)
{
  TEST_BEGIN("ra8_ether_phy_link_status_get MC/DC: link_up && auto_neg_done");
  prep();
  const ra8_ether_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  ra8_ether_phy_link_t link = {};

  /* V1: BMSR=0 -> link_up=0 -> C1=F, decision F. Speed stays no_link. */
  s_io.regs[1] = 0x0000U;
  s_io.regs[k_phy_reg_link_partner] =
    k_phy_ability_100_full; /* LPA=100full but should NOT be consulted */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(0, link.link_up);
  TEST_ASSERT_EQ(0, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_no_link, link.speed);

  /* V2: BMSR=link_up only (no an_complete) -> C1=T, C2=F. Decision F. */
  s_io.regs[1]                      = 0x0004U; /* link_up bit only */
  s_io.regs[k_phy_reg_link_partner] = k_phy_ability_100_full;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(0, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_no_link, link.speed);

  /* V3: BMSR=link_up | an_complete -> C1=T, C2=T. Decision T. LPA read. */
  s_io.regs[1]                      = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[k_phy_reg_link_partner] = k_phy_ability_100_full;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(1, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_100f, link.speed);

  TEST_END("ra8_ether_phy_link_status_get MC/DC: link_up && auto_neg_done");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_addr();
  test_lifecycle();
  test_mdio_io();
  test_link_status();
  test_autoneg_start();
  test_not_initialized();
  test_mcdc_link_status_resolved_speed_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_ether_phy.c\n");
  return 0;
}
