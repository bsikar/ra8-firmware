/**
 * @file test_ra_sdcard.c
 * @brief Unit tests for ra_sdcard.c (SD card high-level driver)
 *
 * @details
 * The SDHI hardware is faked via tests/mocks/ra_sim_mmap.c -- writes
 * to SD_CMD / SD_ARG land in plain RAM, and the polled RSPEND flag in
 * SD_INFO1 is asserted asynchronously by a SIGALRM handler.
 *
 * The interesting bit is that ::ra_sdcard_init issues a sequence of
 * SD commands (CMD0, CMD8, CMD55, ACMD41, CMD2, CMD3, CMD9, CMD7)
 * and reads a different response shape for each one. The alarm
 * handler below inspects the most-recently-written SD_CMD register
 * and stuffs the matching response into SD_RSP10..SD_RSP76 just
 * before re-asserting RSPEND, so the driver decodes the right value
 * for each step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <string.h>
#include <sys/time.h>

#include "ra8d2_sdhi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sdcard.h"
#include "ra_sdhi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_sdcard_test_inst     = 0U,
  k_ra_sdcard_test_inst_alt = 1U,
} ra_sdcard_test_inst_t;

typedef enum : uint32_t {
  k_ra_sdcard_test_cmd8_echo = 0x000001AAUL, /**< CMD8 echo pattern */
  k_ra_sdcard_test_ocr_ready = 0xC0FF8000UL, /**< busy=1 | CCS=1 | voltage win */
  k_ra_sdcard_test_rca       = 0xABCDU,      /**< Card-published RCA */
  /* CSD v2: CSD_STRUCTURE=1 in rsp[3][31:30], C_SIZE=0xF000 (61440)
   * lives in rsp[1][31:16] = 0xF0000000 plus rsp[2][5:0] = 0.
   * Capacity = (61440 + 1) * 1024 = 62,915,584 blocks ~ 32 GiB. */
  k_ra_sdcard_test_csd_w0          = 0x00000000UL,
  k_ra_sdcard_test_csd_w1          = 0xF0000000UL, /**< C_SIZE bits [29:8] of rsp[1] -> upper 16b */
  k_ra_sdcard_test_csd_w2          = 0x00000000UL,
  k_ra_sdcard_test_csd_w3          = 0x40000000UL,              /**< CSD_STRUCTURE = 1 */
  k_ra_sdcard_test_expected_blocks = (0xF000UL + 1UL) * 1024UL, /**< (C_SIZE+1)*1024 */
} ra_sdcard_test_const_t;

static uint8_t s_alarm_inst;

/**
 * @brief SIGALRM handler that simulates RSPEND + per-command response.
 *
 * @details
 * Reads SD_CMD (last command issued) and stuffs the appropriate
 * response into SD_RSP10..SD_RSP76 based on command index. Then
 * asserts RSPEND so ra_sdhi_send_command's polled wait exits.
 */
static void sdcard_alarm_handler(int sig)
{
  (void)sig;
  volatile r_sdhi_regs_t* reg = ra_sdhi(s_alarm_inst);
  if (reg == nullptr) {
    return;
  }
  const uint32_t cmd = reg->SD_CMD;
  switch (cmd) {
    case 8U:
      /* CMD8 R7: echoes the low 12 bits of the argument. */
      reg->SD_RSP10 = (uint32_t)k_ra_sdcard_test_cmd8_echo;
      reg->SD_RSP32 = 0U;
      reg->SD_RSP54 = 0U;
      reg->SD_RSP76 = 0U;
      break;
    case 41U:
      /* ACMD41 R3: OCR with busy-done + CCS bits set. */
      reg->SD_RSP10 = (uint32_t)k_ra_sdcard_test_ocr_ready;
      break;
    case 3U:
      /* CMD3 R6: RCA in upper 16 bits. */
      reg->SD_RSP10 = ((uint32_t)k_ra_sdcard_test_rca) << 16U;
      break;
    case 9U:
      /* CMD9 R2: 128-bit CSD; SDHI shifts so rsp[3][31:30] is
       * CSD_STRUCTURE. */
      reg->SD_RSP10 = (uint32_t)k_ra_sdcard_test_csd_w0;
      reg->SD_RSP32 = (uint32_t)k_ra_sdcard_test_csd_w1;
      reg->SD_RSP54 = (uint32_t)k_ra_sdcard_test_csd_w2;
      reg->SD_RSP76 = (uint32_t)k_ra_sdcard_test_csd_w3;
      break;
    default:
      /* CMD0, CMD2, CMD7, CMD55 -- response content is not consumed
       * by the driver, just clear it so a stale value from a prior
       * command does not leak in. */
      reg->SD_RSP10 = 0U;
      break;
  }
  reg->SD_INFO1 = reg->SD_INFO1 | 1UL;
  reg->SD_INFO2 = reg->SD_INFO2 | 0x300UL;
}

static void arm_alarm(uint8_t inst)
{
  s_alarm_inst = inst;
  struct sigaction sa;
  sa.sa_handler = sdcard_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = 50;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = 50;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  /* Force the driver back to clean state in case a prior test left
   * it initialised. */
  (void)ra_sdcard_deinit();
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
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_init(nullptr));
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

  arm_alarm((uint8_t)k_ra_sdcard_test_inst);
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra_sdcard_test_inst};
  const ra_err_t        err = ra_sdcard_init(&cfg);
  disarm_alarm();
  TEST_ASSERT_EQ(k_ra_ok, err);

  uint32_t blocks = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_get_capacity(&blocks));
  TEST_ASSERT_EQ(k_ra_sdcard_test_expected_blocks, blocks);

  ra_sdcard_card_type_t type = k_ra_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_get_type(&type));
  TEST_ASSERT((type == k_ra_sdcard_type_sdhc) || (type == k_ra_sdcard_type_sdxc));

  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
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
  arm_alarm((uint8_t)k_ra_sdcard_test_inst);
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sdcard_init(&cfg));
  disarm_alarm();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
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
  const ra_sdcard_cfg_t cfg = {.instance = 9U};
  /* SDHI init rejects out-of-range instances with k_ra_err_null_ptr. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_init(&cfg));
  TEST_END("sdcard init: bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_no_alarm_times_out(void)
{
  TEST_BEGIN("sdcard init: no RSPEND -> SDHI propagates timeout");
  prep();
  /* No alarm armed, so SD_INFO1.RSPEND never asserts and the SDHI
   * send_command spin budget eventually exits with hw_timeout. Proves
   * the driver propagates underlying SDHI failures rather than
   * masking them. Spin budget is bounded so this returns in ~2 ms. */
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra_sdcard_test_inst};
  const ra_err_t        err = ra_sdcard_init(&cfg);
  TEST_ASSERT(err != k_ra_ok);
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
  uint8_t  buf[512] = {};
  uint32_t blocks   = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sdcard_read_blocks(0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sdcard_write_blocks(0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sdcard_get_capacity(&blocks));
  ra_sdcard_card_type_t type = k_ra_sdcard_type_unknown;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sdcard_get_type(&type));
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
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_read_blocks(0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_write_blocks(0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_get_capacity(nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdcard_get_type(nullptr));
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
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdcard_read_blocks(0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdcard_write_blocks(0U, buf, 0U));
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
  arm_alarm((uint8_t)k_ra_sdcard_test_inst);
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_init(&cfg));

  /* Single block read at LBA 0. SDHI mock returns whatever SD_BUF0
   * holds; we don't care about content here, only that the call
   * returns ok. */
  uint8_t        buf[512] = {};
  const ra_err_t r        = ra_sdcard_read_blocks(0U, buf, 1U);
  TEST_ASSERT_EQ(k_ra_ok, r);

  /* Single block write. */
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t)i;
  }
  const ra_err_t w = ra_sdcard_write_blocks(1U, buf, 1U);
  TEST_ASSERT_EQ(k_ra_ok, w);

  disarm_alarm();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
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
  arm_alarm((uint8_t)k_ra_sdcard_test_inst);
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_ra_sdcard_test_inst};
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_init(&cfg));
  disarm_alarm();

  uint8_t buf[512] = {};
  /* Capacity is k_ra_sdcard_test_expected_blocks. Read starting at
   * the very last block + a span of 2 should overflow. */
  const uint32_t lba = (uint32_t)k_ra_sdcard_test_expected_blocks - 1U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_sdcard_read_blocks(lba, buf, 2U));
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_sdcard_write_blocks(lba, buf, 2U));

  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
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
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
  TEST_ASSERT_EQ(k_ra_ok, ra_sdcard_deinit());
  TEST_END("sdcard deinit before init is a no-op");
}

int32_t main(void)
{
  test_init_null_cfg();
  test_deinit_idempotent();
  test_init_full_sequence();
  test_init_double_call_rejected();
  test_init_bad_instance();
  test_init_no_alarm_times_out();
  test_io_before_init_rejected();
  test_io_null_args();
  test_io_zero_count();
  test_io_after_init();
  test_io_out_of_range();
  (void)fprintf(stderr, "[OK  ] test_ra_sdcard.c\n");
  return 0;
}
