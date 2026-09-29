/**
 *
 * @file test_ra8_sau.c
 * @brief Unit tests for the Armv8-M SAU partition helper (ra8_sau.c)
 * @details Exercises region validation, limit and NSC encoding, stale-region clearing, enable state, and failure atomicity in hosted registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_sau.h"
#include "ra8_sau_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_sau_layout_t
 * @brief Magic numbers used by the test suite to drive the mock SAU.
 */
typedef enum : uint32_t {
  k_test_sau_region_base  = 0x22100000UL, /**< 32-byte-aligned base. */
  k_test_sau_region_size  = 0x00001000UL, /**< 4 KiB window.         */
  k_test_sau_region_limit = 0x22100FE0UL, /**< base + size - 32.     */
} test_sau_layout_t;

/**
 * @enum test_sau_index_t
 * @brief Region counts and indices the suite drives the mock SAU with.
 */
typedef enum : uint8_t {
  k_test_sau_sregion_count = 8U, /**< Pretend we are an M85.  */
  k_test_sau_probe_index   = 5U, /**< Region used for probes. */
} test_sau_index_t;

/* Publish SAU_TYPE.SREGION -- see header for full description. */
RA8_INTERNAL static void internal_set_sregion(uint8_t n)
{
  ra8_sau_regs()->TYPE = (uint32_t)n;
}

/* see header for full description.
 *
 * @brief Reset the fake SAU register block.
 *
 * @details
 * `ra8_fake_mmap_reset()` zeros ordinary backing regions, while sanitizer
 * builds intentionally exclude the architectural System Control Space window
 * because it lies in the ASan shadow-gap category. Reset the SAU block
 * explicitly so every test starts disabled, then publish SREGION = 8.
 */
RA8_INTERNAL static void internal_setup(void)
{
  ra8_fake_mmap_reset();
  *ra8_sau_regs() = (r_sau_regs_t){0};
  internal_set_sregion((uint8_t)k_test_sau_sregion_count);
}

/* Build a single-region descriptor -- see header for full description. */
RA8_INTERNAL static ra8_sau_region_t internal_region(ra8_sau_attr_t attr)
{
  return (ra8_sau_region_t){
    .base = (uintptr_t)k_test_sau_region_base,
    .size = (uint32_t)k_test_sau_region_size,
    .attr = attr,
  };
}

/* see header for full description.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- checks a static layout contract)
 */
RA8_INTERNAL static void internal_test_register_layout(void)
{
  TEST_BEGIN("r_sau_regs_t offsets match the Armv8-M ARM SAU summary");
  TEST_ASSERT_EQ(0x00, offsetof(r_sau_regs_t, CTRL));
  TEST_ASSERT_EQ(0x04, offsetof(r_sau_regs_t, TYPE));
  TEST_ASSERT_EQ(0x08, offsetof(r_sau_regs_t, RNR));
  TEST_ASSERT_EQ(0x0C, offsetof(r_sau_regs_t, RBAR));
  TEST_ASSERT_EQ(0x10, offsetof(r_sau_regs_t, RLAR));
  TEST_ASSERT_EQ(0x14, sizeof(r_sau_regs_t));
  /* The block must sit where every hand-rolled copy pokes it. */
  TEST_ASSERT_EQ(0xE000EDDCUL, (uintptr_t)&ra8_sau_regs()->RBAR);
  TEST_END("r_sau_regs_t offsets match the Armv8-M ARM SAU summary");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (exercises the null_ptr arm of internal_cfg_valid's first two decisions)
 */
RA8_INTERNAL static void internal_test_configure_null(void)
{
  TEST_BEGIN("ra8_sau_configure rejects null descriptors");
  internal_setup();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sau_configure(nullptr));

  const ra8_sau_cfg_t cfg = {.regions = nullptr, .region_count = 1U, .all_ns = false};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sau_configure(&cfg));
  /* Nothing was programmed. */
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->CTRL);

  /* region_count == 0 with a null table is a legal empty partition: every
   * address stays Secure, which is the default-deny end of the range. */
  const ra8_sau_cfg_t empty = {.regions = nullptr, .region_count = 0U, .all_ns = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_configure(&empty));
  TEST_ASSERT_EQ(true, ra8_sau_is_enabled());
  TEST_END("ra8_sau_configure rejects null descriptors");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (takes the region_count > SREGION arm on its own, with every region valid)
 */
RA8_INTERNAL static void internal_test_configure_too_many_regions(void)
{
  TEST_BEGIN("ra8_sau_configure rejects region_count > SREGION");
  internal_setup();
  internal_set_sregion(4U);

  ra8_sau_region_t regions[8];
  for (uint8_t i = 0U; i < 8U; ++i) {
    regions[i] = internal_region(k_ra8_sau_attr_ns);
  }
  const ra8_sau_cfg_t cfg = {.regions = regions, .region_count = 8U, .all_ns = false};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_configure(&cfg));
  TEST_ASSERT_EQ(false, ra8_sau_is_enabled());
  TEST_END("ra8_sau_configure rejects region_count > SREGION");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (walks each independent arm of internal_region_valid: attr, size floor,
 * size granularity, base alignment, and the end-of-address-space wrap)
 */
RA8_INTERNAL static void internal_test_region_validation(void)
{
  TEST_BEGIN("ra8_sau_configure rejects every malformed region shape");

  const uint8_t  idx  = (uint8_t)k_test_sau_probe_index;
  ra8_sau_region_t bad = internal_region(k_ra8_sau_attr_ns);

  internal_setup();
  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.attr = (ra8_sau_attr_t)7U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.size = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.size = 16U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.size = (uint32_t)k_test_sau_region_size + 8U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.base = (uintptr_t)k_test_sau_region_base + 8U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  /* A window that would run off the top of the address space. */
  bad      = internal_region(k_ra8_sau_attr_ns);
  bad.base = (uintptr_t)0xFFFFF000UL;
  bad.size = 0x00002000UL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_set_region(idx, &bad));

  /* The exact top window is legal: base + size lands on 2^32. */
  ra8_sau_region_t top = {
    .base = (uintptr_t)0xFFFFF000UL,
    .size = 0x00001000UL,
    .attr = k_ra8_sau_attr_ns,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_set_region(idx, &top));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sau_set_region(idx, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sau_set_region((uint8_t)k_test_sau_sregion_count, &top));
  TEST_END("ra8_sau_configure rejects every malformed region shape");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (exercises the attr == nsc arm of internal_write_region in both directions)
 */
RA8_INTERNAL static void internal_test_limit_and_nsc_encoding(void)
{
  TEST_BEGIN("the driver derives the limit and the ENABLE / NSC bits");
  internal_setup();

  const ra8_sau_region_t ns  = internal_region(k_ra8_sau_attr_ns);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_set_region(0U, &ns));
  TEST_ASSERT_EQ((uint32_t)k_test_sau_region_base, ra8_sau_regs()->RBAR);
  TEST_ASSERT_EQ((uint32_t)k_test_sau_region_limit | (uint32_t)k_ra8_sau_rlar_enable,
                 ra8_sau_regs()->RLAR);
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->RLAR & (uint32_t)k_ra8_sau_rlar_nsc);

  const ra8_sau_region_t nsc = internal_region(k_ra8_sau_attr_nsc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_set_region(1U, &nsc));
  TEST_ASSERT_EQ(1U, ra8_sau_regs()->RNR);
  TEST_ASSERT_EQ((uint32_t)k_test_sau_region_limit | (uint32_t)k_ra8_sau_rlar_enable |
                   (uint32_t)k_ra8_sau_rlar_nsc,
                 ra8_sau_regs()->RLAR);
  TEST_END("the driver derives the limit and the ENABLE / NSC bits");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (exercises the cfg->all_ns arm of internal_install in both directions and
 * the clear loop over regions above region_count)
 */
RA8_INTERNAL static void internal_test_configure_clears_and_enables(void)
{
  TEST_BEGIN("ra8_sau_configure clears stale regions and honours all_ns");
  internal_setup();

  /* Leave a stale enabled region behind at index 6. */
  const ra8_sau_region_t stale = internal_region(k_ra8_sau_attr_nsc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_set_region(6U, &stale));
  TEST_ASSERT_EQ(true, (ra8_sau_regs()->RLAR & (uint32_t)k_ra8_sau_rlar_enable) != 0U);

  const ra8_sau_region_t one = internal_region(k_ra8_sau_attr_ns);
  const ra8_sau_cfg_t    cfg = {.regions = &one, .region_count = 1U, .all_ns = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_configure(&cfg));

  /* The hosted register block is flat rather than banked by RNR, so the
   * readable state is whatever the driver wrote last. After a one-region
   * partition on 8-region silicon that is the clear of the top region: RNR
   * parked at 7 with a zeroed RBAR / RLAR pair proves the clear loop walked
   * every region above region_count, stale index 6 included. */
  TEST_ASSERT_EQ((uint32_t)k_test_sau_sregion_count - 1U, ra8_sau_regs()->RNR);
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->RLAR);
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->RBAR);

  TEST_ASSERT_EQ((uint32_t)k_ra8_sau_ctrl_enable, ra8_sau_regs()->CTRL);

  const ra8_sau_cfg_t all_ns = {.regions = &one, .region_count = 1U, .all_ns = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_configure(&all_ns));
  TEST_ASSERT_EQ((uint32_t)k_ra8_sau_ctrl_enable | (uint32_t)k_ra8_sau_ctrl_allns,
                 ra8_sau_regs()->CTRL);
  TEST_END("ra8_sau_configure clears stale regions and honours all_ns");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (no compound decisions -- enable / disable preserve the other CTRL bit)
 */
RA8_INTERNAL static void internal_test_enable_disable(void)
{
  TEST_BEGIN("enable / disable toggle ENABLE and preserve ALLNS");
  internal_setup();
  ra8_sau_regs()->CTRL = (uint32_t)k_ra8_sau_ctrl_allns;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_enable());
  TEST_ASSERT_EQ(true, ra8_sau_is_enabled());
  TEST_ASSERT_EQ((uint32_t)k_ra8_sau_ctrl_allns,
                 ra8_sau_regs()->CTRL & (uint32_t)k_ra8_sau_ctrl_allns);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_disable());
  TEST_ASSERT_EQ(false, ra8_sau_is_enabled());
  TEST_ASSERT_EQ((uint32_t)k_ra8_sau_ctrl_allns, ra8_sau_regs()->CTRL);

  TEST_ASSERT_EQ((uint8_t)k_test_sau_sregion_count, ra8_sau_region_count());
  TEST_END("enable / disable toggle ENABLE and preserve ALLNS");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (checks the boot table against the partition both board files program)
 */
RA8_INTERNAL static void internal_test_boot_map_table(void)
{
  TEST_BEGIN("the boot partition matches the in-tree board partition");
  const ra8_sau_cfg_t* cfg = ra8_sau_boot_map();
  TEST_ASSERT_EQ(true, cfg != nullptr);
  TEST_ASSERT_EQ((uint8_t)k_ra8_sau_boot_region_count, cfg->region_count);
  TEST_ASSERT_EQ(false, cfg->all_ns);

  /* Same four windows the two board trustzone_init.c files carry, expressed
   * as base + size instead of pre-decremented limits. */
  TEST_ASSERT_EQ(0x02080000UL, (uint32_t)cfg->regions[0].base);
  TEST_ASSERT_EQ(0x020FFFE0UL,
                 (uint32_t)cfg->regions[0].base + cfg->regions[0].size - 32U);
  TEST_ASSERT_EQ(k_ra8_sau_attr_ns, cfg->regions[0].attr);

  TEST_ASSERT_EQ(0x22100000UL, (uint32_t)cfg->regions[1].base);
  TEST_ASSERT_EQ(0x221FFFE0UL,
                 (uint32_t)cfg->regions[1].base + cfg->regions[1].size - 32U);

  TEST_ASSERT_EQ(0x6A000000UL, (uint32_t)cfg->regions[2].base);
  TEST_ASSERT_EQ(0x6BFFFFE0UL,
                 (uint32_t)cfg->regions[2].base + cfg->regions[2].size - 32U);

  TEST_ASSERT_EQ(0x10000000UL, (uint32_t)cfg->regions[3].base);
  TEST_ASSERT_EQ(0x100FFFE0UL,
                 (uint32_t)cfg->regions[3].base + cfg->regions[3].size - 32U);
  TEST_ASSERT_EQ(k_ra8_sau_attr_nsc, cfg->regions[3].attr);
  TEST_END("the boot partition matches the in-tree board partition");
}

/* see header for full description.
 *
 * @par MC/DC:
 * (exercises the SREGION guard of ra8_sau_apply_boot_map in both directions)
 */
RA8_INTERNAL static void internal_test_apply_boot_map(void)
{
  TEST_BEGIN("ra8_sau_apply_boot_map installs the partition or refuses");
  internal_setup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_apply_boot_map());
  TEST_ASSERT_EQ(true, ra8_sau_is_enabled());
  TEST_ASSERT_EQ(0U, ra8_sau_regs()->CTRL & (uint32_t)k_ra8_sau_ctrl_allns);

  /* Replay the boot table's NSC window on its own so its encoding is visible
   * in the flat hosted block, which the install's trailing clear otherwise
   * overwrites. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sau_set_region(3U, &ra8_sau_boot_map()->regions[3]));
  TEST_ASSERT_EQ(0x10000000UL, ra8_sau_regs()->RBAR);
  TEST_ASSERT_EQ(0x100FFFE0UL | (uint32_t)k_ra8_sau_rlar_enable |
                   (uint32_t)k_ra8_sau_rlar_nsc,
                 ra8_sau_regs()->RLAR);

  /* Silicon with too few regions: refuse rather than truncate the partition,
   * which is the same guard the board files spell as SREGION < 4. */
  internal_setup();
  internal_set_sregion(3U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sau_apply_boot_map());
  TEST_ASSERT_EQ(false, ra8_sau_is_enabled());
  TEST_END("ra8_sau_apply_boot_map installs the partition or refuses");
}

int main(void)
{
  internal_test_register_layout();
  internal_test_configure_null();
  internal_test_configure_too_many_regions();
  internal_test_region_validation();
  internal_test_limit_and_nsc_encoding();
  internal_test_configure_clears_and_enables();
  internal_test_enable_disable();
  internal_test_boot_map_table();
  internal_test_apply_boot_map();
  return 0;
}
