/**
 * @file test_ra8_rmac_phy.c
 * @brief Unit tests for ra8_rmac_phy.c
 *
 * @details Exercises PHY management, negotiation, reset, timeout, and link-mode decoding with a deterministic MDIO fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_rmac_phy.h"
#include "ra8_rmac_phy_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_phy_reg_t
 * @brief IEEE 802.3 Clause 22 PHY register indices and bit patterns.
 *
 * @details
 * The register numbers and bit assignments are fixed by the standard: BMCR is
 * register 0 with the reset bit at 15, the link-partner ability lives in
 * register 5, and the 1000BASE-T status in register 10.
 */
typedef enum : uint16_t {
  k_t_reg_lp_ability   = 5U,      /**< Register 5: link-partner ability.                    */
  k_t_reg_gbit_status  = 10U,     /**< Register 10: 1000BASE-T status.                      */
  k_t_bmcr_reset       = 0x8000U, /**< BMCR bit 15: software reset, self-clearing.          */
  k_t_lpa_100_half     = 0x0080U, /**< Link-partner ability bit for 100BASE-TX half duplex. */
  k_t_gbit_1000_full   = 0x0800U, /**< 1000BASE-T status bit for full duplex.               */
  k_t_advertise_local  = 0x01E1U, /**< Advertised ability: 10/100, both duplexes,
                                       with the 802.3 selector field.            */
  k_t_advertise_gbit   = 0x0300U, /**< Advertised 1000BASE-T ability, both duplexes. */
  k_t_reset_never_ends = 0xFFFFU, /**< Reads before the reset bit self-clears: set
                                       high enough that it never does, so the
                                       driver's timeout path is the one taken.   */
} t_phy_reg_t;

typedef enum : uint8_t {
  k_test_phy_addr  = 1U,  /**< Test PHY address.    */
  k_test_reg_count = 32U, /**< Test register count. */
  k_test_addr_high = 31U, /**< Test address high.   */
} test_rmac_const_t;

typedef struct {
  uint16_t regs[k_test_reg_count]; /**< Registers.             */
  uint16_t reset_reads_remaining;  /**< Reset reads remaining. */
  uint8_t  fail_next_read;         /**< Fail next read.        */
  uint8_t  fail_next_write;        /**< Fail next write.       */
} test_rmac_io_t;

static test_rmac_io_t s_io = {};

/** @brief Provide the file-local bus read test helper. @details Implements the bus read fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] phy Fixture argument governed by the exercised interface contract. @param[in] reg Fixture argument governed by the exercised interface contract. @param[out] out Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_rmac_io_t* st = (test_rmac_io_t*)ctx;
  if (st->fail_next_read != 0U) {
    st->fail_next_read = 0U;
    return k_ra8_err_hw_error;
  }
  if (reg == 0U) {
    if (st->reset_reads_remaining > 0U) {
      st->reset_reads_remaining = (uint16_t)(st->reset_reads_remaining - 1U);
    } else {
      st->regs[0] = (uint16_t)(st->regs[0] & (uint16_t)~k_t_bmcr_reset);
    }
  }
  *out = st->regs[reg];
  return k_ra8_ok;
}

/** @brief Provide the file-local bus write test helper. @details Implements the bus write fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] phy Fixture argument governed by the exercised interface contract. @param[in] reg Fixture argument governed by the exercised interface contract. @param[in] data Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_rmac_io_t* st = (test_rmac_io_t*)ctx;
  if (st->fail_next_write != 0U) {
    st->fail_next_write = 0U;
    return k_ra8_err_hw_error;
  }
  st->regs[reg] = data;
  return k_ra8_ok;
}

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_rmac_phy_close();
  (void)memset(&s_io, 0, sizeof(s_io));
  s_io.reset_reads_remaining = 1U;
}

/** @brief Prepare the fixture's make cfg state. @details Implements the make cfg fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_rmac_phy_cfg_t internal_make_cfg(void)
{
  ra8_rmac_phy_cfg_t cfg = {};
  cfg.io.read            = internal_bus_read;
  cfg.io.write           = internal_bus_write;
  cfg.io.ctx             = &s_io;
  cfg.lsi_type           = k_ra8_rmac_phy_lsi_ksz8091rnb;
  cfg.phy_address        = (uint8_t)k_test_phy_addr;
  cfg.reset_poll_max     = 4U;
  cfg.local_advertise    = k_t_advertise_local;
  cfg.gbit_advertise     = k_t_advertise_gbit;
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify open null behavior. @details Executes the open null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg / callbacks");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_open(nullptr));
  ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  cfg.io.read            = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_open(&cfg));
  cfg.io.read  = internal_bus_read;
  cfg.io.write = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_open(&cfg));
  TEST_END("open rejects NULL cfg / callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify open bad args behavior. @details Executes the open bad args scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad PHY addr / lsi");
  internal_prep();
  ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  cfg.phy_address        = (uint8_t)(k_test_addr_high + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_open(&cfg));
  cfg          = internal_make_cfg();
  cfg.lsi_type = (ra8_rmac_phy_lsi_t)k_ra8_rmac_phy_lsi_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_open(&cfg));
  TEST_END("open rejects bad PHY addr / lsi");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify lifecycle behavior. @details Executes the lifecycle scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_lifecycle(void)
{
  TEST_BEGIN("open / re-open / close + advertisement written");
  internal_prep();
  const ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));
  TEST_ASSERT_EQ(0x01E1, s_io.regs[4]);
  TEST_ASSERT_EQ(0x0300, s_io.regs[9]);
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_rmac_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rmac_phy_close());
  TEST_END("open / re-open / close + advertisement written");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify reset timeout behavior. @details Executes the reset timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_reset_timeout(void)
{
  TEST_BEGIN("open returns hw_timeout if BMCR.RESET never clears");
  internal_prep();
  s_io.reset_reads_remaining = k_t_reset_never_ends; /* never auto-clear */
  ra8_rmac_phy_cfg_t cfg     = internal_make_cfg();
  cfg.reset_poll_max         = 2U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_rmac_phy_open(&cfg));
  TEST_END("open returns hw_timeout if BMCR.RESET never clears");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify mdio behavior. @details Executes the mdio scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mdio(void)
{
  TEST_BEGIN("mdio read/write validation");
  internal_prep();
  const ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_mdio_write((uint8_t)k_test_reg_count, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_mdio_write(11U, 0xDEADU));

  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_mdio_read(11U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_mdio_read((uint8_t)k_test_reg_count, &v));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_mdio_read(11U, &v));
  TEST_ASSERT_EQ(0xDEAD, v);
  TEST_END("mdio read/write validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify link 1000 behavior. @details Executes the link 1000 scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_link_1000(void)
{
  TEST_BEGIN("link_status resolves 1000Base-T full-duplex from MSR");
  internal_prep();
  const ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));

  s_io.regs[1]                   = (uint16_t)(0x0004U | 0x0020U); /* link + AN done */
  s_io.regs[k_t_reg_gbit_status] = k_t_gbit_1000_full;            /* 1000F          */

  ra8_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_link_status_get(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(1, lk.link_up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_1000f, lk.speed);
  TEST_END("link_status resolves 1000Base-T full-duplex from MSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify link 100half fallback behavior. @details Executes the link 100half fallback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_link_100half_fallback(void)
{
  TEST_BEGIN("link_status falls back to LPA when no gbit advertised");
  internal_prep();
  ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  cfg.gbit_advertise     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));

  s_io.regs[1]                  = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[k_t_reg_lp_ability] = k_t_lpa_100_half; /* 100H */

  ra8_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_100h, lk.speed);
  TEST_END("link_status falls back to LPA when no gbit advertised");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify lsi and autoneg behavior. @details Executes the lsi and autoneg scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_lsi_and_autoneg(void)
{
  TEST_BEGIN("lsi_get + auto_negotiate_start");
  internal_prep();
  const ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));

  ra8_rmac_phy_lsi_t lsi = k_ra8_rmac_phy_lsi_default;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_lsi_get(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_lsi_get(&lsi));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_lsi_ksz8091rnb, lsi);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_auto_negotiate_start());
  TEST_ASSERT_EQ(0x1200, s_io.regs[0]);
  TEST_END("lsi_get + auto_negotiate_start");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify not initialized behavior. @details Executes the not initialized scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_not_initialized(void)
{
  TEST_BEGIN("ops fail with not_initialized when closed");
  internal_prep();
  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_rmac_phy_mdio_read(0U, &v));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_rmac_phy_mdio_write(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_rmac_phy_auto_negotiate_start());
  ra8_rmac_phy_link_t lk = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_rmac_phy_link_status_get(&lk));
  ra8_rmac_phy_lsi_t lsi = k_ra8_rmac_phy_lsi_default;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_rmac_phy_lsi_get(&lsi));
  TEST_END("ops fail with not_initialized when closed");
}

/**
 * @test internal_test_mcdc_link_status_link_and_an
 *
 * @par MC/DC:
 * Decision: `if ((out->link_up != 0U) && (out->auto_neg_done != 0U))`
 * (2 conditions, libs/ra8_hal/src/ra8_rmac_phy.c line 346)
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
 * conditions: minimal MC/DC. @brief Verify mcdc link status link and an behavior. @details Executes the mcdc link status link and an scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_link_status_link_and_an(void)
{
  TEST_BEGIN("rmac_phy link_status MC/DC: link_up && auto_neg_done");
  internal_prep();
  ra8_rmac_phy_cfg_t cfg = internal_make_cfg();
  cfg.gbit_advertise     = 0U; /* skip 1000T branch -> fall to LPA. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_open(&cfg));

  ra8_rmac_phy_link_t lk = {};

  /* Vector 1: BMSR=0 -> no link, no AN -> decision F. */
  s_io.regs[1]                  = 0x0000U;
  s_io.regs[k_t_reg_lp_ability] = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_no_link, lk.speed);

  /* Vector 2: BMSR.LINK only -> C1=T, C2=F -> decision F. */
  s_io.regs[1] = 0x0004U;
  s_io.regs[k_t_reg_lp_ability] =
    k_t_lpa_100_half; /* 100H bit; should be ignored since decision F. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_no_link, lk.speed);

  /* Vector 3: BMSR.LINK + AN_COMPLETE -> decision T -> resolves via LPA. */
  s_io.regs[1]                  = (uint16_t)(0x0004U | 0x0020U);
  s_io.regs[k_t_reg_lp_ability] = k_t_lpa_100_half; /* 100H */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status_get(&lk));
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_100h, lk.speed);

  TEST_END("rmac_phy link_status MC/DC: link_up && auto_neg_done");
}

/**
 * @test internal_test_mcdc_rmac_phy_internal_speed_ok
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_rmac_phy.c (call sites) -> helper at
 * libs/ra8_hal/src/ra8_rmac_phy.c:
 *   ``err == k_ra8_ok && (reg & mask) != 0`` (2 conditions, AND).
 * - V1: err=ok, mask&val=0    -> false
 * - V2: err=ok, mask&val!=0   -> true (varies right)
 * - V3: err=fail, mask&val!=0 -> false (varies left)
 * N+1 = 3. @brief Verify mcdc rmac phy internal speed ok behavior. @details Executes the mcdc rmac phy internal speed ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_rmac_phy_internal_speed_ok(void)
{
  TEST_BEGIN("rmac_phy MC/DC: speed_ok AND");
  TEST_ASSERT(!priv_ra8_rmac_phy_internal_speed_ok(k_ra8_ok, 0x0000U, 0x0800U));
  TEST_ASSERT(priv_ra8_rmac_phy_internal_speed_ok(k_ra8_ok, 0x0800U, 0x0800U));
  TEST_ASSERT(!priv_ra8_rmac_phy_internal_speed_ok(k_ra8_err_invalid_arg, 0x0800U, 0x0800U));
  TEST_END("rmac_phy MC/DC: speed_ok AND");
}

int main(void)
{
  internal_test_open_null();
  internal_test_open_bad_args();
  internal_test_lifecycle();
  internal_test_reset_timeout();
  internal_test_mdio();
  internal_test_link_1000();
  internal_test_link_100half_fallback();
  internal_test_lsi_and_autoneg();
  internal_test_not_initialized();
  internal_test_mcdc_link_status_link_and_an();
  internal_test_mcdc_rmac_phy_internal_speed_ok();
  return 0;
}
