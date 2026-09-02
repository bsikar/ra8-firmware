/**
 * @file test_ra8_usb_device_cov.c
 * @brief Coverage top-up for the USB device-mode lifecycle (ra8_usb_device.c)
 *
 * @details
 * Targets the residual uncovered lines in ``libs/ra8_hal/src/ra8_usb_device.c``
 * that the primary suite (``test_ra8_usb.c``) does not reach:
 *   - ``ra8_usb_device_attach`` bogus-speed rejection (the ``priv_pick``
 *     NULL leg -- ``test_ra8_usb.c`` only drives the valid FS/HS attach
 *     paths, never a speed ``priv_pick`` cannot map).
 *   - ``ra8_usb_device_busreset_rearm`` in full: the bogus-speed NULL leg,
 *     the ``PIPECTR[*]`` clear loop, the ``BRDYSTS`` / ``NRDYSTS`` /
 *     ``BEMPSTS`` W0C drops, the ``internal_dcp_reset_defaults`` helper
 *     (DCPCFG / DCPMAXP / CFIFO / CFIFOCTR.BCLR), and the ``INTENB0``
 *     re-arm store.
 *
 * Every leg is driven deterministically by pre-seeding the fake's
 * register RAM (``PIPECTR`` / ``BRDYSTS`` / ``NRDYSTS`` / ``BEMPSTS`` /
 * ``INTENB0``) to non-default values, then asserting the rearm cleared or
 * re-programmed each one. No timing injection (SIGALRM) is used. Every
 * line targeted here is a plain register store or a return after a
 * pointer-null check, all reachable from the host fake -- so this
 * file adds NO ``GCOVR_EXCL`` markers and ``ra8_usb_device.c`` is not
 * modified.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_device.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_usb_dev_const_t
 * @brief Named constants for the device-mode rearm coverage cases.
 * @details Tests are exempt from the magic-number gate, but naming the
 *          values keeps the intent of each seed/expectation explicit.
 */
typedef enum : uint16_t {
  k_test_usb_speed_bogus  = 9U,      /**< Neither FS nor HS -> priv_pick NULL.          */
  k_test_usb_seed_word    = 0xFFFFU, /**< All-ones seed to prove a store cleared it.    */
  k_test_usb_dcp_maxp     = 64U,     /**< DCPMAXP default after rearm (EP0 max packet). */
  k_test_usb_intenb0_seed = 0x0001U, /**< Bogus INTENB0 seed overwritten by the re-arm. */
  /* Expected INTENB0 mask re-armed by ra8_usb_device_busreset_rearm:
   * BRDY(8) | NRDY(9) | BEMP(10) | CTRT(11) | DVST(12) | VBSE(15). */
  k_test_usb_intenb0_mask = 0x9F00U /**< Test USB intenb0 mask. */
} test_usb_dev_const_t;

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @test internal_test_device_attach_bogus_speed
 *
 * @par MC/DC:
 * (no compound decision on this leg -- ``if (reg == nullptr)`` is a
 * single condition reached by a speed that ``priv_pick`` cannot map
 * to a controller block)
 *
 * @details Drives the ``ra8_usb_device_attach`` speed-rejection leg
 * (source line 267). ``test_ra8_usb.c`` only exercises valid FS/HS
 * attach, so the NULL return is otherwise uncovered. Both attached=true
 * and attached=false are passed to prove the rejection precedes the
 * attach/detach branch. @brief Verify device attach bogus speed behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_device_attach_bogus_speed(void)
{
  TEST_BEGIN("ra8_usb_device_attach rejects a speed priv_pick cannot map");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_device_attach((ra8_usb_speed_t)k_test_usb_speed_bogus, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_device_attach((ra8_usb_speed_t)k_test_usb_speed_bogus, false));

  TEST_END("ra8_usb_device_attach rejects a speed priv_pick cannot map");
}

/**
 * @test internal_test_busreset_rearm_bogus_speed
 *
 * @par MC/DC:
 * (no compound decision on this leg -- ``if (reg == nullptr)`` is a
 * single condition reached by a bogus speed)
 *
 * @details Drives the ``ra8_usb_device_busreset_rearm`` speed-rejection
 * leg (source lines 496-497) so the function returns before touching any
 * register. @brief Verify busreset rearm bogus speed behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_busreset_rearm_bogus_speed(void)
{
  TEST_BEGIN("ra8_usb_device_busreset_rearm rejects bogus speed");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_device_busreset_rearm((ra8_usb_speed_t)k_test_usb_speed_bogus));

  TEST_END("ra8_usb_device_busreset_rearm rejects bogus speed");
}

/**
 * @test internal_test_busreset_rearm_fs_full_path
 *
 * @par MC/DC:
 * (the only decision in ``ra8_usb_device_busreset_rearm`` is the
 * single-condition ``if (reg == nullptr)`` NULL guard, covered false
 * here and true in ::internal_test_busreset_rearm_bogus_speed; the PIPECTR clear
 * loop bound is a counted loop, not a compound decision)
 *
 * @details Pre-seeds every register the rearm is documented to clear or
 * re-program to a non-default value, then asserts each landed:
 *   - ``PIPECTR[*]`` seeded all-ones, expected 0 after the clear loop
 *     (source lines 504-506).
 *   - ``BRDYSTS`` / ``NRDYSTS`` / ``BEMPSTS`` seeded all-ones, expected 0
 *     after the W0C drops (source lines 512-514).
 *   - ``DCPCFG`` seeded non-zero, expected 0; ``DCPMAXP`` expected 64;
 *     ``CFIFOCTR.BCLR`` asserted set -- all via
 *     ``internal_dcp_reset_defaults`` (source lines 468-474, called at
 *     521).
 *   - ``INTENB0`` seeded to a bogus value, expected the re-armed mask
 *     (source lines 536-538) and a ``k_ra8_ok`` return (line 540). @brief Verify busreset rearm fs full path behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_busreset_rearm_fs_full_path(void)
{
  TEST_BEGIN("ra8_usb_device_busreset_rearm FS clears state and re-arms INTENB0");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();

  /* Seed the pipe-control words and per-pipe status latches so the clear
   * loop and the W0C drops have something observable to wipe. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_pipectr_count; i++) {
    reg->PIPECTR[i] = (uint16_t)k_test_usb_seed_word;
  }
  reg->BRDYSTS = (uint16_t)k_test_usb_seed_word;
  reg->NRDYSTS = (uint16_t)k_test_usb_seed_word;
  reg->BEMPSTS = (uint16_t)k_test_usb_seed_word;

  /* Seed the DCP config + a bogus INTENB0 so the helper and re-arm store
   * both have a non-default starting point. */
  reg->DCPCFG   = (uint16_t)k_test_usb_seed_word;
  reg->CFIFOCTR = 0U;
  reg->INTENB0  = (uint16_t)k_test_usb_intenb0_seed;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_busreset_rearm(k_ra8_usb_speed_fs));

  /* PIPECTR[*] cleared to PID=NAK (all fields zero). */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_pipectr_count; i++) {
    TEST_ASSERT_EQ(0U, reg->PIPECTR[i]);
  }
  /* Per-pipe status latches dropped. */
  TEST_ASSERT_EQ(0U, reg->BRDYSTS);
  TEST_ASSERT_EQ(0U, reg->NRDYSTS);
  TEST_ASSERT_EQ(0U, reg->BEMPSTS);

  /* internal_dcp_reset_defaults: DCPCFG=0, DCPMAXP=64, CFIFOCTR.BCLR set. */
  TEST_ASSERT_EQ(0U, reg->DCPCFG);
  TEST_ASSERT_EQ(k_test_usb_dcp_maxp, reg->DCPMAXP);
  TEST_ASSERT((reg->CFIFOCTR & (uint16_t)k_ra8_fifoctr_bclr) != 0U);

  /* INTENB0 re-armed with the post-init device-mode mask. */
  TEST_ASSERT_EQ(k_test_usb_intenb0_mask, reg->INTENB0);

  TEST_END("ra8_usb_device_busreset_rearm FS clears state and re-arms INTENB0");
}

/**
 * @test internal_test_busreset_rearm_hs_full_path
 *
 * @par MC/DC:
 * (same single-condition NULL guard as the FS case; this vector proves
 * ``priv_pick`` routes the HS speed to the HS controller block so the
 * same register-clearing body runs against a different window)
 *
 * @details Repeats the rearm against the HS controller instance to
 * confirm the ``priv_pick(k_ra8_usb_speed_hs)`` selection reaches the
 * same clear/re-arm body. Uses a compact subset of the FS assertions
 * (INTENB0 mask + DCPMAXP default) since the body is speed-independent. @brief Verify busreset rearm hs full path behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_busreset_rearm_hs_full_path(void)
{
  TEST_BEGIN("ra8_usb_device_busreset_rearm HS re-arms the HS controller");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));

  volatile r_usb_regs_t* reg = ra8_usb_hs();
  reg->INTENB0               = (uint16_t)k_test_usb_intenb0_seed;
  reg->DCPCFG                = (uint16_t)k_test_usb_seed_word;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_busreset_rearm(k_ra8_usb_speed_hs));

  TEST_ASSERT_EQ(k_test_usb_intenb0_mask, reg->INTENB0);
  TEST_ASSERT_EQ(0U, reg->DCPCFG);
  TEST_ASSERT_EQ(k_test_usb_dcp_maxp, reg->DCPMAXP);

  TEST_END("ra8_usb_device_busreset_rearm HS re-arms the HS controller");
}

int main(void)
{
  internal_test_device_attach_bogus_speed();
  internal_test_busreset_rearm_bogus_speed();
  internal_test_busreset_rearm_fs_full_path();
  internal_test_busreset_rearm_hs_full_path();
  return 0;
}
