/**
 * @file test_ra_sdhi.c
 * @brief Unit tests for ra_sdhi.c (SD/MMC Host Interface scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <sys/time.h>

#include "ra8d2_sdhi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sdhi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint8_t s_sdhi_alarm_inst;

static void sdhi_sigalarm_handler(int sig)
{
  (void)sig;
  /* Fake hardware: set RSPEND so the bounded poll in send_command
   * exits on the next iteration. */
  volatile r_sdhi_regs_t* reg = ra_sdhi(s_sdhi_alarm_inst);
  if (reg != nullptr) {
    reg->SD_INFO1 = reg->SD_INFO1 | 1UL;
  }
}

static void arm_rspend_alarm(uint8_t inst)
{
  s_sdhi_alarm_inst = inst;
  struct sigaction sa;
  sa.sa_handler = sdhi_sigalarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = 100;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = 0;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_sdhi_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

typedef enum : uint8_t {
  k_ra_sdhi_test_inst_0   = 0U,
  k_ra_sdhi_test_inst_1   = 1U,
  k_ra_sdhi_test_inst_bad = 9U,
} ra_sdhi_test_inst_t;

static uint32_t s_sdhi_cb_count;
static uint32_t s_sdhi_cb_last_mask;
static uint8_t  s_sdhi_cb_last_inst;

static void stub_sdhi_cb(void* ctx, uint8_t inst, uint32_t mask)
{
  (void)ctx;
  ++s_sdhi_cb_count;
  s_sdhi_cb_last_mask = mask;
  s_sdhi_cb_last_inst = inst;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_sdhi_cb_count     = 0U;
  s_sdhi_cb_last_mask = 0U;
  s_sdhi_cb_last_inst = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("sdhi init happy");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_1));
  TEST_END("sdhi init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad(void)
{
  TEST_BEGIN("sdhi init bad");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi init bad");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("sdhi deinit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_deinit((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_deinit((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("sdhi status read + clear");
  prep();

  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0)->SD_INFO1 = 0xCAFEBABEUL;
  uint32_t mask                                     = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_get_status((uint8_t)k_ra_sdhi_test_inst_0, &mask));
  TEST_ASSERT_EQ(0xCAFEBABEU, mask);

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_clear_status((uint8_t)k_ra_sdhi_test_inst_0, 0x000000F0UL));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_get_status((uint8_t)k_ra_sdhi_test_inst_0, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_clear_status((uint8_t)k_ra_sdhi_test_inst_bad, 0U));
  TEST_END("sdhi status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("sdhi attach + dispatch");
  prep();

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_attach_handler(stub_sdhi_cb, (void*)(uintptr_t)0x5DU));
  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_1)->SD_INFO1 = 0xDEADBEEFUL;
  ra_sdhi_dispatch((uint8_t)k_ra_sdhi_test_inst_1);
  TEST_ASSERT_EQ(1, s_sdhi_cb_count);
  TEST_ASSERT_EQ(0xDEADBEEFU, s_sdhi_cb_last_mask);
  TEST_ASSERT_EQ(k_ra_sdhi_test_inst_1, s_sdhi_cb_last_inst);

  ra_sdhi_dispatch((uint8_t)k_ra_sdhi_test_inst_bad);
  TEST_ASSERT_EQ(1, s_sdhi_cb_count);
  TEST_END("sdhi attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("sdhi power transition");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_enter_stop((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_exit_stop((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdhi_enter_stop((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdhi_exit_stop((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_rspend_via_alarm(void)
{
  TEST_BEGIN("sdhi send_command: RSPEND via alarm");
  prep();

  /* Pre-seed response regs. */
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  reg->SD_RSP10               = 0x11111111UL;
  reg->SD_RSP32               = 0x22222222UL;
  reg->SD_RSP54               = 0x33333333UL;
  reg->SD_RSP76               = 0x44444444UL;

  arm_rspend_alarm((uint8_t)k_ra_sdhi_test_inst_0);
  uint32_t       rsp[4] = {0U, 0U, 0U, 0U};
  const ra_err_t err =
    ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_0, 0x0000ABCDU, 0xDEADBEEFUL, rsp);
  disarm_sdhi_alarm();

  TEST_ASSERT_EQ(k_ra_ok, err);
  TEST_ASSERT_EQ(0x11111111, rsp[0]);
  TEST_ASSERT_EQ(0x22222222, rsp[1]);
  TEST_ASSERT_EQ(0x33333333, rsp[2]);
  TEST_ASSERT_EQ(0x44444444, rsp[3]);
  TEST_ASSERT_EQ(0xDEADBEEFUL, reg->SD_ARG);
  TEST_ASSERT_EQ(0x0000ABCDU, reg->SD_CMD);
  TEST_END("sdhi send_command: RSPEND via alarm");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_no_rsp_buffer(void)
{
  TEST_BEGIN("sdhi send_command: null response buffer");
  prep();

  arm_rspend_alarm((uint8_t)k_ra_sdhi_test_inst_1);
  const ra_err_t err = ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_1, 0x01U, 0U, nullptr);
  disarm_sdhi_alarm();
  TEST_ASSERT_EQ(k_ra_ok, err);
  TEST_END("sdhi send_command: null response buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_bad_instance(void)
{
  TEST_BEGIN("sdhi send_command: bad instance");
  prep();
  uint32_t rsp[4] = {0U};
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_bad, 0U, 0U, rsp));
  TEST_END("sdhi send_command: bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_clock(void)
{
  TEST_BEGIN("sdhi set_clock");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_clock((uint8_t)k_ra_sdhi_test_inst_0, 0x0080U));
  TEST_ASSERT_EQ(0x0080U, ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0)->SD_CLK_CTRL);
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_set_clock((uint8_t)k_ra_sdhi_test_inst_bad, 0U));
  TEST_END("sdhi set_clock");
}

/**
 * @brief Lab pattern stamped into SD_BUF0 for the read_block test.
 *
 * @details
 * The simulator backs SD_BUF0 with plain RAM, so reading the
 * register N times in a row returns whatever value was last
 * written. The driver's polled FIFO drain copies SD_BUF0 unchanged
 * into the destination buffer in little-endian order; with a
 * single primed value the entire 512-byte block winds up filled
 * with the same word, which is exactly what we assert below.
 */
typedef enum : uint32_t {
  k_ra_sdhi_test_pattern   = 0xA5B6C7D8UL,
  k_ra_sdhi_test_lba       = 0x00001234UL,
  k_ra_sdhi_test_multi_lba = 0x00005678UL,
} ra_sdhi_test_const_t;

/**
 * @brief Pre-set SD_INFO2 BRE/BWE flags + RSPEND + a SD_BUF0 word.
 *
 * @details
 * The polled block-transfer driver checks SD_INFO1.RSPEND after
 * each command issue and SD_INFO2.BRE/BWE before each FIFO word.
 * Because the simulator backing store is plain RAM, asserting the
 * flags once is enough -- they stay set for the full duration of
 * the transfer.
 */
static void prime_block_xfer_flags(uint8_t inst, uint32_t buf_word)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(inst);
  /* RSPEND + BRE + BWE all asserted ahead of time. The driver
   * clears RSPEND inline (read-modify-write) and zeroes SD_INFO2
   * after the loop -- both are no-ops for the drain itself. */
  reg->SD_INFO1 = 1UL;
  reg->SD_INFO2 = 0x300UL;
  reg->SD_BUF0  = buf_word;
}

static uint8_t s_sdhi_periodic_inst;

static void sdhi_periodic_handler(int sig)
{
  (void)sig;
  /* Re-assert RSPEND + BRE + BWE so subsequent CMD sends and FIFO
   * polls keep finding the flags set even after the driver clears
   * them inline. */
  volatile r_sdhi_regs_t* reg = ra_sdhi(s_sdhi_periodic_inst);
  if (reg != nullptr) {
    reg->SD_INFO1 = reg->SD_INFO1 | 1UL;
    reg->SD_INFO2 = reg->SD_INFO2 | 0x300UL;
  }
}

static void arm_periodic_alarm(uint8_t inst)
{
  s_sdhi_periodic_inst = inst;
  struct sigaction sa;
  sa.sa_handler = sdhi_periodic_handler;
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_block_single(void)
{
  TEST_BEGIN("sdhi read_block: single block fills 512B with pattern");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra_sdhi_test_inst_0, k_ra_sdhi_test_pattern);

  uint8_t buf[512];
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = 0xFFU;
  }
  const ra_err_t err =
    ra_sdhi_read_block((uint8_t)k_ra_sdhi_test_inst_0, k_ra_sdhi_test_lba, buf, 1U);
  TEST_ASSERT_EQ(k_ra_ok, err);

  /* Verify SD_ARG was loaded with the LBA and SD_CMD was CMD17 (17). */
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  TEST_ASSERT_EQ(k_ra_sdhi_test_lba, reg->SD_ARG);
  TEST_ASSERT_EQ(k_ra_sdhi_cmd_read_single_block, reg->SD_CMD);
  TEST_ASSERT_EQ(k_ra_sdhi_block_bytes, reg->SD_SIZE);
  /* Single-block: SD_STOP cleared, SD_SECCNT not configured. */
  TEST_ASSERT_EQ(0, reg->SD_STOP);

  /* The simulator returns the same SD_BUF0 word on every read so
   * each 4-byte slot in the destination should equal the pattern. */
  for (size_t i = 0U; i < sizeof(buf); i += 4U) {
    TEST_ASSERT_EQ((k_ra_sdhi_test_pattern & 0xFFU), buf[i + 0U]);
    TEST_ASSERT_EQ(((k_ra_sdhi_test_pattern >> 8U) & 0xFFU), buf[i + 1U]);
    TEST_ASSERT_EQ(((k_ra_sdhi_test_pattern >> 16U) & 0xFFU), buf[i + 2U]);
    TEST_ASSERT_EQ(((k_ra_sdhi_test_pattern >> 24U) & 0xFFU), buf[i + 3U]);
  }
  TEST_END("sdhi read_block: single block fills 512B with pattern");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_block_multi(void)
{
  TEST_BEGIN("sdhi read_block: multi-block sets SD_SECCNT and CMD18");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra_sdhi_test_inst_0, k_ra_sdhi_test_pattern);
  arm_periodic_alarm((uint8_t)k_ra_sdhi_test_inst_0);

  uint8_t        buf[512U * 4U];
  const ra_err_t err =
    ra_sdhi_read_block((uint8_t)k_ra_sdhi_test_inst_0, k_ra_sdhi_test_multi_lba, buf, 4U);
  disarm_sdhi_alarm();
  TEST_ASSERT_EQ(k_ra_ok, err);

  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  /* Multi-block read: SD_SECCNT=4, SD_STOP.SEC enabled, last
   * SD_CMD value is CMD12 STOP_TRANSMISSION. */
  TEST_ASSERT_EQ(4, reg->SD_SECCNT);
  TEST_ASSERT_EQ(k_ra_sdhi_stop_seccnt_en, reg->SD_STOP);
  TEST_ASSERT_EQ(k_ra_sdhi_cmd_stop_transmission, reg->SD_CMD);
  TEST_END("sdhi read_block: multi-block sets SD_SECCNT and CMD18");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_block_single(void)
{
  TEST_BEGIN("sdhi write_block: single block pushes 512B of payload");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_1));

  prime_block_xfer_flags((uint8_t)k_ra_sdhi_test_inst_1, 0U);

  uint8_t buf[512];
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t)(i & 0xFFU);
  }
  const ra_err_t err =
    ra_sdhi_write_block((uint8_t)k_ra_sdhi_test_inst_1, k_ra_sdhi_test_lba, buf, 1U);
  TEST_ASSERT_EQ(k_ra_ok, err);

  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_1);
  /* CMD24 = WRITE_SINGLE_BLOCK = 24. */
  TEST_ASSERT_EQ(k_ra_sdhi_cmd_write_single_block, reg->SD_CMD);
  TEST_ASSERT_EQ(k_ra_sdhi_test_lba, reg->SD_ARG);
  TEST_ASSERT_EQ(k_ra_sdhi_block_bytes, reg->SD_SIZE);
  /* Last-pushed FIFO word: bytes 508..511 of the buffer
   * (the simulator's SD_BUF0 backing store is overwritten on
   * every push so it ends up holding the final word). */
  const uint32_t last_word = (uint32_t)buf[508] | ((uint32_t)buf[509] << 8U) |
                             ((uint32_t)buf[510] << 16U) | ((uint32_t)buf[511] << 24U);
  TEST_ASSERT_EQ(last_word, reg->SD_BUF0);
  TEST_END("sdhi write_block: single block pushes 512B of payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_block_multi_ends_in_cmd12(void)
{
  TEST_BEGIN("sdhi write_block: multi-block ends in CMD12");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra_sdhi_test_inst_0, 0U);
  arm_periodic_alarm((uint8_t)k_ra_sdhi_test_inst_0);

  uint8_t        buf[512U * 2U] = {};
  const ra_err_t err =
    ra_sdhi_write_block((uint8_t)k_ra_sdhi_test_inst_0, k_ra_sdhi_test_multi_lba, buf, 2U);
  disarm_sdhi_alarm();
  TEST_ASSERT_EQ(k_ra_ok, err);

  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  TEST_ASSERT_EQ(2, reg->SD_SECCNT);
  TEST_ASSERT_EQ(k_ra_sdhi_stop_seccnt_en, reg->SD_STOP);
  TEST_ASSERT_EQ(k_ra_sdhi_cmd_stop_transmission, reg->SD_CMD);
  TEST_END("sdhi write_block: multi-block ends in CMD12");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_block_xfer_null_args(void)
{
  TEST_BEGIN("sdhi block xfer: null args + bad instance");
  prep();
  uint8_t buf[1];
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_read_block(0U, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_write_block(0U, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sdhi_read_block((uint8_t)k_ra_sdhi_test_inst_bad, 0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sdhi_write_block((uint8_t)k_ra_sdhi_test_inst_bad, 0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sdhi_attach_dma((uint8_t)k_ra_sdhi_test_inst_bad, 1U));
  TEST_END("sdhi block xfer: null args + bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_block_xfer_zero_count(void)
{
  TEST_BEGIN("sdhi block xfer: block_count=0 rejected");
  prep();
  uint8_t buf[1];
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdhi_read_block(0U, 0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sdhi_write_block(0U, 0U, buf, 0U));
  TEST_END("sdhi block xfer: block_count=0 rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dma_toggle(void)
{
  TEST_BEGIN("sdhi attach_dma: toggles SD_DMAEN + INFO2_MASK");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_attach_dma((uint8_t)k_ra_sdhi_test_inst_0, 1U));
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  TEST_ASSERT_EQ(k_ra_sdhi_dmaen_set, reg->SD_DMAEN);
  TEST_ASSERT_EQ(k_ra_sdhi_info2_brem_bwem, (reg->SD_INFO2_MASK & k_ra_sdhi_info2_brem_bwem));

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_attach_dma((uint8_t)k_ra_sdhi_test_inst_0, 0U));
  TEST_ASSERT_EQ(0, reg->SD_DMAEN);
  TEST_ASSERT_EQ(0, (reg->SD_INFO2_MASK & k_ra_sdhi_info2_brem_bwem));
  TEST_END("sdhi attach_dma: toggles SD_DMAEN + INFO2_MASK");
}

int32_t main(void)
{
  test_init_happy();
  test_init_bad();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_send_command_rspend_via_alarm();
  test_send_command_no_rsp_buffer();
  test_send_command_bad_instance();
  test_set_clock();
  test_read_block_single();
  test_read_block_multi();
  test_write_block_single();
  test_write_block_multi_ends_in_cmd12();
  test_block_xfer_null_args();
  test_block_xfer_zero_count();
  test_attach_dma_toggle();
  (void)fprintf(stderr, "[OK  ] test_ra_sdhi.c\n");
  return 0;
}
