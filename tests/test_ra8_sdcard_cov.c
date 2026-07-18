/**
 * @file test_ra8_sdcard_cov.c
 * @brief Coverage-extension tests for ra8_sdcard.c -- targets 26 uncovered lines
 *
 * @details
 * Dedicated test binary that exercises the error and alternate-path branches
 * left uncovered by test_ra8_sdcard.c.  Covers:
 *
 *   - CMD8 echo-pattern mismatch (line 293)
 *   - ACMD41 loop: CMD55 hardware timeout (lines 164, 414, 415)
 *   - ACMD41 loop: ACMD41 hardware timeout (lines 173, 414, 415)
 *   - ACMD41 loop: busy-bit never set -> all 1000 retries exhausted
 *     (lines 180, 182, 414, 415)
 *   - CSD v1 (SDSC) decode path (lines 230-245)
 *   - SDSC card classification (line 385)
 *   - SDSC byte-address translation (line 467)
 *   - CSD v2 SDXC large-capacity classification (line 388)
 *   - CSD unknown structure (line 247)
 *   - publish-and-select CSD-decode failure, deinit on error
 *     (lines 346, 423, 424)
 *
 * The ra8_sdhi_deinit failure leg inside ra8_sdcard_deinit is reached by
 * arming the ra8_sim_mmio fault seam on the SDHI module's MSTPCR register so
 * the ra8_mstp_disable readback never settles.
 *
 * Each multi-command test uses a mode-driven ra8_sim_mmio poll-hook
 * identical in spirit to test_ra8_sdcard.c (no wall-clock timer, no
 * servicer thread).  After asserting RSPEND for a command, the hook
 * overwrites SD_CMD with a sentinel value (> 63, outside the valid SD
 * command range) so that subsequent steps -- before the driver writes
 * the next SD_CMD -- cannot accidentally assert RSPEND for the stale
 * command index.  Fail-mode tests
 * skip RSPEND for the failing command; the driver's bounded spin budget
 * then expires naturally.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sdcard.h"
#include "ra8_sdhi.h"
#include "ra8_sdhi_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Test constants
 * ---------------------------------------------------------------------------
 */

/**
 * @enum cov_test_inst_t
 * @brief SDHI instance index used by all coverage tests.
 *
 * @details Single-instance driver; all tests share instance 0.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cov_test_inst = 0U, /**< SDHI instance 0. */
} cov_test_inst_t;

/**
 * @enum cov_sd_cmd_t
 * @brief SD command sentinel and bounds.
 *
 * @details
 * k_cov_sd_cmd_sentinel is written to SD_CMD by the poll-hook after
 * asserting RSPEND for a command.  Its value (255) lies outside the valid
 * SD command range (0-63), so subsequent hook steps see the sentinel
 * and do nothing, preventing spurious RSPEND assertions for stale command
 * indices.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_sd_cmd_max_valid = 63U,   /**< Highest legal SD command index. */
  k_cov_sd_cmd_sentinel  = 0xFFU, /**< Sentinel: out of valid range.   */
} cov_sd_cmd_t;

/**
 * @enum cov_ocr_t
 * @brief OCR response constants used by poll-hook modes.
 *
 * @details
 * - busy bit 31 set means card is ready (ACMD41 loop exits)
 * - CCS bit 30 set means the card is SDHC/SDXC
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_ocr_sdsc_ready = 0x80FF8000UL, /**< Busy=1, CCS=0 -> SDSC             */
  k_cov_ocr_sdhc_ready = 0xC0FF8000UL, /**< Busy=1, CCS=1 -> SDHC/SDXC        */
  k_cov_ocr_not_ready  = 0x00000000UL, /**< Busy=0 -> card still initialising */
} cov_ocr_t;

/**
 * @enum cov_cmd8_echo_t
 * @brief CMD8 echo constants.
 *
 * @details
 * The SD Physical Layer Spec requires the host to send 0x000001AA and
 * the card to echo the low 12 bits (0x1AA).  k_cov_cmd8_good_echo is
 * the correct response; k_cov_cmd8_bad_echo is an intentionally wrong
 * value to trigger the pattern-mismatch path.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_cmd8_good_echo = 0x000001AAUL, /**< Correct CMD8 echo pattern.       */
  k_cov_cmd8_bad_echo  = 0x00000000UL, /**< Wrong echo -- triggers line 293. */
} cov_cmd8_echo_t;

/**
 * @enum cov_rca_t
 * @brief Relative Card Address constants for CMD3 response.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_rca_value         = 0xABCDU,      /**< Card-published RCA.             */
  k_cov_rca_rsp10_shifted = 0xABCD0000UL, /**< RCA in upper 16 bits of rsp[0]. */
} cov_rca_t;

/**
 * @enum cov_csd_v1_t
 * @brief CSD v1 response words for the SDSC decode path.
 *
 * @details
 * SD_RSP76 = 0 -> CSD_STRUCTURE = 0 (v1)
 * SD_RSP54 = 0x00090000 -> READ_BL_LEN = 9 (block_len = 512)
 * SD_RSP32 = 0x1FC00080 -> C_SIZE low10 = 127, C_SIZE_MULT = 1
 * Capacity: blocknr = (127+1)*8 = 1024; blocks = 1024*512/512 = 1024
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_csd_v1_rsp76           = 0x00000000UL, /**< CSD_STRUCTURE=0 in bits [31:30]. */
  k_cov_csd_v1_rsp54           = 0x00090000UL, /**< READ_BL_LEN=9 in bits [19:16].   */
  k_cov_csd_v1_rsp32           = 0x1FC00080UL, /**< C_SIZE=127 | C_SIZE_MULT=1.      */
  k_cov_csd_v1_rsp10           = 0x00000000UL, /**< Unused CSD v1 word.              */
  k_cov_csd_v1_expected_blocks = 1024UL,       /**< (127+1)*2^3 blocks.              */
} cov_csd_v1_t;

/**
 * @enum cov_csd_v2_sdxc_t
 * @brief CSD v2 response words for the SDXC classification path.
 *
 * @details
 * C_SIZE = 65536 (0x10000, 22-bit field) gives:
 *   blocks = (65536+1) * 1024 = 67,109,888 > threshold 67,108,864 -> SDXC
 * Encoding:
 *   C_SIZE bits[21:16] in rsp[2]&0x3F = 1 (high 6 bits of C_SIZE=0x10000)
 *   C_SIZE bits[15:0]  in rsp[1]>>16  = 0 (low 16 bits)
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_csd_v2_sdxc_rsp76 = 0x40000000UL, /**< CSD_STRUCTURE=1 in bits [31:30]. */
  k_cov_csd_v2_sdxc_rsp54 = 0x00000001UL, /**< C_SIZE bits[21:16] = 1.          */
  k_cov_csd_v2_sdxc_rsp32 = 0x00000000UL, /**< C_SIZE bits[15:0]  = 0.          */
  k_cov_csd_v2_sdxc_rsp10 = 0x00000000UL, /**< Unused.                          */
} cov_csd_v2_sdxc_t;

/**
 * @enum cov_csd_bad_t
 * @brief CSD response with unknown CSD_STRUCTURE = 2.
 *
 * @details
 * rsp[3][31:30] = 0b10 = 2 -> neither v1 nor v2 -> line 247.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_csd_bad_rsp76 = 0x80000000UL, /**< CSD_STRUCTURE=2 (unknown). */
  k_cov_csd_bad_rsp54 = 0x00000000UL, /**< Cov csd bad rsp54.         */
  k_cov_csd_bad_rsp32 = 0x00000000UL, /**< Cov csd bad rsp32.         */
  k_cov_csd_bad_rsp10 = 0x00000000UL, /**< Cov csd bad rsp10.         */
} cov_csd_bad_t;

/* ---------------------------------------------------------------------------
 * Servicer state
 * ---------------------------------------------------------------------------
 */

/**
 * @enum cov_hook_mode_t
 * @brief Operating mode for the coverage poll-hook.
 *
 * @details
 * Each mode controls which commands receive RSPEND and which responses are
 * injected into the SDHI response registers.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cov_mode_cmd8_bad_echo   = 0U, /**< CMD0 ok; CMD8 gets wrong echo.             */
  k_cov_mode_fail_cmd55      = 1U, /**< CMD0/8 ok; CMD55 gets no RSPEND.           */
  k_cov_mode_fail_acmd41     = 2U, /**< CMD0/8/55 ok; ACMD41 gets no RSPEND.       */
  k_cov_mode_acmd41_ocr_busy = 3U, /**< CMD0/8/55/41 ok; OCR busy bit stays 0.     */
  k_cov_mode_sdsc_v1         = 4U, /**< Full init: CSD v1, OCR CCS=0 (SDSC).       */
  k_cov_mode_sdxc_v2         = 5U, /**< Full init: CSD v2 large, OCR CCS=1 (SDXC). */
  k_cov_mode_csd_bad_struct  = 6U, /**< Full init up to CMD9; CSD_STRUCTURE=2.     */
} cov_hook_mode_t;

/**
 * @struct cov_hook_cfg_t
 * @brief Shared configuration for the coverage poll-hook.
 *
 * @details
 * Written before arming the poll-hook; read-only inside the hook.
 *
 * @invariant inst < k_ra8_sdhi_instance_count when handler is armed.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t         inst; /**< SDHI instance index. */
  cov_hook_mode_t mode; /**< Handler behaviour.   */
} cov_hook_cfg_t;

/** @brief Current poll-hook configuration. */
static cov_hook_cfg_t s_cov_cfg;

/* ---------------------------------------------------------------------------
 * Poll-hook helpers
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Select the ACMD41 OCR response for the current mode.
 *
 * @param[in] mode Active hook mode.
 * @return 32-bit OCR word to load into SD_RSP10.
 *
 * @pre mode is a valid cov_hook_mode_t value.
 * @pre Called from the poll-hook, on the driver's own poll thread.
 * @post Return value is one of the k_cov_ocr_* constants.
 * @note Thread-safe: reads only static config and constants.
 *
 * @since 0.1.0
 */
static uint32_t cov_ocr_for_mode(cov_hook_mode_t mode)
{
  if (mode == k_cov_mode_sdsc_v1) {
    return (uint32_t)k_cov_ocr_sdsc_ready;
  }
  if (mode == k_cov_mode_acmd41_ocr_busy) {
    return (uint32_t)k_cov_ocr_not_ready;
  }
  return (uint32_t)k_cov_ocr_sdhc_ready;
}

/**
 * @brief Inject the CMD9 CSD response for the current mode.
 *
 * @param[in,out] reg  Pointer to the SDHI register window.
 * @param[in]     mode Active hook mode.
 *
 * @pre reg is a valid, mapped SDHI register window pointer.
 * @pre mode is a valid cov_hook_mode_t value.
 * @post SD_RSP10..76 hold the CSD words for the requested mode.
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static void cov_set_csd(volatile r_sdhi_regs_t* reg, cov_hook_mode_t mode)
{
  if (mode == k_cov_mode_sdsc_v1) {
    reg->SD_RSP10 = (uint32_t)k_cov_csd_v1_rsp10;
    reg->SD_RSP32 = (uint32_t)k_cov_csd_v1_rsp32;
    reg->SD_RSP54 = (uint32_t)k_cov_csd_v1_rsp54;
    reg->SD_RSP76 = (uint32_t)k_cov_csd_v1_rsp76;
    return;
  }
  if (mode == k_cov_mode_csd_bad_struct) {
    reg->SD_RSP10 = (uint32_t)k_cov_csd_bad_rsp10;
    reg->SD_RSP32 = (uint32_t)k_cov_csd_bad_rsp32;
    reg->SD_RSP54 = (uint32_t)k_cov_csd_bad_rsp54;
    reg->SD_RSP76 = (uint32_t)k_cov_csd_bad_rsp76;
    return;
  }
  /* Default: CSD v2 large (SDXC) */
  reg->SD_RSP10 = (uint32_t)k_cov_csd_v2_sdxc_rsp10;
  reg->SD_RSP32 = (uint32_t)k_cov_csd_v2_sdxc_rsp32;
  reg->SD_RSP54 = (uint32_t)k_cov_csd_v2_sdxc_rsp54;
  reg->SD_RSP76 = (uint32_t)k_cov_csd_v2_sdxc_rsp76;
}

/**
 * @brief Zero all four SD response-register lanes (R2-style content).
 * @param[in,out] reg SDHI register block.
 * @return None.
 * @pre @p reg is non-null.
 * @post SD_RSP10/32/54/76 are all zero.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void cov_clear_rsp(volatile r_sdhi_regs_t* reg)
{
  reg->SD_RSP10 = 0U;
  reg->SD_RSP32 = 0U;
  reg->SD_RSP54 = 0U;
  reg->SD_RSP76 = 0U;
}

/**
 * @brief Model the SDHI response for one command per the active coverage mode.
 * @param[in,out] reg SDHI register block whose response lanes are written.
 * @param[in]     cmd The command index latched in SD_CMD.
 * @return true to assert RSPEND afterwards; false for the no-RSPEND fail modes.
 * @pre @p reg is non-null.
 * @post The response lanes reflect @p cmd and the active mode.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static bool cov_dispatch_cmd(volatile r_sdhi_regs_t* reg, uint32_t cmd)
{
  switch (cmd) {
    case 0U: /* CMD0 GO_IDLE_STATE: always succeeds; no response content. */
      cov_clear_rsp(reg);
      break;

    case 8U: /* CMD8 SEND_IF_COND: echo depends on mode. */
      if (s_cov_cfg.mode == k_cov_mode_cmd8_bad_echo) {
        reg->SD_RSP10 = (uint32_t)k_cov_cmd8_bad_echo;
      } else {
        reg->SD_RSP10 = (uint32_t)k_cov_cmd8_good_echo;
      }
      reg->SD_RSP32 = 0U;
      reg->SD_RSP54 = 0U;
      reg->SD_RSP76 = 0U;
      break;

    case 55U: /* CMD55 APP_CMD: fail modes do not assert RSPEND. */
      if (s_cov_cfg.mode == k_cov_mode_fail_cmd55) {
        return false; /* no RSPEND -> driver spin-loop times out naturally */
      }
      reg->SD_RSP10 = 0U;
      break;

    case 41U: /* ACMD41 SD_SEND_OP_COND: fail / busy modes. */
      if (s_cov_cfg.mode == k_cov_mode_fail_acmd41) {
        return false; /* no RSPEND */
      }
      reg->SD_RSP10 = cov_ocr_for_mode(s_cov_cfg.mode);
      break;

    case 2U: /* CMD2 ALL_SEND_CID: R2, content not used by driver. */
      cov_clear_rsp(reg);
      break;

    case 3U: /* CMD3 SEND_RELATIVE_ADDR: RCA in rsp[0][31:16]. */
      reg->SD_RSP10 = (uint32_t)k_cov_rca_rsp10_shifted;
      break;

    case 9U: /* CMD9 SEND_CSD: 128-bit response, mode-specific. */
      cov_set_csd(reg, s_cov_cfg.mode);
      break;

    case 7U: /* CMD7 SELECT_CARD: R1, content not checked here.            */
    default: /* CMD17/CMD24 and any other command: just signal completion. */
      reg->SD_RSP10 = 0U;
      break;
  }
  return true;
}

/**
 * @brief Servicer step -- injects SDHI command responses per mode.
 *
 * @details
 * Runs inline on every ra8_sim_mmio_poll, on the driver's own poll
 * thread.  Reads SD_CMD from the register
 * window.  If the value exceeds the maximum valid SD command index (63), it is
 * the sentinel written by a prior step and the function returns immediately.
 *
 * For commands that should succeed in the current mode, it populates
 * SD_RSP10..76, asserts SD_INFO1.RSPEND (bit 0), sets SD_INFO2.{BRE,BWE}
 * (bits 9:8, required for block-transfer tests), then overwrites SD_CMD with
 * the sentinel so subsequent steps before the next driver write do nothing.
 *
 * For commands that should time out (fail modes), it returns without touching
 * SD_INFO1, causing the driver's bounded spin loop to exhaust and return
 * k_ra8_err_hw_timeout.
 *
 * @pre The coverage poll-hook is installed via cov_hook_arm().
 * @pre s_cov_cfg.inst is a valid SDHI instance index.
 * @post For success cases: SD_INFO1.RSPEND = 1, SD_INFO2.BRE/BWE = 1.
 * @post For fail cases: SD_INFO1 unchanged.
 * @note Volatile register accesses only; the served command is written by the
 *       driver, so the response tracks driver progress, not elapsed time.
 *
 * @since 0.1.0
 */
static void cov_sdcard_step(void)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(s_cov_cfg.inst);
  if (reg == nullptr) {
    return;
  }

  const uint32_t cmd = reg->SD_CMD;

  /* Sentinel: this command was already handled; wait for the next one. */
  if (cmd > (uint32_t)k_cov_sd_cmd_max_valid) {
    return;
  }

  if (!cov_dispatch_cmd(reg, cmd)) {
    return;
  }

  /* Assert RSPEND (bit 0) and BRE/BWE (bits 9:8) then write sentinel. */
  reg->SD_INFO1 = reg->SD_INFO1 | 1U;
  reg->SD_INFO2 = reg->SD_INFO2 | 0x300U;
  reg->SD_CMD   = (uint32_t)k_cov_sd_cmd_sentinel;
}

/* ---------------------------------------------------------------------------
 * Arm / disarm helpers
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Install the coverage poll-hook in @p mode.
 *
 * @details
 * ::cov_sdcard_step then runs on every ra8_sim_mmio_poll, i.e. inline on the
 * driver's own SDHI RSPEND / FIFO poll thread -- so it stuffs the per-command
 * response and asserts (or withholds) RSPEND deterministically, with no
 * concurrent servicer thread and no wall-clock timer.
 *
 * @param[in] inst SDHI instance index to operate on.
 * @param[in] mode Hook mode selecting command response behaviour.
 *
 * @pre inst < k_ra8_sdhi_instance_count.
 * @pre No poll-hook is currently installed.
 * @post ::cov_sdcard_step runs on every ra8_sim_mmio_poll for @p inst.
 * @note Not reentrant; tests are single-threaded.
 *
 * @since 0.1.0
 */
static void cov_hook_arm(uint8_t inst, cov_hook_mode_t mode)
{
  s_cov_cfg.inst = inst;
  s_cov_cfg.mode = mode;
  ra8_sim_mmio_set_poll_hook(cov_sdcard_step);
}

/**
 * @brief Remove the coverage poll-hook.
 *
 * @pre cov_hook_arm() was called previously.
 * @post No poll-hook is installed.
 *
 * @since 0.1.0
 */
static void cov_hook_disarm(void)
{
  ra8_sim_mmio_set_poll_hook(nullptr);
}

/* ---------------------------------------------------------------------------
 * Test fixture
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Reset all state before each test.
 *
 * @details
 * Zeroes every MMIO backing window, reinitialises the MSTP ref-count
 * table, and forces the sdcard driver back to the uninitialized state.
 *
 * @pre Called from test body before any HAL function.
 * @post MMIO windows are zero; sdcard driver is not initialized.
 * @note Not thread-safe; tests are single-threaded.
 *
 * @since 0.1.0
 */
static void cov_prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  (void)ra8_sdcard_deinit();
}

/* ---------------------------------------------------------------------------
 * Individual test functions
 * ---------------------------------------------------------------------------
 */

/**
 * @brief CMD8 echo-pattern mismatch causes hw_init_failed (line 293).
 *
 * @details
 * The hook returns SD_RSP10=0 for CMD8.  The driver checks
 * ``(rsp[0] & 0xFFF) != (0x1AA & 0xFFF)`` and returns
 * k_ra8_err_hw_init_failed at source line 293.
 *
 * @par MC/DC:
 * Decision: ``(rsp[0] & mask) != (pattern & mask)`` -- single condition.
 * - Vector 1 (existing test): pattern matches -> false -> init continues.
 * - Vector 2 (this test): rsp[0]=0, pattern=0x1AA -> mismatch -> true
 *   -> k_ra8_err_hw_init_failed returned.
 *
 * @since 0.1.0
 */
static void test_cov_cmd8_echo_mismatch(void)
{
  TEST_BEGIN("sdcard cov: CMD8 echo mismatch -> hw_init_failed (line 293)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_cmd8_bad_echo);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, err);
  TEST_END("sdcard cov: CMD8 echo mismatch -> hw_init_failed (line 293)");
}

/**
 * @brief CMD55 hardware timeout propagated from ACMD41 loop (lines 164, 414, 415).
 *
 * @details
 * The hook handles CMD0 (identifies the card) and CMD8 (correct echo),
 * but does NOT assert RSPEND for CMD55.  The SDHI spin budget exhausts and
 * ra8_sdhi_send_command returns k_ra8_err_hw_timeout.  internal_run_acmd41
 * propagates it at line 164.  ra8_sdcard_init then calls ra8_sdhi_deinit
 * (line 414) and returns at line 415.
 *
 * @par MC/DC:
 * Decision: ``e55 != k_ra8_ok`` -- single condition.
 * - Vector 1 (existing success tests): e55=k_ra8_ok -> false.
 * - Vector 2 (this test): e55=k_ra8_err_hw_timeout -> true -> return at line 164.
 *
 * @since 0.1.0
 */
static void test_cov_acmd41_cmd55_fails(void)
{
  TEST_BEGIN("sdcard cov: ACMD41 CMD55 timeout -> propagated error (lines 164,414,415)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_fail_cmd55);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  /* CMD0 and CMD8 succeed; CMD55 spin-loop exhausts -> hw_timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("sdcard cov: ACMD41 CMD55 timeout -> propagated error (lines 164,414,415)");
}

/**
 * @brief ACMD41 hardware timeout propagated from ACMD41 loop (lines 173, 414, 415).
 *
 * @details
 * The hook handles CMD0, CMD8 (correct echo), and CMD55, but does NOT
 * assert RSPEND for ACMD41 (cmd=41).  The spin budget exhausts and
 * internal_run_acmd41 propagates the error at line 173.
 *
 * @par MC/DC:
 * Decision: ``e41 != k_ra8_ok`` -- single condition.
 * - Vector 1 (existing success tests): e41=k_ra8_ok -> false.
 * - Vector 2 (this test): e41=k_ra8_err_hw_timeout -> true -> return at line 173.
 *
 * @since 0.1.0
 */
static void test_cov_acmd41_acmd41_fails(void)
{
  TEST_BEGIN("sdcard cov: ACMD41 ACMD41 timeout -> propagated error (lines 173,414,415)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_fail_acmd41);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  /* CMD0, CMD8, CMD55 succeed; ACMD41 spin-loop exhausts -> hw_timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("sdcard cov: ACMD41 ACMD41 timeout -> propagated error (lines 173,414,415)");
}

/**
 * @brief ACMD41 busy bit never set exhausts all retries (lines 180, 182, 414, 415).
 *
 * @details
 * The hook handles CMD0, CMD8, CMD55, and ACMD41.  ACMD41 returns
 * OCR=0 (busy bit 31 is 0) for all 1000 iterations.  Each iteration
 * the loop body falls through to line 180 (loop end) without breaking.
 * After all retries the done==0 check at line 181 is true and line 182
 * returns k_ra8_err_hw_init_failed.
 *
 * This test runs ~1000 iterations of CMD55+ACMD41, each answered
 * synchronously by the poll-hook on the driver's own poll, so the whole
 * retry loop completes in host-CPU time with no timer waits.
 *
 * @par MC/DC:
 * Decisions in internal_run_acmd41 (all single-condition):
 * - ``e55 != k_ra8_ok``: false every iteration (CMD55 succeeds).
 * - ``e41 != k_ra8_ok``: false every iteration (ACMD41 succeeds).
 * - ``(ocr & busy_mask) != 0``: false every iteration -> loop body
 *   falls to closing brace at line 180 without break.
 * - ``done == 0``: true after all retries -> line 182 executed.
 *
 * @since 0.1.0
 */
static void test_cov_acmd41_busy_exhausted(void)
{
  TEST_BEGIN("sdcard cov: ACMD41 busy never set -> 1000-retry exhaustion (lines 180,182)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_acmd41_ocr_busy);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  /* After 1000 CMD55+ACMD41 cycles with OCR busy=0: hw_init_failed. */
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, err);
  TEST_END("sdcard cov: ACMD41 busy never set -> 1000-retry exhaustion (lines 180,182)");
}

/**
 * @brief CSD v1 SDSC decode path and SDSC block-address translation
 *        (lines 230-245, 385, 467).
 *
 * @details
 * The hook injects CSD_STRUCTURE=0 (v1) with READ_BL_LEN=9,
 * C_SIZE=127, C_SIZE_MULT=1.  The OCR has CCS=0 (byte-addressed SDSC).
 *
 * After a successful init, ra8_sdcard_read_blocks is called.  The SDSC
 * branch of internal_to_card_address returns lba * 512 (line 467).
 * The SDHI block-read completes because the hook keeps BRE asserted.
 *
 * @par MC/DC:
 * Decisions in internal_decode_csd:
 * - ``csd_structure == 1U``: false (v1 card) -> fall to next check.
 * - ``csd_structure == 0U``: true -> execute CSD v1 body (lines 234-245).
 * Decision in internal_classify:
 * - ``high_capacity == 0U``: true (CCS=0) -> line 385 returned.
 * Decision in internal_to_card_address:
 * - ``s_sdcard.type == k_ra8_sdcard_type_sdsc``: true -> line 467.
 *
 * @since 0.1.0
 */
static void test_cov_csd_v1_sdsc_card(void)
{
  TEST_BEGIN("sdcard cov: CSD v1 SDSC decode + byte-address translation (lines 230-245,385,467)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_sdsc_v1);
  const ra8_sdcard_cfg_t cfg      = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        init_err = ra8_sdcard_init(&cfg);
  TEST_ASSERT_EQ(k_ra8_ok, init_err);

  /* Verify capacity and type decoded correctly from CSD v1 response. */
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_capacity(&cap));
  TEST_ASSERT_EQ(k_cov_csd_v1_expected_blocks, cap);

  ra8_sdcard_card_type_t t = k_ra8_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_type(&t));
  TEST_ASSERT_EQ(k_ra8_sdcard_type_sdsc, t);

  /* Trigger line 467: internal_to_card_address(0) returns 0*512=0.
   * The hook is still armed so the single-block read succeeds (BRE set). */
  uint8_t buf[512] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_read_blocks(0U, buf, 1U));

  cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard cov: CSD v1 SDSC decode + byte-address translation (lines 230-245,385,467)");
}

/**
 * @brief CSD v2 with SDXC-range capacity triggers SDXC classification (line 388).
 *
 * @details
 * The hook injects C_SIZE=65536 (22-bit field).  Decoded capacity:
 * (65536+1) * 1024 = 67,109,888 blocks, which exceeds the 32-GiB
 * SDHC/SDXC boundary (67,108,864) and yields k_ra8_sdcard_type_sdxc.
 *
 * @par MC/DC:
 * Decision in internal_classify:
 * - ``high_capacity == 0U``: false (CCS=1) -> not SDSC.
 * - ``blocks > threshold * 16``: true (67,109,888 > 67,108,864) -> line 388.
 *
 * @since 0.1.0
 */
static void test_cov_csd_v2_sdxc_card(void)
{
  TEST_BEGIN("sdcard cov: CSD v2 SDXC large capacity classification (line 388)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_sdxc_v2);
  const ra8_sdcard_cfg_t cfg      = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        init_err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, init_err);

  ra8_sdcard_card_type_t t = k_ra8_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_type(&t));
  TEST_ASSERT_EQ(k_ra8_sdcard_type_sdxc, t);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard cov: CSD v2 SDXC large capacity classification (line 388)");
}

/**
 * @brief ra8_sdcard_deinit surfaces an SDHI release failure as invalid_state.
 *
 * @details
 * Initialises a CSD v2 card exactly like the SDXC case, then arms the
 * ra8_sim_mmio fault seam on MSTPCRC (SDHI0 gates on MSTPC12) so the
 * ra8_mstp_disable readback inside ra8_sdhi_deinit never settles.
 * ra8_sdcard_deinit must translate that hw_timeout into
 * k_ra8_err_invalid_state -- the leg that was host-dead while the MSTP
 * readback always matched RAM-backed registers on the first poll. The
 * card state is torn down before the release is attempted, so a
 * follow-up capacity query also reports invalid_state.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the ``err != k_ra8_ok`` guard
 * in ra8_sdcard_deinit is a single-condition ``if`` driven by the armed
 * MSTP readback fault)
 *
 * @since 0.1.0
 */
static void test_cov_deinit_mstp_timeout(void)
{
  TEST_BEGIN("sdcard cov: deinit surfaces the SDHI MSTP release failure");
  cov_prep();
  ra8_sim_mmio_reset();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_sdxc_v2);
  const ra8_sdcard_cfg_t cfg      = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        init_err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, init_err);

  /* SDHI0 is gated by MSTPCRC bit 12: fail that register's readback so
   * the disable inside ra8_sdhi_deinit times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait(&ra8_mstp()->MSTPCRC));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_deinit());
  ra8_sim_mmio_reset();

  /* The card-facing state was torn down before the release failed. */
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_get_capacity(&cap));

  TEST_END("sdcard cov: deinit surfaces the SDHI MSTP release failure");
}

/**
 * @brief Unknown CSD_STRUCTURE triggers failure path through publish-and-select
 *        (lines 247, 346, 423, 424).
 *
 * @details
 * The hook injects rsp[3] = 0x80000000 (CSD_STRUCTURE = 2).
 * internal_decode_csd returns k_ra8_err_hw_init_failed at line 247.
 * internal_sdcard_publish_and_select returns it at line 346.
 * ra8_sdcard_init calls ra8_sdhi_deinit at line 423 and returns at line 424.
 *
 * @par MC/DC:
 * Decisions in internal_decode_csd:
 * - ``csd_structure == 1U``: false (value is 2).
 * - ``csd_structure == 0U``: false (value is 2) -> line 247 executed.
 *
 * @since 0.1.0
 */
static void test_cov_csd_unknown_structure(void)
{
  TEST_BEGIN("sdcard cov: CSD_STRUCTURE=2 -> hw_init_failed through publish-select "
             "(lines 247,346,423,424)");
  cov_prep();
  cov_hook_arm((uint8_t)k_cov_test_inst, k_cov_mode_csd_bad_struct);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, err);
  /* Driver must not be left in an initialised state. */
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_get_capacity(&cap));
  TEST_END("sdcard cov: CSD_STRUCTURE=2 -> hw_init_failed through publish-select "
           "(lines 247,346,423,424)");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Entry point -- runs all coverage-extension tests sequentially.
 *
 * @return 0 on success (each TEST_ASSERT calls exit(1) on failure).
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_cov_cmd8_echo_mismatch();
  test_cov_acmd41_cmd55_fails();
  test_cov_acmd41_acmd41_fails();
  test_cov_acmd41_busy_exhausted();
  test_cov_csd_v1_sdsc_card();
  test_cov_csd_v2_sdxc_card();
  test_cov_deinit_mstp_timeout();
  test_cov_csd_unknown_structure();
  (void)fprintf(stderr, "[OK  ] test_ra8_sdcard_cov.c\n");
  return 0;
}
