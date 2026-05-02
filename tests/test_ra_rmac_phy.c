/**
 * @file test_ra_rmac_phy.c
 * @brief Unit tests for ra_rmac_phy.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_rmac_phy.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_phy_addr  = 1U,
  k_test_reg_count = 32U,
  k_test_addr_high = 31U,
} test_rmac_const_t;

typedef struct {
  uint16_t regs[k_test_reg_count];
  uint16_t reset_reads_remaining;
  uint8_t  fail_next_read;
  uint8_t  fail_next_write;
} test_rmac_io_t;

static test_rmac_io_t s_io = {};

static ra_err_t bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_rmac_io_t* st = (test_rmac_io_t*)ctx;
  if (st->fail_next_read != 0U) {
    st->fail_next_read = 0U;
    return k_ra_err_hw_error;
  }
  if (reg == 0U) {
    if (st->reset_reads_remaining > 0U) {
      st->reset_reads_remaining = (uint16_t)(st->reset_reads_remaining - 1U);
    } else {
      st->regs[0] = (uint16_t)(st->regs[0] & (uint16_t)~0x8000U);
    }
  }
  *out = st->regs[reg];
  return k_ra_ok;
}

static ra_err_t bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_rmac_io_t* st = (test_rmac_io_t*)ctx;
  if (st->fail_next_write != 0U) {
    st->fail_next_write = 0U;
    return k_ra_err_hw_error;
  }
  st->regs[reg] = data;
  return k_ra_ok;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_rmac_phy_close();
  (void)memset(&s_io, 0, sizeof(s_io));
  s_io.reset_reads_remaining = 1U;
}

static ra_rmac_phy_cfg_t make_cfg(void)
{
  ra_rmac_phy_cfg_t cfg = {};
  cfg.io.read           = bus_read;
  cfg.io.write          = bus_write;
  cfg.io.ctx            = &s_io;
  cfg.lsi_type          = k_ra_rmac_phy_lsi_ksz8091rnb;
  cfg.phy_address       = (uint8_t)k_test_phy_addr;
  cfg.reset_poll_max    = 4U;
  cfg.local_advertise   = 0x01E1U;
  cfg.gbit_advertise    = 0x0300U;
  return cfg;
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg / callbacks");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_open(nullptr));
  ra_rmac_phy_cfg_t cfg = make_cfg();
  cfg.io.read           = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_open(&cfg));
  cfg.io.read  = bus_read;
  cfg.io.write = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_open(&cfg));
  TEST_END("open rejects NULL cfg / callbacks");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad PHY addr / lsi");
  prep();
  ra_rmac_phy_cfg_t cfg = make_cfg();
  cfg.phy_address       = (uint8_t)(k_test_addr_high + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rmac_phy_open(&cfg));
  cfg          = make_cfg();
  cfg.lsi_type = (ra_rmac_phy_lsi_t)k_ra_rmac_phy_lsi_count;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rmac_phy_open(&cfg));
  TEST_END("open rejects bad PHY addr / lsi");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / re-open / close + advertisement written");
  prep();
  const ra_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));
  TEST_ASSERT_EQ((int32_t)0x01E1, (int32_t)s_io.regs[4]);
  TEST_ASSERT_EQ((int32_t)0x0300, (int32_t)s_io.regs[9]);
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_rmac_phy_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rmac_phy_close());
  TEST_END("open / re-open / close + advertisement written");
}

static void test_reset_timeout(void)
{
  TEST_BEGIN("open returns hw_timeout if BMCR.RESET never clears");
  prep();
  s_io.reset_reads_remaining = 0xFFFFU; /* never auto-clear */
  ra_rmac_phy_cfg_t cfg      = make_cfg();
  cfg.reset_poll_max         = 2U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_rmac_phy_open(&cfg));
  TEST_END("open returns hw_timeout if BMCR.RESET never clears");
}

static void test_mdio(void)
{
  TEST_BEGIN("mdio read/write validation");
  prep();
  const ra_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rmac_phy_mdio_write((uint8_t)k_test_reg_count, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_mdio_write(11U, 0xDEADU));

  uint16_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_mdio_read(11U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rmac_phy_mdio_read((uint8_t)k_test_reg_count, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_mdio_read(11U, &v));
  TEST_ASSERT_EQ((int32_t)0xDEAD, (int32_t)v);
  TEST_END("mdio read/write validation");
}

static void test_link_1000(void)
{
  TEST_BEGIN("link_status resolves 1000Base-T full-duplex from MSR");
  prep();
  const ra_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));

  s_io.regs[1]  = (uint16_t)(0x0004U | 0x0020U); /* link + AN done */
  s_io.regs[10] = 0x0800U;                       /* 1000F     */

  ra_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_link_status_get(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)lk.link_up);
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_speed_1000f, (int32_t)lk.speed);
  TEST_END("link_status resolves 1000Base-T full-duplex from MSR");
}

static void test_link_100half_fallback(void)
{
  TEST_BEGIN("link_status falls back to LPA when no gbit advertised");
  prep();
  ra_rmac_phy_cfg_t cfg = make_cfg();
  cfg.gbit_advertise    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));

  s_io.regs[1] = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[5] = 0x0080U; /* 100H */

  ra_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_speed_100h, (int32_t)lk.speed);
  TEST_END("link_status falls back to LPA when no gbit advertised");
}

static void test_lsi_and_autoneg(void)
{
  TEST_BEGIN("lsi_get + auto_negotiate_start");
  prep();
  const ra_rmac_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));

  ra_rmac_phy_lsi_t lsi = k_ra_rmac_phy_lsi_default;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rmac_phy_lsi_get(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_lsi_get(&lsi));
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_lsi_ksz8091rnb, (int32_t)lsi);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_auto_negotiate_start());
  TEST_ASSERT_EQ((int32_t)0x1200, (int32_t)s_io.regs[0]);
  TEST_END("lsi_get + auto_negotiate_start");
}

static void test_not_initialized(void)
{
  TEST_BEGIN("ops fail with not_initialized when closed");
  prep();
  uint16_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_rmac_phy_mdio_read(0U, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_rmac_phy_mdio_write(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_rmac_phy_auto_negotiate_start());
  ra_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_rmac_phy_link_status_get(&lk));
  ra_rmac_phy_lsi_t lsi = k_ra_rmac_phy_lsi_default;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_rmac_phy_lsi_get(&lsi));
  TEST_END("ops fail with not_initialized when closed");
}

/**
 * @test test_mcdc_link_status_link_and_an
 *
 * @par MC/DC:
 * Decision: `if ((out->link_up != 0U) && (out->auto_neg_done != 0U))`
 * (2 conditions, libs/ra_hal/src/ra_rmac_phy.c line 346)
 * BMSR.LINK is bit 2 (0x0004); BMSR.AN_COMPLETE is bit 5 (0x0020).
 * - Vector 1: BMSR=0x0000 -> link_up=0, an_done=0 -> C1=F short-circuit.
 *   Decision F -> speed=no_link.
 * - Vector 2: BMSR=0x0004 -> link_up=1, an_done=0 -> C1=T, C2=F.
 *   Decision F -> speed=no_link.
 * - Vector 3: BMSR=0x0024 -> link_up=1, an_done=1 -> C1=T, C2=T.
 *   Decision T -> speed resolution body executed (resolves via LPA).
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T (decision flips, C2
 * masked in V1 by short-circuit). MC/DC pair for C2: V2(T,F)->F vs
 * V3(T,T)->T (decision flips, C1 held T). N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC.
 */
static void test_mcdc_link_status_link_and_an(void)
{
  TEST_BEGIN("rmac_phy link_status MC/DC: link_up && auto_neg_done");
  prep();
  ra_rmac_phy_cfg_t cfg = make_cfg();
  cfg.gbit_advertise    = 0U; /* skip 1000T branch -> fall to LPA. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_open(&cfg));

  ra_rmac_phy_link_t lk = {};

  /* Vector 1: BMSR=0 -> no link, no AN -> decision F. */
  s_io.regs[1] = 0x0000U;
  s_io.regs[5] = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_speed_no_link, (int32_t)lk.speed);

  /* Vector 2: BMSR.LINK only -> C1=T, C2=F -> decision F. */
  s_io.regs[1] = 0x0004U;
  s_io.regs[5] = 0x0080U; /* 100H bit; should be ignored since decision F. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_speed_no_link, (int32_t)lk.speed);

  /* Vector 3: BMSR.LINK + AN_COMPLETE -> decision T -> resolves via LPA. */
  s_io.regs[1] = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[5] = 0x0080U; /* 100H */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ((int32_t)k_ra_rmac_phy_speed_100h, (int32_t)lk.speed);

  TEST_END("rmac_phy link_status MC/DC: link_up && auto_neg_done");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_reset_timeout();
  test_mdio();
  test_link_1000();
  test_link_100half_fallback();
  test_lsi_and_autoneg();
  test_not_initialized();
  test_mcdc_link_status_link_and_an();
  (void)fprintf(stderr, "[OK ] test_ra_rmac_phy.c\n");
  return 0;
}
