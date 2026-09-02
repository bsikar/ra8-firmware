/**
 * @file test_ra8_nsc_wdt.c
 * @brief Unit tests for the secure-side watchdog NSC veneers (ra8_nsc_wdt.c).
 *
 * @details
 * ``ra8_nsc_wdt_start`` and ``ra8_nsc_wdt_refresh`` are the two argument-free
 * Non-Secure Callable gateways the e-reader's ThreadX watchdog supervisor uses
 * to arm and heartbeat the Secure-owned WDT. In the host (single-world) build
 * ``RA8_NSC_VENEER`` compiles away, so both veneers reduce to a direct forward
 * into the ``ra8_wdt`` driver over the fake register block: the arm veneer
 * calls ``ra8_wdt_init`` with the fixed Secure-side config, and the refresh
 * veneer calls ``ra8_wdt_refresh_deferred`` (the WDTRR 0x00/0xFF unlock).
 *
 * The suite drives both from the host with the WDT register block mapped by
 * ``ra8_fake_mmap_reset``: it proves the arm veneer configures and arms cleanly
 * (its fixed 1024-cycle / div-4 config is a legal encoding) and that the refresh
 * veneer leaves 0xFF -- the final unlock byte -- in WDTRR.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_nsc.h"
#include "ra8_wdt.h"
#include "ra8_wdt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_wdt_probe_t
 * @brief Sentinel written into WDTRR before the refresh under test.
 */
typedef enum : uint8_t {
  k_t_wdtrr_sentinel = 0x42U, /**< Any value the refresh must overwrite; proves
                                   the veneer wrote the register at all.        */
} t_wdt_probe_t;

/**
 * @test internal_test_nsc_wdt_start_arms
 * @brief ``ra8_nsc_wdt_start`` forwards the fixed Secure config into ``ra8_wdt_init``
 *        and arms the counter (k_ra8_ok).
 *
 * @details Resets the host register map and checks that the argument-free NSC
 *          veneer accepts its fixed legal watchdog configuration.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the veneer is an argument-free forward
 * into ra8_wdt_init; the single outcome asserted is its k_ra8_ok return.)
 *
 * @pre The fake WDT register mapping is available.
 * @pre No other test concurrently owns the fake watchdog registers.
 * @post The start veneer returns k_ra8_ok.
 * @post The secure watchdog has accepted its fixed configuration.
 *
 * @note The host fake does not model watchdog time progression.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_nsc_wdt_start_arms(void)
{
  TEST_BEGIN("ra8_nsc_wdt_start arms the secure WDT");
  ra8_fake_mmap_reset();
  /* The fixed ::s_wdt_cfg (1024 cycles, div 4, open window, NMI on expiry) is a
   * legal WDTCR encoding, so the arm veneer configures + refreshes and returns
   * k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_wdt_start());
  TEST_END("ra8_nsc_wdt_start arms the secure WDT");
}

/**
 * @test internal_test_nsc_wdt_refresh_kicks
 * @brief ``ra8_nsc_wdt_refresh`` issues the WDTRR heartbeat (final byte 0xFF).
 *
 * @details Arms the secure watchdog, replaces WDTRR with a sentinel, invokes
 *          the refresh veneer, and inspects the final unlock byte.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the veneer is an argument-free forward
 * into ra8_wdt_refresh_deferred; the assertion is over the WDTRR unlock byte.)
 *
 * @pre The fake WDT register mapping is available.
 * @pre The start veneer succeeds before WDTRR is probed.
 * @post WDTRR contains k_ra8_wdt_refresh_b.
 * @post The sentinel value has been overwritten by the refresh sequence.
 *
 * @note The host register exposes the final byte of the two-byte sequence.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_nsc_wdt_refresh_kicks(void)
{
  TEST_BEGIN("ra8_nsc_wdt_refresh writes the WDTRR unlock byte");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_wdt_start());

  volatile r_wdt_regs_t* reg = ra8_wdt();
  reg->WDTRR                 = k_t_wdtrr_sentinel; /* sentinel: overwritten by the refresh */
  ra8_nsc_wdt_refresh();
  /* The unlock sequence writes 0x00 then 0xFF; the last byte latched is 0xFF. */
  TEST_ASSERT_EQ(k_ra8_wdt_refresh_b, reg->WDTRR);
  TEST_END("ra8_nsc_wdt_refresh writes the WDTRR unlock byte");
}

/**
 * @brief Test entry point -- runs the NSC WDT veneer suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int main(void)
{
  internal_test_nsc_wdt_start_arms();
  internal_test_nsc_wdt_refresh_kicks();
  return 0;
}
