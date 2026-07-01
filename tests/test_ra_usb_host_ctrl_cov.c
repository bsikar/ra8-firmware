/**
 * @file test_ra_usb_host_ctrl_cov.c
 * @brief White-box line-coverage tests for the polled host-mode
 *        control-transfer engine (`ra_usb_host_ctrl.c`).
 *
 * @par Tag
 * [Ring 3 / Test] {World: Host}
 *
 * @details
 * The host control engine drives a blocking SETUP / DATA / STATUS
 * transfer over the DCP (pipe 0). Its DATA / STATUS state machine only
 * runs once `internal_host_ctrl_setup` completes, and that stage gates on
 * the SIE raising `INTSTS1.SACK` (SETUP ACKed) or `INTSTS1.SIGN` (three
 * failed attempts). The host simulator backs the USB register block with
 * plain RAM: the engine clears the very SACK/SIGN bits it then spins on,
 * and nothing re-asserts them, so a transfer driven through the public
 * entry point can only ever reach the timeout leg. The same is true of
 * the `internal_wait_frdy` (hard-coded `k_ra_ok` under simulation) and
 * `internal_dcp_push_chunk` transports.
 *
 * To reach the data-driven logic (SACK/SIGN outcomes, the DATA-IN /
 * DATA-OUT / STATUS sub-machines, the FRDY-timeout and STALL error legs)
 * this TU compiles `ra_usb_host_ctrl.c` a second time as a private
 * instrumented copy: the module source is `#include`d with its five
 * exported symbols renamed to `*_cov` (so they do not collide with the
 * production copy linked from `ra_core_hal`) and its three external
 * register transports -- `internal_rmw16`, `internal_wait_frdy`,
 * `internal_dcp_push_chunk` -- redirected via preprocessor rename to
 * deterministic mocks. The `internal_rmw16` mock does the real
 * read-modify-write AND, when the engine asserts `DCPCTR.SUREQ`, scripts
 * the `INTSTS1.SACK`/`SIGN` interrupt edge the RAM sim cannot raise; when
 * it asserts `DCPCTR.CCPL` it can script the STATUS-stage `BRDY`. The
 * remaining register helpers (`internal_pick`, `internal_is_hs`,
 * `internal_dcp_pid`, `internal_select_cfifo`, `internal_fifo_read`) use
 * their real production bodies against the `ra_sim_mmap` window. No
 * hardware line is bypassed by an exclusion marker except the single DCP
 * arm read-back timeout, which no mock can flip (see below).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8d2_usb_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_internal.h"
#include "unity_minimal.h"

/**
 * @enum thc_const_t
 * @brief Named test vectors for the host control-engine coverage suite.
 *
 * @details Tests are exempt from the magic-number gate; these names keep
 * the intent of each seeded argument obvious at the call site.
 */
typedef enum : uint16_t {
  k_thc_dev_addr     = 1U,    /**< A valid DEVADDn slot to program.      */
  k_thc_rhst_fs      = 2U,    /**< DVSTCTR0.RHST = 10b (Full-Speed).     */
  k_thc_want_full    = 64U,   /**< DATA-IN capacity >= packet.           */
  k_thc_want_small   = 4U,    /**< DATA-IN capacity < packet (clamp).    */
  k_thc_mxps         = 64U,   /**< DCP max packet size.                  */
  k_thc_mxps_zero    = 0U,    /**< mxps == 0 -> step defaults to 1.      */
  k_thc_dtln         = 8U,    /**< Short-packet DTLN (< mxps).           */
  k_thc_write_len    = 4U,    /**< Single-packet control-write length.   */
  k_thc_write_big    = 100U,  /**< Multi-packet control-write length.    */
  k_thc_cap          = 64U,   /**< dcp_out_read destination capacity.    */
  k_thc_breq_getdesc = 0x06U, /**< bRequest GET_DESCRIPTOR (arbitrary).  */
  k_thc_bmreq_in     = 0x80U, /**< bmRequestType device-to-host (IN).    */
  k_thc_bmreq_out    = 0x00U, /**< bmRequestType host-to-device (OUT).   */
  k_thc_speed_bogus  = 9U,    /**< Not FS, not HS -> internal_pick NULL. */
  k_thc_fill         = 0xA5U, /**< Source payload fill byte.             */
} thc_const_t;

/**
 * @enum thc_setup_inject_t
 * @brief Scripts the SETUP-stage interrupt edge the RAM sim cannot raise.
 *
 * @details Consumed by ::mock_rmw16 when the engine asserts DCPCTR.SUREQ.
 */
typedef enum : uint8_t {
  k_thc_inject_none = 0U, /**< Inject nothing -> SETUP spins to timeout. */
  k_thc_inject_ack  = 1U, /**< Inject INTSTS1.SACK -> SETUP succeeds.    */
  k_thc_inject_sign = 2U, /**< Inject INTSTS1.SIGN -> SETUP hw_error.    */
} thc_setup_inject_t;

/* =============================================================================
 * Test-scripted mock backends for the control engine's register transports.
 * =============================================================================
 */

/** @brief Current SETUP-stage injection script (::mock_rmw16). */
static thc_setup_inject_t s_setup_inject;
/** @brief When true, ::mock_rmw16 asserts DCP BRDY on a DCPCTR.CCPL set. */
static bool s_inject_brdy_on_ccpl;
/** @brief Value ::mock_wait_frdy returns for the CFIFO ready wait. */
static ra_err_t s_frdy_result;
/** @brief When true, ::mock_wait_frdy loads CFIFOCTR with ::s_frdy_dtln. */
static bool s_frdy_set_dtln;
/**
 * @brief CFIFOCTR value ::mock_wait_frdy installs when ::s_frdy_set_dtln.
 *
 * @details The DATA-IN arm (`internal_host_ctrl_data_arm`) issues CFIFOCTR
 * BCLR before the receive loop, which clears any pre-seeded DTLN. On real
 * silicon the SIE reloads DTLN when the device's IN packet lands; the
 * plain-RAM sim cannot, so the FRDY wait -- the last step before the DTLN
 * read -- installs the scripted packet length here.
 */
static uint16_t s_frdy_dtln;
/** @brief Value ::mock_push_chunk returns for a DATA-OUT chunk push. */
static ra_err_t s_push_result;
/** @brief Number of ::mock_push_chunk calls since the last ::prep. */
static uint16_t s_push_calls;

/**
 * @brief Faithful read-modify-write plus scripted SIE-edge injection.
 *
 * @details Replaces ::internal_rmw16 for the instrumented copy. Performs
 * the real `(*reg & ~clr_mask) | set_mask` store so downstream register
 * reads stay coherent, then -- because the plain-RAM register window
 * cannot model the SIE raising an interrupt latch -- scripts the two
 * edges the control engine spins on: on a DCPCTR.SUREQ assert it injects
 * INTSTS1.SACK or SIGN per ::s_setup_inject, and on a DCPCTR.CCPL assert
 * it optionally injects BRDYSTS (the STATUS-stage IN ZLP receipt). The
 * DCPCTR block base is recovered from the passed field pointer; only the
 * SUREQ / CCPL set-masks (unique to DCPCTR writes) trigger injection, so
 * DCPCFG writes never mis-fire.
 *
 * @param[in,out] reg      Target 16-bit register field pointer.
 * @param[in]     set_mask Bits to set (after the clear).
 * @param[in]     clr_mask Bits to clear first.
 * @pre @p reg is non-NULL and points inside a mapped register block.
 * @pre The caller serialises access (single-threaded test).
 * @post `*reg == (old & ~clr_mask) | set_mask`.
 * @post Scripted INTSTS1 / BRDYSTS edges are applied on SUREQ / CCPL.
 * @note Not thread-safe; test-only.
 * @since 0.1.0
 */
static void mock_rmw16(volatile uint16_t* reg, uint16_t set_mask, uint16_t clr_mask)
{
  *reg = (uint16_t)(((uint16_t)(*reg) & (uint16_t)~clr_mask) | set_mask);
  volatile r_usb_regs_t* base =
    (volatile r_usb_regs_t*)((uintptr_t)reg - (uintptr_t)offsetof(r_usb_regs_t, DCPCTR));
  const uint16_t sureq_bit = (uint16_t)(1U << k_ra_dcpctr_bit_sureq);
  const uint16_t ccpl_bit  = (uint16_t)(1U << k_ra_dcpctr_bit_ccpl);
  if ((set_mask & sureq_bit) != 0U) {
    if (s_setup_inject == k_thc_inject_ack) {
      base->INTSTS1 = (uint16_t)(base->INTSTS1 | (uint16_t)(1U << k_ra_int1_bit_sack));
    }
    if (s_setup_inject == k_thc_inject_sign) {
      base->INTSTS1 = (uint16_t)(base->INTSTS1 | (uint16_t)(1U << k_ra_int1_bit_sign));
    }
  }
  if ((set_mask & ccpl_bit) != 0U) {
    if (s_inject_brdy_on_ccpl) {
      base->BRDYSTS = (uint16_t)(base->BRDYSTS | (uint16_t)k_ra_usb_dcp_pipe0_bit);
    }
  }
}

/**
 * @brief Scripted CFIFO-ready transport (replaces ::internal_wait_frdy).
 *
 * @param[in] reg Register block; CFIFOCTR is loaded when ::s_frdy_set_dtln.
 * @return ::s_frdy_result.
 * @pre None.
 * @post CFIFOCTR carries ::s_frdy_dtln when ::s_frdy_set_dtln is true.
 * @note Test-only.
 * @since 0.1.0
 */
static ra_err_t mock_wait_frdy(volatile r_usb_regs_t* reg)
{
  if (s_frdy_set_dtln) {
    reg->CFIFOCTR = s_frdy_dtln;
  }
  return s_frdy_result;
}

/**
 * @brief Scripted DATA-OUT chunk push (replaces ::internal_dcp_push_chunk).
 *
 * @param[in] reg Unused; kept for signature parity.
 * @param[in] p   Unused source payload pointer.
 * @param[in] n   Unused chunk length.
 * @return ::s_push_result.
 * @pre None.
 * @post ::s_push_calls incremented.
 * @note Test-only.
 * @since 0.1.0
 */
static ra_err_t mock_push_chunk(volatile r_usb_regs_t* reg, const uint8_t* p, uint16_t n)
{
  (void)reg;
  (void)p;
  (void)n;
  ++s_push_calls;
  return s_push_result;
}

/**
 * @brief Reset the simulated register window and mock scripts to baseline.
 *
 * @details Scrubs the mapped USB window so a prior test's writes cannot
 * leak, and returns every mock script to its "hardware-cooperates" default
 * (no injection, FRDY ready, pushes succeed).
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  s_setup_inject        = k_thc_inject_none;
  s_inject_brdy_on_ccpl = false;
  s_frdy_result         = k_ra_ok;
  s_frdy_set_dtln       = false;
  s_frdy_dtln           = 0U;
  s_push_result         = k_ra_ok;
  s_push_calls          = 0U;
}

/* Rename the five exported symbols so the instrumented copy does not clash
 * with the production definitions linked from ra_core_hal, and redirect the
 * three scriptable register transports to the deterministic mocks above. */
#define ra_usb_host_ctrl_stage       ra_usb_host_ctrl_stage_cov
#define internal_host_program_devadd internal_host_program_devadd_cov
#define ra_usb_dcp_out_arm           ra_usb_dcp_out_arm_cov
#define ra_usb_dcp_out_read          ra_usb_dcp_out_read_cov
#define ra_usb_host_control_xfer     ra_usb_host_control_xfer_cov
#define internal_rmw16               mock_rmw16
#define internal_wait_frdy           mock_wait_frdy
#define internal_dcp_push_chunk      mock_push_chunk

#include "ra_usb_host_ctrl.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * White-box tests: each drives the real production logic on scripted bytes.
 * =============================================================================
 */

/** @brief Read a DEVADDn slot by raw offset (past the modelled struct). */
static uint16_t thc_read_devadd(volatile r_usb_regs_t* reg, uint8_t dev_addr)
{
  const uintptr_t off =
    (uintptr_t)k_ra_usb_devadd0_off + ((uintptr_t)dev_addr * (uintptr_t)k_ra_usb_devadd_stride);
  return *(volatile uint16_t*)((uintptr_t)reg + off);
}

/**
 * @test test_stage_and_program_devadd
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the stage getter is a
 * pure read and `internal_host_program_devadd` is straight-line register
 * arithmetic; no `&&` or `||`.)
 */
static void test_stage_and_program_devadd(void)
{
  TEST_BEGIN("stage getter reads the diagnostic; program_devadd copies RHST->USBSPD");
  prep();

  /* The stage getter returns the current bring-up diagnostic. */
  (void)ra_usb_host_ctrl_stage_cov();

  /* program_devadd copies DVSTCTR0.RHST into DEVADDn.USBSPD[7:6]. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->DVSTCTR0              = (uint16_t)k_thc_rhst_fs;
  internal_host_program_devadd_cov(reg, (uint8_t)k_thc_dev_addr);
  const uint16_t expect = (uint16_t)((uint16_t)k_thc_rhst_fs << (uint16_t)k_ra_usb_usbspd_shift);
  TEST_ASSERT_EQ(expect, thc_read_devadd(reg, (uint8_t)k_thc_dev_addr));

  TEST_END("stage getter reads the diagnostic; program_devadd copies RHST->USBSPD");
}

/**
 * @test test_wait_sts_both_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `internal_host_wait_sts`
 * spins on a single-condition `if ((*sts & mask) != 0)`; no `&&` or `||`.)
 */
static void test_wait_sts_both_legs(void)
{
  TEST_BEGIN("wait_sts returns ok when the bit is set, timeout when it never is");
  prep();

  const uint16_t mask = (uint16_t)k_ra_usb_dcp_pipe0_bit;

  /* Bit already set -> ok on the first spin iteration. */
  uint16_t sts_set = mask;
  TEST_ASSERT_EQ(k_ra_ok, internal_host_wait_sts(&sts_set, mask));

  /* Bit never asserts -> the bounded spin exits with a hardware timeout. */
  uint16_t sts_clear = 0U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, internal_host_wait_sts(&sts_clear, mask));

  TEST_END("wait_sts returns ok when the bit is set, timeout when it never is");
}

/**
 * @test test_dcp_in_wait_all_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the BRDY, STALL and NRDY
 * checks are three separate single-condition `if` statements, each with its
 * own effect; no `&&` or `||`.)
 */
static void test_dcp_in_wait_all_legs(void)
{
  TEST_BEGIN("dcp_in_wait: BRDY->ok, STALL->hw_error, NRDY re-arm then timeout");
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();

  /* A packet has landed (BRDY set) -> ok on the first iteration. */
  reg->BRDYSTS = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  TEST_ASSERT_EQ(k_ra_ok, internal_host_dcp_in_wait(reg));

  /* The DCP is STALLed (PID[1]) -> hw_error. */
  prep();
  reg          = ra_usb_fs();
  reg->BRDYSTS = 0U;
  reg->DCPCTR  = (uint16_t)k_ra_usb_pid_stall_bit;
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_host_dcp_in_wait(reg));

  /* Only NRDY is pending -> clear it, re-arm PID=BUF, then spin to timeout
   * because no BRDY ever follows in the RAM sim. */
  prep();
  reg          = ra_usb_fs();
  reg->BRDYSTS = 0U;
  reg->DCPCTR  = 0U;
  reg->NRDYSTS = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, internal_host_dcp_in_wait(reg));
  /* The NRDY latch was acknowledged (W0C-cleared) during the re-arm. */
  TEST_ASSERT_EQ(0U, (uint16_t)(reg->NRDYSTS & (uint16_t)k_ra_usb_dcp_pipe0_bit));

  TEST_END("dcp_in_wait: BRDY->ok, STALL->hw_error, NRDY re-arm then timeout");
}

/**
 * @test test_ctrl_setup_busy_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the wedged-SUREQ guard
 * and the HS SUREQCLR guard are nested single-condition `if` statements;
 * each is driven in isolation, no `&&` or `||`.)
 */
static void test_ctrl_setup_busy_legs(void)
{
  TEST_BEGIN("ctrl_setup: wedged SUREQ reports busy (HS tries SUREQCLR, FS does not)");

  const uint16_t sureq = (uint16_t)(1U << k_ra_dcpctr_bit_sureq);
  ra_usb_setup_t setup = {.bm_request_type = (uint8_t)k_thc_bmreq_in,
                          .b_request       = (uint8_t)k_thc_breq_getdesc,
                          .w_value         = 0U,
                          .w_index         = 0U,
                          .w_length        = 0U};

  /* HS instance: SUREQ wedged -> the HS-only SUREQCLR path runs, but the RAM
   * sim does not model the abort, so SUREQ stays set and busy is returned. */
  prep();
  volatile r_usb_regs_t* hs = internal_pick(k_ra_usb_speed_hs);
  hs->DCPCTR                = sureq;
  TEST_ASSERT_EQ(k_ra_err_busy, internal_host_ctrl_setup(hs, &setup));

  /* FS instance: SUREQ wedged -> SUREQCLR is skipped (FS has no such bit)
   * and busy is returned directly. */
  prep();
  volatile r_usb_regs_t* fs = internal_pick(k_ra_usb_speed_fs);
  fs->DCPCTR                = sureq;
  TEST_ASSERT_EQ(k_ra_err_busy, internal_host_ctrl_setup(fs, &setup));

  TEST_END("ctrl_setup: wedged SUREQ reports busy (HS tries SUREQCLR, FS does not)");
}

/**
 * @test test_ctrl_setup_outcomes
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the SETUP spin tests
 * SACK and SIGN with two separate single-condition `if` statements; each
 * outcome is scripted independently, no `&&` or `||`.)
 */
static void test_ctrl_setup_outcomes(void)
{
  TEST_BEGIN("ctrl_setup: SACK->ok, SIGN->hw_error, no edge->hw_timeout");

  ra_usb_setup_t setup = {.bm_request_type = (uint8_t)k_thc_bmreq_in,
                          .b_request       = (uint8_t)k_thc_breq_getdesc,
                          .w_value         = 0U,
                          .w_index         = 0U,
                          .w_length        = 0U};

  /* SACK latches -> the SETUP token was ACKed. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  s_setup_inject             = k_thc_inject_ack;
  TEST_ASSERT_EQ(k_ra_ok, internal_host_ctrl_setup(reg, &setup));

  /* SIGN latches -> three transmission attempts failed. */
  prep();
  reg            = ra_usb_fs();
  s_setup_inject = k_thc_inject_sign;
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_host_ctrl_setup(reg, &setup));

  /* Neither edge asserts -> the bounded spin reports a hardware timeout. */
  prep();
  reg            = ra_usb_fs();
  s_setup_inject = k_thc_inject_none;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, internal_host_ctrl_setup(reg, &setup));

  TEST_END("ctrl_setup: SACK->ok, SIGN->hw_error, no edge->hw_timeout");
}

/**
 * @test test_ctrl_data_in_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the receive loop uses
 * single-condition `if` checks for the wait outcome, the chunk clamp, the
 * short-packet exit and the buffer-full exit; no `&&` or `||`.)
 */
static void test_ctrl_data_in_legs(void)
{
  TEST_BEGIN("ctrl_data_in: short packet, overflow clamp, dcp-wait fail, frdy fail");
  uint8_t  buf[k_thc_want_full] = {};
  uint16_t rx                   = 0U;

  /* Short packet (DTLN < mxps) with room to spare -> ok, DTLN bytes read.
   * The DATA-IN arm BCLRs CFIFOCTR, so the scripted DTLN is installed by the
   * FRDY hook that fires just before the length read. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->BRDYSTS               = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  s_frdy_set_dtln            = true;
  s_frdy_dtln                = (uint16_t)k_thc_dtln;
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_ctrl_data_in(reg, buf, (uint16_t)k_thc_want_full, (uint16_t)k_thc_mxps, &rx));
  TEST_ASSERT_EQ((uint16_t)k_thc_dtln, rx);

  /* Overflow: DTLN(8) > want(4) -> chunk clamps to 4, the buffer-full exit
   * fires (rx >= want) alongside the short-packet exit. */
  prep();
  reg             = ra_usb_fs();
  reg->BRDYSTS    = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  s_frdy_set_dtln = true;
  s_frdy_dtln     = (uint16_t)k_thc_dtln;
  rx              = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_ctrl_data_in(reg, buf, (uint16_t)k_thc_want_small, (uint16_t)k_thc_mxps, &rx));
  TEST_ASSERT_EQ((uint16_t)k_thc_dtln, rx);

  /* dcp_in_wait never sees a packet -> the loop's error leg parks NAK and
   * returns the timeout. */
  prep();
  reg          = ra_usb_fs();
  reg->BRDYSTS = 0U;
  reg->NRDYSTS = 0U;
  reg->DCPCTR  = 0U;
  rx           = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_hw_timeout,
    internal_host_ctrl_data_in(reg, buf, (uint16_t)k_thc_want_full, (uint16_t)k_thc_mxps, &rx));

  /* A packet lands but CFIFO never reports FRDY -> the FRDY-timeout leg. */
  prep();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  s_frdy_result = k_ra_err_hw_timeout;
  rx            = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_hw_timeout,
    internal_host_ctrl_data_in(reg, buf, (uint16_t)k_thc_want_full, (uint16_t)k_thc_mxps, &rx));

  TEST_END("ctrl_data_in: short packet, overflow clamp, dcp-wait fail, frdy fail");
}

/**
 * @test test_ctrl_data_out_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the send loop uses a
 * single-condition `while (off < want)`, a single-condition chunk clamp and
 * a single-condition push-error check; no `&&` or `||`.)
 */
static void test_ctrl_data_out_legs(void)
{
  TEST_BEGIN("ctrl_data_out: multi-packet clamp, mxps==0 step, push failure");
  uint8_t src[k_thc_write_big];
  (void)memset(src, (int)k_thc_fill, sizeof src);

  /* Multi-packet (want 100, mxps 64): first chunk clamps to 64, the tail
   * chunk (36) does not clamp -> two pushes, then ok. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_ctrl_data_out(reg, src, (uint16_t)k_thc_write_big, (uint16_t)k_thc_mxps));
  TEST_ASSERT_EQ(2U, s_push_calls);

  /* mxps == 0 -> the step defaults to 1 (single-byte chunk) and completes. */
  prep();
  reg = ra_usb_fs();
  TEST_ASSERT_EQ(k_ra_ok, internal_host_ctrl_data_out(reg, src, 1U, (uint16_t)k_thc_mxps_zero));

  /* The first push fails -> the engine parks NAK and returns the error. */
  prep();
  reg           = ra_usb_fs();
  s_push_result = k_ra_err_hw_error;
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    internal_host_ctrl_data_out(reg, src, (uint16_t)k_thc_write_len, (uint16_t)k_thc_mxps));

  TEST_END("ctrl_data_out: multi-packet clamp, mxps==0 step, push failure");
}

/**
 * @test test_ctrl_status_both_directions
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- a single-condition
 * `if (!write_zlp)` selects the DIR restore, and a single-condition
 * `if (write_zlp)` selects the OUT-ZLP vs IN-ZLP status stage; no `&&` or
 * `||`.)
 */
static void test_ctrl_status_both_directions(void)
{
  TEST_BEGIN("ctrl_status: control-read OUT-ZLP, control-write IN-ZLP (both wait legs)");

  /* Control-read status (write_zlp = true): drives the OUT ZLP, best-effort
   * BEMP wait, always ok. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ(k_ra_ok, internal_host_ctrl_status(reg, true));

  /* Control-write status (write_zlp = false) with no scripted BRDY: the IN
   * ZLP never arrives, so dcp_in_wait times out and that propagates. */
  prep();
  reg = ra_usb_fs();
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, internal_host_ctrl_status(reg, false));

  /* Control-write status with the STATUS-stage BRDY scripted on the CCPL
   * assert: the IN ZLP is "received" and the status stage returns ok. */
  prep();
  reg                   = ra_usb_fs();
  s_inject_brdy_on_ccpl = true;
  TEST_ASSERT_EQ(k_ra_ok, internal_host_ctrl_status(reg, false));

  TEST_END("ctrl_status: control-read OUT-ZLP, control-write IN-ZLP (both wait legs)");
}

/**
 * @test test_dcp_out_arm
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `ra_usb_dcp_out_arm` uses
 * a single-condition speed guard and a single-condition BRDYENB read-back
 * guard; no `&&` or `||`.)
 */
static void test_dcp_out_arm(void)
{
  TEST_BEGIN("dcp_out_arm: bad speed rejected, valid speed arms DCP BRDY + PID=BUF");
  prep();

  /* Bogus speed -> internal_pick returns nullptr -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_usb_dcp_out_arm_cov((ra_usb_speed_t)k_thc_speed_bogus));

  /* Valid speed -> DCP armed; the BRDYENB read-back carries the DCP bit. */
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_arm_cov(k_ra_usb_speed_fs));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT((reg->BRDYENB & (uint16_t)k_ra_usb_dcp_pipe0_bit) != 0U);

  TEST_END("dcp_out_arm: bad speed rejected, valid speed arms DCP BRDY + PID=BUF");
}

/**
 * @test test_dcp_out_read
 *
 * @par MC/DC:
 * Decision: `if ((buf == nullptr) || (out_rx == nullptr))` (2 conditions)
 * - Vector 1: buf=valid, out_rx=valid -> false (control: both false).
 * - Vector 2: buf=NULL,  out_rx=valid -> true  (varies buf only).
 * - Vector 3: buf=valid, out_rx=NULL  -> true  (varies out_rx only).
 * Vectors 1+2 prove buf independently affects the outcome; 1+3 prove the
 * same for out_rx. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_dcp_out_read(void)
{
  TEST_BEGIN("dcp_out_read: guards, no-data, ZLP drain, data drain, frdy timeout");
  uint8_t  buf[k_thc_cap] = {};
  uint16_t rx             = 0U;

  /* Bogus speed -> invalid_arg. */
  prep();
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_usb_dcp_out_read_cov((ra_usb_speed_t)k_thc_speed_bogus, buf, (uint16_t)k_thc_cap, &rx));

  /* Null destination (MC/DC vector 2). */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, nullptr, (uint16_t)k_thc_cap, &rx));

  /* Null out-count (MC/DC vector 3). */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, buf, (uint16_t)k_thc_cap, nullptr));

  /* BRDY not set -> the OUT packet has not landed -> no_data. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->BRDYSTS               = 0U;
  TEST_ASSERT_EQ(k_ra_err_no_data,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));

  /* BRDY set, FRDY never asserts -> the CFIFO wait times out (MC/DC vector 1
   * for the null guard: both pointers valid). */
  prep();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  s_frdy_result = k_ra_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));

  /* BRDY set, zero-length packet (DTLN == 0) -> released via BCLR, ok. */
  prep();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  reg->CFIFOCTR = 0U;
  rx            = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  TEST_ASSERT_EQ(0U, rx);

  /* BRDY set, non-zero DTLN within capacity -> drained via the CFIFO, ok. */
  prep();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  reg->CFIFOCTR = (uint16_t)k_thc_dtln;
  rx            = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_dcp_out_read_cov(k_ra_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  TEST_ASSERT_EQ((uint16_t)k_thc_dtln, rx);

  TEST_END("dcp_out_read: guards, no-data, ZLP drain, data drain, frdy timeout");
}

/**
 * @test test_data_phase_directions
 *
 * @par MC/DC:
 * Decision: `if ((w_length > 0) && !is_read && (data != nullptr))`
 * (3 conditions: A = w_length>0, B = !is_read, C = data!=nullptr)
 * - Vector 1 (no-data):       A=0,B=1,C=1 -> false (varies A).
 * - Vector 2 (control-read):  A=1,B=0,C=1 -> false (varies B).
 * - Vector 3 (write no buf):  A=1,B=1,C=0 -> false (varies C).
 * - Vector 4 (control-write): A=1,B=1,C=1 -> true  (control: all true).
 * Vectors 4+1 prove A independent, 4+2 prove B, 4+3 prove C. N+1 = 4
 * vectors for N=3 conditions: minimal MC/DC.
 */
static void test_data_phase_directions(void)
{
  TEST_BEGIN("data_phase: no-data, control-read, write-without-buffer, control-write");
  uint8_t  data[k_thc_want_full] = {};
  uint16_t rx                    = 0U;
  bool     is_read               = false;

  /* Vector 1: no data stage (w_length == 0) -> is_read false, returns ok. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->DCPMAXP               = (uint16_t)k_thc_mxps;
  ra_usb_setup_t s_none      = {.bm_request_type = (uint8_t)k_thc_bmreq_in,
                                .b_request       = (uint8_t)k_thc_breq_getdesc,
                                .w_value         = 0U,
                                .w_index         = 0U,
                                .w_length        = 0U};
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_data_phase(reg, &s_none, data, (uint16_t)k_thc_want_full, &rx, &is_read));
  TEST_ASSERT(!is_read);

  /* Vector 2: control-read (IN + buffer) -> is_read true, DATA-IN runs. */
  prep();
  reg                 = ra_usb_fs();
  reg->DCPMAXP        = (uint16_t)k_thc_mxps;
  reg->BRDYSTS        = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  s_frdy_set_dtln     = true;
  s_frdy_dtln         = (uint16_t)k_thc_dtln;
  ra_usb_setup_t s_in = {.bm_request_type = (uint8_t)k_thc_bmreq_in,
                         .b_request       = (uint8_t)k_thc_breq_getdesc,
                         .w_value         = 0U,
                         .w_index         = 0U,
                         .w_length        = (uint16_t)k_thc_want_full};
  rx                  = 0U;
  is_read             = false;
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_data_phase(reg, &s_in, data, (uint16_t)k_thc_want_full, &rx, &is_read));
  TEST_ASSERT(is_read);
  TEST_ASSERT_EQ((uint16_t)k_thc_dtln, rx);

  /* Vector 3: IN request but no buffer (data == NULL) -> no data stage. */
  prep();
  reg          = ra_usb_fs();
  reg->DCPMAXP = (uint16_t)k_thc_mxps;
  rx           = 0U;
  is_read      = false;
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_data_phase(reg, &s_in, nullptr, (uint16_t)k_thc_want_full, &rx, &is_read));
  TEST_ASSERT(!is_read);

  /* Vector 4: control-write (OUT + buffer + length) -> DATA-OUT runs. */
  prep();
  reg                  = ra_usb_fs();
  reg->DCPMAXP         = (uint16_t)k_thc_mxps;
  ra_usb_setup_t s_out = {.bm_request_type = (uint8_t)k_thc_bmreq_out,
                          .b_request       = (uint8_t)k_thc_breq_getdesc,
                          .w_value         = 0U,
                          .w_index         = 0U,
                          .w_length        = (uint16_t)k_thc_write_len};
  rx                   = 0U;
  is_read              = false;
  TEST_ASSERT_EQ(
    k_ra_ok,
    internal_host_data_phase(reg, &s_out, data, (uint16_t)k_thc_want_full, &rx, &is_read));
  TEST_ASSERT(!is_read);

  TEST_END("data_phase: no-data, control-read, write-without-buffer, control-write");
}

/**
 * @test test_control_xfer_end_to_end
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `ra_usb_host_control_xfer`
 * is a sequence of single-condition error short-circuits and a
 * single-condition `out_received != nullptr` copy guard; no `&&` or `||`.)
 */
static void test_control_xfer_end_to_end(void)
{
  TEST_BEGIN("control_xfer: null/speed guards, setup fail, data fail, full success");
  uint8_t  data[k_thc_write_len] = {};
  uint16_t received              = 0U;

  ra_usb_setup_t s_write = {.bm_request_type = (uint8_t)k_thc_bmreq_out,
                            .b_request       = (uint8_t)k_thc_breq_getdesc,
                            .w_value         = 0U,
                            .w_index         = 0U,
                            .w_length        = (uint16_t)k_thc_write_len};

  /* Null setup -> RA_CHECK_NULL_PTR rejects it. */
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_usb_host_control_xfer_cov(k_ra_usb_speed_fs,
                                              nullptr,
                                              data,
                                              (uint16_t)k_thc_write_len,
                                              &received));

  /* Bogus speed -> internal_pick returns nullptr -> invalid_arg. */
  prep();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_host_control_xfer_cov((ra_usb_speed_t)k_thc_speed_bogus,
                                              &s_write,
                                              data,
                                              (uint16_t)k_thc_write_len,
                                              &received));

  /* SETUP stage fails (SIGN) -> the error propagates out before any data. */
  prep();
  s_setup_inject = k_thc_inject_sign;
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_usb_host_control_xfer_cov(k_ra_usb_speed_fs,
                                              &s_write,
                                              data,
                                              (uint16_t)k_thc_write_len,
                                              &received));

  /* SETUP passes (SACK) but the DATA-OUT push fails -> data-phase error. */
  prep();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->DCPMAXP               = (uint16_t)k_thc_mxps;
  s_setup_inject             = k_thc_inject_ack;
  s_push_result              = k_ra_err_hw_error;
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_usb_host_control_xfer_cov(k_ra_usb_speed_fs,
                                              &s_write,
                                              data,
                                              (uint16_t)k_thc_write_len,
                                              &received));

  /* Full success: SACK setup, DATA-OUT push ok, STATUS IN-ZLP scripted via
   * the CCPL BRDY injection -> the whole ladder completes and the stage
   * diagnostic advances to "done". */
  prep();
  reg                   = ra_usb_fs();
  reg->DCPMAXP          = (uint16_t)k_thc_mxps;
  s_setup_inject        = k_thc_inject_ack;
  s_inject_brdy_on_ccpl = true;
  received              = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_host_control_xfer_cov(k_ra_usb_speed_fs,
                                              &s_write,
                                              data,
                                              (uint16_t)k_thc_write_len,
                                              &received));
  TEST_ASSERT_EQ(0U, received);
  TEST_ASSERT_EQ((uint8_t)k_ra_usb_cs_done, ra_usb_host_ctrl_stage_cov());

  TEST_END("control_xfer: null/speed guards, setup fail, data fail, full success");
}

int32_t main(void)
{
  test_stage_and_program_devadd();
  test_wait_sts_both_legs();
  test_dcp_in_wait_all_legs();
  test_ctrl_setup_busy_legs();
  test_ctrl_setup_outcomes();
  test_ctrl_data_in_legs();
  test_ctrl_data_out_legs();
  test_ctrl_status_both_directions();
  test_dcp_out_arm();
  test_dcp_out_read();
  test_data_phase_directions();
  test_control_xfer_end_to_end();
  (void)fprintf(stderr, "[OK ] test_ra_usb_host_ctrl_cov.c\n");
  return 0;
}
