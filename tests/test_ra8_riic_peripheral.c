/**
 * @file test_ra8_riic_peripheral.c
 * @brief Unit tests for the RIIC (I2C) target/peripheral role in ra8_i2c_peripheral.c
 *
 * @details
 * Exercises the own-address bring-up (``ra8_i2c_peripheral_init`` /
 * ``ra8_i2c_peripheral_deinit``), the address-match poll (``ra8_i2c_peripheral_poll``)
 * and the polling target transfers (``ra8_i2c_peripheral_receive`` /
 * ``ra8_i2c_peripheral_transmit``), plus the four promoted pure decision
 * predicates that carry the new compound decisions under MC/DC.
 *
 * Register sequencing cannot be driven by real hardware on the host, so each
 * transfer test pre-loads the RIIC status registers (RDRF / STOP / TDRE /
 * TEND / NACKF) in the fake MMIO window and checks the resulting data
 * path and flag clears.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_riic_t_const_t
 * @brief Byte-wide test constants (channel, addresses, payload bytes).
 */
typedef enum : uint8_t {
  k_t_ch       = 0U,    /**< Channel under test.                       */
  k_t_ch_bad   = 3U,    /**< Out-of-range channel (no register block). */
  k_t_addr     = 0x42U, /**< 7-bit own address.                        */
  k_t_addr_bad = 0x80U, /**< Above the 7-bit range.                    */
  k_t_slot_bad = 3U,    /**< Out-of-range own-address slot.            */
  k_t_sarl_exp = 0x84U, /**< 0x42 << 1 -- expected SARLy value.        */
  k_t_byte_a   = 0xABU, /**< Receive-path payload byte.                */
  k_t_byte_b   = 0x55U, /**< Receive-path payload byte (variant).      */
  k_t_byte_c   = 0x3CU, /**< Transmit-path payload byte.               */
} ra8_riic_t_const_t;

/**
 * @enum ra8_riic_t_size_t
 * @brief Buffer-size test constants.
 */
typedef enum : uint32_t {
  k_t_cap     = 3U, /**< Receive capacity for the multi-byte case.      */
  k_t_cap_big = 8U, /**< Receive capacity for the STOP-terminated case. */
  k_t_buf_len = 4U, /**< Stack buffer length.                           */
  k_t_tx_len  = 3U, /**< Transmit byte count.                           */
} ra8_riic_t_size_t;

/**
 * @brief Reset the fake MMIO and disarm the target on the test channel.
 *
 * @details Clears the per-channel ``peripheral_active`` flag (it lives in the
 * persistent ``s_i2c_state`` table, not in MMIO) so each test starts from a
 * known disarmed state. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_i2c_peripheral_deinit((uint8_t)k_t_ch);
}

/**
 * @brief Build a configuration descriptor for slot 0, address k_t_addr. @details Implements the make cfg fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_i2c_peripheral_cfg_t internal_make_cfg(void)
{
  const ra8_i2c_peripheral_cfg_t cfg = {
    .own_addr_7b   = (uint8_t)k_t_addr,
    .slot          = k_ra8_i2c_peripheral_slot_0,
    .general_call  = false,
    .clock_stretch = false,
    .irq_enable    = false,
  };
  return cfg;
}

/** @brief Last event a dispatch callback observed (recorder state). */
static ra8_i2c_peripheral_event_t s_cb_event = k_ra8_i2c_peripheral_event_none;
/** @brief Last context pointer a dispatch callback observed. */
static void* s_cb_ctx = nullptr;
/** @brief Count of dispatch-callback invocations since the last reset. */
static uint32_t s_cb_count = 0U;
/** @brief Token whose address is the dispatch-callback context under test. */
static uint8_t s_ctx_token = 0U;

/** @brief Dispatch callback: latch the event + context and bump the count. @details Implements the record cb fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] event Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_record_cb(void* ctx, ra8_i2c_peripheral_event_t event)
{
  s_cb_event = event;
  s_cb_ctx   = ctx;
  s_cb_count += 1U;
}

/** @brief Reset the dispatch-callback recorder before a case. @details Implements the reset cb fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_reset_cb(void)
{
  s_cb_event = k_ra8_i2c_peripheral_event_none;
  s_cb_ctx   = nullptr;
  s_cb_count = 0U;
}

/* ===========================================================================
 * Pure predicate MC/DC tests.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Decision: `(icsr1 & match) != 0 || (icsr2 & stop) != 0` (2 conditions).
 * - V1: match=0, stop=0 -> false (control: both false)
 * - V2: match=1, stop=0 -> true  (varies left, independent influence)
 * - V3: match=0, stop=1 -> true  (varies right, independent influence)
 * Vectors V1+V2 prove the match flag's independent influence; V1+V3 prove
 * the STOP flag's. N+1 = 3 vectors for N=2. @brief Verify pred poll done behavior. @details Executes the pred poll done scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pred_poll_done(void)
{
  TEST_BEGIN("pred poll_done MC/DC");
  TEST_ASSERT(!priv_ra8_i2c_internal_peripheral_poll_done(0U, 0U));
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_poll_done((uint8_t)k_ra8_i2c_msk_icsr1_aas0, 0U));
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_poll_done(0U, (uint8_t)k_ra8_i2c_msk_icsr2_stop));
  TEST_END("pred poll_done MC/DC");
}

/**
 * @par MC/DC:
 * Decision: `(icsr2 & stop) == 0 && received < capacity` (2 conditions).
 * - V1: stop=0, received<capacity   -> true  (control: both true)
 * - V2: stop=1, received<capacity   -> false (varies left)
 * - V3: stop=0, received==capacity  -> false (varies right)
 * V1+V2 prove the STOP flag's independent influence; V1+V3 prove the
 * room check's. N+1 = 3 vectors for N=2. @brief Verify pred rx continue behavior. @details Executes the pred rx continue scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pred_rx_continue(void)
{
  TEST_BEGIN("pred rx_continue MC/DC");
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_rx_continue(0U, 0U, (uint32_t)k_t_cap));
  TEST_ASSERT(!priv_ra8_i2c_internal_peripheral_rx_continue((uint8_t)k_ra8_i2c_msk_icsr2_stop,
                                                            0U,
                                                            (uint32_t)k_t_cap));
  TEST_ASSERT(
    !priv_ra8_i2c_internal_peripheral_rx_continue(0U, (uint32_t)k_t_cap, (uint32_t)k_t_cap));
  TEST_END("pred rx_continue MC/DC");
}

/**
 * @par MC/DC:
 * Decision: `(icsr2 & nackf) != 0 || (icsr2 & tend) != 0` (2 conditions).
 * - V1: nackf=0, tend=0 -> false (control: both false)
 * - V2: nackf=1, tend=0 -> true  (varies left)
 * - V3: nackf=0, tend=1 -> true  (varies right)
 * N+1 = 3 vectors for N=2. @brief Verify pred tx done behavior. @details Executes the pred tx done scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pred_tx_done(void)
{
  TEST_BEGIN("pred tx_done MC/DC");
  TEST_ASSERT(!priv_ra8_i2c_internal_peripheral_tx_done(0U));
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_tx_done((uint8_t)k_ra8_i2c_msk_icsr2_nackf));
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_tx_done((uint8_t)k_ra8_i2c_msk_icsr2_tend));
  TEST_END("pred tx_done MC/DC");
}

/**
 * @par MC/DC:
 * Decision: `(icsr2 & nackf) == 0 && sent < len` (2 conditions).
 * - V1: nackf=0, sent<len  -> true  (control: both true)
 * - V2: nackf=1, sent<len  -> false (varies left)
 * - V3: nackf=0, sent==len -> false (varies right)
 * N+1 = 3 vectors for N=2. @brief Verify pred tx continue behavior. @details Executes the pred tx continue scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pred_tx_continue(void)
{
  TEST_BEGIN("pred tx_continue MC/DC");
  TEST_ASSERT(priv_ra8_i2c_internal_peripheral_tx_continue(0U, 0U, (uint32_t)k_t_tx_len));
  TEST_ASSERT(!priv_ra8_i2c_internal_peripheral_tx_continue((uint8_t)k_ra8_i2c_msk_icsr2_nackf,
                                                            0U,
                                                            (uint32_t)k_t_tx_len));
  TEST_ASSERT(
    !priv_ra8_i2c_internal_peripheral_tx_continue(0U, (uint32_t)k_t_tx_len, (uint32_t)k_t_tx_len));
  TEST_END("pred tx_continue MC/DC");
}

/* ===========================================================================
 * target_init / target_deinit.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decision in the path this case touches -- each guard in
 * `ra8_i2c_peripheral_init` is a single condition) @brief Verify init slot0 behavior. @details Executes the init slot0 scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_slot0(void)
{
  TEST_BEGIN("target_init slot 0");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_t_sarl_exp, reg->SARL0);
  TEST_ASSERT_EQ(0, reg->SARU0);
  TEST_ASSERT_EQ(k_ra8_i2c_msk_icser_sar0e, reg->ICSER);
  TEST_END("target_init slot 0");
}

/**
 * @par MC/DC:
 * (no compound decision -- single-condition guards only) @brief Verify init slot1 slot2 behavior. @details Executes the init slot1 slot2 scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_slot1_slot2(void)
{
  TEST_BEGIN("target_init slot 1 + slot 2");
  internal_prep();

  ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  cfg.slot                     = k_ra8_i2c_peripheral_slot_1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(k_t_sarl_exp, reg->SARL1);
  TEST_ASSERT_EQ(k_ra8_i2c_msk_icser_sar1e, reg->ICSER);

  internal_prep();
  cfg.slot = k_ra8_i2c_peripheral_slot_2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  TEST_ASSERT_EQ(k_t_sarl_exp, reg->SARL2);
  TEST_ASSERT_EQ(k_ra8_i2c_msk_icser_sar2e, reg->ICSER);
  TEST_END("target_init slot 1 + slot 2");
}

/**
 * @par MC/DC:
 * (no compound decision -- exercises the general-call + clock-stretch options) @brief Verify init gca and stretch behavior. @details Executes the init gca and stretch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_gca_and_stretch(void)
{
  TEST_BEGIN("target_init general-call + clock stretch");
  internal_prep();

  ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  cfg.general_call             = true;
  cfg.clock_stretch            = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(((uint8_t)k_ra8_i2c_msk_icser_sar0e | (uint8_t)k_ra8_i2c_msk_icser_gcae),
                 reg->ICSER);
  TEST_ASSERT((reg->ICMR3 & (uint8_t)k_ra8_i2c_msk_icmr3_wait) != 0U);
  TEST_END("target_init general-call + clock stretch");
}

/**
 * @par MC/DC:
 * (no compound decision -- single-condition rejection guards) @brief Verify init rejects behavior. @details Executes the init rejects scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_rejects(void)
{
  TEST_BEGIN("target_init rejects bad args");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_peripheral_init((uint8_t)k_t_ch, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_peripheral_init((uint8_t)k_t_ch_bad, &cfg));

  ra8_i2c_peripheral_cfg_t bad = internal_make_cfg();
  bad.own_addr_7b              = (uint8_t)k_t_addr_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &bad));

  bad      = internal_make_cfg();
  bad.slot = (ra8_i2c_peripheral_slot_t)k_t_slot_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &bad));
  TEST_END("target_init rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decision -- deinit clears ICSER / WAIT and the active flag) @brief Verify deinit behavior. @details Executes the deinit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deinit(void)
{
  TEST_BEGIN("target_deinit clears state");
  internal_prep();

  ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  cfg.clock_stretch            = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_deinit((uint8_t)k_t_ch));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(0, reg->ICSER);
  TEST_ASSERT((reg->ICMR3 & (uint8_t)k_ra8_i2c_msk_icmr3_wait) == 0U);
  /* A second deinit on the now-disarmed channel reports not-initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_i2c_peripheral_deinit((uint8_t)k_t_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_peripheral_deinit((uint8_t)k_t_ch_bad));
  TEST_END("target_deinit clears state");
}

/* ===========================================================================
 * target_poll.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * The poll-exit decision `(match) || (stop)` is promoted to
 * `priv_ra8_i2c_internal_peripheral_poll_done` and covered by internal_test_pred_poll_done.
 * This case drives the match-true branch (controller WRITE) end to end. @brief Verify poll write event behavior. @details Executes the poll write event scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_poll_write_event(void)
{
  TEST_BEGIN("target_poll controller-write event");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = (uint8_t)k_ra8_i2c_msk_icsr1_aas0;
  reg->ICCR2                 = 0U; /* TRS = 0 -> controller writes */

  ra8_i2c_peripheral_event_t ev = k_ra8_i2c_peripheral_event_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_poll((uint8_t)k_t_ch, &ev));
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_write, ev);
  TEST_END("target_poll controller-write event");
}

/**
 * @par MC/DC:
 * Drives the match-true / TRS-set branch (controller READ) end to end. @brief Verify poll read event behavior. @details Executes the poll read event scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_poll_read_event(void)
{
  TEST_BEGIN("target_poll controller-read event");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = (uint8_t)k_ra8_i2c_msk_icsr1_aas0;
  reg->ICCR2                 = (uint8_t)k_ra8_i2c_msk_iccr2_trs; /* controller reads */

  ra8_i2c_peripheral_event_t ev = k_ra8_i2c_peripheral_event_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_poll((uint8_t)k_t_ch, &ev));
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_read, ev);
  TEST_END("target_poll controller-read event");
}

/**
 * @par MC/DC:
 * Exercises the poll-exit OR right-side (STOP set, no address match): the
 * spin exits on STOP and the event classifies to `none`. @brief Verify poll none on stop behavior. @details Executes the poll none on stop scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_poll_none_on_stop(void)
{
  TEST_BEGIN("target_poll none on stop");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = 0U;
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_stop;

  ra8_i2c_peripheral_event_t ev = k_ra8_i2c_peripheral_event_read;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_poll((uint8_t)k_t_ch, &ev));
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_none, ev);
  TEST_END("target_poll none on stop");
}

/**
 * @par MC/DC:
 * (no compound decision -- the not-armed and null guards are single-condition) @brief Verify poll rejects behavior. @details Executes the poll rejects scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_poll_rejects(void)
{
  TEST_BEGIN("target_poll rejects");
  internal_prep();

  ra8_i2c_peripheral_event_t ev = k_ra8_i2c_peripheral_event_none;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_i2c_peripheral_poll((uint8_t)k_t_ch, &ev));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_peripheral_poll((uint8_t)k_t_ch, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_peripheral_poll((uint8_t)k_t_ch_bad, &ev));
  TEST_END("target_poll rejects");
}

/* ===========================================================================
 * target_receive.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * The receive loop's `(no STOP) && (room)` decision is promoted to
 * `priv_ra8_i2c_internal_peripheral_rx_continue` and covered by internal_test_pred_rx_continue.
 * This case drives the true branch repeatedly until the buffer fills. @brief Verify receive fills to capacity behavior. @details Executes the receive fills to capacity scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_fills_to_capacity(void)
{
  TEST_BEGIN("target_receive fills to capacity");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_rdrf; /* RDRF, no STOP */
  reg->ICDRR                 = (uint8_t)k_t_byte_a;

  uint8_t  buf[k_t_buf_len] = {};
  uint32_t got              = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_peripheral_receive((uint8_t)k_t_ch, buf, (uint32_t)k_t_cap, &got));
  TEST_ASSERT_EQ(k_t_cap, got);
  TEST_ASSERT_EQ(k_t_byte_a, buf[0]);
  TEST_ASSERT_EQ(k_t_byte_a, buf[(uint32_t)k_t_cap - 1U]);
  /* STOP flag cleared on exit. */
  TEST_ASSERT((reg->ICSR2 & (uint8_t)k_ra8_i2c_msk_icsr2_stop) == 0U);
  TEST_END("target_receive fills to capacity");
}

/**
 * @par MC/DC:
 * Drives the rx_continue STOP-true branch: STOP latched with a final pending
 * byte, so exactly one trailing byte is drained then the loop stops. @brief Verify receive stops on stop behavior. @details Executes the receive stops on stop scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_stops_on_stop(void)
{
  TEST_BEGIN("target_receive stops on STOP");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_rdrf | (uint8_t)k_ra8_i2c_msk_icsr2_stop);
  reg->ICDRR = (uint8_t)k_t_byte_b;

  uint8_t  buf[k_t_buf_len] = {};
  uint32_t got              = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_peripheral_receive((uint8_t)k_t_ch, buf, (uint32_t)k_t_cap_big, &got));
  TEST_ASSERT_EQ(1, got);
  TEST_ASSERT_EQ(k_t_byte_b, buf[0]);
  TEST_END("target_receive stops on STOP");
}

/**
 * @par MC/DC:
 * (no compound decision -- timeout / rejection guards are single-condition) @brief Verify receive rejects and timeout behavior. @details Executes the receive rejects and timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_rejects_and_timeout(void)
{
  TEST_BEGIN("target_receive rejects + timeout");
  internal_prep();

  uint8_t  buf[k_t_buf_len] = {};
  uint32_t got              = 0U;

  /* Not armed yet. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_i2c_peripheral_receive((uint8_t)k_t_ch, buf, (uint32_t)k_t_cap, &got));

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_peripheral_receive((uint8_t)k_t_ch, nullptr, (uint32_t)k_t_cap, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_peripheral_receive((uint8_t)k_t_ch, buf, 0U, &got));

  /* RDRF never sets -> the address-phase wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_peripheral_receive((uint8_t)k_t_ch, buf, (uint32_t)k_t_cap, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_END("target_receive rejects + timeout");
}

/* ===========================================================================
 * target_transmit.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * The transmit loop's `(no NACK) && (data remains)` decision is promoted to
 * `priv_ra8_i2c_internal_peripheral_tx_continue` (internal_test_pred_tx_continue); the
 * completion `(NACK) || (TEND)` decision is `priv_ra8_i2c_internal_peripheral_tx_done`
 * (internal_test_pred_tx_done). This case drives the all-bytes-sent + TEND path. @brief Verify transmit sends all behavior. @details Executes the transmit sends all scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_transmit_sends_all(void)
{
  TEST_BEGIN("target_transmit sends all bytes");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend);

  const uint8_t data[k_t_tx_len] = {
    (uint8_t)k_t_byte_a,
    (uint8_t)k_t_byte_b,
    (uint8_t)k_t_byte_c,
  };
  uint32_t sent = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, data, (uint32_t)k_t_tx_len, &sent));
  TEST_ASSERT_EQ(k_t_tx_len, sent);
  /* Last byte landed in ICDRT. */
  TEST_ASSERT_EQ(k_t_byte_c, reg->ICDRT);
  /* NACKF / STOP cleared on exit. */
  TEST_ASSERT((reg->ICSR2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) == 0U);
  TEST_END("target_transmit sends all bytes");
}

/**
 * @par MC/DC:
 * Drives the tx_continue NACK-true branch: NACKF latched before any byte is
 * queued, so the loop stops with zero sent and the call still returns ok
 * (a controller NACK is a normal end-of-read). @brief Verify transmit nack early behavior. @details Executes the transmit nack early scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_transmit_nack_early(void)
{
  TEST_BEGIN("target_transmit NACK before first byte");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_nackf);

  const uint8_t data[k_t_tx_len] = {
    (uint8_t)k_t_byte_a,
    (uint8_t)k_t_byte_b,
    (uint8_t)k_t_byte_c,
  };
  uint32_t sent = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, data, (uint32_t)k_t_tx_len, &sent));
  TEST_ASSERT_EQ(0, sent);
  TEST_END("target_transmit NACK before first byte");
}

/**
 * @par MC/DC:
 * Drives the tx_done false branch with bytes already queued: TDRE lets every
 * byte out but TEND/NACKF never set, so the completion classifies as a
 * timeout. @brief Verify transmit completion timeout behavior. @details Executes the transmit completion timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_transmit_completion_timeout(void)
{
  TEST_BEGIN("target_transmit completion timeout");
  internal_prep();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre; /* TDRE only */

  const uint8_t data[k_t_tx_len] = {
    (uint8_t)k_t_byte_a,
    (uint8_t)k_t_byte_b,
    (uint8_t)k_t_byte_c,
  };
  uint32_t sent = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, data, (uint32_t)k_t_tx_len, &sent));
  TEST_ASSERT_EQ(k_t_tx_len, sent);
  TEST_END("target_transmit completion timeout");
}

/**
 * @par MC/DC:
 * (no compound decision -- rejection guards are single-condition) @brief Verify transmit rejects behavior. @details Executes the transmit rejects scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_transmit_rejects(void)
{
  TEST_BEGIN("target_transmit rejects");
  internal_prep();

  const uint8_t data[k_t_tx_len] = {
    (uint8_t)k_t_byte_a,
    (uint8_t)k_t_byte_b,
    (uint8_t)k_t_byte_c,
  };
  uint32_t sent = 0U;

  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, data, (uint32_t)k_t_tx_len, &sent));

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, nullptr, (uint32_t)k_t_tx_len, &sent));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_peripheral_transmit((uint8_t)k_t_ch, data, 0U, &sent));
  TEST_END("target_transmit rejects");
}

/* ===========================================================================
 * init irq_enable / attach_handler / dispatch.
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decision -- the ``irq_enable`` guard is single-condition; this
 * drives the true branch and confirms deinit clears the armed ICIER bits) @brief Verify init irq enable arms icier behavior. @details Executes the init irq enable arms icier scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_irq_enable_arms_icier(void)
{
  TEST_BEGIN("init irq_enable arms ICIER");
  internal_prep();

  ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  cfg.irq_enable               = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  const uint8_t          arm =
    (uint8_t)((1U << (uint8_t)k_ra8_i2c_icier_rie_pos) | (1U << (uint8_t)k_ra8_i2c_icier_tie_pos) |
              (1U << (uint8_t)k_ra8_i2c_icier_spie_pos));
  TEST_ASSERT_EQ(arm, (reg->ICIER & arm));

  /* deinit clears the target interrupt-enable bits. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_deinit((uint8_t)k_t_ch));
  TEST_ASSERT_EQ(0, (reg->ICIER & arm));
  TEST_END("init irq_enable arms ICIER");
}

/**
 * @par MC/DC:
 * (no compound decision -- attach_handler has one channel-range guard; this
 * drives both the reject and the store/detach paths) @brief Verify attach handler behavior. @details Executes the attach handler scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_handler(void)
{
  TEST_BEGIN("attach_handler stores + rejects");
  internal_prep();

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch_bad, internal_record_cb, &s_ctx_token));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, nullptr, nullptr));
  TEST_END("attach_handler stores + rejects");
}

/**
 * @par MC/DC:
 * (no compound decision -- internal_i2c_dispatch_event is sequential
 * single-condition ifs; this drives the match/write branch end to end) @brief Verify dispatch write event behavior. @details Executes the dispatch write event scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_write_event(void)
{
  TEST_BEGIN("dispatch fires write");
  internal_prep();
  internal_reset_cb();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = (uint8_t)k_ra8_i2c_msk_icsr1_aas0;
  reg->ICCR2                 = 0U; /* TRS = 0 -> controller writes */

  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_write, s_cb_event);
  TEST_ASSERT((void*)&s_ctx_token == s_cb_ctx);
  TEST_END("dispatch fires write");
}

/**
 * @par MC/DC:
 * (no compound decision -- drives the match/read branch: TRS = 1) @brief Verify dispatch read event behavior. @details Executes the dispatch read event scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_read_event(void)
{
  TEST_BEGIN("dispatch fires read");
  internal_prep();
  internal_reset_cb();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = (uint8_t)k_ra8_i2c_msk_icsr1_aas0;
  reg->ICCR2                 = (uint8_t)k_ra8_i2c_msk_iccr2_trs; /* controller reads */

  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_read, s_cb_event);
  TEST_END("dispatch fires read");
}

/**
 * @par MC/DC:
 * (no compound decision -- drives the no-match / STOP branch: dispatch reports
 * ``stop`` when ICSR2.STOP is latched with no own-address match) @brief Verify dispatch stop event behavior. @details Executes the dispatch stop event scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_stop_event(void)
{
  TEST_BEGIN("dispatch fires stop");
  internal_prep();
  internal_reset_cb();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));

  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = 0U; /* no own-address match */
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_stop;

  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_i2c_peripheral_event_stop, s_cb_event);
  TEST_END("dispatch fires stop");
}

/**
 * @par MC/DC:
 * (no compound decision -- drives the ``none`` branch plus the three
 * single-condition dispatch guards: no handler, not armed, bad channel) @brief Verify dispatch none and guards behavior. @details Executes the dispatch none and guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_none_and_guards(void)
{
  TEST_BEGIN("dispatch none + guards");
  internal_prep();
  internal_reset_cb();

  const ra8_i2c_peripheral_cfg_t cfg = internal_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_t_ch, &cfg));

  /* Handler attached but neither a match nor a STOP is latched -> no fire. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_t_ch);
  reg->ICSR1                 = 0U;
  reg->ICSR2                 = 0U;
  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(0, s_cb_count);

  /* Handler detached: a fresh match must not fire. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, nullptr, nullptr));
  reg->ICSR1 = (uint8_t)k_ra8_i2c_msk_icsr1_aas0;
  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(0, s_cb_count);

  /* Not armed: dispatch is a no-op even with a handler attached. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_peripheral_attach_handler((uint8_t)k_t_ch, internal_record_cb, &s_ctx_token));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_deinit((uint8_t)k_t_ch));
  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch);
  TEST_ASSERT_EQ(0, s_cb_count);

  /* Bad channel: no register block, no crash. */
  ra8_i2c_peripheral_dispatch((uint8_t)k_t_ch_bad);
  TEST_ASSERT_EQ(0, s_cb_count);
  TEST_END("dispatch none + guards");
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
  internal_test_pred_poll_done,
  internal_test_pred_rx_continue,
  internal_test_pred_tx_done,
  internal_test_pred_tx_continue,
  internal_test_init_slot0,
  internal_test_init_slot1_slot2,
  internal_test_init_gca_and_stretch,
  internal_test_init_rejects,
  internal_test_deinit,
  internal_test_poll_write_event,
  internal_test_poll_read_event,
  internal_test_poll_none_on_stop,
  internal_test_poll_rejects,
  internal_test_receive_fills_to_capacity,
  internal_test_receive_stops_on_stop,
  internal_test_receive_rejects_and_timeout,
  internal_test_transmit_sends_all,
  internal_test_transmit_nack_early,
  internal_test_transmit_completion_timeout,
  internal_test_transmit_rejects,
  internal_test_init_irq_enable_arms_icier,
  internal_test_attach_handler,
  internal_test_dispatch_write_event,
  internal_test_dispatch_read_event,
  internal_test_dispatch_stop_event,
  internal_test_dispatch_none_and_guards,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
