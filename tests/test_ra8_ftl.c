/**
 * @file test_ra8_ftl.c
 * @brief Unit tests for the Flash Translation Layer (issue #165).
 *
 * @details
 * Builds a RAM-backed FAKE erase-before-write block device whose write
 * primitive rejects any program to a non-blank (non-0xFF) sector -- modelling
 * the RA8D2 MRAM contract. The FTL wraps that fake and is exercised through the
 * clean free-overwrite block device it presents:
 *
 * - the fake itself rejects a write to a non-blank sector (sanity of the model);
 * - random logical writes / reads / overwrites keep a shadow model in sync over
 *   many operations, proving data integrity across copy-on-write relocation;
 * - overwrites cause real physical erases on the underlying device;
 * - erase counts stay tightly spread across physical blocks (wear-levelling).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ftl.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "unity_minimal.h"

/** @brief Payload fill for the FTL round-trip assertions. */
typedef enum : uint8_t {
  k_ftl_fill_payload = 0xABU, /**< Arbitrary non-trivial byte pattern. */
} ftl_fill_t;

/**
 * @enum ftl_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them, plus the payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_ftl_pattern_stride =
    31U, /**< Block stride `i * 31 + lbn * 7 + tag`; prime, so the pattern never repeats. */
  k_ftl_pattern_lbn_mul =
    7U, /**< Logical-block-number multiplier, so two blocks of one file still differ. */
  /** Logical block the round-trip writes and reads back. */
  k_ftl_probe_lbn = 5U,
  k_ftl_probe_tag =
    9U, /**< Its generation tag, so a stale mapping returning an older copy is detectable. */
} ftl_fixture_t;

/**
 * @enum test_ftl_const_t
 * @brief Fixture sizes and the RNG constants for the random workload.
 *
 * @details
 * The fake device has ::k_test_ftl_phys physical blocks; the FTL presents
 * ::k_test_ftl_logical logical blocks, leaving the rest as copy-on-write spare.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_ftl_phys       = 24,         /**< Physical blocks in the fake device. */
  k_test_ftl_logical    = 16,         /**< Logical blocks the FTL presents.    */
  k_test_ftl_block      = 512,        /**< Bytes per block.                    */
  k_test_ftl_ops        = 4000,       /**< Random operations in the soak loop. */
  k_test_ftl_erase_byte = 0xFF,       /**< Fake medium erase value.            */
  k_test_rng_mul        = 1103515245, /**< LCG multiplier.                     */
  k_test_rng_add        = 12345,      /**< LCG increment.                      */
  k_test_rng_seed       = 0xC0FFEE,   /**< LCG seed.                           */
} test_ftl_const_t;

/* =============================================================================
 * Fake erase-before-write block device
 * =============================================================================
 */

/**
 * @struct test_fake_state_t
 * @brief State of the RAM-backed erase-before-write fake device.
 *
 * @details
 * `store` holds the raw bytes; `erases` counts erase operations issued by the
 * FTL, used to assert that overwrites actually trigger physical erases.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  store[(size_t)k_test_ftl_phys * (size_t)k_test_ftl_block]; /**< Backing.     */
  uint32_t erases;                                                    /**< Erase tally. */
} test_fake_state_t;

/** @brief Fake device backing state (single instance for the fixture). */
static test_fake_state_t s_fake;

/**
 * @par MC/DC:
 * (no compound decision -- single bounds compare)
 */
static ra8_err_t fake_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  test_fake_state_t* st = (test_fake_state_t*)ctx;
  if (lba + count > (uint32_t)k_test_ftl_phys) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &st->store[(size_t)lba * (size_t)k_test_ftl_block],
               (size_t)count * (size_t)k_test_ftl_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (each guard is an independent single-condition early return; the blank scan is
 * a bounded loop, not a compound decision)
 */
static ra8_err_t fake_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  test_fake_state_t* st = (test_fake_state_t*)ctx;
  if (lba + count > (uint32_t)k_test_ftl_phys) {
    return k_ra8_err_out_of_range;
  }
  /* Erase-before-write contract: every target byte must read back as 0xFF. */
  const size_t base = (size_t)lba * (size_t)k_test_ftl_block;
  const size_t n    = (size_t)count * (size_t)k_test_ftl_block;
  for (size_t i = 0; i < n; ++i) {
    if (st->store[base + i] != (uint8_t)k_test_ftl_erase_byte) {
      return k_ra8_err_invalid_state; /* program to non-blank sector rejected */
    }
  }
  (void)memcpy(&st->store[base], buf, n);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no compound decision -- single bounds compare then a memset)
 */
static ra8_err_t fake_erase(void* ctx, uint32_t lba, uint32_t count)
{
  test_fake_state_t* st = (test_fake_state_t*)ctx;
  if (lba + count > (uint32_t)k_test_ftl_phys) {
    return k_ra8_err_out_of_range;
  }
  (void)memset(&st->store[(size_t)lba * (size_t)k_test_ftl_block],
               (int)k_test_ftl_erase_byte,
               (size_t)count * (size_t)k_test_ftl_block);
  st->erases += count;
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no decision -- populates a struct)
 */
static ra8_err_t fake_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  (void)ctx;
  out->block_count             = (uint32_t)k_test_ftl_phys;
  out->erase_unit_blocks       = 1U;
  out->program_size_bytes      = (uint32_t)k_test_ftl_block;
  out->logical_block_bytes     = (uint16_t)k_test_ftl_block;
  out->erase_value             = (uint8_t)k_test_ftl_erase_byte;
  out->must_erase_before_write = true;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief Fake device vtable (erase-before-write semantics). */
static const ra8_io_blockdev_iface_t k_fake_iface = {
  .read     = fake_read,
  .write    = fake_write,
  .erase    = fake_erase,
  .get_caps = fake_get_caps,
  .sync     = nullptr,
};

/**
 * @brief Bind the fake device, fully erased, into `bd`.
 *
 * @param[out] bd Block-device handle to bind.
 */
static void fake_bind(ra8_io_blockdev_t* bd)
{
  (void)memset(s_fake.store, (int)k_test_ftl_erase_byte, sizeof(s_fake.store));
  s_fake.erases = 0U;
  bd->iface     = &k_fake_iface;
  bd->ctx       = &s_fake;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decision under test -- asserts the fake rejects a non-blank write)
 */
static void test_fake_rejects_nonblank(void)
{
  TEST_BEGIN("fake rejects non-blank write");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  uint8_t blk[(size_t)k_test_ftl_block];
  (void)memset(blk, k_ftl_fill_payload, sizeof(blk));
  /* First write to a blank sector succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&fake, 0U, 1U, blk));
  /* Second write to the now non-blank sector is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_write(&fake, 0U, 1U, blk));
  /* After an erase the sector is writable again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&fake, 0U, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&fake, 0U, 1U, blk));
  TEST_END("fake rejects non-blank write");
}

/**
 * @par MC/DC:
 * (init rejects each NULL / bad-size argument via independent single-condition
 * guards; no compound decision)
 */
static void test_ftl_init_validation(void)
{
  TEST_BEGIN("ftl init validation");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_test_ftl_logical];
  ra8_ftl_pblock_t pb[(size_t)k_test_ftl_phys] = {};
  uint8_t          scratch[(size_t)k_test_ftl_block];

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ftl_init(nullptr, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ftl_init(&ftl, nullptr, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ftl_init(&ftl, &fake, nullptr, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_ftl_init(&ftl, &fake, map, 0U, pb, k_test_ftl_phys, scratch));
  /* No spare block (logical == physical) is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_phys, pb, k_test_ftl_phys, scratch));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  TEST_END("ftl init validation");
}

/**
 * @par MC/DC:
 * (no compound decision -- unmapped logical block reads back the erase value)
 */
static void test_ftl_unwritten_reads_erase_value(void)
{
  TEST_BEGIN("ftl unwritten reads erase value");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_test_ftl_logical];
  ra8_ftl_pblock_t pb[(size_t)k_test_ftl_phys] = {};
  uint8_t          scratch[(size_t)k_test_ftl_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  uint8_t got[(size_t)k_test_ftl_block];
  (void)memset(got, 0x00, sizeof(got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 3U, 1U, got));
  bool all_ff = true;
  for (size_t i = 0; i < sizeof(got); ++i) {
    if (got[i] != (uint8_t)k_test_ftl_erase_byte) {
      all_ff = false;
    }
  }
  TEST_ASSERT(all_ff);
  TEST_END("ftl unwritten reads erase value");
}

/**
 * @brief Fill `blk` with a deterministic pattern keyed by `lbn` and `tag`.
 *
 * @param[out] blk 512-byte block to fill.
 * @param[in]  lbn Logical block number.
 * @param[in]  tag Generation tag (distinguishes successive overwrites).
 */
static void pattern_fill(uint8_t* blk, uint32_t lbn, uint32_t tag)
{
  for (uint32_t i = 0; i < (uint32_t)k_test_ftl_block; ++i) {
    blk[i] = (uint8_t)((i * k_ftl_pattern_stride) + (lbn * k_ftl_pattern_lbn_mul) + tag);
  }
}

/**
 * @brief Random read/write soak: drive writes and check reads against a shadow.
 * @param[in,out] bd         The FTL block device under test.
 * @param[in,out] shadow_tag Per-logical generation tags (0 == never written).
 * @param[out]    writes     Receives the number of write operations performed.
 * @pre @p bd is a valid FTL block device; @p shadow_tag is zeroed.
 * @post Every read of a written block matched its shadow pattern.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ftl_soak_loop(ra8_io_blockdev_t* bd, uint32_t* shadow_tag, uint32_t* writes)
{
  uint32_t rng = (uint32_t)k_test_rng_seed;
  *writes      = 0;
  for (uint32_t op = 0; op < (uint32_t)k_test_ftl_ops; ++op) {
    rng                 = (rng * (uint32_t)k_test_rng_mul) + (uint32_t)k_test_rng_add;
    const uint32_t lbn  = rng % (uint32_t)k_test_ftl_logical;
    rng                 = (rng * (uint32_t)k_test_rng_mul) + (uint32_t)k_test_rng_add;
    const bool do_write = ((rng >> 16) & 1U) != 0U;

    if (do_write) {
      const uint32_t tag = op + 1U;
      uint8_t        blk[(size_t)k_test_ftl_block];
      pattern_fill(blk, lbn, tag);
      TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(bd, lbn, 1U, blk));
      shadow_tag[lbn] = tag;
      ++(*writes);
      continue;
    }

    uint8_t got[(size_t)k_test_ftl_block];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(bd, lbn, 1U, got));
    if (shadow_tag[lbn] == 0U) {
      continue; /* never written -- value is erase byte, not pattern */
    }
    uint8_t want[(size_t)k_test_ftl_block];
    pattern_fill(want, lbn, shadow_tag[lbn]);
    TEST_ASSERT(memcmp(got, want, sizeof(want)) == 0);
  }
}

/**
 * @brief Full read-back of every written logical block against the shadow.
 * @param[in,out] bd         The FTL block device under test.
 * @param[in]     shadow_tag Per-logical generation tags from the soak loop.
 * @pre @p bd is a valid FTL block device.
 * @post Every written block read back byte-identical to its shadow pattern.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ftl_soak_verify(ra8_io_blockdev_t* bd, const uint32_t* shadow_tag)
{
  for (uint32_t lbn = 0; lbn < (uint32_t)k_test_ftl_logical; ++lbn) {
    if (shadow_tag[lbn] == 0U) {
      continue;
    }
    uint8_t got[(size_t)k_test_ftl_block];
    uint8_t want[(size_t)k_test_ftl_block];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(bd, lbn, 1U, got));
    pattern_fill(want, lbn, shadow_tag[lbn]);
    TEST_ASSERT(memcmp(got, want, sizeof(want)) == 0);
  }
}

/**
 * @par MC/DC:
 * (the soak loop has no compound decisions; each integrity / wear assertion is a
 * single comparison driven by the shadow model)
 */
static void test_ftl_soak_integrity_wear(void)
{
  TEST_BEGIN("ftl soak integrity + wear");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_test_ftl_logical];
  ra8_ftl_pblock_t pb[(size_t)k_test_ftl_phys] = {};
  uint8_t          scratch[(size_t)k_test_ftl_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Shadow: per-logical-block generation tag (0 == never written). */
  uint32_t shadow_tag[(size_t)k_test_ftl_logical] = {};
  uint32_t writes                                 = 0;
  ftl_soak_loop(&bd, shadow_tag, &writes);

  /* Final full read-back against the shadow proves end-state integrity. */
  ftl_soak_verify(&bd, shadow_tag);

  /* Overwrites must have driven real physical erases on the underlying device. */
  TEST_ASSERT(writes > 0U);
  TEST_ASSERT(s_fake.erases > 0U);

  /* Wear spread: max erase count must stay close to the min (no hot block). */
  uint32_t hi = 0;
  uint32_t lo = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_wear_stats(&ftl, &hi, &lo));
  TEST_ASSERT(hi >= lo);
  /*
   * With ~2000 writes spread over 24 physical blocks the least-erased-free
   * policy keeps every block within a handful of erases of every other -- the
   * measured spread is 4, so a bound of 6 proves "no single block erased far
   * more than others" with margin while staying honest.
   */
  TEST_ASSERT((hi - lo) <= 6U);
  TEST_END("ftl soak integrity + wear");
}

/**
 * @par MC/DC:
 * (presented erase unmaps each block; subsequent read returns erase value -- no
 * compound decision)
 */
static void test_ftl_presented_erase_unmaps(void)
{
  TEST_BEGIN("ftl presented erase unmaps");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_test_ftl_logical];
  ra8_ftl_pblock_t pb[(size_t)k_test_ftl_phys] = {};
  uint8_t          scratch[(size_t)k_test_ftl_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  uint8_t blk[(size_t)k_test_ftl_block];
  pattern_fill(blk, k_ftl_probe_lbn, k_ftl_probe_tag);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 5U, 1U, blk));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 5U, 1U));

  uint8_t got[(size_t)k_test_ftl_block];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 5U, 1U, got));
  bool all_ff = true;
  for (size_t i = 0; i < sizeof(got); ++i) {
    if (got[i] != (uint8_t)k_test_ftl_erase_byte) {
      all_ff = false;
    }
  }
  TEST_ASSERT(all_ff);
  TEST_END("ftl presented erase unmaps");
}

/**
 * @par MC/DC:
 * (presented caps advertise free overwrite -- single field assertions)
 */
static void test_ftl_caps_free_overwrite(void)
{
  TEST_BEGIN("ftl caps free overwrite");
  ra8_io_blockdev_t fake = {};
  fake_bind(&fake);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_test_ftl_logical];
  ra8_ftl_pblock_t pb[(size_t)k_test_ftl_phys] = {};
  uint8_t          scratch[(size_t)k_test_ftl_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl, &fake, map, k_test_ftl_logical, pb, k_test_ftl_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));
  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_test_ftl_logical, caps.block_count);
  TEST_ASSERT(caps.must_erase_before_write == false);
  TEST_ASSERT_EQ(k_test_ftl_erase_byte, caps.erase_value);
  TEST_END("ftl caps free overwrite");
}

int32_t main(void)
{
  test_fake_rejects_nonblank();
  test_ftl_init_validation();
  test_ftl_unwritten_reads_erase_value();
  test_ftl_soak_integrity_wear();
  test_ftl_presented_erase_unmaps();
  test_ftl_caps_free_overwrite();
  (void)fprintf(stderr, "[OK  ] test_ra8_ftl.c\n");
  return 0;
}
