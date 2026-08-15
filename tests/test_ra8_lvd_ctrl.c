/**
 * @file test_ra8_lvd_ctrl.c
 * @brief Runtime-control tests for ra8_lvd.c (PVD driver)
 *
 * @details
 * Split out of test_ra8_lvd.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the filter / RHSEL / RN
 * runtime updates, status read / clear, security attribution, ELC + standby,
 * filter-delay helper, callback registration / ISR demux, threshold-table
 * sweep, and MC/DC vector tests; the init / deinit and single-bit setter
 * contract tests stay in test_ra8_lvd.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_lvd.h"
#include "ra8_lvd_internal.h"
#include "ra8_lvd_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_lvd_test_const_t
 * @brief Test fixtures (literals used across the test file).
 */
typedef enum : uint32_t {
  k_ra8_lvd_test_bogus_ch   = 99U,        /**< Channel id outside {1,2,4,5}.   */
  k_ra8_lvd_test_ctx_val    = 0xC0FFEEU,  /**< Cookie passed to attach.        */
  k_ra8_lvd_test_ctx_chan   = 0xBADBEEFU, /**< Per-channel ctx cookie.         */
  k_ra8_lvd_test_loco_8khz  = 8000U,      /**< Hypothetical LOCO frequency.    */
  k_ra8_lvd_test_pvdsar_bad = 0x4U,       /**< Reserved bit -- driver rejects. */
} ra8_lvd_test_const_t;

static uint32_t          s_cb_count;
static uint32_t          s_chan_cb_count;
static ra8_lvd_channel_t s_cb_last_ch;
static void*             s_cb_last_ctx;

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  s_cb_count      = 0U;
  s_chan_cb_count = 0U;
  s_cb_last_ch    = k_ra8_lvd_ch1;
  s_cb_last_ctx   = nullptr;
  /* Always clear the static callbacks so leftover state from a previous
   * test doesn't fire when a later test only verifies registers. */
  (void)ra8_lvd_attach_handler(nullptr, nullptr);
  (void)ra8_lvd_attach_channel_handler(k_ra8_lvd_ch1, nullptr, nullptr);
  (void)ra8_lvd_attach_channel_handler(k_ra8_lvd_ch2, nullptr, nullptr);
}

/** @brief Prepare the fixture's make cfg state. @details Implements the make cfg fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_lvd_cfg_t internal_make_cfg(void)
{
  const ra8_lvd_cfg_t cfg = {
    .threshold    = k_ra8_lvd_pvdlvl_2_85v,
    .edge         = k_ra8_lvd_edge_fall,
    .irq_type     = k_ra8_lvd_irq_maskable,
    .response     = k_ra8_lvd_response_interrupt,
    .negate       = k_ra8_lvd_negate_after_voltage,
    .hysteresis   = k_ra8_lvd_hysteresis_lvd,
    .filter_div   = k_ra8_lvd_loco_div_2,
    .filter_en    = true,
    .irq_enable   = true,
    .clear_status = true,
  };
  return cfg;
}

/** @brief Provide the file-local stub cb test helper. @details Implements the stub cb fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] ch Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_stub_cb(void* ctx, ra8_lvd_channel_t ch)
{
  ++s_cb_count;
  s_cb_last_ch  = ch;
  s_cb_last_ctx = ctx;
}

/** @brief Provide the file-local stub chan cb test helper. @details Implements the stub chan cb fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] ch Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_stub_chan_cb(void* ctx, ra8_lvd_channel_t ch)
{
  ++s_chan_cb_count;
  s_cb_last_ch  = ch;
  s_cb_last_ctx = ctx;
}

/* =============================================================================
 * Filter + RHSEL + RN runtime updates
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify set filter full range behavior. @details Executes the set filter full range scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_filter_full_range(void)
{
  TEST_BEGIN("lvd set_filter exercises every FSAMP value");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.filter_en     = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  const ra8_lvd_loco_div_t divs[] = {
    k_ra8_lvd_loco_div_2,
    k_ra8_lvd_loco_div_4,
    k_ra8_lvd_loco_div_8,
    k_ra8_lvd_loco_div_16,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(divs) / sizeof(divs[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_filter(k_ra8_lvd_ch1, divs[i], true));
    const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
    const uint8_t got_div =
      (uint8_t)((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_fsamp) >> (uint8_t)k_ra8_lvd_cr0_shift_fsamp);
    TEST_ASSERT_EQ(divs[i], got_div);
    TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) == 0U);
  }

  /* filter_en=false leaves DFDIS set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_filter(k_ra8_lvd_ch1, k_ra8_lvd_loco_div_2, false));
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) != 0U);

  /* Bad div / bad channel rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_set_filter(k_ra8_lvd_ch1, (ra8_lvd_loco_div_t)0x4U, true));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_lvd_set_filter((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch, k_ra8_lvd_loco_div_2, true));
  TEST_END("lvd set_filter exercises every FSAMP value");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set hysteresis lvd then hvd behavior. @details Executes the set hysteresis lvd then hvd scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_hysteresis_lvd_then_hvd(void)
{
  TEST_BEGIN("lvd set_hysteresis_mode LVD then HVD");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.response      = k_ra8_lvd_response_reset; /* sets RI=1 so HVD is allowed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  /* LVD -> HVD */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, k_ra8_lvd_hysteresis_hvd));
  TEST_ASSERT_EQ(k_ra8_lvd_fcr_mask_rhsel,
                 (*ra8_lvd_reg8(k_ra8_lvd_pvd1_fcr_off) & (uint8_t)k_ra8_lvd_fcr_mask_rhsel));

  /* HVD -> LVD */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, k_ra8_lvd_hysteresis_lvd));
  TEST_ASSERT_EQ(0U, (*ra8_lvd_reg8(k_ra8_lvd_pvd1_fcr_off) & (uint8_t)k_ra8_lvd_fcr_mask_rhsel));

  /* Bad value rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, (ra8_lvd_hysteresis_t)0x2U));
  TEST_END("lvd set_hysteresis_mode LVD then HVD");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set hysteresis hvd requires ri behavior. @details Executes the set hysteresis hvd requires ri scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_hysteresis_hvd_requires_ri(void)
{
  TEST_BEGIN("lvd set_hysteresis_mode HVD blocked when RI=0 (m chan)");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.response      = k_ra8_lvd_response_interrupt; /* RI stays 0 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, k_ra8_lvd_hysteresis_hvd));

  /* n channels are not subject to the RI check. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_unlock_n_channels());
  ra8_lvd_cfg_t ncfg = internal_make_cfg();
  ncfg.response      = k_ra8_lvd_response_reset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch4, &ncfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch4, k_ra8_lvd_hysteresis_hvd));
  TEST_END("lvd set_hysteresis_mode HVD blocked when RI=0 (m chan)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set negate mode behavior. @details Executes the set negate mode scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_negate_mode(void)
{
  TEST_BEGIN("lvd set_negate_mode happy + RHSEL conflict");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_negate_mode(k_ra8_lvd_ch1, k_ra8_lvd_negate_after_assert));
  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rn) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_negate_mode(k_ra8_lvd_ch1, k_ra8_lvd_negate_after_voltage));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rn) == 0U);

  /* Bad arg + n-channel rejection + RHSEL=HVD conflict. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_set_negate_mode(k_ra8_lvd_ch1, (ra8_lvd_negate_t)0x2U));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_lvd_set_negate_mode(k_ra8_lvd_ch4, k_ra8_lvd_negate_after_voltage));

  /* Force RI=1 so HVD becomes legal. Then HVD + RN=1 conflicts. */
  ra8_lvd_cfg_t cfg2 = internal_make_cfg();
  cfg2.response      = k_ra8_lvd_response_reset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch2, &cfg2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch2, k_ra8_lvd_hysteresis_hvd));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_lvd_set_negate_mode(k_ra8_lvd_ch2, k_ra8_lvd_negate_after_assert));
  TEST_END("lvd set_negate_mode happy + RHSEL conflict");
}

/* =============================================================================
 * Status read / clear
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify status read and clear behavior. @details Executes the status read and clear scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_status_read_and_clear(void)
{
  TEST_BEGIN("lvd status read + clear");
  internal_prep();

  /* Pre-load PVDmSR to MON=1, DET=1 like the hardware would after a
   * detected crossing. */
  *ra8_lvd_reg8(k_ra8_lvd_pvd1_sr_off) =
    (uint8_t)((uint8_t)k_ra8_lvd_sr_mask_det | (uint8_t)k_ra8_lvd_sr_mask_mon);

  ra8_lvd_status_t st = {.crossed = false, .above = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_get_status(k_ra8_lvd_ch1, &st));
  TEST_ASSERT(st.crossed);
  TEST_ASSERT(st.above);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_clear_status(k_ra8_lvd_ch1));

  /* DET should be cleared, MON preserved. */
  const uint8_t sr = *ra8_lvd_reg8(k_ra8_lvd_pvd1_sr_off);
  TEST_ASSERT_EQ(0U, (sr & (uint8_t)k_ra8_lvd_sr_mask_det));
  TEST_ASSERT_EQ(k_ra8_lvd_sr_mask_mon, (sr & (uint8_t)k_ra8_lvd_sr_mask_mon));

  /* n-channels: get_status / clear_status must report unsupported. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_get_status(k_ra8_lvd_ch4, &st));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_clear_status(k_ra8_lvd_ch5));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lvd_get_status(k_ra8_lvd_ch1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_get_status((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch, &st));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_clear_status((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_END("lvd status read + clear");
}

/* =============================================================================
 * Security attribution + n-channel lock
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify set security behavior. @details Executes the set security scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_security(void)
{
  TEST_BEGIN("lvd set_security PVDSAR write");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_security((uint32_t)k_ra8_lvd_pvdsar_mask_nonsec0));
  TEST_ASSERT_EQ(k_ra8_lvd_pvdsar_mask_nonsec0, *ra8_lvd_reg32(k_ra8_lvd_pvdsar_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_security((uint32_t)k_ra8_lvd_pvdsar_mask_all));
  TEST_ASSERT_EQ(k_ra8_lvd_pvdsar_mask_all, *ra8_lvd_reg32(k_ra8_lvd_pvdsar_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_security(0U));
  TEST_ASSERT_EQ(0U, *ra8_lvd_reg32(k_ra8_lvd_pvdsar_off));

  /* Reserved bit -- driver must reject. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_set_security((uint32_t)k_ra8_lvd_test_pvdsar_bad));
  TEST_END("lvd set_security PVDSAR write");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify unlock relock n channels behavior. @details Executes the unlock relock n channels scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_unlock_relock_n_channels(void)
{
  TEST_BEGIN("lvd unlock/relock n-channels (PVDLR)");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_unlock_n_channels());
  TEST_ASSERT_EQ(0U, *ra8_lvd_reg8(k_ra8_lvd_pvdlr_off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_relock_n_channels());
  TEST_ASSERT_EQ(k_ra8_lvd_pvdlr_mask_lock, *ra8_lvd_reg8(k_ra8_lvd_pvdlr_off));
  TEST_END("lvd unlock/relock n-channels (PVDLR)");
}

/* =============================================================================
 * ELC + standby + cancel-deep-standby
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify elc event enable disable behavior. @details Executes the elc event enable disable scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_elc_event_enable_disable(void)
{
  TEST_BEGIN("lvd enable/disable ELC event");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  /* Force CMPE off, then re-enable via the ELC helper. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_cmpe(k_ra8_lvd_ch1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_enable_elc_event(k_ra8_lvd_ch1));
  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_elc_event(k_ra8_lvd_ch1));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) == 0U);

  /* n channels reject ELC ops. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_enable_elc_event(k_ra8_lvd_ch4));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_disable_elc_event(k_ra8_lvd_ch4));
  TEST_END("lvd enable/disable ELC event");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify configure for standby m behavior. @details Executes the configure for standby m scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_configure_for_standby_m(void)
{
  TEST_BEGIN("lvd configure_for_standby (m channel sets DFDIS, clears RI/RN)");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  cfg.negate        = k_ra8_lvd_negate_after_assert;
  /* RHSEL must stay LVD so RN=1 is allowed; default already does. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_configure_for_standby(k_ra8_lvd_ch1));
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_ri) == 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rn) == 0U);
  TEST_END("lvd configure_for_standby (m channel sets DFDIS, clears RI/RN)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify configure for standby n behavior. @details Executes the configure for standby n scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_configure_for_standby_n(void)
{
  TEST_BEGIN("lvd configure_for_standby (n channel only sets DFDIS)");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_unlock_n_channels());
  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch4, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_configure_for_standby(k_ra8_lvd_ch4));
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd4_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) != 0U);
  /* Reserved n bit-6 still 1. */
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_n_bit6) != 0U);
  TEST_END("lvd configure_for_standby (n channel only sets DFDIS)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify cancel deep standby path behavior. @details Executes the cancel deep standby path scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cancel_deep_standby_path(void)
{
  TEST_BEGIN("lvd cancel_deep_standby_path clears RI on PVD1 + PVD2");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch2, &cfg));

  /* Both RI bits should be 1 after init with reset response. */
  TEST_ASSERT((*ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off) & (uint8_t)k_ra8_lvd_cr0_mask_ri) != 0U);
  TEST_ASSERT((*ra8_lvd_reg8(k_ra8_lvd_pvd2_cr0_off) & (uint8_t)k_ra8_lvd_cr0_mask_ri) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_cancel_deep_standby_path());
  TEST_ASSERT((*ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off) & (uint8_t)k_ra8_lvd_cr0_mask_ri) == 0U);
  TEST_ASSERT((*ra8_lvd_reg8(k_ra8_lvd_pvd2_cr0_off) & (uint8_t)k_ra8_lvd_cr0_mask_ri) == 0U);
  TEST_END("lvd cancel_deep_standby_path clears RI on PVD1 + PVD2");
}

/* =============================================================================
 * Filter delay helper (pure)
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify filter delay us behavior. @details Executes the filter delay us scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_filter_delay_us(void)
{
  TEST_BEGIN("lvd filter_delay_us spans every divider");
  internal_prep();

  /* For LOCO=32768, div=0 -> s=2 -> cycles=2*2+3=7 -> 7*1e6/32768 + 1
   *  = 213 + 1 = 214 us. */
  const uint32_t d0 = ra8_lvd_filter_delay_us(k_ra8_lvd_loco_div_2, 0U);
  TEST_ASSERT_EQ(214U, d0);

  /* div=3 -> s=16 -> cycles=2*16+3=35 -> 35*1e6/32768 + 1 = 1068 + 1 = 1069. */
  const uint32_t d3 = ra8_lvd_filter_delay_us(k_ra8_lvd_loco_div_16, 0U);
  TEST_ASSERT_EQ(1069U, d3);

  /* Out-of-range div clamps to 3. */
  const uint32_t dx = ra8_lvd_filter_delay_us((ra8_lvd_loco_div_t)0x7U, 0U);
  TEST_ASSERT_EQ(d3, dx);

  /* Custom LOCO frequency must be honoured. */
  const uint32_t d_custom =
    ra8_lvd_filter_delay_us(k_ra8_lvd_loco_div_2, (uint32_t)k_ra8_lvd_test_loco_8khz);
  /* 7*1e6/8000 + 1 = 875 + 1 = 876 us. */
  TEST_ASSERT_EQ(876U, d_custom);
  TEST_END("lvd filter_delay_us spans every divider");
}

/* =============================================================================
 * Callback registration + ISR demux
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify attach and dispatch shared behavior. @details Executes the attach and dispatch shared scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_and_dispatch_shared(void)
{
  TEST_BEGIN("lvd attach + dispatch (shared callback)");
  internal_prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lvd_attach_handler(internal_stub_cb, (void*)(uintptr_t)k_ra8_lvd_test_ctx_val));

  /* Pre-set DET so dispatch will fire the callback. */
  *ra8_lvd_reg8(k_ra8_lvd_pvd2_sr_off) = (uint8_t)k_ra8_lvd_sr_mask_det;
  ra8_lvd_dispatch(k_ra8_lvd_ch2);
  TEST_ASSERT_EQ(1U, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_lvd_ch2, s_cb_last_ch);
  TEST_ASSERT_EQ(k_ra8_lvd_test_ctx_val, (uintptr_t)s_cb_last_ctx);
  /* DET should now be cleared. */
  TEST_ASSERT_EQ(0U, (*ra8_lvd_reg8(k_ra8_lvd_pvd2_sr_off) & (uint8_t)k_ra8_lvd_sr_mask_det));

  /* Spurious dispatch (no DET set) is a no-op. */
  ra8_lvd_dispatch(k_ra8_lvd_ch2);
  TEST_ASSERT_EQ(1U, s_cb_count);

  /* Unknown channel must NOT fire the callback. */
  ra8_lvd_dispatch((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch);
  TEST_ASSERT_EQ(1U, s_cb_count);

  /* n channels go to the no-op path. */
  ra8_lvd_dispatch(k_ra8_lvd_ch4);
  TEST_ASSERT_EQ(1U, s_cb_count);

  /* Detaching makes dispatch a no-op even with DET set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_attach_handler(nullptr, nullptr));
  *ra8_lvd_reg8(k_ra8_lvd_pvd1_sr_off) = (uint8_t)k_ra8_lvd_sr_mask_det;
  ra8_lvd_dispatch(k_ra8_lvd_ch1);
  TEST_ASSERT_EQ(1U, s_cb_count);
  TEST_END("lvd attach + dispatch (shared callback)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify attach per channel handler behavior. @details Executes the attach per channel handler scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_per_channel_handler(void)
{
  TEST_BEGIN("lvd attach_channel_handler overrides shared cb");
  internal_prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_lvd_attach_handler(internal_stub_cb, (void*)(uintptr_t)k_ra8_lvd_test_ctx_val));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_lvd_attach_channel_handler(k_ra8_lvd_ch1,
                                                internal_stub_chan_cb,
                                                (void*)(uintptr_t)k_ra8_lvd_test_ctx_chan));

  /* Per-channel handler fires on PVD1; shared handler fires on PVD2. */
  *ra8_lvd_reg8(k_ra8_lvd_pvd1_sr_off) = (uint8_t)k_ra8_lvd_sr_mask_det;
  ra8_lvd_dispatch(k_ra8_lvd_ch1);
  TEST_ASSERT_EQ(1U, s_chan_cb_count);
  TEST_ASSERT_EQ(0U, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_lvd_test_ctx_chan, (uintptr_t)s_cb_last_ctx);

  *ra8_lvd_reg8(k_ra8_lvd_pvd2_sr_off) = (uint8_t)k_ra8_lvd_sr_mask_det;
  ra8_lvd_dispatch(k_ra8_lvd_ch2);
  TEST_ASSERT_EQ(1U, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_lvd_test_ctx_val, (uintptr_t)s_cb_last_ctx);

  /* Bad channel + n-chan rejection. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_attach_channel_handler((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch,
                                                internal_stub_chan_cb,
                                                nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_lvd_attach_channel_handler(k_ra8_lvd_ch4, internal_stub_chan_cb, nullptr));
  TEST_END("lvd attach_channel_handler overrides shared cb");
}

/* =============================================================================
 * Threshold-table sweep
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify every threshold value behavior. @details Executes the every threshold value scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_every_threshold_value(void)
{
  TEST_BEGIN("lvd every PVDLVL value accepted");
  internal_prep();

  ra8_lvd_cfg_t cfg = internal_make_cfg();
  for (uint8_t v = (uint8_t)k_ra8_lvd_pvdlvl_min; v <= (uint8_t)k_ra8_lvd_pvdlvl_max; ++v) {
    cfg.threshold = (ra8_lvd_pvdlvl_t)v;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
    const uint8_t cmpcr = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cmpcr_off);
    TEST_ASSERT_EQ(v, (cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvdlvl));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_deinit(k_ra8_lvd_ch1));
  }
  TEST_END("lvd every PVDLVL value accepted");
}

/**
 * @test internal_test_mcdc_lvd
 *
 * @par MC/DC:
 * Decision: ``ra8_lvd_set_hysteresis_mode`` line 794,
 * libs/ra8_hal/src/ra8_lvd.c:
 * ``if (map.has_irq && (hyst == k_ra8_lvd_hysteresis_hvd))``
 * (2 conditions, ``&&`` short-circuit).
 *
 * N+1 = 3 vectors:
 * - V1: m channel (has_irq=T) + hyst=HVD + RI=0
 *       -> C1=T,C2=T -> dec T -> invalid_state (RI gate trips)
 * - V2: m channel (has_irq=T) + hyst=LVD
 *       -> C1=T,C2=F -> dec F -> ok
 * - V3: n channel (has_irq=F) + hyst=HVD
 *       -> C1=F (short-circuits) -> dec F -> ok (no RI gate)
 * Pairs: (V1,V2) flip C2 with C1 fixed; (V1,V3) flip C1 with C2 fixed. @brief Verify mcdc lvd behavior. @details Executes the mcdc lvd scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_lvd(void)
{
  TEST_BEGIN("lvd MC/DC: set_hysteresis_mode RI gate (2-cond)");
  internal_prep();

  /* V1: m chan, RI=0 (response=interrupt sets RIE not RI), HVD -> dec T. */
  ra8_lvd_cfg_t cfg_v1 = internal_make_cfg();
  cfg_v1.response      = k_ra8_lvd_response_interrupt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg_v1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, k_ra8_lvd_hysteresis_hvd));

  /* V2: m chan, hyst=LVD -> C2=F -> dec F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch1, k_ra8_lvd_hysteresis_lvd));

  /* V3: n chan (has_irq=F), HVD -> C1=F (short-circuits) -> dec F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_unlock_n_channels());
  ra8_lvd_cfg_t cfg_v3 = internal_make_cfg();
  cfg_v3.response      = k_ra8_lvd_response_reset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch4, &cfg_v3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_hysteresis_mode(k_ra8_lvd_ch4, k_ra8_lvd_hysteresis_hvd));
  TEST_END("lvd MC/DC: set_hysteresis_mode RI gate (2-cond)");
}

/**
 * @test internal_test_mcdc_lvd_internal_reject_hvd_after
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_lvd.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_lvd.c:
 *   ``hyst == hvd && negate == after_assert`` (2 conditions, AND).
 * - V1: hyst=lvd, negate=after  -> false
 * - V2: hyst=hvd, negate=after  -> true  (varies hyst)
 * - V3: hyst=hvd, negate=normal -> false (varies negate)
 * N+1 = 3. @brief Verify mcdc lvd internal reject hvd after behavior. @details Executes the mcdc lvd internal reject hvd after scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_lvd_internal_reject_hvd_after(void)
{
  TEST_BEGIN("lvd MC/DC: reject_hvd_after AND");
  TEST_ASSERT(!priv_ra8_lvd_internal_reject_hvd_after((uint32_t)k_ra8_lvd_hysteresis_hvd,
                                                      (uint32_t)k_ra8_lvd_negate_after_assert,
                                                      (uint32_t)k_ra8_lvd_hysteresis_lvd,
                                                      (uint32_t)k_ra8_lvd_negate_after_assert));
  TEST_ASSERT(priv_ra8_lvd_internal_reject_hvd_after((uint32_t)k_ra8_lvd_hysteresis_hvd,
                                                     (uint32_t)k_ra8_lvd_negate_after_assert,
                                                     (uint32_t)k_ra8_lvd_hysteresis_hvd,
                                                     (uint32_t)k_ra8_lvd_negate_after_assert));
  TEST_ASSERT(!priv_ra8_lvd_internal_reject_hvd_after((uint32_t)k_ra8_lvd_hysteresis_hvd,
                                                      (uint32_t)k_ra8_lvd_negate_after_assert,
                                                      (uint32_t)k_ra8_lvd_hysteresis_hvd,
                                                      (uint32_t)k_ra8_lvd_negate_after_voltage));
  TEST_END("lvd MC/DC: reject_hvd_after AND");
}

/**
 * @test internal_test_mcdc_lvd_internal_set_ri_bit
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_lvd.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_lvd.c:
 *   ``response == reset || response == reset_on_rise`` (2 conditions, OR).
 * - V1: resp=interrupt     -> false
 * - V2: resp=reset         -> true (varies left)
 * - V3: resp=reset_on_rise -> true (varies right)
 * N+1 = 3. @brief Verify mcdc lvd internal set ri bit behavior. @details Executes the mcdc lvd internal set ri bit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_lvd_internal_set_ri_bit(void)
{
  TEST_BEGIN("lvd MC/DC: set_ri_bit OR");
  TEST_ASSERT(!priv_ra8_lvd_internal_set_ri_bit((uint32_t)k_ra8_lvd_response_reset,
                                                (uint32_t)k_ra8_lvd_response_reset_on_rise,
                                                (uint32_t)k_ra8_lvd_response_interrupt));
  TEST_ASSERT(priv_ra8_lvd_internal_set_ri_bit((uint32_t)k_ra8_lvd_response_reset,
                                               (uint32_t)k_ra8_lvd_response_reset_on_rise,
                                               (uint32_t)k_ra8_lvd_response_reset));
  TEST_ASSERT(priv_ra8_lvd_internal_set_ri_bit((uint32_t)k_ra8_lvd_response_reset,
                                               (uint32_t)k_ra8_lvd_response_reset_on_rise,
                                               (uint32_t)k_ra8_lvd_response_reset_on_rise));
  TEST_END("lvd MC/DC: set_ri_bit OR");
}

int32_t main(void)
{
  internal_test_set_filter_full_range();
  internal_test_set_hysteresis_lvd_then_hvd();
  internal_test_set_hysteresis_hvd_requires_ri();
  internal_test_set_negate_mode();
  internal_test_status_read_and_clear();
  internal_test_set_security();
  internal_test_unlock_relock_n_channels();
  internal_test_elc_event_enable_disable();
  internal_test_configure_for_standby_m();
  internal_test_configure_for_standby_n();
  internal_test_cancel_deep_standby_path();
  internal_test_filter_delay_us();
  internal_test_attach_and_dispatch_shared();
  internal_test_attach_per_channel_handler();
  internal_test_every_threshold_value();
  internal_test_mcdc_lvd();
  internal_test_mcdc_lvd_internal_reject_hvd_after();
  internal_test_mcdc_lvd_internal_set_ri_bit();
  return 0;
}
