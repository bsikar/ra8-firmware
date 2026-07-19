/**
 * @file test_ra8_sdcard.c
 * @brief Unit tests for ra8_sdcard.c (SD card high-level driver)
 *
 * @details
 * The SDHI hardware is faked via tests/mocks/ra8_sim_mmap.c -- writes
 * to SD_CMD / SD_ARG land in plain RAM, and the polled RSPEND flag in
 * SD_INFO1 is asserted by the ra8_sim_mmio poll-hook (no timer, no thread).
 *
 * The interesting bit is that ::ra8_sdcard_init issues a sequence of
 * SD commands (CMD0, CMD8, CMD55, ACMD41, CMD2, CMD3, CMD9, CMD7)
 * and reads a different response shape for each one. The poll-hook
 * (installed via ra8_sim_mmio_set_poll_hook) inspects the driver's
 * most-recently-written SD_CMD register, stuffs
 * the matching response into SD_RSP10..SD_RSP76, asserts RSPEND, then
 * overwrites SD_CMD with a sentinel (> 63) so each command is served
 * exactly once; the next real SD_CMD write re-arms it. The response is
 * driven by the driver's own command write, never by elapsed time.
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

/**
 * @enum sdcard_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_sdcard_sentinel_55 = 55U,
  k_sdcard_val_41      = 41U,
  k_sdcard_val_9       = 9U,
} sdcard_uint8_const_t;

/**
 * @enum sdcard_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_sdcard_val_512 = 512,
} sdcard_uint16_const_t;

typedef enum : uint8_t {
  k_ra8_sdcard_test_inst     = 0U, /**< RA8 sdcard test inst.     */
  k_ra8_sdcard_test_inst_alt = 1U, /**< RA8 sdcard test inst alt. */
} ra8_sdcard_test_inst_t;

typedef enum : uint32_t {
  k_ra8_sdcard_test_cmd8_echo = 0x000001AAUL, /**< CMD8 echo pattern            */
  k_ra8_sdcard_test_ocr_ready = 0xC0FF8000UL, /**< busy=1 | CCS=1 | voltage win */
  k_ra8_sdcard_test_rca       = 0xABCDU,      /**< Card-published RCA           */
  /* CSD v2: CSD_STRUCTURE=1 in rsp[3][31:30], C_SIZE=0xF000 (61440)
   * lives in rsp[1][31:16] = 0xF0000000 plus rsp[2][5:0] = 0.
   * Capacity = (61440 + 1) * 1024 = 62,915,584 blocks ~ 32 GiB. */
  k_ra8_sdcard_test_csd_w0 = 0x00000000UL, /**< RA8 sdcard test csd w0.                   */
  k_ra8_sdcard_test_csd_w1 = 0xF0000000UL, /**< C_SIZE bits [29:8] of rsp[1] -> upper 16b */
  k_ra8_sdcard_test_csd_w2 = 0x00000000UL, /**< RA8 sdcard test csd w2.                   */
  k_ra8_sdcard_test_csd_w3 = 0x40000000UL, /**< CSD_STRUCTURE = 1                         */
  k_ra8_sdcard_test_expected_blocks = (0xF000UL + 1UL) * 1024UL, /**< (C_SIZE+1)*1024 */
} ra8_sdcard_test_const_t;

/** @brief When non-zero the CMD55 mock withholds APP_CMD, forcing a 4-bit decline. */
static uint8_t s_decline_4bit;

/* Deterministic SD command servicing via the ra8_sim_mmio poll-hook -- no
 * wall-clock timer, no servicer thread. The hook decodes each command from the
 * driver's SD_CMD write on its OWN poll thread rather than off a timer, and
 * gates each response behind a sentinel so a command is served exactly once:
 * after responding it writes SD_CMD = k_sdcard_srv_cmd_done (> the max valid SD
 * command index), so it does nothing until ra8_sdhi_send_command writes the next
 * real command. It also holds SD_INFO2.BRE/BWE asserted every poll for the block
 * read/write FIFO drains. */

/** @brief Servicer constants. */
typedef enum : uint32_t {
  k_sdcard_srv_rspend   = 0x00000001UL, /**< SD_INFO1.RSPEND (bit 0).          */
  k_sdcard_srv_brebwe   = 0x00000300UL, /**< SD_INFO2.BRE | BWE (bits 9:8).    */
  k_sdcard_srv_cmd_max  = 63UL,         /**< Highest valid SD command index.   */
  k_sdcard_srv_cmd_done = 0xFFFFFFFFUL, /**< Sentinel: command already served. */
} sdcard_srv_const_t;

/** @brief SDHI instance the poll-hook operates on. */
static uint8_t s_srv_inst;

/**
 * @brief Load SD_RSP10..76 with the per-command response the driver expects.
 *
 * @param[in,out] reg Mapped SDHI register window.
 * @param[in]     cmd SD command index just written by the driver.
 */
static void sdcard_srv_respond(volatile r_sdhi_regs_t* reg, uint32_t cmd)
{
  switch (cmd) {
    case 8U: /* CMD8 R7: echoes the low 12 bits of the argument. */
      reg->SD_RSP10 = (uint32_t)k_ra8_sdcard_test_cmd8_echo;
      reg->SD_RSP32 = 0U;
      reg->SD_RSP54 = 0U;
      reg->SD_RSP76 = 0U;
      break;
    case k_sdcard_val_41: /* ACMD41 R3: OCR with busy-done + CCS bits set. */
      reg->SD_RSP10 = (uint32_t)k_ra8_sdcard_test_ocr_ready;
      break;
    case 3U: /* CMD3 R6: RCA in upper 16 bits. */
      reg->SD_RSP10 = ((uint32_t)k_ra8_sdcard_test_rca) << 16U;
      break;
    case k_sdcard_val_9: /* CMD9 R2: 128-bit CSD; rsp[3][31:30] is CSD_STRUCTURE. */
      reg->SD_RSP10 = (uint32_t)k_ra8_sdcard_test_csd_w0;
      reg->SD_RSP32 = (uint32_t)k_ra8_sdcard_test_csd_w1;
      reg->SD_RSP54 = (uint32_t)k_ra8_sdcard_test_csd_w2;
      reg->SD_RSP76 = (uint32_t)k_ra8_sdcard_test_csd_w3;
      break;
    case 6U: /* ACMD6 SET_BUS_WIDTH R1: clean (no error bits). */
      reg->SD_RSP10 = 0U;
      break;
    case k_sdcard_sentinel_55: /* CMD55 APP_CMD R1: echo APP_CMD unless a test forces a decline. */
      reg->SD_RSP10 = (s_decline_4bit != 0U) ? 0U : (uint32_t)k_ra8_sdhi_r1_app_cmd_mask;
      break;
    default: /* CMD0/2/7 and block CMD17/24: response content unused. */
      reg->SD_RSP10 = 0U;
      break;
  }
}

/**
 * @brief Poll-hook body: respond to each SD command exactly once.
 *
 * @details
 * Runs inline on the driver's own SDHI RSPEND / FIFO poll (see
 * ra8_sim_mmio_set_poll_hook). It holds SD_INFO2.BRE/BWE asserted for the block
 * FIFO drains, then -- when SD_CMD holds a real command (not the served
 * sentinel) -- stuffs the per-command response, asserts RSPEND, and rewrites
 * SD_CMD to the sentinel so each command is answered exactly once. Because it
 * fires on the driver's own poll, the handshake is deterministic on any host
 * with no concurrent thread and no wall-clock timer.
 */
static void sdcard_hook(void)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(s_srv_inst);
  if (reg == nullptr) {
    return;
  }
  reg->SD_INFO2      = reg->SD_INFO2 | (uint32_t)k_sdcard_srv_brebwe;
  const uint32_t cmd = reg->SD_CMD;
  if (cmd <= (uint32_t)k_sdcard_srv_cmd_max) {
    sdcard_srv_respond(reg, cmd);
    reg->SD_INFO1 = reg->SD_INFO1 | (uint32_t)k_sdcard_srv_rspend;
    reg->SD_CMD   = (uint32_t)k_sdcard_srv_cmd_done;
  }
}

/**
 * @brief Install the SD command poll-hook for @p inst.
 *
 * @param[in] inst SDHI instance index to service.
 */
static void sdcard_hook_arm(uint8_t inst)
{
  s_srv_inst = inst;
  ra8_sim_mmio_set_poll_hook(sdcard_hook);
}

/**
 * @brief Remove the SD command poll-hook.
 */
static void sdcard_hook_disarm(void)
{
  ra8_sim_mmio_set_poll_hook(nullptr);
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  s_decline_4bit = 0U;
  /* Force the driver back to clean state in case a prior test left
   * it initialized. */
  (void)ra8_sdcard_deinit();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("sdcard init: null cfg rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_init(nullptr));
  TEST_END("sdcard init: null cfg rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_full_sequence(void)
{
  TEST_BEGIN("sdcard init: full SD init sequence succeeds");
  prep();

  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra8_sdcard_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  sdcard_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);

  uint32_t blocks = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_capacity(&blocks));
  TEST_ASSERT_EQ(k_ra8_sdcard_test_expected_blocks, blocks);

  ra8_sdcard_card_type_t type = k_ra8_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_type(&type));
  TEST_ASSERT((type == k_ra8_sdcard_type_sdhc) || (type == k_ra8_sdcard_type_sdxc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard init: full SD init sequence succeeds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_double_call_rejected(void)
{
  TEST_BEGIN("sdcard init: double init returns invalid_state");
  prep();
  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra8_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_init(&cfg));
  sdcard_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard init: double init returns invalid_state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_instance(void)
{
  TEST_BEGIN("sdcard init: bad instance");
  prep();
  const ra8_sdcard_cfg_t cfg = {.instance = 9U};
  /* SDHI init rejects out-of-range instances with k_ra8_err_null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_init(&cfg));
  TEST_END("sdcard init: bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_no_rspend_times_out(void)
{
  TEST_BEGIN("sdcard init: no RSPEND -> SDHI propagates timeout");
  prep();
  /* No poll-hook installed, so SD_INFO1.RSPEND never asserts and the SDHI
   * send_command spin budget eventually exits with hw_timeout. Proves
   * the driver propagates underlying SDHI failures rather than
   * masking them. Spin budget is bounded so this returns in ~2 ms. */
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra8_sdcard_test_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("sdcard init: no RSPEND -> SDHI propagates timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_io_before_init_rejected(void)
{
  TEST_BEGIN("sdcard read/write before init rejected");
  prep();
  uint8_t  buf[k_sdcard_val_512] = {};
  uint32_t blocks                = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_read_blocks(0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_write_blocks(0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_get_capacity(&blocks));
  ra8_sdcard_card_type_t type = k_ra8_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdcard_get_type(&type));
  TEST_END("sdcard read/write before init rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_io_null_args(void)
{
  TEST_BEGIN("sdcard read/write null args rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_read_blocks(0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_write_blocks(0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_get_capacity(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdcard_get_type(nullptr));
  TEST_END("sdcard read/write null args rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_io_zero_count(void)
{
  TEST_BEGIN("sdcard read/write count=0 rejected");
  prep();
  uint8_t buf[1];
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdcard_read_blocks(0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdcard_write_blocks(0U, buf, 0U));
  TEST_END("sdcard read/write count=0 rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_io_after_init(void)
{
  TEST_BEGIN("sdcard read/write after init: pass through to SDHI");
  prep();
  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra8_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_init(&cfg));

  /* Single block read at LBA 0. SDHI mock returns whatever SD_BUF0
   * holds; we don't care about content here, only that the call
   * returns ok. */
  uint8_t         buf[k_sdcard_val_512] = {};
  const ra8_err_t r                     = ra8_sdcard_read_blocks(0U, buf, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, r);

  /* Single block write. */
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t)i;
  }
  const ra8_err_t w = ra8_sdcard_write_blocks(1U, buf, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, w);

  sdcard_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard read/write after init: pass through to SDHI");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_io_out_of_range(void)
{
  TEST_BEGIN("sdcard read/write past capacity rejected");
  prep();
  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra8_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_init(&cfg));
  sdcard_hook_disarm();

  uint8_t buf[k_sdcard_val_512] = {};
  /* Capacity is k_ra8_sdcard_test_expected_blocks. Read starting at
   * the very last block + a span of 2 should overflow. */
  const uint32_t lba = (uint32_t)k_ra8_sdcard_test_expected_blocks - 1U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdcard_read_blocks(lba, buf, 2U));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdcard_write_blocks(lba, buf, 2U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard read/write past capacity rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_idempotent(void)
{
  TEST_BEGIN("sdcard deinit before init is a no-op");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard deinit before init is a no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the ra8_sdcard 4-bit
 * config-knob integration path; the ACMD6 compound decision's MC/DC
 * vectors live in test_ra8_sdhi_width.c)
 */
static void test_init_4bit_widens(void)
{
  TEST_BEGIN("sdcard init: 4-bit config knob widens host via ACMD6");
  prep();
  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance  = (uint8_t)k_ra8_sdcard_test_inst,
                                .bus_width = k_ra8_sdhi_bus_width_4bit};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  sdcard_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  /* Host SD_OPTION switched to the 4-bit encoding. */
  TEST_ASSERT_EQ(k_ra8_sdhi_option_bus_4bit, ra8_sdhi((uint8_t)k_ra8_sdcard_test_inst)->SD_OPTION);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard init: 4-bit config knob widens host via ACMD6");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives ra8_sdcard's best-effort
 * fallback when the card declines 4-bit; the helper's guard is a single
 * condition `if (we != k_ra8_ok)`, not a compound decision)
 */
static void test_init_4bit_declined_stays_1bit(void)
{
  TEST_BEGIN("sdcard init: declined 4-bit falls back to 1-bit, init ok");
  prep();
  s_decline_4bit = 1U;
  sdcard_hook_arm((uint8_t)k_ra8_sdcard_test_inst);
  const ra8_sdcard_cfg_t cfg = {.instance  = (uint8_t)k_ra8_sdcard_test_inst,
                                .bus_width = k_ra8_sdhi_bus_width_4bit};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  sdcard_hook_disarm();
  /* Best-effort: init still succeeds even though the card declined. */
  TEST_ASSERT_EQ(k_ra8_ok, err);
  /* Host left at the 1-bit default (SD_OPTION unchanged). */
  TEST_ASSERT_EQ(k_ra8_sdhi_option_bus_1bit, ra8_sdhi((uint8_t)k_ra8_sdcard_test_inst)->SD_OPTION);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdcard init: declined 4-bit falls back to 1-bit, init ok");
}

int32_t main(void)
{
  test_init_null_cfg();
  test_deinit_idempotent();
  test_init_full_sequence();
  test_init_double_call_rejected();
  test_init_bad_instance();
  test_init_no_rspend_times_out();
  test_io_before_init_rejected();
  test_io_null_args();
  test_io_zero_count();
  test_io_after_init();
  test_io_out_of_range();
  test_init_4bit_widens();
  test_init_4bit_declined_stays_1bit();
  (void)fprintf(stderr, "[OK  ] test_ra8_sdcard.c\n");
  return 0;
}
