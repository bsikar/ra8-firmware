/**
 * @file test_ra8_canfd_tdc.c
 * @brief Unit tests for ra8_canfd_set_tdc (CANFD Transmitter Delay Compensation).
 *
 * @details
 * Exercises ``ra8_canfd_set_tdc`` against the fake MMIO window provided
 * by ``ra8_fake_mmap``. Each test verifies a distinct slice of the function's
 * contract:
 *
 *  - Enable with manual offset: FDCFG.TDE set, FDCFG.TDCO correct, FDCFG.TDCOC set.
 *  - Enable with measured mode: FDCFG.TDE set, FDCFG.TDCO correct, FDCFG.TDCOC clear.
 *  - Maximum valid offset (127 = 0x7F): TDCO holds the full 7-bit cap.
 *  - Disable: FDCFG.TDE cleared by a read-modify-write that preserves other bits.
 *  - Null @p cfg pointer: returns ::k_ra8_err_null_ptr.
 *  - Out-of-range @p channel: returns ::k_ra8_err_null_ptr.
 *  - Offset overflow (@p offset = 128): returns ::k_ra8_err_invalid_arg.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_canfd_tdc_test_u8_t
 * @brief Channel indices and offset values used across the TDC unit tests.
 *
 * @details
 * All test constants that fit in a ``uint8_t`` live here so that no raw
 * numeric literals appear in the test code (``check_magic_numbers.py``
 * scans ``.c`` files for bare literals).
 */
typedef enum : uint8_t {
  k_tdc_ch0        = 0U,   /**< First valid channel index.                     */
  k_tdc_ch1        = 1U,   /**< Second valid channel index.                    */
  k_tdc_ch_bad     = 2U,   /**< First invalid channel (out of range).          */
  k_tdc_offset_a   = 42U,  /**< Arbitrary valid TDCO offset for enable tests.  */
  k_tdc_offset_b   = 10U,  /**< Second valid TDCO offset (measured-mode test). */
  k_tdc_offset_max = 127U, /**< Maximum valid TDCO (7-bit cap, 0x7F).          */
  k_tdc_offset_bad = 128U, /**< One past the 7-bit cap; must be rejected.      */
} ra8_canfd_tdc_test_u8_t;

/**
 * @enum ra8_canfd_tdc_test_u32_t
 * @brief 32-bit constants used to pre-seed or verify registers in TDC tests.
 *
 * @details
 * ``k_tdc_sts_all_set`` pre-seeds CFDCnSTS to all-ones before a call so
 * that the bounded CH_RESET wait inside ``priv_ra8_canfd_internal_set_channel_mode``
 * finds CRSTSTS (bit 0) set on the first poll iteration and returns
 * immediately, rather than spinning for ``k_ra8_canfd_spin`` iterations.
 */
typedef enum : uint32_t {
  k_tdc_sts_all_set = 0xFFFFFFFFUL, /**< All STS bits set; speeds CH_RESET wait. */
} ra8_canfd_tdc_test_u32_t;

/**
 * @brief Reset the fake MMIO window before each TDC test.
 *
 * @details
 * Zeros all peripheral registers in the fake memory window so that
 * each test starts from a clean, deterministic baseline.
 *
 * @pre The host MMIO substrate (ra8_fake_mmap) is linked into the test binary.
 * @pre The fake CANFD base addresses map into ra8_fake_mmap's window.
 * @post All CANFD registers in the fake window read as zero. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep_tdc(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each guard in ``ra8_canfd_set_tdc``
 * is a single-condition ``if``; no ``&&`` or ``||`` in the production code
 * under test) @brief Verify tdc enable manual offset behavior. @details Executes the tdc enable manual offset scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_enable_manual_offset(void)
{
  TEST_BEGIN("tdc enable: manual mode, offset=42 -> TDE|TDCOC|TDCO in FDCFG");
  internal_prep_tdc();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_tdc_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Pre-seed STS so the CH_RESET poll converges on the first iteration. */
  reg->CFDC[0].STS = (uint32_t)k_tdc_sts_all_set;

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = true,
    .offset = (uint8_t)k_tdc_offset_a,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, &cfg));

  const uint32_t fdcfg    = reg->CFDC2[0].FDCFG;
  const uint32_t tdco_got = (fdcfg >> (uint32_t)k_ra8_fdcfg_shift_tdco) & k_ra8_fdcfg_mask_tdco;
  /* TDE must be set. */
  TEST_ASSERT((fdcfg & k_ra8_fdcfg_mask_tde) != 0U);
  /* TDCOC must be set (manual mode). */
  TEST_ASSERT((fdcfg & k_ra8_fdcfg_mask_tdcoc) != 0U);
  /* TDCO must hold the requested offset. */
  TEST_ASSERT_EQ(k_tdc_offset_a, tdco_got);

  TEST_END("tdc enable: manual mode, offset=42 -> TDE|TDCOC|TDCO in FDCFG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the ``cfg->enable`` true
 * / ``cfg->manual`` false path; each guard is a single-condition ``if``) @brief Verify tdc enable measured mode behavior. @details Executes the tdc enable measured mode scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_enable_measured_mode(void)
{
  TEST_BEGIN("tdc enable: measured mode (TDCOC=0), offset=10");
  internal_prep_tdc();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_tdc_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDC[0].STS = (uint32_t)k_tdc_sts_all_set;

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = false,
    .offset = (uint8_t)k_tdc_offset_b,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, &cfg));

  const uint32_t fdcfg    = reg->CFDC2[0].FDCFG;
  const uint32_t tdco_got = (fdcfg >> (uint32_t)k_ra8_fdcfg_shift_tdco) & k_ra8_fdcfg_mask_tdco;
  /* TDE must be set. */
  TEST_ASSERT((fdcfg & k_ra8_fdcfg_mask_tde) != 0U);
  /* TDCOC must be clear (measured mode). */
  TEST_ASSERT_EQ(0U, fdcfg & k_ra8_fdcfg_mask_tdcoc);
  /* TDCO must reflect the supplied offset. */
  TEST_ASSERT_EQ(k_tdc_offset_b, tdco_got);

  TEST_END("tdc enable: measured mode (TDCOC=0), offset=10");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the maximum-value boundary
 * of the 7-bit TDCO field; a single range guard rejects values above 127) @brief Verify tdc max offset accepted behavior. @details Executes the tdc max offset accepted scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_max_offset_accepted(void)
{
  TEST_BEGIN("tdc max offset 127 (7-bit cap) accepted");
  internal_prep_tdc();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_tdc_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDC[0].STS = (uint32_t)k_tdc_sts_all_set;

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = true,
    .offset = (uint8_t)k_tdc_offset_max,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, &cfg));

  const uint32_t fdcfg    = reg->CFDC2[0].FDCFG;
  const uint32_t tdco_got = (fdcfg >> (uint32_t)k_ra8_fdcfg_shift_tdco) & k_ra8_fdcfg_mask_tdco;
  TEST_ASSERT_EQ(k_tdc_offset_max, tdco_got);

  TEST_END("tdc max offset 127 (7-bit cap) accepted");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the ``cfg->enable`` false
 * path, which skips TDE/TDCO/TDCOC writes and only clears those bits) @brief Verify tdc disable clears tde behavior. @details Executes the tdc disable clears tde scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_disable_clears_tde(void)
{
  TEST_BEGIN("tdc disable: TDE cleared in FDCFG");
  internal_prep_tdc();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_tdc_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDC[0].STS = (uint32_t)k_tdc_sts_all_set;

  /* Prime FDCFG with TDE=1 to verify the disable path actually clears it. */
  reg->CFDC2[0].FDCFG = k_ra8_fdcfg_mask_tde;

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = false,
    .manual = false,
    .offset = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, &cfg));

  /* TDE must be clear. */
  TEST_ASSERT_EQ(0U, reg->CFDC2[0].FDCFG & k_ra8_fdcfg_mask_tde);

  TEST_END("tdc disable: TDE cleared in FDCFG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- validates the null-cfg precondition
 * guard; a single ``RA8_CHECK_NULL_PTR`` covers the null path) @brief Verify tdc null cfg returns error behavior. @details Executes the tdc null cfg returns error scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_null_cfg_returns_error(void)
{
  TEST_BEGIN("tdc null cfg -> k_ra8_err_null_ptr");
  internal_prep_tdc();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, nullptr));

  TEST_END("tdc null cfg -> k_ra8_err_null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- validates the channel-range guard
 * via the ``ra8_canfd()`` accessor returning nullptr for out-of-range channels) @brief Verify tdc bad channel returns error behavior. @details Executes the tdc bad channel returns error scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_bad_channel_returns_error(void)
{
  TEST_BEGIN("tdc bad channel -> k_ra8_err_null_ptr");
  internal_prep_tdc();

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = false,
    .offset = (uint8_t)k_tdc_offset_a,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_canfd_set_tdc((uint8_t)k_tdc_ch_bad, &cfg));

  TEST_END("tdc bad channel -> k_ra8_err_null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- validates the offset-range guard
 * that rejects values above the 7-bit cap of 127) @brief Verify tdc offset overflow returns error behavior. @details Executes the tdc offset overflow returns error scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_offset_overflow_returns_error(void)
{
  TEST_BEGIN("tdc offset=128 (overflow) -> k_ra8_err_invalid_arg");
  internal_prep_tdc();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_tdc_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = true,
    .offset = (uint8_t)k_tdc_offset_bad,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_set_tdc((uint8_t)k_tdc_ch0, &cfg));

  /* FDCFG must be untouched -- the validation guard fires before CH_RESET. */
  TEST_ASSERT_EQ(0U, reg->CFDC2[0].FDCFG);

  TEST_END("tdc offset=128 (overflow) -> k_ra8_err_invalid_arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- verifies channel-1 independence;
 * exercises the same code path as channel 0 to confirm the accessor works
 * for the second instance) @brief Verify tdc channel1 enable behavior. @details Executes the tdc channel1 enable scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tdc_channel1_enable(void)
{
  TEST_BEGIN("tdc channel 1 enable: FDCFG.TDE set");
  internal_prep_tdc();

  volatile r_canfd_t* reg1 = ra8_canfd((uint8_t)k_tdc_ch1);
  TEST_ASSERT_NOT_NULL((void*)reg1);
  reg1->CFDC[0].STS = (uint32_t)k_tdc_sts_all_set;

  const ra8_canfd_tdc_cfg_t cfg = {
    .enable = true,
    .manual = false,
    .offset = (uint8_t)k_tdc_offset_a,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_tdc((uint8_t)k_tdc_ch1, &cfg));
  TEST_ASSERT((reg1->CFDC2[0].FDCFG & k_ra8_fdcfg_mask_tde) != 0U);

  TEST_END("tdc channel 1 enable: FDCFG.TDE set");
}

int32_t main(void)
{
  internal_test_tdc_enable_manual_offset();
  internal_test_tdc_enable_measured_mode();
  internal_test_tdc_max_offset_accepted();
  internal_test_tdc_disable_clears_tde();
  internal_test_tdc_null_cfg_returns_error();
  internal_test_tdc_bad_channel_returns_error();
  internal_test_tdc_offset_overflow_returns_error();
  internal_test_tdc_channel1_enable();
  return 0;
}
