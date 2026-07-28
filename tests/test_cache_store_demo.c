/**
 * @file test_cache_store_demo.c
 * @brief Host unit test for the ra8_cache_store_demo example core (#257).
 *
 * @details
 * Drives the exact `cache_store_demo_run` core the ARM example runs, bound to the
 * same RAM-backed LevelX NOR driver (`lx_nor_ram_init`) the example uses on the
 * target -- so the host test and the ra8_emulator gate exercise byte-identical
 * logic. Asserts the end-to-end verdict (mount, put/get, pin, forced eviction,
 * reuse, checkpoint-close, remount-persistence), the survivor count, and the
 * NULL-argument guards. LevelX runs standalone (`LX_STANDALONE_ENABLE`).
 *
 * The store's own primitives (put/get/evict/pin/recovery + their MC/DC vectors)
 * are covered separately in test_ra8_cache_store.c; this test proves the example
 * wrapper drives them correctly end to end.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "cache_store_demo.h"
#include "lx_api.h"
#include "lx_nor_ram.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_demo_const_t
 * @brief Fixture geometry for the demo host test.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_demo_index_cap     = 16U,   /**< cache_store index slots.                   */
  k_t_demo_staging_bytes = 512U,  /**< One LevelX logical sector.                 */
  k_t_demo_scratch_bytes = 1536U, /**< Read/write scratch (>= largest demo blob). */
  k_t_demo_logical       = 128U,  /**< Usable LevelX logical-sector span.         */
  k_t_demo_survivors     = 4U,    /**< Entries expected live after the remount.   */
} t_demo_const_t;

/** @brief LevelX control block bound by the RAM driver. */
static LX_NOR_FLASH s_flash;
/** @brief cache_store index array (caller-owned). */
static ra8_cache_store_entry_t s_index[k_t_demo_index_cap];
/** @brief One-sector staging buffer (caller-owned). */
static uint8_t s_staging[k_t_demo_staging_bytes];
/** @brief Read/write scratch buffer for the demo core (caller-owned). */
static uint8_t s_scratch[k_t_demo_scratch_bytes];

/**
 * @brief Build a demo config over the fixture buffers.
 * @return A populated config bound to the RAM NOR driver.
 * @pre The fixture buffers are file-static and alive.
 * @pre A fresh ::s_flash control block backs the run.
 * @post The returned config references the fixture buffers.
 * @post No global state is changed.
 * @since 0.1.0
 */
static cache_store_demo_cfg_t t_demo_cfg(void)
{
  return (cache_store_demo_cfg_t){
    .nor_flash       = &s_flash,
    .nor_driver_init = lx_nor_ram_init,
    .index           = s_index,
    .staging         = s_staging,
    .scratch         = s_scratch,
    .staging_bytes   = (uint32_t)sizeof(s_staging),
    .scratch_bytes   = (uint32_t)sizeof(s_scratch),
    .logical_sectors = (uint32_t)k_t_demo_logical,
    .index_cap       = (uint16_t)k_t_demo_index_cap,
  };
}

/**
 * @test demo_end_to_end
 * @brief The full demo runs to PASS: survivors + eviction + pin all persist.
 * @par MC/DC:
 * (no compound decisions under test -- the demo core's checks are all
 * single-condition sequential guards; this asserts the end-to-end verdict and
 * every observable result field)
 */
static void test_demo_end_to_end(void)
{
  TEST_BEGIN("cache_store demo end to end");
  lx_nor_ram_wipe();
  cache_store_demo_cfg_t    cfg = t_demo_cfg();
  cache_store_demo_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, cache_store_demo_run(&cfg, &res));
  TEST_ASSERT_EQ(k_cache_store_demo_ok, res.status);
  TEST_ASSERT_EQ(k_cache_store_demo_stage_persisted, res.last_stage);
  TEST_ASSERT_EQ(k_t_demo_survivors, res.survivors);
  TEST_ASSERT(res.evicted_stayed_gone);
  TEST_ASSERT(res.pin_survived);
  TEST_ASSERT(res.evicted_key != 0U);
  TEST_END("cache_store demo end to end");
}

/**
 * @test demo_rerunnable
 * @brief A wiped fresh device re-runs the demo deterministically to PASS.
 * @par MC/DC:
 * (no compound decisions under test -- a second independent run over a fresh
 * (wiped) backing yields the same verdict)
 */
static void test_demo_rerunnable(void)
{
  TEST_BEGIN("cache_store demo re-runnable");
  lx_nor_ram_wipe();
  cache_store_demo_cfg_t    cfg = t_demo_cfg();
  cache_store_demo_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, cache_store_demo_run(&cfg, &res));
  TEST_ASSERT_EQ(k_cache_store_demo_ok, res.status);
  TEST_ASSERT_EQ(k_t_demo_survivors, res.survivors);
  TEST_END("cache_store demo re-runnable");
}

/**
 * @test demo_arg_guards
 * @brief NULL config / result are rejected without touching the media.
 * @par MC/DC:
 * (no compound decisions under test -- each guard is a single-condition
 * null-pointer check exercised directly)
 */
static void test_demo_arg_guards(void)
{
  TEST_BEGIN("cache_store demo arg guards");
  cache_store_demo_cfg_t    cfg = t_demo_cfg();
  cache_store_demo_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, cache_store_demo_run(nullptr, &res));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, cache_store_demo_run(&cfg, nullptr));
  TEST_END("cache_store demo arg guards");
}

/**
 * @brief Run every ra8_cache_store_demo host test.
 * @return 0 on success (a failing assertion exits non-zero before returning).
 * @pre The host provides stdio for the Unity-style macros.
 * @pre The RAM NOR driver backing is available (static storage).
 * @post Every registered test has run.
 * @post A non-zero exit indicates the first failing assertion.
 * @since 0.1.0
 */
int main(void)
{
  test_demo_end_to_end();
  test_demo_rerunnable();
  test_demo_arg_guards();
  (void)fprintf(stderr, "[DONE] cache_store_demo\n");
  return 0;
}
