/**
 * @file test_tz_ipc_attr.c
 * @brief Host unit tests for the declarative IPC attribution descriptor
 *
 * @details
 * Covers the three obligations of `ra8_tz_ipc_attr.h`:
 *
 *   1. The encoding: each target lands on its documented bit, in the word
 *      that asks its question, and an empty descriptor is the cold-reset
 *      pair rather than something the chip would have to be told.
 *   2. The canonical CPU1 ping-pong map encodes bit for bit to the literal
 *      pair `cpu1_pingpong_ipc` writes today (IPCSAR 0x00050000, IPCPAR 0).
 *   3. The refusals: NULL arguments and an out-of-range enum, and that a
 *      refused encode writes neither output.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tz_ipc_attr.h"
#include "ra8_tz_secure_boot.h"
#include "unity_minimal.h"

/**
 * @enum t_ipc_attr_t
 * @brief Expected register values and the sentinel used for no-write checks.
 */
typedef enum : uint32_t {
  k_t_pingpong_ipcsar = 0x00050000U, /**< SAIPCIR0 + SAIPCIR2 set.      */
  k_t_pingpong_ipcpar = 0x00000000U, /**< Every target Privileged-only. */
  k_t_sentinel        = 0xA5A5A5A5U, /**< Sentinel proving no write.    */
  k_t_all_ns          = 0x000F0303U, /**< Every target Non-Secure.      */
} t_ipc_attr_t;

/**
 * @brief Fill every target with one world / access pair.
 *
 * @param[out] cfg     Descriptor to fill.
 * @param[in]  world   World for every target.
 * @param[in]  access  Access level for every target.
 */
static void internal_fill(ra8_tz_ipc_attribution_t* cfg,
                          ra8_tz_ipc_world_t        world,
                          ra8_tz_ipc_access_t       access)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_tz_ipc_target_count; i++) {
    cfg->target[i].world  = world;
    cfg->target[i].access = access;
  }
}

/**
 * @brief An all-Secure, all-Privileged descriptor encodes to the reset pair.
 */
static void test_encode_empty_is_reset_default(void)
{
  TEST_BEGIN("tz_ipc_attr: empty map is the cold-reset pair");
  ra8_tz_ipc_attribution_t cfg    = {};
  uint32_t                 ipcsar = k_t_sentinel;
  uint32_t                 ipcpar = k_t_sentinel;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(0U, ipcsar);
  TEST_ASSERT_EQ(0U, ipcpar);
  TEST_END("tz_ipc_attr: empty map is the cold-reset pair");
}

/**
 * @brief Each target sets exactly its documented bit, in exactly one word.
 *
 * @details
 * The table is the contract of HUM Ch 3.2.1 p 205-207: semaphore groups at
 * bits 0/1, NMI units at 8/9, channel groups at 16..19. Setting one target
 * Non-Secure must move one bit of IPCSAR and leave IPCPAR at zero, which is
 * the property that a single shared word written to both registers breaks.
 */
static void test_encode_bit_per_target(void)
{
  TEST_BEGIN("tz_ipc_attr: one target, one bit, one word");
  const uint32_t expect[k_ra8_tz_ipc_target_count] = {
    0x00000001U, 0x00000002U, 0x00000100U, 0x00000200U,
    0x00010000U, 0x00020000U, 0x00040000U, 0x00080000U,
  };

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_tz_ipc_target_count; i++) {
    ra8_tz_ipc_attribution_t cfg    = {};
    uint32_t                 ipcsar = k_t_sentinel;
    uint32_t                 ipcpar = k_t_sentinel;

    cfg.target[i].world = k_ra8_tz_ipc_world_non_secure;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
    TEST_ASSERT_EQ(expect[i], ipcsar);
    TEST_ASSERT_EQ(0U, ipcpar);

    ra8_tz_ipc_attribution_t priv_cfg = {};
    priv_cfg.target[i].access         = k_ra8_tz_ipc_access_unprivileged;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&priv_cfg, &ipcsar, &ipcpar));
    TEST_ASSERT_EQ(0U, ipcsar);
    TEST_ASSERT_EQ(expect[i], ipcpar);
  }
  TEST_END("tz_ipc_attr: one target, one bit, one word");
}

/**
 * @brief The two words are independent: a full map fills both, separately.
 *
 * @details
 * `k_t_all_ns` is the word an earlier bench attempt wrote into IPCSAR by
 * hand. Reaching it here takes eight explicit `world` fields, and reaching
 * the same value in IPCPAR takes eight explicit `access` fields, so the two
 * can no longer be the same decision spelled once.
 */
static void test_encode_words_are_independent(void)
{
  TEST_BEGIN("tz_ipc_attr: the two words are asked separately");
  ra8_tz_ipc_attribution_t cfg    = {};
  uint32_t                 ipcsar = k_t_sentinel;
  uint32_t                 ipcpar = k_t_sentinel;

  internal_fill(&cfg, k_ra8_tz_ipc_world_non_secure, k_ra8_tz_ipc_access_privileged);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(k_t_all_ns, ipcsar);
  TEST_ASSERT_EQ(0U, ipcpar);

  internal_fill(&cfg, k_ra8_tz_ipc_world_secure, k_ra8_tz_ipc_access_unprivileged);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(0U, ipcsar);
  TEST_ASSERT_EQ(k_t_all_ns, ipcpar);
  TEST_END("tz_ipc_attr: the two words are asked separately");
}

/**
 * @brief The canonical map is bit for bit what cpu1_pingpong_ipc writes.
 */
static void test_cpu1_pingpong_matches_literal(void)
{
  TEST_BEGIN("tz_ipc_attr: cpu1 ping-pong map equals the literal pair");
  ra8_tz_ipc_attribution_t cfg    = {};
  uint32_t                 ipcsar = k_t_sentinel;
  uint32_t                 ipcpar = k_t_sentinel;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_cpu1_pingpong(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(k_t_pingpong_ipcsar, ipcsar);
  TEST_ASSERT_EQ(k_t_pingpong_ipcpar, ipcpar);

  /* The invariant the app states in prose: channels 1 and 3 stay Secure
   * because CPU0 owns them. Here it is a field, not a comment. */
  TEST_ASSERT(cfg.target[k_ra8_tz_ipc_target_channel0].world ==
              k_ra8_tz_ipc_world_non_secure);
  TEST_ASSERT(cfg.target[k_ra8_tz_ipc_target_channel2].world ==
              k_ra8_tz_ipc_world_non_secure);
  TEST_ASSERT(cfg.target[k_ra8_tz_ipc_target_channel1].world ==
              k_ra8_tz_ipc_world_secure);
  TEST_ASSERT(cfg.target[k_ra8_tz_ipc_target_channel3].world ==
              k_ra8_tz_ipc_world_secure);
  TEST_END("tz_ipc_attr: cpu1 ping-pong map equals the literal pair");
}

/**
 * @brief NULL arguments are refused and no output is written.
 */
static void test_encode_rejects_null(void)
{
  TEST_BEGIN("tz_ipc_attr: NULL arguments refused");
  ra8_tz_ipc_attribution_t cfg    = {};
  uint32_t                 ipcsar = k_t_sentinel;
  uint32_t                 ipcpar = k_t_sentinel;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_ipc_attribution_encode(nullptr, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_ipc_attribution_encode(&cfg, nullptr, &ipcpar));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, nullptr));
  TEST_ASSERT_EQ(k_t_sentinel, ipcsar);
  TEST_ASSERT_EQ(k_t_sentinel, ipcpar);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_ipc_attribution_cpu1_pingpong(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_secure_boot_security_init_map(nullptr));
  TEST_END("tz_ipc_attr: NULL arguments refused");
}

/**
 * @brief A value outside either enum is refused, leaving both outputs alone.
 *
 * @details
 * The guard exists because an out-of-range value would shift something wider
 * than one bit into the word and corrupt the neighbouring targets rather than
 * just its own.
 */
static void test_encode_rejects_out_of_range(void)
{
  TEST_BEGIN("tz_ipc_attr: out-of-range attribute refused");
  ra8_tz_ipc_attribution_t cfg    = {};
  uint32_t                 ipcsar = k_t_sentinel;
  uint32_t                 ipcpar = k_t_sentinel;

  cfg.target[k_ra8_tz_ipc_target_channel1].world = (ra8_tz_ipc_world_t)3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_ipc_attribution_encode(&cfg, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(k_t_sentinel, ipcsar);
  TEST_ASSERT_EQ(k_t_sentinel, ipcpar);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_secure_boot_security_init_map(&cfg));

  ra8_tz_ipc_attribution_t bad_access                     = {};
  bad_access.target[k_ra8_tz_ipc_target_sem_low].access   = (ra8_tz_ipc_access_t)7U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tz_ipc_attribution_encode(&bad_access, &ipcsar, &ipcpar));
  TEST_ASSERT_EQ(k_t_sentinel, ipcsar);
  TEST_ASSERT_EQ(k_t_sentinel, ipcpar);
  TEST_END("tz_ipc_attr: out-of-range attribute refused");
}

/**
 * @brief The typed door runs the same sequence the raw one does.
 */
static void test_security_init_map_runs(void)
{
  TEST_BEGIN("tz_ipc_attr: typed security_init accepts a valid map");
  ra8_tz_ipc_attribution_t cfg = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_ipc_attribution_cpu1_pingpong(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_secure_boot_security_init_map(&cfg));
  TEST_ASSERT(ra8_tz_secure_boot_get_step() == k_ra8_tz_secure_boot_step_prcr_relocked);
  TEST_END("tz_ipc_attr: typed security_init accepts a valid map");
}

int main(void)
{
  test_encode_empty_is_reset_default();
  test_encode_bit_per_target();
  test_encode_words_are_independent();
  test_cpu1_pingpong_matches_literal();
  test_encode_rejects_null();
  test_encode_rejects_out_of_range();
  test_security_init_map_runs();
  return 0;
}
