/**
 * @file test_ra8_ftl_mount.c
 * @brief Unit tests for the FTL mount lifecycle.
 *
 * @details
 * Covers ::ra8_ftl_mount / ::ra8_ftl_sync / ::ra8_ftl_unmount, the lifecycle
 * that gives the FTL's checkpoint a place to live on the same medium it
 * manages:
 *
 * - the FTL's physical span is derived from the device's own block count minus
 *   the declared reserved tail, so the caller cannot under-report the device
 *   and let a relocation land on the checkpoint;
 * - a blank tail cold-starts, a written tail resumes, and a tail holding
 *   anything that is not a loadable checkpoint fails the mount instead of
 *   silently presenting a full medium as an empty one;
 * - ::ra8_ftl_sync programs the tail and ::ra8_ftl_unmount syncs then unbinds;
 * - a handle from ::ra8_ftl_init alone has no tail, so ::ra8_ftl_sync refuses
 *   it rather than guessing where the checkpoint goes.
 *
 * The headline test is the power-cycle round trip driven entirely through the
 * lifecycle: mount, write, unmount, lose SRAM, mount again, read back intact.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ftl.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum mount_const_t
 * @brief Fixture geometry for the mount tests.
 *
 * @details
 * The fake device advertises ::k_mount_total blocks. A mount reserving
 * ::k_mount_tail of them hands the FTL ::k_mount_phys, of which
 * ::k_mount_logical are presented upward and the rest are copy-on-write spare.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mount_total      = 12,   /**< Blocks the fake device advertises.       */
  k_mount_tail       = 1,    /**< Blocks reserved for the checkpoint.      */
  k_mount_phys       = 11,   /**< Blocks the FTL should derive for itself. */
  k_mount_logical    = 5,    /**< Logical blocks the FTL presents.         */
  k_mount_block      = 512,  /**< Bytes per block.                         */
  k_mount_erase_byte = 0xFF, /**< Fake medium erase value.                 */
  k_mount_pattern    = 17,   /**< Payload stride (prime).                  */
  k_mount_lbn_mul    = 5,    /**< Logical-block payload bias.              */
  k_mount_tag        = 40,   /**< Payload generation tag.                  */
} mount_const_t;

/**
 * @struct mount_fake_t
 * @brief RAM-backed erase-before-write fake device.
 *
 * @details
 * `store` holds every block of the advertised device; the write primitive
 * rejects a program to a non-blank byte, modelling the MRAM contract, so a
 * checkpoint written twice without an intervening erase fails here exactly as
 * it would on the part.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  store[(size_t)k_mount_total * (size_t)k_mount_block]; /**< Backing.         */
  uint32_t block_count;                                          /**< Advertised size. */
} mount_fake_t;

/** @brief Discard expected validation logs without touching host-unmapped ITM. */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @par MC/DC:
 * (single bounds compare; no compound decision)
 */
static ra8_err_t mount_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mount_fake_t* st = (mount_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &st->store[(size_t)lba * (size_t)k_mount_block],
               (size_t)count * (size_t)k_mount_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (each guard is an independent single-condition return; the blank scan is a
 * bounded loop, not a compound decision)
 */
static ra8_err_t mount_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mount_fake_t* st = (mount_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  const size_t base = (size_t)lba * (size_t)k_mount_block;
  const size_t n    = (size_t)count * (size_t)k_mount_block;
  for (size_t i = 0; i < n; ++i) {
    if (st->store[base + i] != (uint8_t)k_mount_erase_byte) {
      return k_ra8_err_invalid_state;
    }
  }
  (void)memcpy(&st->store[base], buf, n);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (single bounds compare then a memset; no compound decision)
 */
static ra8_err_t mount_erase(void* ctx, uint32_t lba, uint32_t count)
{
  mount_fake_t* st = (mount_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memset(&st->store[(size_t)lba * (size_t)k_mount_block],
               (int)k_mount_erase_byte,
               (size_t)count * (size_t)k_mount_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no decision -- populates a struct)
 */
static ra8_err_t mount_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  const mount_fake_t* st       = (const mount_fake_t*)ctx;
  out->block_count             = st->block_count;
  out->erase_unit_blocks       = 1U;
  out->program_size_bytes      = (uint32_t)k_mount_block;
  out->logical_block_bytes     = (uint16_t)k_mount_block;
  out->erase_value             = (uint8_t)k_mount_erase_byte;
  out->must_erase_before_write = true;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief Fake device vtable (erase-before-write semantics). */
static const ra8_io_blockdev_iface_t k_mount_iface = {
  .read     = mount_read,
  .write    = mount_write,
  .erase    = mount_erase,
  .get_caps = mount_get_caps,
  .sync     = nullptr,
};

/**
 * @struct mount_storage_t
 * @brief One caller's worth of FTL storage, as an app would declare it.
 */
typedef struct {
  /** Checkpoint staging, one whole reserved tail. */
  uint8_t          ckbuf[(size_t)k_mount_tail * (size_t)k_mount_block];
  uint16_t         map[(size_t)k_mount_logical];   /**< Logical->phys map. */
  ra8_ftl_pblock_t pb[(size_t)k_mount_phys];       /**< Per-phys metadata. */
  uint8_t          scratch[(size_t)k_mount_block]; /**< Copy scratch.      */
} mount_storage_t;

/** @brief Bind a fully-erased fake of `blocks` blocks into `bd` via `st`. */
static void mount_bind(ra8_io_blockdev_t* bd, mount_fake_t* st, uint32_t blocks)
{
  (void)memset(st->store, (int)k_mount_erase_byte, sizeof(st->store));
  st->block_count = blocks;
  bd->iface       = &k_mount_iface;
  bd->ctx         = st;
}

/** @brief Fill `cfg` from `raw` and `sto` with the fixture geometry. */
static void mount_cfg(ra8_ftl_cfg_t* cfg, const ra8_io_blockdev_t* raw, mount_storage_t* sto)
{
  cfg->raw                  = raw;
  cfg->map                  = sto->map;
  cfg->pblocks              = sto->pb;
  cfg->scratch              = sto->scratch;
  cfg->checkpoint           = sto->ckbuf;
  cfg->checkpoint_bytes     = (uint32_t)sizeof(sto->ckbuf);
  cfg->logical_blocks       = (uint32_t)k_mount_logical;
  cfg->reserved_tail_blocks = (uint32_t)k_mount_tail;
}

/** @brief Fill `blk` with a deterministic pattern keyed by `lbn`. */
static void mount_pattern(uint8_t* blk, uint32_t lbn)
{
  for (uint32_t i = 0; i < (uint32_t)k_mount_block; ++i) {
    blk[i] = (uint8_t)((i * (uint32_t)k_mount_pattern) + (lbn * (uint32_t)k_mount_lbn_mul) +
                       (uint32_t)k_mount_tag);
  }
}

/** @brief Write the fixture pattern through `bd` into every logical block. */
static void mount_fill(ra8_io_blockdev_t* bd)
{
  uint8_t blk[(size_t)k_mount_block];
  for (uint32_t lbn = 0; lbn < (uint32_t)k_mount_logical; ++lbn) {
    mount_pattern(blk, lbn);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(bd, lbn, 1U, blk));
  }
}

/** @brief Assert every logical block still reads the fixture pattern. */
static void mount_expect_fill(ra8_io_blockdev_t* bd)
{
  uint8_t want[(size_t)k_mount_block];
  uint8_t got[(size_t)k_mount_block];
  for (uint32_t lbn = 0; lbn < (uint32_t)k_mount_logical; ++lbn) {
    mount_pattern(want, lbn);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(bd, lbn, 1U, got));
    TEST_ASSERT(memcmp(want, got, sizeof(got)) == 0);
  }
}

/**
 * @brief A blank medium cold-starts, and the FTL sizes itself from the device.
 */
static void test_mount_cold_derives_geometry(void)
{
  TEST_BEGIN("ftl mount cold-start derives its own span");

  mount_fake_t      fake_st = {};
  ra8_io_blockdev_t fake    = {};
  mount_bind(&fake, &fake_st, (uint32_t)k_mount_total);

  static mount_storage_t sto;
  (void)memset(&sto, 0, sizeof(sto));
  ra8_ftl_cfg_t cfg = {};
  mount_cfg(&cfg, &fake, &sto);

  ra8_ftl_t             ftl   = {};
  ra8_ftl_mount_state_t state = k_ra8_ftl_mount_resumed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_mount(&ftl, &cfg, &state));
  TEST_ASSERT_EQ(k_ra8_ftl_mount_cold, state);

  /* The span the caller never passed: total minus the reserved tail. */
  ra8_io_blockdev_t      bd   = {};
  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ((uint32_t)k_mount_logical, caps.block_count);

  /* Every physical block the FTL may touch is below the reserved tail. */
  mount_fill(&bd);
  for (uint32_t lbn = 0; lbn < (uint32_t)k_mount_logical; ++lbn) {
    uint16_t phys = 0;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_phys_of(&ftl, lbn, &phys));
    TEST_ASSERT(phys < (uint16_t)k_mount_phys);
  }

  /* ... and the tail is still blank, because nothing has synced yet. */
  const size_t tail_base = (size_t)k_mount_phys * (size_t)k_mount_block;
  for (size_t i = tail_base; i < sizeof(fake_st.store); ++i) {
    TEST_ASSERT_EQ((uint8_t)k_mount_erase_byte, fake_st.store[i]);
  }

  TEST_END("ftl mount cold-start derives its own span");
}

/**
 * @brief The lifecycle round trip: mount, write, unmount, lose SRAM, remount.
 */
static void test_mount_power_cycle_roundtrip(void)
{
  TEST_BEGIN("ftl mount/unmount power-cycle round trip");

  mount_fake_t      fake_st = {};
  ra8_io_blockdev_t fake    = {};
  mount_bind(&fake, &fake_st, (uint32_t)k_mount_total);

  static mount_storage_t sto;
  (void)memset(&sto, 0, sizeof(sto));
  ra8_ftl_cfg_t cfg = {};
  mount_cfg(&cfg, &fake, &sto);

  ra8_ftl_t         ftl = {};
  ra8_io_blockdev_t bd  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_mount(&ftl, &cfg, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));
  mount_fill(&bd);

  /* Unmount syncs, so the tail is no longer blank, and the handle is gone. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_unmount(&ftl));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_as_blockdev(&ftl, &bd));

  /* --- simulated power cycle: SRAM lost, MRAM retained. --- */
  (void)memset(&ftl, 0, sizeof(ftl));
  (void)memset(&bd, 0, sizeof(bd));
  (void)memset(sto.map, 0, sizeof(sto.map));
  (void)memset(sto.pb, 0, sizeof(sto.pb));
  (void)memset(sto.ckbuf, 0, sizeof(sto.ckbuf));

  ra8_ftl_mount_state_t state = k_ra8_ftl_mount_cold;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_mount(&ftl, &cfg, &state));
  TEST_ASSERT_EQ(k_ra8_ftl_mount_resumed, state);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));
  mount_expect_fill(&bd);

  /* Sync is repeatable: it erases the tail before re-programming it. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_sync(&ftl));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_sync(&ftl));
  mount_expect_fill(&bd);

  TEST_END("ftl mount/unmount power-cycle round trip");
}

/**
 * @brief A tail that is neither blank nor loadable fails the mount.
 */
static void test_mount_refuses_corrupt_tail(void)
{
  TEST_BEGIN("ftl mount refuses a tail it cannot load");

  mount_fake_t      fake_st = {};
  ra8_io_blockdev_t fake    = {};
  mount_bind(&fake, &fake_st, (uint32_t)k_mount_total);

  static mount_storage_t sto;
  (void)memset(&sto, 0, sizeof(sto));
  ra8_ftl_cfg_t cfg = {};
  mount_cfg(&cfg, &fake, &sto);

  ra8_ftl_t ftl = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_mount(&ftl, &cfg, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_sync(&ftl));

  /* Flip one byte of the stored checkpoint: still not blank, no longer valid. */
  const size_t tail_base        = (size_t)k_mount_phys * (size_t)k_mount_block;
  fake_st.store[tail_base + 1U] = (uint8_t)(fake_st.store[tail_base + 1U] ^ 0xA5U);

  ra8_ftl_t         again = {};
  ra8_io_blockdev_t bd    = {};
  TEST_ASSERT(ra8_ftl_mount(&again, &cfg, nullptr) != k_ra8_ok);
  /* A refused mount leaves nothing bound. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_as_blockdev(&again, &bd));

  TEST_END("ftl mount refuses a tail it cannot load");
}

/**
 * @brief Null and sizing guards on the configuration itself.
 */
static void test_mount_cfg_guards(void)
{
  TEST_BEGIN("ftl mount configuration guards");

  mount_fake_t      fake_st = {};
  ra8_io_blockdev_t fake    = {};
  mount_bind(&fake, &fake_st, (uint32_t)k_mount_total);

  static mount_storage_t sto;
  (void)memset(&sto, 0, sizeof(sto));
  ra8_ftl_cfg_t cfg = {};
  mount_cfg(&cfg, &fake, &sto);

  ra8_ftl_t ftl = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_mount(nullptr, &cfg, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_mount(&ftl, nullptr, nullptr));

  ra8_ftl_cfg_t bad = cfg;
  bad.raw           = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_mount(&ftl, &bad, nullptr));

  bad            = cfg;
  bad.checkpoint = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_mount(&ftl, &bad, nullptr));

  bad                = cfg;
  bad.logical_blocks = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_mount(&ftl, &bad, nullptr));

  bad                      = cfg;
  bad.reserved_tail_blocks = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_mount(&ftl, &bad, nullptr));

  /* Staging buffer smaller than one reserved block. */
  bad                  = cfg;
  bad.checkpoint_bytes = (uint32_t)k_mount_block - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_mount(&ftl, &bad, nullptr));

  /* Tail plus logical plus a spare exceeds the device. */
  bad                = cfg;
  bad.logical_blocks = (uint32_t)k_mount_phys;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_mount(&ftl, &bad, nullptr));

  /* A tail as large as the device leaves the FTL nothing at all. */
  bad                      = cfg;
  bad.reserved_tail_blocks = (uint32_t)k_mount_total;
  bad.checkpoint_bytes     = (uint32_t)k_mount_total * (uint32_t)k_mount_block;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_mount(&ftl, &bad, nullptr));

  TEST_END("ftl mount configuration guards");
}

/**
 * @brief sync and unmount refuse a handle no mount ever bound.
 */
static void test_mount_sync_needs_a_mount(void)
{
  TEST_BEGIN("ftl sync refuses an unmounted handle");

  mount_fake_t      fake_st = {};
  ra8_io_blockdev_t fake    = {};
  mount_bind(&fake, &fake_st, (uint32_t)k_mount_total);

  static mount_storage_t sto;
  (void)memset(&sto, 0, sizeof(sto));

  ra8_ftl_t init_only = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&init_only,
                              &fake,
                              sto.map,
                              (uint32_t)k_mount_logical,
                              sto.pb,
                              (uint32_t)k_mount_phys,
                              sto.scratch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ftl_sync(&init_only));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ftl_unmount(&init_only));

  ra8_ftl_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_sync(&unbound));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_sync(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_unmount(nullptr));

  TEST_END("ftl sync refuses an unmounted handle");
}

int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  test_mount_cold_derives_geometry();
  test_mount_power_cycle_roundtrip();
  test_mount_refuses_corrupt_tail();
  test_mount_cfg_guards();
  test_mount_sync_needs_a_mount();
  return 0;
}
