/**
 * @file test_lx_fs_backend.c
 * @brief Host test: the LevelX -> ra8_fs block-device backend, end to end.
 *
 * @details
 * Compiles the REAL vendored LevelX NOR core (`LX_STANDALONE_ENABLE`, no
 * ThreadX) over the RAM NOR fake (`tests/mocks/lx_nor_fake_ram.c`), binds it
 * through `port/levelx/src/lx_fs_backend.c`, and drives the first-party
 * `ra8_fs` FAT implementation on top -- the exact stack the
 * `threadx_fs_demo` / `threadx_fs_levelx_demo` HIL apps run on the EK-RA8D2's
 * OSPI flash, minus only the xSPI silicon underneath LevelX.
 *
 * Covered:
 *   - `lx_fs_backend_bind()` precondition guards (NULL args, unopened flash).
 *   - Capacity reporting: `total_blocks * (physical_sectors_per_block - 1)`
 *     512-byte sectors, the FAT-usable window.
 *   - The erased-space contract: reading a logical sector LevelX has never
 *     mapped fills the buffer with 0xFF and SUCCEEDS (unmapped wear-levelled
 *     NOR is erased space, not an I/O error).
 *   - Range and fault handling: out-of-range reads/writes are rejected, an
 *     injected LevelX write failure surfaces as an error.
 *   - The full demo roundtrip: `ra8_fs_format` -> `ra8_fs_mount` ->
 *     `ra8_fs_write_file` x2 -> `ra8_fs_listdir` -> read + verify ->
 *     `ra8_fs_unlink` -> stat confirms gone -> `ra8_fs_unmount`, over the
 *     LevelX-backed backend -- the host twin of the two ported HIL demos.
 *
 * @author Brighton Sikarskie
 * @date 2026-08-10
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "lx_api.h"
#include "lx_fs_backend.h"
#include "lx_nor_fake_ram.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/** @brief Fixture constants (geometry is read out of the LevelX control block). */
typedef enum : uint32_t {
  k_lxfsbe_sector_bytes   = 512U,  /**< Backend block size.                  */
  k_lxfsbe_erased_byte    = 0xFFU, /**< NOR erased fill value.               */
  k_lxfsbe_probe_sector   = 7U,    /**< Arbitrary in-range sector for R/W.   */
  k_lxfsbe_pattern_seed   = 0xA5U, /**< First byte of the write pattern.     */
  k_lxfsbe_read_chunk     = 64U,   /**< Roundtrip read-back chunk size.      */
  k_lxfsbe_max_root_names = 8U,    /**< Listdir capture capacity.            */
  k_lxfsbe_name_cap       = 32U,   /**< Listdir captured-name byte capacity. */
} lxfsbe_fixture_t;

/** @brief LevelX control block under test (re-formatted per scenario). */
static LX_NOR_FLASH s_nor_flash;

/**
 * @brief The usable-sector count the backend must advertise for ::s_nor_flash.
 *
 * @details The shim's contract: LevelX's ``physical_sectors_per_block`` (which
 * already excludes the per-block metadata sector) minus one more sector per
 * block of wear-levelling headroom, times ``total_blocks``. Derived from the
 * open control block rather than hardcoded because the per-block sector count
 * depends on ``sizeof(ULONG)`` (128-word target sectors vs 64-word LP64 host
 * sectors for the same 512 bytes).
 *
 * @return Expected `get_capacity` block count.
 * @pre ::s_nor_flash is open.
 * @post No state is modified.
 */
static uint64_t expected_usable_sectors(void)
{
  uint64_t ppb = (uint64_t)s_nor_flash.lx_nor_flash_physical_sectors_per_block;
  return (uint64_t)s_nor_flash.lx_nor_flash_total_blocks * (ppb - 1U);
}

/** @brief Names collected by the listdir callback in the roundtrip scenario. */
static char s_seen_names[k_lxfsbe_max_root_names][k_lxfsbe_name_cap];

/** @brief Number of names collected by the listdir callback. */
static uint32_t s_seen_count;

/**
 * @brief Fresh LevelX world: wipe the RAM NOR, format + open a partition.
 *
 * @pre The RAM NOR fake is linked as the LevelX driver-init callback.
 * @post ::s_nor_flash is open over an all-erased backing.
 */
static void reset_levelx(void)
{
  (void)memset(&s_nor_flash, 0, sizeof(s_nor_flash));
  lx_nor_fake_ram_wipe();
  lx_nor_flash_initialize();
  TEST_ASSERT_EQ(
    (UINT)LX_SUCCESS,
    lx_nor_flash_format(&s_nor_flash, (CHAR*)"fake_nor", lx_nor_fake_ram_init, LX_NULL));
  TEST_ASSERT_EQ((UINT)LX_SUCCESS,
                 lx_nor_flash_open(&s_nor_flash, (CHAR*)"fake_nor", lx_nor_fake_ram_init));
}

/**
 * @brief Bind guards: NULL flash, NULL out, unopened flash.
 *
 * @par MC/DC: not applicable -- the guards are sequential single-condition
 * early returns (`RA8_CHECK_NULL_PTR` per argument, then the zero-capacity
 * check), no compound boolean decisions.
 */
static void test_bind_rejects_bad_args(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: bind precondition guards");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, lx_fs_backend_bind(nullptr, &be));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, lx_fs_backend_bind(&s_nor_flash, nullptr));
  LX_NOR_FLASH unopened = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, lx_fs_backend_bind(&unopened, &be));
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));
  TEST_ASSERT(be.read_block != nullptr);
  TEST_ASSERT(be.write_block != nullptr);
  TEST_ASSERT(be.get_capacity != nullptr);
  TEST_ASSERT(be.erase_blocks == nullptr);
  TEST_END("lx_fs_backend: bind precondition guards");
}

/**
 * @brief Capacity: the FAT-usable window and the fixed 512-byte block size.
 *
 * @par MC/DC: not applicable -- sequential null guards and a single-condition
 * zero-capacity check.
 */
static void test_capacity_reports_usable_window(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: capacity = blocks * (phys_per_block - 1)");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));
  uint64_t block_count = 0U;
  uint32_t block_size  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.get_capacity(be.ctx, nullptr, &block_size));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.get_capacity(be.ctx, &block_count, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, be.get_capacity(be.ctx, &block_count, &block_size));
  TEST_ASSERT_EQ(expected_usable_sectors(), block_count);
  TEST_ASSERT(block_count > 0U);
  TEST_ASSERT_EQ(k_lxfsbe_sector_bytes, block_size);
  TEST_END("lx_fs_backend: capacity = blocks * (phys_per_block - 1)");
}

/**
 * @brief A never-written logical sector reads back successfully as erased space.
 *
 * @details Two branches of the shim's read dispatch:
 * 1. With free physical sectors available, LevelX itself serves an unmapped
 *    read (`LX_SUCCESS`, allocate-on-read): the shim must pass it through as
 *    `k_ra8_ok`. The byte PATTERN of that path belongs to the media model
 *    (the RAM fake stores the 32-bit `0xFFFFFFFF` erased word in host-sized
 *    `ULONG`s), so only the status is asserted here.
 * 2. With `lx_nor_flash_free_physical_sectors` forced to zero -- a full
 *    flash -- LevelX cannot allocate and returns `LX_SECTOR_NOT_FOUND`; the
 *    shim must serve the shim-owned erased fill: `k_ra8_ok` and every byte
 *    `0xFF`, because unmapped wear-levelled NOR is erased space, not an I/O
 *    error. This path is byte-exact on host and target alike.
 *
 * @par MC/DC: not applicable -- the read loop's status dispatch is a
 * three-way `if / else if / else` on one LevelX status value per branch, no
 * compound boolean decisions.
 */
static void test_unmapped_read_serves_erased_fill(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: unmapped sector reads as erased space");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));
  uint8_t buf[k_lxfsbe_sector_bytes];
  (void)memset(buf, 0, sizeof(buf));
  /* Branch 1: LevelX allocate-on-read path passes through as success. */
  TEST_ASSERT_EQ(k_ra8_ok, be.read_block(be.ctx, (uint64_t)k_lxfsbe_probe_sector, 1U, buf));
  /* Branch 2: full flash -> LX_SECTOR_NOT_FOUND -> shim's 0xFF fill. */
  ULONG saved_free                               = s_nor_flash.lx_nor_flash_free_physical_sectors;
  s_nor_flash.lx_nor_flash_free_physical_sectors = 0U;
  (void)memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_ok, be.read_block(be.ctx, (uint64_t)k_lxfsbe_probe_sector + 1U, 1U, buf));
  s_nor_flash.lx_nor_flash_free_physical_sectors = saved_free;
  bool all_erased                                = true;
  for (uint32_t i = 0U; i < (uint32_t)k_lxfsbe_sector_bytes; i++) {
    if (buf[i] != (uint8_t)k_lxfsbe_erased_byte) {
      all_erased = false;
    }
  }
  TEST_ASSERT(all_erased);
  TEST_END("lx_fs_backend: unmapped sector reads as erased space");
}

/**
 * @brief Write a sector, read it back byte-identical through the backend.
 *
 * @par MC/DC: not applicable -- straight-line write-then-read with per-call
 * single-condition status checks.
 */
static void test_write_read_roundtrip(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: sector write/read roundtrip");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));
  uint8_t wr[k_lxfsbe_sector_bytes];
  uint8_t rd[k_lxfsbe_sector_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_lxfsbe_sector_bytes; i++) {
    wr[i] = (uint8_t)((i + (uint32_t)k_lxfsbe_pattern_seed) & (uint32_t)k_lxfsbe_erased_byte);
  }
  TEST_ASSERT_EQ(k_ra8_ok, be.write_block(be.ctx, (uint64_t)k_lxfsbe_probe_sector, 1U, wr));
  (void)memset(rd, 0, sizeof(rd));
  TEST_ASSERT_EQ(k_ra8_ok, be.read_block(be.ctx, (uint64_t)k_lxfsbe_probe_sector, 1U, rd));
  TEST_ASSERT_EQ(0, memcmp(wr, rd, sizeof(wr)));
  TEST_END("lx_fs_backend: sector write/read roundtrip");
}

/**
 * @brief Range guards + injected write fault surface as errors.
 *
 * @par MC/DC: not applicable -- each guard is one relational comparison
 * (`lba + count > usable`, one condition), and the fault path is a single
 * status test.
 */
static void test_range_and_fault_paths(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: range guards + injected write fault");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));
  uint8_t buf[k_lxfsbe_sector_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.read_block(nullptr, 0U, 1U, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.read_block(be.ctx, 0U, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.write_block(nullptr, 0U, 1U, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, be.write_block(be.ctx, 0U, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, be.read_block(be.ctx, expected_usable_sectors(), 1U, buf));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 be.write_block(be.ctx, expected_usable_sectors(), 1U, buf));
  lx_nor_fake_ram_fail_writes(1U);
  TEST_ASSERT(be.write_block(be.ctx, (uint64_t)k_lxfsbe_probe_sector, 1U, buf) != k_ra8_ok);
  TEST_END("lx_fs_backend: range guards + injected write fault");
}

/**
 * @brief ra8_fs_listdir capture callback for the roundtrip scenario.
 *
 * @param[in] name Entry name.
 * @param[in] attr FAT attribute bits (unused).
 * @param[in] size Entry byte size (unused).
 * @param[in] ctx  Unused cookie.
 * @pre Invoked by ra8_fs_listdir only.
 * @post The name is recorded in ::s_seen_names (up to the fixture capacity).
 */
static void roundtrip_list_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  if (s_seen_count >= (uint32_t)k_lxfsbe_max_root_names) {
    return;
  }
  (void)strncpy(s_seen_names[s_seen_count], name, (size_t)k_lxfsbe_name_cap - 1U);
  s_seen_names[s_seen_count][k_lxfsbe_name_cap - 1U] = '\0';
  s_seen_count++;
}

/**
 * @brief ASCII-case-folding string equality (8.3 listings may be upper-cased).
 *
 * @param[in] a First NUL-terminated string.
 * @param[in] b Second NUL-terminated string.
 * @return true when the strings match ignoring ASCII case.
 * @pre Both arguments are NUL-terminated.
 * @post No state is modified.
 */
static bool ascii_ieq(const char* a, const char* b)
{
  uint32_t i = 0U;
  while (a[i] != '\0') {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A') {
      if (ca <= 'Z') {
        ca = (char)(ca + ('a' - 'A'));
      }
    }
    if (cb >= 'A') {
      if (cb <= 'Z') {
        cb = (char)(cb + ('a' - 'A'));
      }
    }
    if (ca != cb) {
      return false;
    }
    i++;
  }
  return b[i] == '\0';
}

/**
 * @brief Report whether the listdir capture saw @p want (case-insensitive 8.3).
 *
 * @param[in] want Name to look for.
 * @return true when a captured name matches.
 * @pre ::s_seen_count entries are populated.
 * @post No state is modified.
 */
static bool roundtrip_saw(const char* want)
{
  for (uint32_t i = 0U; i < s_seen_count; i++) {
    if (ascii_ieq(s_seen_names[i], want)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief The ported HIL demos' flow, host-side: format/mount/write/list/
 *        read/unlink/stat/unmount over LevelX.
 *
 * @par MC/DC: not applicable -- a straight-line integration scenario;
 * every branch taken is a TEST_ASSERT on one call's status.
 */
static void test_full_fat_roundtrip_over_levelx(void)
{
  reset_levelx();
  TEST_BEGIN("lx_fs_backend: full ra8_fs FAT roundtrip over LevelX");
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &be));

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "LXFSBE";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be, &opts));

  ra8_fs_mount_t* mnt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &mnt));

  static const char payload[] = "ra8_fs FAT on LevelX (host twin of the HIL demos)";
  uint32_t          len       = (uint32_t)strlen(payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mnt, "/readme.txt", (const uint8_t*)payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mnt, "/scratch.txt", (const uint8_t*)payload, len));

  s_seen_count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mnt, "/", roundtrip_list_cb, nullptr));
  TEST_ASSERT(roundtrip_saw("readme.txt"));
  TEST_ASSERT(roundtrip_saw("scratch.txt"));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mnt, "/readme.txt", k_ra8_fs_mode_read, &file));
  uint8_t  rd[k_lxfsbe_sector_bytes] = {};
  uint32_t off                       = 0U;
  uint32_t got                       = 0U;
  do {
    got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, &rd[off], (uint32_t)k_lxfsbe_read_chunk, &got));
    off += got;
  } while (got > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(len, off);
  TEST_ASSERT_EQ(0, memcmp(payload, rd, (size_t)len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(mnt, "/scratch.txt"));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(mnt, "/scratch.txt", &st));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mnt));
  TEST_END("lx_fs_backend: full ra8_fs FAT roundtrip over LevelX");
}

int main(void)
{
  test_bind_rejects_bad_args();
  test_capacity_reports_usable_window();
  test_unmapped_read_serves_erased_fill();
  test_write_read_roundtrip();
  test_range_and_fault_paths();
  test_full_fat_roundtrip_over_levelx();
  return 0;
}
