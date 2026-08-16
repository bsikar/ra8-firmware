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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ftl.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum ftl_persist_fixture_t
 * @brief The payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_ftl_pattern_stride  = 17U, /**< Block stride `i * 17 + lbn * 5 + tag`; prime,
                                 pattern never repeats in a block. */
  k_ftl_pattern_lbn_mul = 5U,  /**< Logical-block-number multiplier, so two
                                 blocks of one file still differ. */
  /**
   * Base generation tag of the rewrite loop; `+ rep` makes each rewrite of
   * the same block distinguishable, which is what proves the newest copy wins.
   */
  k_ftl_tag_base = 100U,
} ftl_persist_fixture_t;

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
  k_persist_phys           = 12,          /**< Physical blocks in the fake device.      */
  k_persist_logical        = 6,           /**< Logical blocks the FTL presents.         */
  k_persist_block          = 512,         /**< Bytes per block and FTL scratch.         */
  k_persist_erase_byte     = 0xFF,        /**< Fake medium erase value.                 */
  k_persist_header_bytes   = 20,          /**< Canonical version-1 header bytes.        */
  k_persist_crc_bytes      = 4,           /**< Canonical CRC-32 trailer bytes.          */
  k_persist_map_bytes      = 2,           /**< Canonical bytes per map entry.           */
  k_persist_pblock_bytes   = 5,           /**< Canonical bytes per pblock entry.        */
  k_persist_ckbuf          = 1024,        /**< Checkpoint scratch buffer size (bytes).  */
  k_persist_golden_bytes   = 36,          /**< One-logical/two-physical golden length.  */
  k_persist_off_version    = 4,           /**< Version field byte offset.               */
  k_persist_off_total      = 8,           /**< Exact-length field byte offset.          */
  k_persist_off_logical    = 12,          /**< Logical-count field byte offset.         */
  k_persist_off_physical   = 16,          /**< Physical-count field byte offset.        */
  k_persist_off_map        = 20,          /**< First map-entry byte offset.             */
  k_persist_off_pb_state   = 4,           /**< State within one pblock record.          */
  k_persist_crc_seed       = 0xFFFFFFFFU, /**< CRC-32 initial/final XOR.                */
  k_persist_crc_poly       = 0xEDB88320U, /**< Reflected ISO-HDLC polynomial.           */
  k_persist_byte_bits      = 8,           /**< Bits processed per CRC input byte.       */
  k_persist_byte_3_shift   = 24U,         /**< Shift of byte three in a LE32.           */
  k_persist_golden_erase_0 = 0x01020304U, /**< First golden erase count.                */
  k_persist_golden_erase_1 = 0xA0B0C0D0U, /**< Second golden erase count.               */
  k_persist_padding_fill   = 0xA5,        /**< Native-padding poison and tail sentinel. */
  k_persist_invalid_state  = 3,           /**< State immediately above the valid range. */
} persist_const_t;

/** @brief Canonical version-1 bytes for one live mapping and one stale block.
 */
static const uint8_t k_golden_checkpoint[k_persist_golden_bytes] = {
  0x52U, 0x46U, 0x54U, 0x4CU, 0x01U, 0x00U, 0x14U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U,
  0x01U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x04U, 0x03U,
  0x02U, 0x01U, 0x02U, 0xD0U, 0xC0U, 0xB0U, 0xA0U, 0x01U, 0x87U, 0xC8U, 0xCDU, 0x63U,
};

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
  /** Backing. */
  uint8_t  store[(size_t)k_persist_phys * (size_t)k_persist_block];
  uint32_t block_count; /**< Advertised physical block count. */
} persist_fake_t;

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
    blk[i] = (uint8_t)((i * k_ftl_pattern_stride) + (lbn * k_ftl_pattern_lbn_mul) + tag);
  }
}

/** @brief Test-side CRC-32 reference for intentionally edited wire vectors. */
static uint32_t persist_crc32(const uint8_t* data, uint32_t length)
{
  uint32_t crc = (uint32_t)k_persist_crc_seed;
  for (uint32_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < (uint8_t)k_persist_byte_bits; ++bit) {
      const uint32_t mask = (uint32_t)(0U - (crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_persist_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_persist_crc_seed;
}

/** @brief Write a little-endian 16-bit test field. */
static void persist_put_le16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
}

/** @brief Write a little-endian 32-bit test field. */
static void persist_put_le32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
  out[2] = (uint8_t)(value >> 16U);
  out[3] = (uint8_t)(value >> (uint32_t)k_persist_byte_3_shift);
}

/** @brief Refresh the CRC trailer after a deliberate semantic corruption. */
static void persist_refresh_crc(uint8_t* buf, uint32_t length)
{
  const uint32_t crc_offset = length - (uint32_t)k_persist_crc_bytes;
  persist_put_le32(&buf[crc_offset], persist_crc32(buf, crc_offset));
}

/** @brief Assert both live metadata tables still match their snapshots. */
static void persist_expect_state(const uint16_t*         map,
                                 const uint16_t*         map_before,
                                 const ra8_ftl_pblock_t* pblocks,
                                 const ra8_ftl_pblock_t* pblocks_before)
{
  TEST_ASSERT(memcmp(map, map_before, (size_t)k_persist_logical * sizeof(map[0])) == 0);
  for (uint32_t i = 0U; i < (uint32_t)k_persist_phys; ++i) {
    TEST_ASSERT_EQ(pblocks_before[i].erase_count, pblocks[i].erase_count);
    TEST_ASSERT_EQ(pblocks_before[i].state, pblocks[i].state);
  }
}

/** @brief Fully-owned fixture for transactional checkpoint-load fault tests. */
typedef struct {
  persist_fake_t    fake_state;                             /**< Backing fake state.      */
  ra8_io_blockdev_t fake;                                   /**< Bound fake block device. */
  ra8_ftl_t         ftl;                                    /**< FTL under test.          */
  uint16_t          map[(size_t)k_persist_logical];         /**< Live logical map.        */
  uint16_t          map_before[(size_t)k_persist_logical];  /**< Pre-load map snapshot.   */
  ra8_ftl_pblock_t  pblocks[(size_t)k_persist_phys];        /**< Live pblock table.       */
  ra8_ftl_pblock_t  pblocks_before[(size_t)k_persist_phys]; /**< Pre-load snapshot.       */
  uint8_t           scratch[(size_t)k_persist_block];       /**< FTL scratch block.       */
  uint8_t           good[(size_t)k_persist_ckbuf];          /**< Valid checkpoint.        */
  uint8_t           edit[(size_t)k_persist_ckbuf];          /**< Corruption workspace.    */
  uint32_t          need;                                   /**< Exact wire length.       */
} persist_load_fixture_t;

/** @brief Initialise a valid load fixture and snapshot its live tables. */
static void persist_load_fixture_init(persist_load_fixture_t* fixture)
{
  (void)memset(fixture, 0, sizeof(*fixture));
  persist_bind(&fixture->fake, &fixture->fake_state, (uint32_t)k_persist_phys);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&fixture->ftl,
                              &fixture->fake,
                              fixture->map,
                              (uint32_t)k_persist_logical,
                              fixture->pblocks,
                              (uint32_t)k_persist_phys,
                              fixture->scratch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&fixture->ftl, &fixture->need));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_checkpoint_save(&fixture->ftl, fixture->good, (uint32_t)sizeof(fixture->good)));
  (void)memcpy(fixture->map_before, fixture->map, sizeof(fixture->map_before));
  (void)memcpy(fixture->pblocks_before, fixture->pblocks, sizeof(fixture->pblocks_before));
}

/** @brief Assert that a rejected checkpoint left both live tables unchanged. */
static void persist_load_expect_state(const persist_load_fixture_t* fixture)
{
  persist_expect_state(fixture->map,
                       fixture->map_before,
                       fixture->pblocks,
                       fixture->pblocks_before);
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

  uint32_t sz = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_size(nullptr, &sz));
  TEST_ASSERT_EQ(UINT32_MAX, sz);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_size(&ftl, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_checkpoint_size(&ftl, &sz));
  TEST_ASSERT_EQ(UINT32_MAX, sz);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ftl_init(&ftl,
                              &fake,
                              map,
                              (uint32_t)k_persist_logical,
                              pb,
                              (uint32_t)k_persist_phys,
                              scratch));

  /* Canonical bytes never depend on native uint16_t/struct sizes or padding. */
  const uint32_t want = (uint32_t)k_persist_header_bytes + (uint32_t)k_persist_crc_bytes +
                        ((uint32_t)k_persist_logical * (uint32_t)k_persist_map_bytes) +
                        ((uint32_t)k_persist_phys * (uint32_t)k_persist_pblock_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &sz));
  TEST_ASSERT_EQ(want, sz);

  ftl.logical_blocks  = UINT32_MAX;
  ftl.physical_blocks = UINT32_MAX;
  sz                  = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_checkpoint_size(&ftl, &sz));
  TEST_ASSERT_EQ(UINT32_MAX, sz);

  TEST_END("ftl checkpoint_size");
}

/**
 * @test canonical checkpoint golden vector and architecture independence
 *
 * @par MC/DC:
 * (no compound production decision; exact bytes independently cover every
 * header, map, pblock, padding-exclusion, endianness, and CRC field)
 */
static void test_persist_golden_wire(void)
{
  TEST_BEGIN("ftl canonical checkpoint golden bytes");
  persist_fake_t    fake_st = {};
  ra8_io_blockdev_t fake    = {};
  persist_bind(&fake, &fake_st, 2U);
  ra8_ftl_t        ftl = {};
  uint16_t         map[1U];
  ra8_ftl_pblock_t pb[2U];
  uint8_t          scratch[(size_t)k_persist_block];
  (void)memset(pb, (int)k_persist_padding_fill, sizeof(pb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_init(&ftl, &fake, map, 1U, pb, 2U, scratch));
  map[0]            = 1U;
  pb[0].erase_count = (uint32_t)k_persist_golden_erase_0;
  pb[0].state       = (uint8_t)k_ra8_ftl_pstate_stale;
  pb[1].erase_count = (uint32_t)k_persist_golden_erase_1;
  pb[1].state       = (uint8_t)k_ra8_ftl_pstate_live;

  uint8_t wire[k_persist_golden_bytes + 8U];
  (void)memset(wire, (int)k_persist_padding_fill, sizeof(wire));
  uint32_t need = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_size(&ftl, &need));
  TEST_ASSERT_EQ((uint32_t)k_persist_golden_bytes, need);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&ftl, wire, (uint32_t)sizeof(wire)));
  TEST_ASSERT(memcmp(wire, k_golden_checkpoint, sizeof(k_golden_checkpoint)) == 0);
  for (size_t i = sizeof(k_golden_checkpoint); i < sizeof(wire); ++i) {
    TEST_ASSERT_EQ((uint8_t)k_persist_padding_fill, wire[i]);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_init(&ftl, &fake, map, 1U, pb, 2U, scratch));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_checkpoint_load(&ftl, k_golden_checkpoint, (uint32_t)sizeof(k_golden_checkpoint)));
  TEST_ASSERT_EQ(1U, map[0]);
  TEST_ASSERT_EQ((uint32_t)k_persist_golden_erase_0, pb[0].erase_count);
  TEST_ASSERT_EQ((uint8_t)k_ra8_ftl_pstate_stale, pb[0].state);
  TEST_ASSERT_EQ((uint32_t)k_persist_golden_erase_1, pb[1].erase_count);
  TEST_ASSERT_EQ((uint8_t)k_ra8_ftl_pstate_live, pb[1].state);
  TEST_END("ftl canonical checkpoint golden bytes");
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
  /* Uninitialised handle: checkpoint_size returns not_initialized, propagated.
   */
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
  (void)memset(buf, (int)k_persist_padding_fill, sizeof(buf));
  uint8_t before[(size_t)k_persist_ckbuf];
  (void)memcpy(before, buf, sizeof(before));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ftl_checkpoint_save(&ftl, buf, need - 1U));
  TEST_ASSERT(memcmp(buf, before, sizeof(buf)) == 0);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_checkpoint_save(&ftl, (uint8_t*)map, need));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ftl_checkpoint_save(&ftl, scratch, need));

  map[0] = (uint16_t)k_persist_phys;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ftl_checkpoint_save(&ftl, buf, need));
  TEST_ASSERT(memcmp(buf, before, sizeof(buf)) == 0);
  map[0] = (uint16_t)k_ra8_ftl_unmapped;
  /* Exactly the required size succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_checkpoint_save(&ftl, buf, need));

  TEST_END("ftl checkpoint_save errors");
}

/** @brief Exercise geometry rejection with otherwise valid exact-size blobs. */
static void persist_check_load_geometry(persist_load_fixture_t* fixture)
{
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le32(&fixture->edit[k_persist_off_logical], (uint32_t)k_persist_logical - 1U);
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le32(&fixture->edit[k_persist_off_physical], (uint32_t)k_persist_phys - 1U);
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
}

/**
 * @brief Write every logical block (with a wear-spreading overwrite of block
 * 2).
 * @param[in,out] bd The FTL exposed as a block device.
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
    persist_pattern(blk, 2U, k_ftl_tag_base + rep);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(bd, 2U, 1U, blk));
  }
}

/**
 * @brief After a naive re-open (no checkpoint), block 2 reads the erase value.
 * @param[in,out] bd The freshly re-inited FTL block device.
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

/** @brief Exercise exact-length and non-aliasing load guards. */
static void persist_check_load_lengths_aliases(persist_load_fixture_t* fixture)
{
  for (uint32_t length = 0U; length < fixture->need; ++length) {
    TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                   ra8_ftl_checkpoint_load(&fixture->ftl, fixture->good, length));
    persist_load_expect_state(fixture);
  }
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->good, fixture->need + 1U));
  persist_load_expect_state(fixture);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ftl_checkpoint_load(&fixture->ftl, (const uint8_t*)fixture->map, fixture->need));
  persist_load_expect_state(fixture);
  ra8_ftl_t alias_ftl = fixture->ftl;
  alias_ftl.scratch   = (uint8_t*)fixture->map;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&alias_ftl, fixture->good, fixture->need));
  persist_load_expect_state(fixture);
}

/** @brief Exercise identity, version, length, and CRC wire guards. */
static void persist_check_load_identity_crc(persist_load_fixture_t* fixture)
{
  (void)memset(fixture->edit, 0, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le16(&fixture->edit[k_persist_off_version], 2U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  (void)memcpy(fixture->edit, "1LTF", 4U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  (void)memcpy(fixture->edit, "FTL1", 4U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  fixture->edit[k_persist_off_map] ^= 1U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  fixture->edit[fixture->need - 1U] ^= 1U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le32(&fixture->edit[k_persist_off_total], fixture->need + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
}

/** @brief Exercise semantic range, uniqueness, and pblock-state guards. */
static void persist_check_load_semantics(persist_load_fixture_t* fixture)
{
  const uint32_t pblock_offset = (uint32_t)k_persist_header_bytes +
                                 ((uint32_t)k_persist_logical * (uint32_t)k_persist_map_bytes);
  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le16(&fixture->edit[k_persist_off_map], (uint16_t)k_persist_phys);
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le16(&fixture->edit[k_persist_off_map], 0U);
  persist_put_le16(&fixture->edit[k_persist_off_map + k_persist_map_bytes], 0U);
  fixture->edit[pblock_offset + k_persist_off_pb_state] = (uint8_t)k_ra8_ftl_pstate_live;
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  persist_put_le16(&fixture->edit[k_persist_off_map], 0U);
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  fixture->edit[pblock_offset + k_persist_off_pb_state] = (uint8_t)k_ra8_ftl_pstate_live;
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);

  (void)memcpy(fixture->edit, fixture->good, fixture->need);
  fixture->edit[pblock_offset + k_persist_off_pb_state] = (uint8_t)k_persist_invalid_state;
  persist_refresh_crc(fixture->edit, fixture->need);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ftl_checkpoint_load(&fixture->ftl, fixture->edit, fixture->need));
  persist_load_expect_state(fixture);
}

/**
 * @test ra8_ftl_checkpoint_load rejects malformed input transactionally
 *
 * @par MC/DC:
 * Every wire guard is exercised independently: null/uninitialised handles,
 * every truncation, trailing bytes, aliasing, magic/version/legacy identity,
 * header/payload CRC corruption, geometry, map bounds/duplicates, and pblock
 * state/reference invariants. Each rejection rechecks both live tables.
 */
static void test_persist_load_errors(void)
{
  TEST_BEGIN("ftl checkpoint_load errors");
  ra8_ftl_t blank = {};
  uint8_t   buf[(size_t)k_persist_ckbuf];
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ftl_checkpoint_load(nullptr, buf, (uint32_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ftl_checkpoint_load(&blank, nullptr, (uint32_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ftl_checkpoint_load(&blank, buf, (uint32_t)sizeof(buf)));

  persist_load_fixture_t fixture;
  persist_load_fixture_init(&fixture);
  persist_check_load_lengths_aliases(&fixture);
  persist_check_load_identity_crc(&fixture);
  persist_check_load_semantics(&fixture);
  persist_check_load_geometry(&fixture);
  TEST_END("ftl checkpoint_load errors");
}

/* =============================================================================
 * Power-cycle survival round-trip
 * =============================================================================
 */

/**
 * @brief Initialise the FTL at the reference geometry and expose it as a
 * blockdev.
 * @param[out]    ftl     FTL handle to initialise.
 * @param[in,out] fake    Backing block device.
 * @param[out]    map     Logical->physical map table.
 * @param[out]    pb      Physical-block table.
 * @param[out]    scratch Scratch block buffer.
 * @param[out]    bd      Receives the FTL block-device facade.
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
 * value -- and only ::ra8_ftl_checkpoint_load restores it so the data
 * reappears.
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

  /* Naive re-open has lost the map: logical block 2 now reads the erase value.
   */
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

int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  test_persist_phys_of();
  test_persist_size();
  test_persist_golden_wire();
  test_persist_save_errors();
  test_persist_load_errors();
  test_persist_power_cycle_roundtrip();
  return 0;
}
