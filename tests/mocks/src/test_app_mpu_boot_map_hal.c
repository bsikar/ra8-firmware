/**
 * @file test_app_mpu_boot_map_hal.c
 * @brief Integration test: MPU boot map brought up via the ra8_mpu HAL (#576)
 *
 * @details
 * Host twin of
 * examples/ek_ra8d2/hw_pending/mpu_boot_map_hal/src/main.c. That app ships a per-app
 * `system_init.c` whose `SystemInit()` installs the boot MPU memory-attribute
 * map through `ra8_mpu_apply_boot_map()`; its `main()` then self-tests the
 * result. The app's own `main.c` cannot be compiled on the host (it contains a
 * `wfi` instruction and calls the on-target clock/console bring-up), so this
 * test mirrors the app's self-test verdict against the fake MMIO instead: it
 * runs the same `ra8_mpu_apply_boot_map()` the boot path runs, then asserts the
 * same facts the app's three steps check --
 *
 *   1. the MPU is enabled (`ra8_mpu_is_enabled()`, the app's step 1);
 *   2. the canonical 5-region map is exposed, including the 640 KiB
 *      (non-power-of-two) non-cacheable shared bank (the app's step 2);
 *   3. the installed register state is correct: MAIR0/MAIR1 + CTRL, and the
 *      region-4 RBAR/RLAR the base+limit encoder produced for the 640 KiB span.
 *
 * All deterministic and host-side; the on-target self-test in the app's main.c
 * (step 3's live SCKDIVCR readback) is exercised separately by the emulator run.
 *
 * The fake MPU block is seeded via a whole-struct write and read back via a
 * struct snapshot, so no line performs a direct `reg->FIELD` MMIO access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mpu.h"
#include "ra8_mpu_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_app_mpu_boot_t
 * @brief Golden constants mirrored from the example's boot map + self-test.
 * @details Region 4 (shared M85<->M33 SRAM) is 640 KiB -- not a power of two --
 *          so its RBAR/RLAR pair is the value that proves the base+limit boot
 *          encoding installed a region the size-checked setter cannot express.
 */
typedef enum : uint32_t {
  k_app_mpu_dregion  = 8U,          /**< DREGION the emulator/M85 report (>= 5).  */
  k_app_mpu_few      = 4U,          /**< DREGION below the map's 5 regions.       */
  k_app_mpu_mair0    = 0x000444FFU, /**< Boot MAIR0: WB/WA | non-cacheable | Dev. */
  k_app_shram_base   = 0x22100000U, /**< Region 4 base (shared SRAM).             */
  k_app_shram_size   = 0x000A0000U, /**< Region 4 size (640 KiB, not pow2).       */
  k_app_shram_rbar   = 0x22100003U, /**< Region 4 RBAR: base | RW | XN.           */
  k_app_shram_rlar   = 0x2219FFE3U, /**< Region 4 RLAR: limit | AttrIdx1 | EN.    */
  k_app_region_count = 5U,          /**< Regions in the canonical boot map.       */
  k_app_idx_shram    = 4U,          /**< Shared-SRAM region index.                */
} test_app_mpu_boot_t;

/**
 * @brief Reset the fake MMIO and seed MPU_TYPE.DREGION to @p dregion.
 *
 * @details Zeroes the whole MPU block (MPU disabled) and sets only DREGION, via
 * a whole-struct write to `*ra8_mpu_regs()` so no line is a direct `reg->FIELD`
 * MMIO access. Mirrors the reset posture the boot path sees out of reset (MPU
 * off) with the silicon's hardwired DREGION.
 */
static void app_reset_world(uint8_t dregion)
{
  ra8_fake_mmap_reset();
  r_mpu_regs_t init = {};
  init.TYPE         = (uint32_t)((uint32_t)dregion << 8U);
  *ra8_mpu_regs()   = init;
}

/**
 * @brief Mirror of the example's `mpu_boot_test_map()` step.
 * @details Confirms `ra8_mpu_boot_map()` exposes the 5-region table and that
 *          region 4 is the 640 KiB non-cacheable shared bank.
 * @return true when the canonical map is present and region 4 matches.
 */
static bool app_map_is_canonical(void)
{
  uint8_t                 count = 0U;
  const ra8_mpu_region_t* map   = ra8_mpu_boot_map(&count);
  if (map == nullptr) {
    return false;
  }
  if (count != (uint8_t)k_app_region_count) {
    return false;
  }
  const ra8_mpu_region_t* shram = &map[k_app_idx_shram];
  if (shram->base != (uintptr_t)k_app_shram_base) {
    return false;
  }
  if (shram->size != (uint32_t)k_app_shram_size) {
    return false;
  }
  if (shram->attr_idx != k_ra8_mpu_attr_idx_1) {
    return false;
  }
  return true;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- runs the example's boot-map bring-up
 * and asserts its self-test verdict; no `&&` or `||` in the code path it
 * touches)
 */
static void test_app_boot_map_installs_and_enables(void)
{
  TEST_BEGIN("mpu_boot_map_hal: apply_boot_map installs the map + enables MPU");
  app_reset_world((uint8_t)k_app_mpu_dregion);

  /* This is exactly what the app's per-app SystemInit() runs. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_apply_boot_map());

  /* App step 1: mpu_boot_test_enabled(). */
  TEST_ASSERT_EQ(true, ra8_mpu_is_enabled());
  /* App step 2: mpu_boot_test_map(). */
  TEST_ASSERT_EQ(true, app_map_is_canonical());

  /* Installed register state (snapshot -> field reads are not MMIO accesses). */
  const r_mpu_regs_t s = *ra8_mpu_regs();
  TEST_ASSERT_EQ(k_app_mpu_mair0, s.MAIR0);
  TEST_ASSERT_EQ(0, s.MAIR1);
  const uint32_t expected_ctrl =
    (uint32_t)k_ra8_mpu_ctrl_enable | (uint32_t)k_ra8_mpu_ctrl_privdefena;
  TEST_ASSERT_EQ(expected_ctrl, s.CTRL);

  /* Region 4 (640 KiB, non-power-of-two) is the last descriptor written, so its
     RBAR/RLAR remain observable -- the base+limit encoding of a span the
     size-checked public setter would reject. */
  TEST_ASSERT_EQ((k_app_region_count - 1U), s.RNR);
  TEST_ASSERT_EQ(k_app_shram_rbar, s.RBAR);
  TEST_ASSERT_EQ(k_app_shram_rlar, s.RLAR);
  TEST_END("mpu_boot_map_hal: apply_boot_map installs the map + enables MPU");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- confirms the app's step-1 verdict is
 * false before the boot map is applied; single relational condition only)
 */
static void test_app_verdict_fails_without_apply(void)
{
  TEST_BEGIN("mpu_boot_map_hal: step-1 verdict is FAIL when MPU never enabled");
  app_reset_world((uint8_t)k_app_mpu_dregion);
  /* Without the SystemInit() call, the app's mpu_boot_test_enabled() reports
     the MPU off, so its self-test verdict is not PASS. */
  TEST_ASSERT_EQ(false, ra8_mpu_is_enabled());
  TEST_END("mpu_boot_map_hal: step-1 verdict is FAIL when MPU never enabled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the DREGION precondition; the
 * `implemented < count` guard inside ra8_mpu_apply_boot_map is a single
 * relational condition, not a compound decision)
 */
static void test_app_boot_map_insufficient_regions(void)
{
  TEST_BEGIN("mpu_boot_map_hal: apply fails + MPU stays off on too few regions");
  app_reset_world((uint8_t)k_app_mpu_few); /* DREGION = 4 < 5. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_apply_boot_map());
  /* The app's SystemInit() gates the cache enables on k_ra8_ok, so on this path
     the MPU stays disabled. */
  TEST_ASSERT_EQ(false, ra8_mpu_is_enabled());
  TEST_END("mpu_boot_map_hal: apply fails + MPU stays off on too few regions");
}

int main(void)
{
  test_app_boot_map_installs_and_enables();
  test_app_verdict_fails_without_apply();
  test_app_boot_map_insufficient_regions();
  return 0;
}
