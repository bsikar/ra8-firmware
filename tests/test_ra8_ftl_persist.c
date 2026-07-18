/**
 * @file test_ra8_ftl_persist.c
 * @brief Unit tests for FTL mapping telemetry + checkpoint persistence (#258).
 *
 * @details
 * Covers the ra8_ftl additions that back the wear-levelling / power-cycle demo:
 *
 * - ::ra8_ftl_phys_of -- reads the physical block currently backing a logical
 *   block, and shows the copy-on-write relocation moving the physical index
 *   while the logical address stays fixed (wear-levelling made observable).
 * - ::ra8_ftl_checkpoint_size / ::ra8_ftl_checkpoint_save /
 *   ::ra8_ftl_checkpoint_load -- serialise the volatile mapping tables to a
 *   caller buffer and restore them into a freshly re-initialised handle.
 *
 * The headline test is the power-cycle survival round-trip: write data through
 * an FTL over a RAM-backed erase-before-write fake device (modelling MRAM),
 * checkpoint the mapping, discard the handle + caller tables (SRAM loss) while
 * keeping the backing store (MRAM retention), re-init, prove a naive re-open
 * has lost the data, then restore the checkpoint and read the data back intact.
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

/**
 * @enum persist_const_t
 * @brief Fixture sizes for the persistence tests.
 *
 * @details
 * The fake device has ::k_persist_phys physical blocks; the FTL presents
 * ::k_persist_logical logical blocks, leaving the remainder as copy-on-write
 * spare. ::k_persist_ckbuf sizes a checkpoint scratch buffer comfortably above
 * the largest checkpoint any fixture here produces.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_persist_phys       = 12,   /**< Physical blocks in the fake device.        */
  k_persist_logical    = 6,    /**< Logical blocks the FTL presents.           */
  k_persist_block      = 512,  /**< Bytes per block.                           */
  k_persist_erase_byte = 0xFF, /**< Fake medium erase value.                   */
  k_persist_hdr_bytes  = 12,   /**< Checkpoint header: magic + 2 geometry u32. */
  k_persist_ckbuf      = 1024, /**< Checkpoint scratch buffer size (bytes).    */
} persist_const_t;

/* =============================================================================
 * Configurable erase-before-write fake block device
 * =============================================================================
 */

/**
 * @struct persist_fake_t
 * @brief RAM-backed erase-before-write fake device with a settable size.
 *
 * @details
 * `store` holds the raw bytes for up to ::k_persist_phys physical blocks;
 * `block_count` is the size the device advertises through get_caps, letting one
 * fake stand in for several geometries. The write primitive rejects any program
 * to a non-blank sector, modelling the MRAM erase-before-write contract.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  store[(size_t)k_persist_phys * (size_t)k_persist_block]; /**< Backing. */
  uint32_t block_count; /**< Advertised physical block count. */
} persist_fake_t;

/**
 * @par MC/DC:
 * (single bounds compare; no compound decision)
 */
static ra8_err_t persist_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  persist_fake_t* st = (persist_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &st->store[(size_t)lba * (size_t)k_persist_block],
               (size_t)count * (size_t)k_persist_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (each guard is an independent single-condition return; the blank scan is a
 * bounded loop, not a compound decision)
 */
static ra8_err_t persist_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  persist_fake_t* st = (persist_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  const size_t base = (size_t)lba * (size_t)k_persist_block;
  const size_t n    = (size_t)count * (size_t)k_persist_block;
  for (size_t i = 0; i < n; ++i) {
    if (st->store[base + i] != (uint8_t)k_persist_erase_byte) {
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
static ra8_err_t persist_erase(void* ctx, uint32_t lba, uint32_t count)
{
  persist_fake_t* st = (persist_fake_t*)ctx;
  if (lba + count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memset(&st->store[(size_t)lba * (size_t)k_persist_block],
               (int)k_persist_erase_byte,
               (size_t)count * (size_t)k_persist_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no decision -- populates a struct)
 */
static ra8_err_t persist_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  const persist_fake_t* st     = (const persist_fake_t*)ctx;
  out->block_count             = st->block_count;
  out->erase_unit_blocks       = 1U;
  out->program_size_bytes      = (uint32_t)k_persist_block;
  out->logical_block_bytes     = (uint16_t)k_persist_block;
  out->erase_value             = (uint8_t)k_persist_erase_byte;
  out->must_erase_before_write = true;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief Fake device vtable (erase-before-write semantics). */
static const ra8_io_blockdev_iface_t k_persist_iface = {
  .read     = persist_read,
  .write    = persist_write,
  .erase    = persist_erase,
  .get_caps = persist_get_caps,
  .sync     = nullptr,
};

/**
 * @brief Bind a fully-erased fake of `blocks` blocks into `bd` via `st`.
 *
 * @param[out] bd     Block-device handle to bind.
 * @param[out] st     Fake state to reset and bind.
 * @param[in]  blocks Physical block count the fake advertises.
 */
static void persist_bind(ra8_io_blockdev_t* bd, persist_fake_t* st, uint32_t blocks)
{
  (void)memset(st->store, (int)k_persist_erase_byte, sizeof(st->store));
  st->block_count = blocks;
  bd->iface       = &k_persist_iface;
  bd->ctx         = st;
}

/**
 * @brief Fill `blk` with a deterministic pattern keyed by `lbn` and `tag`.
 *
 * @param[out] blk 512-byte block to fill.
 * @param[in]  lbn Logical block number.
 * @param[in]  tag Generation tag (distinguishes successive overwrites).
 */
static void persist_pattern(uint8_t* blk, uint32_t lbn, uint32_t tag)
{
  for (uint32_t i = 0; i < (uint32_t)k_persist_block; ++i) {
    blk[i] = (uint8_t)((i * 17U) + (lbn * 5U) + tag);
  }
}

/* =============================================================================
 * ra8_ftl_phys_of
 * =============================================================================
 */

/**
 * @test ra8_ftl_phys_of validation + relocation telemetry
 *
 * @par MC/DC:
 * (each guard in ra8_ftl_phys_of is an independent single-condition early
 * return -- null ftl, null out, uninitialised map, out-of-range lbn -- with no
 * compound decision; the relocation assertions are single comparisons)
 */
static void test_persist_phys_of(void)
{
  TEST_BEGIN("ftl phys_of telemetry");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, (uint32_t)k_persist_phys);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t pb[(size_t)k_persist_phys] = {};
  uint8_t          scratch[(size_t)k_persist_block];

  uint16_t phys = 0;
  /* Null / uninitialised guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_phys_of(nullptr, 0U, &phys));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_phys_of(&ftl, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_phys_of(&ftl, 0U, &phys));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl,
                              &fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Out-of-range logical block. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_ftl_phys_of(&ftl, (uint32_t)k_persist_logical, &phys));

  /* Unwritten logical block reports the unmapped sentinel. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_phys_of(&ftl, 3U, &phys));
  TEST_ASSERT_EQ(k_ra8_ftl_unmapped, phys);

  /* First write maps the block to a real physical index. */
  uint8_t blk[(size_t)k_persist_block];
  persist_pattern(blk, 3U, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 3U, 1U, blk));
  uint16_t phys_a = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_phys_of(&ftl, 3U, &phys_a));
  TEST_ASSERT(phys_a != (uint16_t)k_ra8_ftl_unmapped);
  TEST_ASSERT((uint32_t)phys_a < (uint32_t)k_persist_phys);

  /* Overwrite relocates (copy-on-write): the physical index must move. */
  persist_pattern(blk, 3U, 2U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 3U, 1U, blk));
  uint16_t phys_b = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_phys_of(&ftl, 3U, &phys_b));
  TEST_ASSERT(phys_b != phys_a);

  TEST_END("ftl phys_of telemetry");
}

/* =============================================================================
 * ra8_ftl_checkpoint_size
 * =============================================================================
 */

/**
 * @test ra8_ftl_checkpoint_size validation + exact byte count
 *
 * @par MC/DC:
 * (the null and uninitialised guards are independent single-condition returns;
 * the size assertion is a single equality against the documented format)
 */
static void test_persist_size(void)
{
  TEST_BEGIN("ftl checkpoint_size");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, (uint32_t)k_persist_phys);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t pb[(size_t)k_persist_phys] = {};
  uint8_t          scratch[(size_t)k_persist_block];

  uint32_t sz = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_size(nullptr, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_size(&ftl, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_checkpoint_size(&ftl, &sz));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl,
                              &fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));

  /* Format: header + logical_blocks * uint16 + physical_blocks * pblock. */
  const uint32_t want = (uint32_t)k_persist_hdr_bytes +
                        ((uint32_t)k_persist_logical * (uint32_t)sizeof(uint16_t)) +
                        ((uint32_t)k_persist_phys * (uint32_t)sizeof(ra8_ftl_pblock_t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &sz));
  TEST_ASSERT_EQ(want, sz);

  TEST_END("ftl checkpoint_size");
}

/* =============================================================================
 * Checkpoint save/load error arms
 * =============================================================================
 */

/**
 * @test ra8_ftl_checkpoint_save validation arms
 *
 * @par MC/DC:
 * (each arm exercises one independent single-condition guard: null ftl, null
 * buf, uninitialised handle propagated from checkpoint_size, and the
 * buf_len < need size guard; no compound decision)
 */
static void test_persist_save_errors(void)
{
  TEST_BEGIN("ftl checkpoint_save errors");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, (uint32_t)k_persist_phys);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t pb[(size_t)k_persist_phys] = {};
  uint8_t          scratch[(size_t)k_persist_block];
  uint8_t          buf[(size_t)k_persist_ckbuf];

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_save(nullptr, buf, (uint32_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_save(&ftl, nullptr, (uint32_t)sizeof(buf)));
  /* Uninitialised handle: checkpoint_size returns not_initialized, propagated. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ftl_checkpoint_save(&ftl, buf, (uint32_t)sizeof(buf)));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl,
                              &fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));
  uint32_t need = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &need));
  /* One byte short of the checkpoint is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_checkpoint_save(&ftl, buf, need - 1U));
  /* Exactly the required size succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&ftl, buf, need));

  TEST_END("ftl checkpoint_save errors");
}

/**
 * @brief Save a checkpoint from an FTL with one fewer logical block, expect reject.
 * @param[in,out] ftl     The reference FTL the checkpoint is loaded into.
 * @param[out]    buf     Checkpoint scratch buffer.
 * @param[in]     buf_cap Capacity of @p buf in bytes.
 * @param[in]     need    The reference FTL's checkpoint size.
 * @return None.
 * @pre @p ftl is initialised at the reference geometry.
 * @post The geometry-mismatched load returned k_ra8_err_invalid_arg.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
persist_load_geometry_logical(ra8_ftl_t* ftl, uint8_t* buf, uint32_t buf_cap, uint32_t need)
{
  persist_fake_t    lt_st = {};
  ra8_io_blockdev_t lt    = {};
  persist_bind(&lt, &lt_st, (uint32_t)k_persist_phys);
  ra8_ftl_t        lt_ftl = {};
  uint16_t         lt_map[(size_t)k_persist_logical - 1U];
  ra8_ftl_pblock_t lt_pb[(size_t)k_persist_phys] = {};
  uint8_t          lt_scratch[(size_t)k_persist_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&lt_ftl,
                              &lt,
                              lt_map,
                              (uint32_t)k_persist_logical - 1U,
                              lt_pb,
                              (uint32_t)k_persist_phys,
                              lt_scratch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&lt_ftl, buf, buf_cap));
  /* Magic + physical match; logical (5 vs 6) differs -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_checkpoint_load(ftl, buf, need));
}

/**
 * @brief Save a checkpoint from an FTL with one fewer physical block, expect reject.
 * @param[in,out] ftl     The reference FTL the checkpoint is loaded into.
 * @param[out]    buf     Checkpoint scratch buffer.
 * @param[in]     buf_cap Capacity of @p buf in bytes.
 * @param[in]     need    The reference FTL's checkpoint size.
 * @return None.
 * @pre @p ftl is initialised at the reference geometry.
 * @post The geometry-mismatched load returned k_ra8_err_invalid_arg.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
persist_load_geometry_physical(ra8_ftl_t* ftl, uint8_t* buf, uint32_t buf_cap, uint32_t need)
{
  persist_fake_t    lp_st = {};
  ra8_io_blockdev_t lp    = {};
  persist_bind(&lp, &lp_st, (uint32_t)k_persist_phys - 1U);
  ra8_ftl_t        lp_ftl = {};
  uint16_t         lp_map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t lp_pb[(size_t)k_persist_phys - 1U] = {};
  uint8_t          lp_scratch[(size_t)k_persist_block];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&lp_ftl,
                              &lp,
                              lp_map,
                              (uint32_t)k_persist_logical,
                              lp_pb,
                              (uint32_t)k_persist_phys - 1U,
                              lp_scratch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&lp_ftl, buf, buf_cap));
  /* Magic + logical match; physical (11 vs 12) differs -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_checkpoint_load(ftl, buf, need));
}

/**
 * @brief Write every logical block (with a wear-spreading overwrite of block 2).
 * @param[in,out] bd The FTL exposed as a block device.
 * @return None.
 * @pre @p bd is a valid FTL block device.
 * @post Every logical block holds its generator pattern.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void persist_write_all(ra8_io_blockdev_t* bd)
{
  for (uint32_t lbn = 0; lbn < (uint32_t)k_persist_logical; ++lbn) {
    uint8_t blk[(size_t)k_persist_block];
    persist_pattern(blk, lbn, lbn + 1U);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(bd, lbn, 1U, blk));
  }
  for (uint32_t rep = 0; rep < 4U; ++rep) {
    uint8_t blk[(size_t)k_persist_block];
    persist_pattern(blk, 2U, 100U + rep);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(bd, 2U, 1U, blk));
  }
}

/**
 * @brief After a naive re-open (no checkpoint), block 2 reads the erase value.
 * @param[in,out] bd The freshly re-inited FTL block device.
 * @return None.
 * @pre @p bd was re-inited over retained media with no checkpoint load.
 * @post Logical block 2 read all erase bytes (the lost-mapping proof).
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void persist_check_naive_lost(ra8_io_blockdev_t* bd)
{
  uint8_t got[(size_t)k_persist_block];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(bd, 2U, 1U, got));
  bool pre_all_ff = true;
  for (size_t i = 0; i < sizeof(got); ++i) {
    if (got[i] != (uint8_t)k_persist_erase_byte) {
      pre_all_ff = false;
    }
  }
  TEST_ASSERT(pre_all_ff);
}

/**
 * @test ra8_ftl_checkpoint_load validation arms
 *
 * @par MC/DC:
 * (each arm exercises one independent single-condition guard: null ftl, null
 * buf, uninitialised handle, buf_len < need, bad magic, logical-count mismatch,
 * physical-count mismatch; all separate single-condition returns)
 */
static void test_persist_load_errors(void)
{
  TEST_BEGIN("ftl checkpoint_load errors");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, (uint32_t)k_persist_phys);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t pb[(size_t)k_persist_phys] = {};
  uint8_t          scratch[(size_t)k_persist_block];
  uint8_t          buf[(size_t)k_persist_ckbuf];

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_load(nullptr, buf, (uint32_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_load(&ftl, nullptr, (uint32_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ftl_checkpoint_load(&ftl, buf, (uint32_t)sizeof(buf)));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl,
                              &fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));
  uint32_t need = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &need));

  /* buf_len one short of the checkpoint. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_checkpoint_load(&ftl, buf, need - 1U));

  /* Blank buffer (magic == 0) is not a checkpoint. */
  (void)memset(buf, 0x00, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ftl_checkpoint_load(&ftl, buf, need));

  /* Valid checkpoint with a smaller logical count -> geometry mismatch. */
  persist_load_geometry_logical(&ftl, buf, (uint32_t)sizeof(buf), need);

  /* Valid checkpoint with a smaller physical count -> geometry mismatch. */
  persist_load_geometry_physical(&ftl, buf, (uint32_t)sizeof(buf), need);

  TEST_END("ftl checkpoint_load errors");
}

/* =============================================================================
 * Power-cycle survival round-trip
 * =============================================================================
 */

/**
 * @brief Initialise the FTL at the reference geometry and expose it as a blockdev.
 * @param[out]    ftl     FTL handle to initialise.
 * @param[in,out] fake    Backing block device.
 * @param[out]    map     Logical->physical map table.
 * @param[out]    pb      Physical-block table.
 * @param[out]    scratch Scratch block buffer.
 * @param[out]    bd      Receives the FTL block-device facade.
 * @return None.
 * @pre All buffers are sized for the reference geometry.
 * @post @p ftl is initialised and @p bd is bound to it.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void persist_ftl_open(ra8_ftl_t*         ftl,
                             ra8_io_blockdev_t* fake,
                             uint16_t*          map,
                             ra8_ftl_pblock_t*  pb,
                             uint8_t*           scratch,
                             ra8_io_blockdev_t* bd)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(ftl,
                              fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(ftl, bd));
}

/**
 * @test checkpoint save + restore survives a simulated power cycle
 *
 * @par MC/DC:
 * (no compound decision under test; the survival property is proven by
 * byte-comparing every restored logical block against a pre-teardown snapshot)
 *
 * @details
 * Models a reset over non-volatile media: the fake's backing store persists
 * (MRAM retention) while the FTL handle and caller tables are zeroed (SRAM
 * loss). A naive re-init loses the mapping -- proven by reading back the erase
 * value -- and only ::ra8_ftl_checkpoint_load restores it so the data reappears.
 */
static void test_persist_power_cycle_roundtrip(void)
{
  TEST_BEGIN("ftl power-cycle survival");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, (uint32_t)k_persist_phys);
  ra8_ftl_t         ftl = {};
  uint16_t          map[(size_t)k_persist_logical];
  ra8_ftl_pblock_t  pb[(size_t)k_persist_phys] = {};
  uint8_t           scratch[(size_t)k_persist_block];
  ra8_io_blockdev_t bd = {};
  persist_ftl_open(&ftl, &fake, map, pb, scratch, &bd);

  /* Write every logical block; overwrite one a few times to spread wear. */
  persist_write_all(&bd);

  /* Snapshot expected contents through the FTL before teardown. */
  uint8_t expect[(size_t)k_persist_logical][(size_t)k_persist_block];
  for (uint32_t lbn = 0; lbn < (uint32_t)k_persist_logical; ++lbn) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, lbn, 1U, expect[lbn]));
  }

  /* Persist the mapping to a checkpoint buffer (would be a reserved block). */
  uint8_t  ckbuf[(size_t)k_persist_ckbuf];
  uint32_t need = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &need));
  TEST_ASSERT(need <= (uint32_t)sizeof(ckbuf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&ftl, ckbuf, (uint32_t)sizeof(ckbuf)));

  /* --- simulated power cycle: SRAM (handle + tables) lost, MRAM kept. --- */
  (void)memset(&ftl, 0, sizeof(ftl));
  (void)memset(map, 0, sizeof(map));
  (void)memset(pb, 0, sizeof(pb));
  (void)memset(&bd, 0, sizeof(bd));

  /* Re-init over the retained backing store (fake_st.store is untouched). */
  persist_ftl_open(&ftl, &fake, map, pb, scratch, &bd);

  /* Naive re-open has lost the map: logical block 2 now reads the erase value. */
  persist_check_naive_lost(&bd);

  uint8_t got[(size_t)k_persist_block];
  /* Restore the checkpoint and read every logical block back intact. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_load(&ftl, ckbuf, need));
  for (uint32_t lbn = 0; lbn < (uint32_t)k_persist_logical; ++lbn) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, lbn, 1U, got));
    TEST_ASSERT(memcmp(got, expect[lbn], sizeof(got)) == 0);
  }

  TEST_END("ftl power-cycle survival");
}

int32_t main(void)
{
  test_persist_phys_of();
  test_persist_size();
  test_persist_save_errors();
  test_persist_load_errors();
  test_persist_power_cycle_roundtrip();
  (void)fprintf(stderr, "[OK  ] test_ra8_ftl_persist.c\n");
  return 0;
}
