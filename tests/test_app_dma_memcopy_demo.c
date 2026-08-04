/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_dma_memcopy_demo.c
 * @brief Integration test: DMAC0 1 KB SRAM->SRAM copy bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/dma_memcopy_demo/main.c bring-up. The
 * host ra8_fake_mmap shim does not actually move bytes through the
 * mocked DMAC channel, so the test focuses on:
 *
 *  - ra8_dmac_start programmes DMSAR / DMDAR / DMCRA / DMCNT.
 *  - The verify helper detects matches and mismatches.
 *  - The fill helper produces a non-trivial pattern.
 *
 * Each test exercises one branch of the demo's compound decisions
 * for MC/DC coverage.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dmac.h"
#include "ra8_dmac_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_t_dma_words   = 256U, /**< T DMA words.   */
  k_t_dma_channel = 0U,   /**< T DMA channel. */
  k_t_dma_byte_sh = 8U,   /**< T DMA byte sh. */
} t_dma_const_t;

static uint32_t s_src[k_t_dma_words];
static uint32_t s_dst[k_t_dma_words];

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)memset(s_src, 0, sizeof(s_src));
  (void)memset(s_dst, 0, sizeof(s_dst));
}

/** @brief Copy of the demo's pattern-fill helper. */
static void fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_dma_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_t_dma_byte_sh);
    s_dst[i] = 0U;
  }
}

/** @brief Copy of the demo's verify helper. */
static uint8_t verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_dma_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Pattern fill produces a non-trivial source buffer.
 *
 * @par MC/DC:
 * No compound decision; the fill loop only has the bound check.
 * Two vectors prove the pattern is not all-zero and the destination
 * is fully cleared.
 */
static void test_dma_app_fill(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_demo: fill produces non-trivial src + zero dst");
  fill_buffers();
  uint32_t accum_src = 0U;
  uint32_t accum_dst = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_dma_words; ++i) {
    accum_src |= s_src[i];
    accum_dst |= s_dst[i];
  }
  TEST_ASSERT(accum_src != 0U);
  TEST_ASSERT_EQ(0U, accum_dst);
  TEST_END("dma_memcopy_demo: fill produces non-trivial src + zero dst");
}

/**
 * @brief verify returns 1 when the buffers match and 0 when they do not.
 *
 * @par MC/DC:
 * Compound decision: ``s_dst[i] != s_src[i]``. One atomic condition
 * x 2 vectors -- equal (this test, vector A) + first mismatch
 * (vector B).
 */
static void test_dma_app_verify_branches(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_demo: verify covers match + mismatch");
  fill_buffers();
  /* Manually mirror src -> dst to exercise the match branch. */
  (void)memcpy(s_dst, s_src, sizeof(s_src));
  TEST_ASSERT_EQ(1U, verify());
  /* Flip one byte and re-verify -- mismatch branch. */
  s_dst[0] ^= 0x1U;
  TEST_ASSERT_EQ(0U, verify());
  TEST_END("dma_memcopy_demo: verify covers match + mismatch");
}

/**
 * @brief ra8_dmac_start programmes DMSAR / DMDAR / DMCRA on the channel.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_dmac_start != ok``. One atomic condition x
 * 2 vectors -- valid channel + cfg (this) + the bad-channel test
 * below for the err branch.
 */
static void test_dma_app_start_programmes_regs(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_demo: start programmes DMSAR/DMDAR/DMCRA");
  fill_buffers();
  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)(uintptr_t)s_src,
    .dst     = (uint32_t)(uintptr_t)s_dst,
    .count   = (uint16_t)k_t_dma_words,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_t_dma_channel, &cfg));
  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_t_dma_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* DMSAR/DMDAR are 32-bit registers; cfg.src/dst already truncated to uint32_t. */
  const uint32_t exp_src = (uint32_t)(uintptr_t)s_src;
  const uint32_t exp_dst = (uint32_t)(uintptr_t)s_dst;
  TEST_ASSERT_EQ(exp_src, reg->DMSAR);
  TEST_ASSERT_EQ(exp_dst, reg->DMDAR);
  TEST_ASSERT_EQ(k_t_dma_words, reg->DMCRA);
  (void)ra8_dmac_stop((uint8_t)k_t_dma_channel);
  TEST_END("dma_memcopy_demo: start programmes DMSAR/DMDAR/DMCRA");
}

/**
 * @brief ra8_dmac_start rejects an out-of-range channel.
 *
 * @par MC/DC:
 * Decision: ``channel >= 8``. One atomic condition x 2 vectors --
 * in-range + out-of-range.
 */
static void test_dma_app_start_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_demo: bad channel rejected");
  const ra8_dmac_config_t cfg = {
    .src     = 0U,
    .dst     = 0U,
    .count   = 1U,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT(ra8_dmac_start(9U, &cfg) != k_ra8_ok);
  TEST_END("dma_memcopy_demo: bad channel rejected");
}

int main(void)
{
  test_dma_app_fill();
  test_dma_app_verify_branches();
  test_dma_app_start_programmes_regs();
  test_dma_app_start_bad_channel();
  return 0;
}
