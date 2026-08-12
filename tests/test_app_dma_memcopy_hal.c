/**
 * @file test_app_dma_memcopy_hal.c
 * @brief Integration test: DMAC0 1 KB copy bring-up via the ra8_dmac HAL
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/dma_memcopy_hal/main.c
 * bring-up. Where the raw ``dma_memcopy_demo`` twin pokes
 * ``DMREQ.SWREQ`` / ``DMSTS.ACT`` by hand, this app fires and waits
 * through the ``ra8_dmac`` HAL primitives -- ``ra8_dmac_software_trigger``
 * and ``ra8_dmac_wait_idle`` -- so this test asserts that the primitives
 * substitute cleanly at the application call site (Liskov).
 *
 * The host ra8_fake_mmap shim does not move bytes through the mocked
 * DMAC channel, so the test focuses on:
 *
 *  - the fill / verify helpers detect the pattern and its mismatch,
 *  - ``ra8_dmac_start_block`` programmes DMSAR / DMDAR / DMCRA,
 *  - ``ra8_dmac_software_trigger`` asserts ``DMREQ.SWREQ``,
 *  - ``ra8_dmac_wait_idle`` returns ok when ACT is clear and times out
 *    when ACT is stuck.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
  k_t_hal_words   = 256U, /**< T HAL DMA words.   */
  k_t_hal_channel = 0U,   /**< T HAL DMA channel. */
  k_t_hal_byte_sh = 8U,   /**< T HAL DMA byte sh. */
} t_hal_const_t;

typedef enum : uint32_t {
  k_t_hal_poll_limit = 64U, /**< wait_idle poll bound for the test. */
} t_hal_poll_t;

static uint32_t s_src[k_t_hal_words];
static uint32_t s_dst[k_t_hal_words];

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)memset(s_src, 0, sizeof(s_src));
  (void)memset(s_dst, 0, sizeof(s_dst));
}

/** @brief Copy of the demo's pattern-fill helper. */
static void fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_hal_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_t_hal_byte_sh);
    s_dst[i] = 0U;
  }
}

/** @brief Copy of the demo's verify helper. */
static uint8_t verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_hal_words; ++i) {
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
 * No compound decision; the fill loop only has the bound check. Two
 * vectors prove the pattern is not all-zero and the destination is
 * fully cleared.
 */
static void test_hal_app_fill(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_hal: fill produces non-trivial src + zero dst");
  fill_buffers();
  uint32_t accum_src = 0U;
  uint32_t accum_dst = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_hal_words; ++i) {
    accum_src |= s_src[i];
    accum_dst |= s_dst[i];
  }
  TEST_ASSERT(accum_src != 0U);
  TEST_ASSERT_EQ(0U, accum_dst);
  TEST_END("dma_memcopy_hal: fill produces non-trivial src + zero dst");
}

/**
 * @brief verify returns 1 when the buffers match and 0 when they do not.
 *
 * @par MC/DC:
 * Compound decision: ``s_dst[i] != s_src[i]``. One atomic condition
 * x 2 vectors -- equal (vector A) + first mismatch (vector B).
 */
static void test_hal_app_verify_branches(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_hal: verify covers match + mismatch");
  fill_buffers();
  (void)memcpy(s_dst, s_src, sizeof(s_src));
  TEST_ASSERT_EQ(1U, verify());
  s_dst[0] ^= 0x1U;
  TEST_ASSERT_EQ(0U, verify());
  TEST_END("dma_memcopy_hal: verify covers match + mismatch");
}

/**
 * @brief The HAL fire-and-wait path programmes, triggers, and drains.
 *
 * @par MC/DC:
 * No compound decision; each HAL call is checked against ``k_ra8_ok``
 * as a single atomic condition. With ``DMSTS.ACT`` clear in the fake
 * MMIO, ``ra8_dmac_wait_idle`` returns on its first read.
 */
static void test_hal_app_start_trigger_wait(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_hal: start_block + software_trigger + wait_idle");
  fill_buffers();
  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)(uintptr_t)s_src,
    .dst         = (uint32_t)(uintptr_t)s_dst,
    .count       = (uint16_t)k_t_hal_words,
    .block_count = 1U,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start_block((uint8_t)k_t_hal_channel, &cfg));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_t_hal_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint32_t exp_src = (uint32_t)(uintptr_t)s_src;
  const uint32_t exp_dst = (uint32_t)(uintptr_t)s_dst;
  /* HUM Ch 17.2.4 "DMSAR : DMA Source Address Register" p 734,
   * 17.2.6 "DMDAR : DMA Destination Address Register" p 735 and
   * 17.2.8 "DMCRA : DMA Transfer Count Register" p 736. */
  TEST_ASSERT_EQ(exp_src, reg->DMSAR);
  TEST_ASSERT_EQ(exp_dst, reg->DMDAR);
  TEST_ASSERT_EQ(k_t_hal_words, (reg->DMCRA & 0xFFFFU));

  /* Software-fire the block through the HAL primitive. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_software_trigger((uint8_t)k_t_hal_channel));
  /* HUM Ch 17.2.15 "DMREQ : DMA Software Start Register" p 744 */
  TEST_ASSERT((reg->DMREQ & (uint8_t)k_ra8_dmreq_swreq_mask) != 0U);

  /* ACT is clear in the fake MMIO, so wait_idle returns immediately. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dmac_wait_idle((uint8_t)k_t_hal_channel, (uint32_t)k_t_hal_poll_limit));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_stop((uint8_t)k_t_hal_channel));
  TEST_END("dma_memcopy_hal: start_block + software_trigger + wait_idle");
}

/**
 * @brief wait_idle reports a timeout when the controller never idles.
 *
 * @par MC/DC:
 * No compound decision; a single ``DMSTS.ACT`` condition inside the
 * bounded poll loop. Forcing ACT high drives the timeout branch.
 */
static void test_hal_app_wait_idle_timeout(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_hal: wait_idle times out on stuck ACT");
  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_t_hal_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* HUM Ch 17.2.16 "DMSTS : DMA Status Register" p 745 */
  reg->DMSTS = (uint8_t)k_ra8_dmsts_act_mask;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_dmac_wait_idle((uint8_t)k_t_hal_channel, (uint32_t)k_t_hal_poll_limit));
  TEST_END("dma_memcopy_hal: wait_idle times out on stuck ACT");
}

/**
 * @brief ra8_dmac_start_block rejects an out-of-range channel.
 *
 * @par MC/DC:
 * Decision: ``channel >= 8``. One atomic condition x 2 vectors --
 * in-range (above) + out-of-range (here).
 */
static void test_hal_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("dma_memcopy_hal: bad channel rejected");
  const ra8_dmac_config_t cfg = {
    .src         = 0U,
    .dst         = 0U,
    .count       = 1U,
    .block_count = 1U,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
  };
  TEST_ASSERT(ra8_dmac_start_block(9U, &cfg) != k_ra8_ok);
  TEST_END("dma_memcopy_hal: bad channel rejected");
}

int main(void)
{
  test_hal_app_fill();
  test_hal_app_verify_branches();
  test_hal_app_start_trigger_wait();
  test_hal_app_wait_idle_timeout();
  test_hal_app_bad_channel();
  return 0;
}
