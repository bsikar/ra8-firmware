/**
 * @file test_ra8_eth_link_cov.c
 * @brief White-box coverage tests for ra8_eth_link.c (PHY link poll + MAC resync)
 *
 * @details
 * ra8_eth_link.c realigns the on-chip RMAC's MPIC.LSC / MPIC.PIPP with
 * whatever the off-chip PHY auto-negotiated. Every negotiated-speed
 * decision hangs off the *value* returned by ra8_rmac_mdio_c22_read.
 * The plain host register backing cannot present a non-zero MDIO read
 * result: ra8_rmac.c issues the MPSM transaction with the PRD data
 * field written as 0 for reads and then re-reads that same field, so
 * on the host every PHY register reads back 0. That leaves BMSR.LINK
 * de-asserted forever, so the resync state machine and the ANLPAR /
 * GBSR speed-decode ladder are unreachable through the public
 * ra8_eth_link_status entry point.
 *
 * To drive those paths honestly this file follows the repository's
 * established white-box pattern (see test_ra8_usb_hmsc_enum_cov.c): it
 * renames the one exported symbol so the instrumented copy does not
 * clash with the production ra8_eth_link_status linked from
 * ra8_core_hal, redirects the four hardware-transport calls
 * (ra8_rmac_mdio_c22_read, ra8_rmac_set_link, ra8_etha_set_mode,
 * ra8_delay_ms) to deterministic mocks that let the test inject real
 * PHY register contents / transfer status, then includes the
 * translation unit so its static helpers become reachable. Nothing
 * about the code under test is altered; only the values presented by
 * the mocked bus are controlled.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
/* Pull the real prototypes of every mocked transport call in with
 * their production names BEFORE the redirect macros are defined. Each
 * header is #pragma once, so the later #include of the TU under test
 * re-uses these declarations and only the call sites in the TU body
 * are rewritten to the mocks below. ra8_etha.h transitively provides
 * ra8_rmac.h (ra8_rmac_mdio_c22_read / ra8_rmac_set_link), ra8_etha_ops.h
 * (ra8_etha_set_mode) and every enum type the mocks need. */
#include "ra8_etha.h"
#include "ra8_fake_mmap.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum ra8_eth_link_cov_reg_t
 * @brief Clause-22 PHY register indices mirrored by the mock bus.
 *
 * @details Duplicated here rather than pulled from ra8_eth_internal.h
 * because the mock definitions precede the include of the TU under
 * test (which is what publishes that header). Tests are exempt from
 * the magic-number gate; these names keep the injection sites legible.
 */
typedef enum : uint8_t {
  k_cov_reg_bmcr   = 0U,  /**< BMCR   (basic mode control).      */
  k_cov_reg_bmsr   = 1U,  /**< BMSR   (basic mode status).       */
  k_cov_reg_anlpar = 5U,  /**< ANLPAR (10/100 link partner).     */
  k_cov_reg_gbsr   = 10U, /**< GBSR   (1000BASE-T link partner). */
  k_cov_reg_max    = 31U, /**< 5-bit MDIO register-address mask. */
} ra8_eth_link_cov_reg_t;

/**
 * @enum ra8_eth_link_cov_bits_t
 * @brief PHY status/ability bit patterns injected by the tests.
 */
typedef enum : uint16_t {
  k_cov_bmsr_link_up  = 0x0004U, /**< BMSR.LINK_STATUS bit 2.      */
  k_cov_bmsr_an_done  = 0x0020U, /**< BMSR.AUTONEG_COMPLETE bit 5. */
  k_cov_bmcr_speed100 = 0x2000U, /**< BMCR.SPEED_SELECT bit 13.    */
  k_cov_anlpar_10h    = 0x0020U, /**< ANLPAR 10BASE-T HD.          */
  k_cov_anlpar_10f    = 0x0040U, /**< ANLPAR 10BASE-T FD.          */
  k_cov_anlpar_100h   = 0x0080U, /**< ANLPAR 100BASE-TX HD.        */
  k_cov_anlpar_100f   = 0x0100U, /**< ANLPAR 100BASE-TX FD.        */
  k_cov_gbsr_1000h    = 0x0400U, /**< GBSR 1000BASE-T HD.          */
  k_cov_gbsr_1000f    = 0x0800U, /**< GBSR 1000BASE-T FD.          */
} ra8_eth_link_cov_bits_t;

/**
 * @enum ra8_eth_link_cov_call_t
 * @brief 1-based ETHA-mode call indices inside internal_program_mpic.
 *
 * @details internal_program_mpic issues four ra8_etha_set_mode calls in
 * a fixed order: DISABLE, CONFIG, DISABLE, OPERATION. Selecting the
 * index to fail lets each bracketed-write error leg be driven in turn.
 */
typedef enum : uint32_t {
  k_cov_etha_call_none      = 0U, /**< Never fail an ETHA transition. */
  k_cov_etha_call_disable1  = 1U, /**< First DISABLE (line 328).      */
  k_cov_etha_call_config    = 2U, /**< CONFIG (line 332).             */
  k_cov_etha_call_disable2  = 3U, /**< Second DISABLE (line 345).     */
  k_cov_etha_call_operation = 4U, /**< OPERATION (line 346).          */
} ra8_eth_link_cov_call_t;

/* --- Mock bus state (injected by each test, read by the mocks) --- */

/** @brief Backing store the mock MDIO read returns per register index. */
static uint16_t s_phy_regs[k_cov_reg_max + 1U];
/** @brief When true, the mock MDIO read fails on register ::s_mdio_fail_reg. */
static bool s_mdio_fail_enable;
/** @brief Register index that triggers the injected MDIO read failure. */
static uint8_t s_mdio_fail_reg;
/** @brief Error code the mock MDIO read reports on the failing register. */
static ra8_err_t s_mdio_fail_err;

/** @brief Running count of ra8_etha_set_mode calls in the current test. */
static uint32_t s_etha_call_count;
/** @brief 1-based ETHA call index to fail (::k_cov_etha_call_none = never). */
static uint32_t s_etha_fail_on_call;
/** @brief Error code the mock ETHA transition reports on its failing call. */
static ra8_err_t s_etha_fail_err;

/** @brief Error code the mock ra8_rmac_set_link returns (default k_ra8_ok). */
static ra8_err_t s_setlink_err;

/**
 * @brief Deterministic stand-in for ra8_rmac_mdio_c22_read.
 *
 * @details Returns the pre-seeded ::s_phy_regs value for the requested
 * register, or the injected failure code when ::s_mdio_fail_enable is
 * armed for that register. Sequential ifs (no compound decision) keep
 * the mock MC/DC-trivial.
 *
 * @param[in]  port     Ignored -- the TU picks the port; the mock is
 *                      port-agnostic.
 * @param[in]  phy_addr Ignored -- single fake PHY.
 * @param[in]  reg_addr Clause-22 register index to fetch.
 * @param[out] out      Receives the seeded register value on success.
 * @return k_ra8_ok on a served read, else the injected failure code. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_mdio_read(ra8_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t* out)
{
  (void)port;
  (void)phy_addr;
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (s_mdio_fail_enable) {
    if (reg_addr == s_mdio_fail_reg) {
      return s_mdio_fail_err;
    }
  }
  *out = s_phy_regs[reg_addr & (uint8_t)k_cov_reg_max];
  return k_ra8_ok;
}

/**
 * @brief Deterministic stand-in for ra8_etha_set_mode.
 *
 * @details Counts calls so a specific transition in the four-step
 * bracketed MPIC write can be failed on demand.
 *
 * @param[in] port Ignored.
 * @param[in] mode Ignored -- the ordering, not the opcode, selects the
 *                 failing leg.
 * @return k_ra8_ok, or ::s_etha_fail_err on the armed call index. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_etha_set_mode(ra8_etha_port_t port, ra8_etha_opc_t mode)
{
  (void)port;
  (void)mode;
  ++s_etha_call_count;
  if (s_etha_fail_on_call != (uint32_t)k_cov_etha_call_none) {
    if (s_etha_call_count == s_etha_fail_on_call) {
      return s_etha_fail_err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Deterministic stand-in for ra8_rmac_set_link.
 *
 * @param[in] port   Ignored.
 * @param[in] iface  Ignored.
 * @param[in] speed  Ignored.
 * @param[in] duplex Ignored.
 * @return ::s_setlink_err (k_ra8_ok unless a test forces rejection). @details Implements the mock rmac set link fixture operation used only by this focused test executable. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_rmac_set_link(ra8_rmac_port_t   port,
                                                          ra8_rmac_pis_t    iface,
                                                          ra8_rmac_lsc_t    speed,
                                                          ra8_rmac_duplex_t duplex)
{
  (void)port;
  (void)iface;
  (void)speed;
  (void)duplex;
  return s_setlink_err;
}

/**
 * @brief No-op stand-in for ra8_delay_ms.
 *
 * @details Collapses the 4 s auto-negotiation busy-wait to nothing so
 * the exhaust-path test runs instantly and never arms a timer.
 *
 * @param[in] ms Ignored. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_mock_delay_ms(uint32_t ms)
{
  (void)ms;
}

/* Redirect the exported symbol + the four transport calls, then pull
 * in the production TU. ra8_eth_link_status becomes ra8_eth_link_status_cov
 * so it never clashes with the copy already inside ra8_core_hal; the
 * bench-debug globals defined by the TU are likewise suffixed so both
 * copies keep private storage. s_eth_state / g_eth_mac_speed_resynced /
 * priv_ra8_eth_channel_to_port stay un-renamed and resolve to their single
 * ra8_eth.c definitions in ra8_core_hal. */
/** @brief RA8 Ethernet link status. */
/* Include-time interposition seam: each macro below must be spelled
 * EXACTLY like the production symbol it replaces, so the project rule
 * that macro names are UPPER_CASE cannot apply here -- renaming one
 * silently un-hooks the mock and the test would exercise the real
 * driver while still passing. */
// NOLINTBEGIN(readability-identifier-naming)
#define ra8_eth_link_status ra8_eth_link_status_cov
/** @brief G RA8 Ethernet PHY bmsr after wait. */
#define g_ra8_eth_phy_bmsr_after_wait g_ra8_eth_phy_bmsr_after_wait_cov
/** @brief G RA8 Ethernet anlpar. */
#define g_ra8_eth_anlpar g_ra8_eth_anlpar_cov
/** @brief G RA8 Ethernet gbsr. */
#define g_ra8_eth_gbsr g_ra8_eth_gbsr_cov
/** @brief G RA8 Ethernet resync speed lsc. */
#define g_ra8_eth_resync_speed_lsc g_ra8_eth_resync_speed_lsc_cov
/** @brief G RA8 Ethernet resync duplex. */
#define g_ra8_eth_resync_duplex g_ra8_eth_resync_duplex_cov
/** @brief RA8 rmac mdio c22 read. */
#define ra8_rmac_mdio_c22_read internal_mock_mdio_read
/** @brief RA8 rmac set link. */
#define ra8_rmac_set_link internal_mock_rmac_set_link
/** @brief RA8 etha set mode. */
#define ra8_etha_set_mode internal_mock_etha_set_mode
/** @brief RA8 delay ms. */
#define ra8_delay_ms internal_mock_delay_ms
// NOLINTEND(readability-identifier-naming)

#include "ra8_eth_link.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/**
 * @brief Reset every mock knob + shared driver latch before each test. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)memset(s_phy_regs, 0, sizeof(s_phy_regs));
  s_mdio_fail_enable = false;
  s_mdio_fail_reg    = 0U;
  s_mdio_fail_err    = k_ra8_err_hw_error;

  s_etha_call_count   = 0U;
  s_etha_fail_on_call = (uint32_t)k_cov_etha_call_none;
  s_etha_fail_err     = k_ra8_err_hw_timeout;

  s_setlink_err = k_ra8_ok;

  s_eth_state.opened       = 0U;
  s_eth_state.cfg.channel  = 0U;
  g_eth_mac_speed_resynced = false;
}

/* =============================================================================
 * internal_pick_negotiated_speed -- pure ANLPAR/GBSR priority ladder.
 * =============================================================================
 */

/**
 * @test internal_test_pick_negotiated_speed_ladder
 *
 * @par MC/DC:
 * The function under test is a run of six independent single-condition
 * ifs (no `&&` / `||`); MC/DC therefore reduces to branch coverage.
 * Each capability bit is exercised once true (its dedicated vector)
 * and once false (the all-zero vector and every higher-priority
 * vector where the bit is clear), so every if takes both directions.
 * The all-bits vector proves the documented priority (1000F wins). @brief Verify pick negotiated speed ladder behavior. @details Executes the pick negotiated speed ladder scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pick_negotiated_speed_ladder(void)
{
  TEST_BEGIN("pick_negotiated_speed: ANLPAR/GBSR priority ladder");
  internal_prep();
  ra8_rmac_lsc_t    speed  = k_ra8_rmac_lsc_1000mbit;
  ra8_rmac_duplex_t duplex = k_ra8_rmac_duplex_full;

  /* All abilities clear -> the default lowest tier survives. */
  internal_pick_negotiated_speed(0U, 0U, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_10mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_half, duplex);

  /* 10BASE-T half. */
  internal_pick_negotiated_speed((uint16_t)k_cov_anlpar_10h, 0U, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_10mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_half, duplex);

  /* 10BASE-T full. */
  internal_pick_negotiated_speed((uint16_t)k_cov_anlpar_10f, 0U, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_10mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_full, duplex);

  /* 100BASE-TX half. */
  internal_pick_negotiated_speed((uint16_t)k_cov_anlpar_100h, 0U, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_100mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_half, duplex);

  /* 100BASE-TX full. */
  internal_pick_negotiated_speed((uint16_t)k_cov_anlpar_100f, 0U, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_100mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_full, duplex);

  /* 1000BASE-T half (GBSR path). */
  internal_pick_negotiated_speed(0U, (uint16_t)k_cov_gbsr_1000h, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_1000mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_half, duplex);

  /* 1000BASE-T full (GBSR path). */
  internal_pick_negotiated_speed(0U, (uint16_t)k_cov_gbsr_1000f, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_1000mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_full, duplex);

  /* Every ability set -> highest priority (1000F) wins. */
  const uint16_t all_anlpar = (uint16_t)((uint16_t)k_cov_anlpar_10h | (uint16_t)k_cov_anlpar_10f |
                                         (uint16_t)k_cov_anlpar_100h | (uint16_t)k_cov_anlpar_100f);
  const uint16_t all_gbsr   = (uint16_t)((uint16_t)k_cov_gbsr_1000h | (uint16_t)k_cov_gbsr_1000f);
  internal_pick_negotiated_speed(all_anlpar, all_gbsr, &speed, &duplex);
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_1000mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_full, duplex);

  TEST_END("pick_negotiated_speed: ANLPAR/GBSR priority ladder");
}

/* =============================================================================
 * internal_wait_for_autoneg -- bounded BMSR.AN_COMPLETE poll.
 * =============================================================================
 */

/**
 * @test internal_test_wait_for_autoneg_an_done
 *
 * @par MC/DC:
 * (no compound decision in the code under test -- the poll loop guards
 * are two independent single-condition ifs; this vector takes the
 * AN_COMPLETE-set exit.) @brief Verify wait for autoneg an done behavior. @details Executes the wait for autoneg an done scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_wait_for_autoneg_an_done(void)
{
  TEST_BEGIN("wait_for_autoneg: AN_COMPLETE already asserted");
  internal_prep();
  s_phy_regs[k_cov_reg_bmsr] = (uint16_t)k_cov_bmsr_an_done;
  internal_wait_for_autoneg(k_ra8_rmac_port_0);
  TEST_ASSERT_EQ(k_cov_bmsr_an_done, g_ra8_eth_phy_bmsr_after_wait_cov);
  TEST_END("wait_for_autoneg: AN_COMPLETE already asserted");
}

/**
 * @test internal_test_wait_for_autoneg_mdio_fail
 *
 * @par MC/DC:
 * (no compound decision -- this vector drives the MDIO-read-failed
 * early return leg of the poll loop.) @brief Verify wait for autoneg mdio fail behavior. @details Executes the wait for autoneg mdio fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_wait_for_autoneg_mdio_fail(void)
{
  TEST_BEGIN("wait_for_autoneg: MDIO read failure aborts the poll");
  internal_prep();
  s_mdio_fail_enable = true;
  s_mdio_fail_reg    = (uint8_t)k_cov_reg_bmsr;
  s_mdio_fail_err    = k_ra8_err_hw_error;
  internal_wait_for_autoneg(k_ra8_rmac_port_0);
  /* Read never populated bmsr, so the snapshot stays 0. */
  TEST_ASSERT_EQ(0U, g_ra8_eth_phy_bmsr_after_wait_cov);
  TEST_END("wait_for_autoneg: MDIO read failure aborts the poll");
}

/**
 * @test internal_test_wait_for_autoneg_exhaust
 *
 * @par MC/DC:
 * (no compound decision -- BMSR never asserts AN_COMPLETE and the read
 * always succeeds, so the loop runs to its iteration ceiling and falls
 * through to the post-loop snapshot.) @brief Verify wait for autoneg exhaust behavior. @details Executes the wait for autoneg exhaust scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_wait_for_autoneg_exhaust(void)
{
  TEST_BEGIN("wait_for_autoneg: budget exhausted with AN never done");
  internal_prep();
  s_phy_regs[k_cov_reg_bmsr] = 0U; /* link partner never finishes AN. */
  internal_wait_for_autoneg(k_ra8_rmac_port_0);
  TEST_ASSERT_EQ(0U, g_ra8_eth_phy_bmsr_after_wait_cov);
  TEST_END("wait_for_autoneg: budget exhausted with AN never done");
}

/* =============================================================================
 * internal_query_negotiated_speed -- read ANLPAR + GBSR, decode.
 * =============================================================================
 */

/**
 * @test internal_test_query_negotiated_speed_ok
 *
 * @par MC/DC:
 * (no compound decision -- both MDIO reads succeed; the two error
 * guards are single-condition and take their false leg here.) @brief Verify query negotiated speed ok behavior. @details Executes the query negotiated speed ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_query_negotiated_speed_ok(void)
{
  TEST_BEGIN("query_negotiated_speed: both reads OK -> decoded speed");
  internal_prep();
  s_phy_regs[k_cov_reg_anlpar] = (uint16_t)k_cov_anlpar_100f;
  s_phy_regs[k_cov_reg_gbsr]   = 0U;
  ra8_rmac_lsc_t    speed      = k_ra8_rmac_lsc_10mbit;
  ra8_rmac_duplex_t duplex     = k_ra8_rmac_duplex_half;
  TEST_ASSERT_EQ(k_ra8_ok, internal_query_negotiated_speed(k_ra8_rmac_port_0, &speed, &duplex));
  TEST_ASSERT_EQ(k_ra8_rmac_lsc_100mbit, speed);
  TEST_ASSERT_EQ(k_ra8_rmac_duplex_full, duplex);
  TEST_ASSERT_EQ(k_cov_anlpar_100f, g_ra8_eth_anlpar_cov);
  TEST_ASSERT_EQ(0U, g_ra8_eth_gbsr_cov);
  TEST_END("query_negotiated_speed: both reads OK -> decoded speed");
}

/**
 * @test internal_test_query_negotiated_speed_anlpar_fail
 *
 * @par MC/DC:
 * (no compound decision -- the ANLPAR read fails, taking the first
 * single-condition error guard's true leg.) @brief Verify query negotiated speed anlpar fail behavior. @details Executes the query negotiated speed anlpar fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_query_negotiated_speed_anlpar_fail(void)
{
  TEST_BEGIN("query_negotiated_speed: ANLPAR read failure propagates");
  internal_prep();
  s_mdio_fail_enable       = true;
  s_mdio_fail_reg          = (uint8_t)k_cov_reg_anlpar;
  s_mdio_fail_err          = k_ra8_err_hw_error;
  ra8_rmac_lsc_t    speed  = k_ra8_rmac_lsc_10mbit;
  ra8_rmac_duplex_t duplex = k_ra8_rmac_duplex_half;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_query_negotiated_speed(k_ra8_rmac_port_0, &speed, &duplex));
  TEST_END("query_negotiated_speed: ANLPAR read failure propagates");
}

/**
 * @test internal_test_query_negotiated_speed_gbsr_fail
 *
 * @par MC/DC:
 * (no compound decision -- ANLPAR read succeeds while the GBSR read is
 * failed, taking the second single-condition error guard's true leg.) @brief Verify query negotiated speed gbsr fail behavior. @details Executes the query negotiated speed gbsr fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_query_negotiated_speed_gbsr_fail(void)
{
  TEST_BEGIN("query_negotiated_speed: GBSR read failure propagates");
  internal_prep();
  s_phy_regs[k_cov_reg_anlpar] = (uint16_t)k_cov_anlpar_10h;
  s_mdio_fail_enable           = true;
  s_mdio_fail_reg              = (uint8_t)k_cov_reg_gbsr;
  s_mdio_fail_err              = k_ra8_err_hw_timeout;
  ra8_rmac_lsc_t    speed      = k_ra8_rmac_lsc_10mbit;
  ra8_rmac_duplex_t duplex     = k_ra8_rmac_duplex_half;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_query_negotiated_speed(k_ra8_rmac_port_0, &speed, &duplex));
  TEST_END("query_negotiated_speed: GBSR read failure propagates");
}

/* =============================================================================
 * internal_program_mpic -- ETHA CONFIG-bracketed MPIC write.
 * =============================================================================
 */

/**
 * @test internal_test_program_mpic_gmii_ok
 *
 * @par MC/DC:
 * (no compound decision -- the port ternary and the speed==1000 gate
 * are single conditions. This vector takes port 0 and the GMII leg
 * with every bracketed transition succeeding.) @brief Verify program mpic gmii ok behavior. @details Executes the program mpic gmii ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_gmii_ok(void)
{
  TEST_BEGIN("program_mpic: 1 Gbps -> GMII, full bracket succeeds");
  internal_prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_1000mbit, k_ra8_rmac_duplex_full));
  /* DISABLE, CONFIG, DISABLE, OPERATION == four transitions. */
  TEST_ASSERT_EQ(k_cov_etha_call_operation, s_etha_call_count);
  TEST_END("program_mpic: 1 Gbps -> GMII, full bracket succeeds");
}

/**
 * @test internal_test_program_mpic_mii_ok
 *
 * @par MC/DC:
 * (no compound decision -- takes port 1 and the sub-gigabit MII leg,
 * i.e. the speed==1000 gate's false direction.) @brief Verify program mpic mii ok behavior. @details Executes the program mpic mii ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_mii_ok(void)
{
  TEST_BEGIN("program_mpic: 100 Mbps -> MII, port 1 path");
  internal_prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_program_mpic(k_ra8_rmac_port_1, k_ra8_rmac_lsc_100mbit, k_ra8_rmac_duplex_half));
  TEST_END("program_mpic: 100 Mbps -> MII, port 1 path");
}

/**
 * @test internal_test_program_mpic_first_disable_fail
 *
 * @par MC/DC:
 * (no compound decision -- the first DISABLE transition fails, taking
 * the first single-condition error guard's true leg.) @brief Verify program mpic first disable fail behavior. @details Executes the program mpic first disable fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_first_disable_fail(void)
{
  TEST_BEGIN("program_mpic: first DISABLE transition timeout");
  internal_prep();
  s_etha_fail_on_call = (uint32_t)k_cov_etha_call_disable1;
  s_etha_fail_err     = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_1000mbit, k_ra8_rmac_duplex_full));
  TEST_END("program_mpic: first DISABLE transition timeout");
}

/**
 * @test internal_test_program_mpic_config_fail
 *
 * @par MC/DC:
 * (no compound decision -- the CONFIG transition fails, taking the
 * second single-condition error guard's true leg.) @brief Verify program mpic config fail behavior. @details Executes the program mpic config fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_config_fail(void)
{
  TEST_BEGIN("program_mpic: CONFIG transition timeout");
  internal_prep();
  s_etha_fail_on_call = (uint32_t)k_cov_etha_call_config;
  s_etha_fail_err     = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_100mbit, k_ra8_rmac_duplex_full));
  TEST_END("program_mpic: CONFIG transition timeout");
}

/**
 * @test internal_test_program_mpic_setlink_fail
 *
 * @par MC/DC:
 * (no compound decision -- ra8_rmac_set_link rejects the args so the
 * set_err single-condition guard takes its true leg while ETHA is
 * still restored to OPERATION.) @brief Verify program mpic setlink fail behavior. @details Executes the program mpic setlink fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_setlink_fail(void)
{
  TEST_BEGIN("program_mpic: set_link rejects -> invalid_arg returned");
  internal_prep();
  s_setlink_err = k_ra8_err_invalid_arg;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_100mbit, k_ra8_rmac_duplex_half));
  /* All four transitions still ran (bracket is always closed). */
  TEST_ASSERT_EQ(k_cov_etha_call_operation, s_etha_call_count);
  TEST_END("program_mpic: set_link rejects -> invalid_arg returned");
}

/**
 * @test internal_test_program_mpic_second_disable_fail
 *
 * @par MC/DC:
 * (no compound decision -- set_link succeeds but the second DISABLE
 * fails, taking the dis_err single-condition guard's true leg.) @brief Verify program mpic second disable fail behavior. @details Executes the program mpic second disable fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_second_disable_fail(void)
{
  TEST_BEGIN("program_mpic: second DISABLE transition timeout");
  internal_prep();
  s_etha_fail_on_call = (uint32_t)k_cov_etha_call_disable2;
  s_etha_fail_err     = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_100mbit, k_ra8_rmac_duplex_half));
  TEST_END("program_mpic: second DISABLE transition timeout");
}

/**
 * @test internal_test_program_mpic_operation_fail
 *
 * @par MC/DC:
 * (no compound decision -- set_link and both DISABLEs succeed, only the
 * final OPERATION restore fails, so both preceding guards take their
 * false leg and the op_err is returned unfiltered.) @brief Verify program mpic operation fail behavior. @details Executes the program mpic operation fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_program_mpic_operation_fail(void)
{
  TEST_BEGIN("program_mpic: OPERATION restore timeout");
  internal_prep();
  s_etha_fail_on_call = (uint32_t)k_cov_etha_call_operation;
  s_etha_fail_err     = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_program_mpic(k_ra8_rmac_port_0, k_ra8_rmac_lsc_100mbit, k_ra8_rmac_duplex_half));
  TEST_END("program_mpic: OPERATION restore timeout");
}

/* =============================================================================
 * internal_resync_mac_speed -- top-level resync helper.
 * =============================================================================
 */

/**
 * @test internal_test_resync_mac_speed_ok
 *
 * @par MC/DC:
 * (no compound decision -- AN_COMPLETE is pre-asserted and every
 * mocked transition succeeds, so both single-condition error guards
 * take their false leg and the resync latch is set.) @brief Verify resync mac speed ok behavior. @details Executes the resync mac speed ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_resync_mac_speed_ok(void)
{
  TEST_BEGIN("resync_mac_speed: happy path sets the resync latch");
  internal_prep();
  s_phy_regs[k_cov_reg_bmsr]   = (uint16_t)k_cov_bmsr_an_done;
  s_phy_regs[k_cov_reg_anlpar] = (uint16_t)k_cov_anlpar_100f;
  TEST_ASSERT_EQ(k_ra8_ok, internal_resync_mac_speed(k_ra8_rmac_port_0, 0U));
  TEST_ASSERT_EQ(1, g_eth_mac_speed_resynced ? 1 : 0);
  TEST_END("resync_mac_speed: happy path sets the resync latch");
}

/**
 * @test internal_test_resync_mac_speed_query_fail
 *
 * @par MC/DC:
 * (no compound decision -- the ANLPAR read fails so the query-error
 * single-condition guard takes its true leg and the latch stays low.) @brief Verify resync mac speed query fail behavior. @details Executes the resync mac speed query fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_resync_mac_speed_query_fail(void)
{
  TEST_BEGIN("resync_mac_speed: query failure propagates, latch unset");
  internal_prep();
  s_phy_regs[k_cov_reg_bmsr] = (uint16_t)k_cov_bmsr_an_done;
  s_mdio_fail_enable         = true;
  s_mdio_fail_reg            = (uint8_t)k_cov_reg_anlpar;
  s_mdio_fail_err            = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_resync_mac_speed(k_ra8_rmac_port_0, 0U));
  TEST_ASSERT_EQ(0, g_eth_mac_speed_resynced ? 1 : 0);
  TEST_END("resync_mac_speed: query failure propagates, latch unset");
}

/**
 * @test internal_test_resync_mac_speed_program_fail
 *
 * @par MC/DC:
 * (no compound decision -- the query succeeds but the first ETHA
 * transition fails, so the mpic-error single-condition guard takes its
 * true leg and the latch stays low.) @brief Verify resync mac speed program fail behavior. @details Executes the resync mac speed program fail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_resync_mac_speed_program_fail(void)
{
  TEST_BEGIN("resync_mac_speed: MPIC program failure propagates");
  internal_prep();
  s_phy_regs[k_cov_reg_bmsr]   = (uint16_t)k_cov_bmsr_an_done;
  s_phy_regs[k_cov_reg_anlpar] = (uint16_t)k_cov_anlpar_100f;
  s_etha_fail_on_call          = (uint32_t)k_cov_etha_call_disable1;
  s_etha_fail_err              = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_resync_mac_speed(k_ra8_rmac_port_0, 0U));
  TEST_ASSERT_EQ(0, g_eth_mac_speed_resynced ? 1 : 0);
  TEST_END("resync_mac_speed: MPIC program failure propagates");
}

/* =============================================================================
 * ra8_eth_link_status -- public poller (via the renamed _cov copy).
 * =============================================================================
 */

/**
 * @test internal_test_link_status_speed100_decode
 *
 * @par MC/DC:
 * (no compound decision -- BMCR.SPEED_SELECT is a single-condition
 * gate inside internal_phy_read_link; this vector takes its true leg
 * so speed_mbps resolves to 100. Link is left down so the poller
 * returns before the resync gate.) @brief Verify link status speed100 decode behavior. @details Executes the link status speed100 decode scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_link_status_speed100_decode(void)
{
  TEST_BEGIN("link_status: BMCR speed100 -> speed_mbps == 100");
  internal_prep();
  s_eth_state.opened         = 1U;
  s_phy_regs[k_cov_reg_bmcr] = (uint16_t)k_cov_bmcr_speed100;
  s_phy_regs[k_cov_reg_bmsr] = 0U; /* link down. */
  ra8_eth_link_t out         = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_link_status_cov(&out));
  TEST_ASSERT_EQ(100, out.speed_mbps);
  TEST_ASSERT_EQ(0, out.link_up);
  TEST_END("link_status: BMCR speed100 -> speed_mbps == 100");
}

/**
 * @test internal_test_link_status_resynced_skip
 *
 * @par MC/DC:
 * (no compound decision -- the two link-status gates are sequential
 * single-condition ifs. Link is up and the resync latch is already
 * set, so the second gate short-circuits to k_ra8_ok without a MPIC
 * write.) @brief Verify link status resynced skip behavior. @details Executes the link status resynced skip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_link_status_resynced_skip(void)
{
  TEST_BEGIN("link_status: link up + already resynced -> no-op");
  internal_prep();
  s_eth_state.opened         = 1U;
  g_eth_mac_speed_resynced   = true;
  s_phy_regs[k_cov_reg_bmsr] = (uint16_t)k_cov_bmsr_link_up;
  ra8_eth_link_t out         = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_link_status_cov(&out));
  TEST_ASSERT_EQ(1, out.link_up);
  /* Latch untouched; no ETHA bracket ran. */
  TEST_ASSERT_EQ(0, s_etha_call_count);
  TEST_END("link_status: link up + already resynced -> no-op");
}

/**
 * @test internal_test_link_status_triggers_resync
 *
 * @par MC/DC:
 * (no compound decision -- link is up and the resync latch is clear,
 * so the second single-condition gate takes its false leg and delegates
 * to internal_resync_mac_speed, which then sets the latch.) @brief Verify link status triggers resync behavior. @details Executes the link status triggers resync scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_link_status_triggers_resync(void)
{
  TEST_BEGIN("link_status: first link-up drives the MPIC resync");
  internal_prep();
  s_eth_state.opened       = 1U;
  g_eth_mac_speed_resynced = false;
  s_phy_regs[k_cov_reg_bmsr] =
    (uint16_t)((uint16_t)k_cov_bmsr_link_up | (uint16_t)k_cov_bmsr_an_done);
  s_phy_regs[k_cov_reg_anlpar] = (uint16_t)k_cov_anlpar_100f;
  ra8_eth_link_t out           = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_link_status_cov(&out));
  TEST_ASSERT_EQ(1, out.link_up);
  TEST_ASSERT_EQ(1, g_eth_mac_speed_resynced ? 1 : 0);
  /* The full ETHA CONFIG bracket ran once (four transitions). */
  TEST_ASSERT_EQ(k_cov_etha_call_operation, s_etha_call_count);
  TEST_END("link_status: first link-up drives the MPIC resync");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  internal_test_pick_negotiated_speed_ladder,
  internal_test_wait_for_autoneg_an_done,
  internal_test_wait_for_autoneg_mdio_fail,
  internal_test_wait_for_autoneg_exhaust,
  internal_test_query_negotiated_speed_ok,
  internal_test_query_negotiated_speed_anlpar_fail,
  internal_test_query_negotiated_speed_gbsr_fail,
  internal_test_program_mpic_gmii_ok,
  internal_test_program_mpic_mii_ok,
  internal_test_program_mpic_first_disable_fail,
  internal_test_program_mpic_config_fail,
  internal_test_program_mpic_setlink_fail,
  internal_test_program_mpic_second_disable_fail,
  internal_test_program_mpic_operation_fail,
  internal_test_resync_mac_speed_ok,
  internal_test_resync_mac_speed_query_fail,
  internal_test_resync_mac_speed_program_fail,
  internal_test_link_status_speed100_decode,
  internal_test_link_status_resynced_skip,
  internal_test_link_status_triggers_resync,
};

/**
 * @brief Test entry point -- runs every white-box coverage case.
 * @return 0 on success (Unity aborts the process on any failure).
 */
int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
