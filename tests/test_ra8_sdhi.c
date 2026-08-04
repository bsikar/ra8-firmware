/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_sdhi.c
 * @brief Unit tests for ra8_sdhi.c (SD/MMC Host Interface scaffold)
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_sdhi.h"
#include "ra8_sdhi_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_sdhi_buf_t
 * @brief Block sizes and fill bytes for the PIO transfer arms.
 */
typedef enum : uint16_t {
  k_t_block_len = 512U,  /**< SD block size, bytes. */
  k_t_fill_ones = 0xFFU, /**< All-ones fill; also the modulus of the ramp
                              pattern the read-back arm writes.               */
} t_sdhi_buf_t;

/**
 * @enum t_sdhi_reg_t
 * @brief Register values staged into the SDHI mirror.
 *
 * @details
 * Distinctive patterns rather than realistic ones: each read-back assertion
 * proves the driver returned the register it was asked for, so a swapped
 * response word shows as an obviously wrong value. The ascending RSP nibbles
 * make a mis-ordered 128-bit R2 response self-evident.
 */
typedef enum : uint32_t {
  k_t_info1_inst0 = 0xCAFEBABEUL, /**< SD_INFO1 pattern for instance 0. */
  k_t_info1_inst1 = 0xDEADBEEFUL, /**< A different pattern for instance 1, so
                                       the two mirrors cannot be confused.     */
  k_t_rsp10       = 0x11111111UL, /**< R2 response word 0. */
  k_t_rsp32       = 0x22222222UL, /**< R2 response word 1. */
  k_t_rsp54       = 0x33333333UL, /**< R2 response word 2. */
  k_t_rsp76       = 0x44444444UL, /**< R2 response word 3. */
} t_sdhi_reg_t;

/* Deterministic SDHI response servicing via the ra8_fake_mmio poll-hook -- it runs
 * inline on the driver's OWN poll (no wall-clock timer, no concurrent thread).
 * The polled driver clears SD_INFO1.RSPEND after each command and its FIFO drains
 * poll SD_INFO2.BRE/BWE, so the hook re-asserts all three flags on every poll;
 * each ra8_sdhi_send_command / block-FIFO poll finds them set. Response registers
 * are pre-seeded by the test (the driver only reads them), so the hook never
 * touches SD_RSP10..76, SD_CMD, or SD_ARG. */

/** @brief Status bits the poll-hook holds asserted. */
typedef enum : uint32_t {
  k_sdhi_srv_rspend = 0x00000001UL, /**< SD_INFO1.RSPEND (bit 0).       */
  k_sdhi_srv_brebwe = 0x00000300UL, /**< SD_INFO2.BRE | BWE (bits 9:8). */
} sdhi_srv_bits_t;

/** @brief SDHI instance the poll-hook holds flags asserted on. */
static uint8_t s_srv_inst;

/**
 * @brief Poll-hook body: hold SD_INFO1.RSPEND + SD_INFO2.BRE/BWE asserted.
 */
static void sdhi_flags_hook(void)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(s_srv_inst);
  if (reg != nullptr) {
    reg->SD_INFO1 = reg->SD_INFO1 | (uint32_t)k_sdhi_srv_rspend;
    reg->SD_INFO2 = reg->SD_INFO2 | (uint32_t)k_sdhi_srv_brebwe;
  }
}

/**
 * @brief Install the RSPEND/BRE/BWE poll-hook for @p inst.
 *
 * @param[in] inst SDHI instance index to service.
 */
static void sdhi_flags_hook_arm(uint8_t inst)
{
  s_srv_inst = inst;
  ra8_fake_mmio_set_poll_hook(sdhi_flags_hook);
}

/**
 * @brief Remove the RSPEND/BRE/BWE poll-hook.
 */
static void sdhi_flags_hook_disarm(void)
{
  ra8_fake_mmio_set_poll_hook(nullptr);
}

typedef enum : uint8_t {
  k_ra8_sdhi_test_inst_0   = 0U, /**< RA8 SDHI test inst 0.   */
  k_ra8_sdhi_test_inst_1   = 1U, /**< RA8 SDHI test inst 1.   */
  k_ra8_sdhi_test_inst_bad = 9U, /**< RA8 SDHI test inst bad. */
} ra8_sdhi_test_inst_t;

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
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_1));
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_bad));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_deinit((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_deinit((uint8_t)k_ra8_sdhi_test_inst_bad));
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

  ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0)->SD_INFO1 = k_t_info1_inst0;
  uint32_t mask                                       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_get_status((uint8_t)k_ra8_sdhi_test_inst_0, &mask));
  TEST_ASSERT_EQ(0xCAFEBABEU, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_clear_status((uint8_t)k_ra8_sdhi_test_inst_0, 0x000000F0UL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_get_status((uint8_t)k_ra8_sdhi_test_inst_0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_clear_status((uint8_t)k_ra8_sdhi_test_inst_bad, 0U));
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

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_attach_handler(stub_sdhi_cb, (void*)(uintptr_t)0x5DU));
  ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_1)->SD_INFO1 = k_t_info1_inst1;
  ra8_sdhi_dispatch((uint8_t)k_ra8_sdhi_test_inst_1);
  TEST_ASSERT_EQ(1, s_sdhi_cb_count);
  TEST_ASSERT_EQ(0xDEADBEEFU, s_sdhi_cb_last_mask);
  TEST_ASSERT_EQ(k_ra8_sdhi_test_inst_1, s_sdhi_cb_last_inst);

  ra8_sdhi_dispatch((uint8_t)k_ra8_sdhi_test_inst_bad);
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_enter_stop((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_exit_stop((uint8_t)k_ra8_sdhi_test_inst_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdhi_enter_stop((uint8_t)k_ra8_sdhi_test_inst_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdhi_exit_stop((uint8_t)k_ra8_sdhi_test_inst_bad));
  TEST_END("sdhi power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_rspend_via_poll_hook(void)
{
  TEST_BEGIN("sdhi send_command: RSPEND via poll-hook");
  prep();

  /* Pre-seed response regs. */
  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0);
  reg->SD_RSP10               = k_t_rsp10;
  reg->SD_RSP32               = k_t_rsp32;
  reg->SD_RSP54               = k_t_rsp54;
  reg->SD_RSP76               = k_t_rsp76;

  sdhi_flags_hook_arm((uint8_t)k_ra8_sdhi_test_inst_0);
  uint32_t        rsp[4] = {0U, 0U, 0U, 0U};
  const ra8_err_t err =
    ra8_sdhi_send_command((uint8_t)k_ra8_sdhi_test_inst_0, 0x0000ABCDU, 0xDEADBEEFUL, rsp);
  sdhi_flags_hook_disarm();

  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(0x11111111, rsp[0]);
  TEST_ASSERT_EQ(0x22222222, rsp[1]);
  TEST_ASSERT_EQ(0x33333333, rsp[2]);
  TEST_ASSERT_EQ(0x44444444, rsp[3]);
  TEST_ASSERT_EQ(0xDEADBEEFUL, reg->SD_ARG);
  TEST_ASSERT_EQ(0x0000ABCDU, reg->SD_CMD);
  TEST_END("sdhi send_command: RSPEND via poll-hook");
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

  sdhi_flags_hook_arm((uint8_t)k_ra8_sdhi_test_inst_1);
  const ra8_err_t err = ra8_sdhi_send_command((uint8_t)k_ra8_sdhi_test_inst_1, 0x01U, 0U, nullptr);
  sdhi_flags_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sdhi_send_command((uint8_t)k_ra8_sdhi_test_inst_bad, 0U, 0U, rsp));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_set_clock((uint8_t)k_ra8_sdhi_test_inst_0, 0x0080U));
  TEST_ASSERT_EQ(0x0080U, ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0)->SD_CLK_CTRL);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_set_clock((uint8_t)k_ra8_sdhi_test_inst_bad, 0U));
  TEST_END("sdhi set_clock");
}

/**
 * @brief Lab pattern stamped into SD_BUF0 for the read_block test.
 *
 * @details
 * The fake backs SD_BUF0 with plain RAM, so reading the
 * register N times in a row returns whatever value was last
 * written. The driver's polled FIFO drain copies SD_BUF0 unchanged
 * into the destination buffer in little-endian order; with a
 * single primed value the entire 512-byte block winds up filled
 * with the same word, which is exactly what we assert below.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_test_pattern   = 0xA5B6C7D8UL, /**< RA8 SDHI test pattern.   */
  k_ra8_sdhi_test_lba       = 0x00001234UL, /**< RA8 SDHI test lba.       */
  k_ra8_sdhi_test_multi_lba = 0x00005678UL, /**< RA8 SDHI test multi lba. */
} ra8_sdhi_test_const_t;

/**
 * @brief Pre-set SD_INFO2 BRE/BWE flags + RSPEND + a SD_BUF0 word.
 *
 * @details
 * The polled block-transfer driver checks SD_INFO1.RSPEND after
 * each command issue and SD_INFO2.BRE/BWE before each FIFO word.
 * Because the fake backing store is plain RAM, asserting the
 * flags once is enough -- they stay set for the full duration of
 * the transfer.
 */
static void prime_block_xfer_flags(uint8_t inst, uint32_t buf_word)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(inst);
  /* RSPEND + BRE + BWE all asserted ahead of time. The driver
   * clears RSPEND inline (read-modify-write) and zeroes SD_INFO2
   * after the loop -- both are no-ops for the drain itself. */
  reg->SD_INFO1 = 1UL;
  reg->SD_INFO2 = k_sdhi_srv_brebwe;
  reg->SD_BUF0  = buf_word;
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra8_sdhi_test_inst_0, k_ra8_sdhi_test_pattern);

  uint8_t buf[k_t_block_len];
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = k_t_fill_ones;
  }
  const ra8_err_t err =
    ra8_sdhi_read_block((uint8_t)k_ra8_sdhi_test_inst_0, k_ra8_sdhi_test_lba, buf, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* Verify SD_ARG was loaded with the LBA and SD_CMD was CMD17 (17). */
  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0);
  TEST_ASSERT_EQ(k_ra8_sdhi_test_lba, reg->SD_ARG);
  TEST_ASSERT_EQ(k_ra8_sdhi_cmd_read_single_block, reg->SD_CMD);
  TEST_ASSERT_EQ(k_ra8_sdhi_block_bytes, reg->SD_SIZE);
  /* Single-block: SD_STOP cleared, SD_SECCNT not configured. */
  TEST_ASSERT_EQ(0, reg->SD_STOP);

  /* The fake returns the same SD_BUF0 word on every read so
   * each 4-byte slot in the destination should equal the pattern. */
  for (size_t i = 0U; i < sizeof(buf); i += 4U) {
    TEST_ASSERT_EQ((k_ra8_sdhi_test_pattern & 0xFFU), buf[i + 0U]);
    TEST_ASSERT_EQ(((k_ra8_sdhi_test_pattern >> 8U) & 0xFFU), buf[i + 1U]);
    TEST_ASSERT_EQ(((k_ra8_sdhi_test_pattern >> 16U) & 0xFFU), buf[i + 2U]);
    TEST_ASSERT_EQ(((k_ra8_sdhi_test_pattern >> 24U) & 0xFFU), buf[i + 3U]);
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra8_sdhi_test_inst_0, k_ra8_sdhi_test_pattern);
  sdhi_flags_hook_arm((uint8_t)k_ra8_sdhi_test_inst_0);

  uint8_t         buf[k_t_block_len * 4U];
  const ra8_err_t err =
    ra8_sdhi_read_block((uint8_t)k_ra8_sdhi_test_inst_0, k_ra8_sdhi_test_multi_lba, buf, 4U);
  sdhi_flags_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0);
  /* Multi-block read: SD_SECCNT=4, SD_STOP.SEC enabled, last
   * SD_CMD value is CMD12 STOP_TRANSMISSION. */
  TEST_ASSERT_EQ(4, reg->SD_SECCNT);
  TEST_ASSERT_EQ(k_ra8_sdhi_stop_seccnt_en, reg->SD_STOP);
  TEST_ASSERT_EQ(k_ra8_sdhi_cmd_stop_transmission, reg->SD_CMD);
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_1));

  prime_block_xfer_flags((uint8_t)k_ra8_sdhi_test_inst_1, 0U);

  uint8_t buf[k_t_block_len];
  for (size_t i = 0U; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t)(i & k_t_fill_ones);
  }
  const ra8_err_t err =
    ra8_sdhi_write_block((uint8_t)k_ra8_sdhi_test_inst_1, k_ra8_sdhi_test_lba, buf, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_1);
  /* CMD24 = WRITE_SINGLE_BLOCK = 24. */
  TEST_ASSERT_EQ(k_ra8_sdhi_cmd_write_single_block, reg->SD_CMD);
  TEST_ASSERT_EQ(k_ra8_sdhi_test_lba, reg->SD_ARG);
  TEST_ASSERT_EQ(k_ra8_sdhi_block_bytes, reg->SD_SIZE);
  /* Last-pushed FIFO word: bytes 508..511 of the buffer
   * (the fake's SD_BUF0 backing store is overwritten on
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));

  prime_block_xfer_flags((uint8_t)k_ra8_sdhi_test_inst_0, 0U);
  sdhi_flags_hook_arm((uint8_t)k_ra8_sdhi_test_inst_0);

  uint8_t         buf[k_t_block_len * 2U] = {};
  const ra8_err_t err =
    ra8_sdhi_write_block((uint8_t)k_ra8_sdhi_test_inst_0, k_ra8_sdhi_test_multi_lba, buf, 2U);
  sdhi_flags_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0);
  TEST_ASSERT_EQ(2, reg->SD_SECCNT);
  TEST_ASSERT_EQ(k_ra8_sdhi_stop_seccnt_en, reg->SD_STOP);
  TEST_ASSERT_EQ(k_ra8_sdhi_cmd_stop_transmission, reg->SD_CMD);
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_read_block(0U, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_write_block(0U, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sdhi_read_block((uint8_t)k_ra8_sdhi_test_inst_bad, 0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sdhi_write_block((uint8_t)k_ra8_sdhi_test_inst_bad, 0U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdhi_attach_dma((uint8_t)k_ra8_sdhi_test_inst_bad, 1U));
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdhi_read_block(0U, 0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdhi_write_block(0U, 0U, buf, 0U));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_init((uint8_t)k_ra8_sdhi_test_inst_0));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_attach_dma((uint8_t)k_ra8_sdhi_test_inst_0, 1U));
  volatile r_sdhi_regs_t* reg = ra8_sdhi((uint8_t)k_ra8_sdhi_test_inst_0);
  TEST_ASSERT_EQ(k_ra8_sdhi_dmaen_set, reg->SD_DMAEN);
  TEST_ASSERT_EQ(k_ra8_sdhi_info2_brem_bwem, (reg->SD_INFO2_MASK & k_ra8_sdhi_info2_brem_bwem));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdhi_attach_dma((uint8_t)k_ra8_sdhi_test_inst_0, 0U));
  TEST_ASSERT_EQ(0, reg->SD_DMAEN);
  TEST_ASSERT_EQ(0, (reg->SD_INFO2_MASK & k_ra8_sdhi_info2_brem_bwem));
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
  test_send_command_rspend_via_poll_hook();
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
  (void)fprintf(stderr, "[OK  ] test_ra8_sdhi.c\n");
  return 0;
}
