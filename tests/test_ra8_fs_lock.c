/*
 */
/**
 * @file test_ra8_fs_lock.c
 * @brief Tests for the optional `ra8_fs` lock seam (#608).
 *
 * @details
 * The seam's whole claim is that every public entry point takes the lock on
 * the way in and drops it on the way out -- on the success path and on every
 * error path -- so a fake lock that counts calls and tracks nesting depth is
 * enough to prove or disprove it. The fake asserts three things at once:
 *
 *   - `acquired == released` after each call: nothing leaks the lock, which is
 *     the failure mode that hangs an RTOS-world app the first time an argument
 *     is NULL rather than the hundredth time it is not;
 *   - `max_depth <= 1`: no entry point takes the lock twice. That is what would
 *     deadlock a plain non-recursive mutex, and `ra8_fs_write_file()` is the
 *     one that could: it drives open / write / close, and it must reach the
 *     unlocked bodies of all three;
 *   - the cookie handed to the callbacks is the one that was installed.
 *
 * The default -- no lock installed -- is asserted too: a seam that costs the
 * bare-metal world a callback it never asked for is not the seam #608 wanted.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum lock_bpb_field_t
 * @brief Byte offsets of the BPB fields this fixture writes, per the FAT spec.
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec.                        */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus.                        */
  k_bpb_off_rsvd_sec_cnt  = 14U,  /**< BPB_RsvdSecCnt.                        */
  k_bpb_off_num_fats      = 16U,  /**< BPB_NumFATs.                           */
  k_bpb_off_root_ent_cnt  = 17U,  /**< BPB_RootEntCnt.                        */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16.                          */
  k_bpb_off_fat_sz16      = 22U,  /**< BPB_FATSz16.                           */
  k_bpb_off_sig_lo        = 510U, /**< Low byte of the 0xAA55 boot signature. */
  k_bpb_off_sig_hi        = 511U, /**< Its high byte.                         */
} lock_bpb_field_t;

/**
 * @enum lock_fixture_t
 * @brief Magic values and sizes the synthetic FAT16 volume is built from.
 */
typedef enum : uint32_t {
  k_bpb_sig_lo        = 0x55U,      /**< Boot-signature low byte.        */
  k_bpb_sig_hi        = 0xAAU,      /**< Boot-signature high byte.       */
  k_byte_mask         = 0xFFU,      /**< Low-byte mask for put16().      */
  k_disk_block_size   = 512U,       /**< Bytes per block.                */
  k_disk_blocks       = 8U * 1024U, /**< Blocks in the synthetic volume. */
  k_fat_sectors       = 32U,        /**< Sectors per FAT.                */
  k_root_entries      = 16U,        /**< Root-directory entries.         */
  k_num_fats          = 2U,         /**< FAT copies.                     */
  k_payload_bytes     = 48U,        /**< Bytes in the fixture's file.    */
  k_payload_seed      = 0x3CU,      /**< Fill seed for that payload.     */
  k_payload_stride    = 7U,         /**< Fill stride for that payload.   */
  k_expected_brackets = 1U,         /**< Acquire count for one API call. */
} lock_fixture_t;

/**
 * @struct mem_disk_t
 * @brief RAM-backed block device the fixture volume lives in.
 */
typedef struct {
  uint8_t* bytes;       /**< Backing store.       */
  uint32_t block_count; /**< Blocks in the store. */
  uint32_t byte_count;  /**< Bytes in the store.  */
} mem_disk_t;

/** @brief The one synthetic disk every test in this file mounts. */
static mem_disk_t s_disk = {};

/** @brief Backend read: copy `count` blocks out of the RAM disk. */
static ra8_err_t mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

/** @brief Backend write: copy `count` blocks into the RAM disk. */
static ra8_err_t mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

/** @brief Backend capacity: the RAM disk's size in 512-byte blocks. */
static ra8_err_t mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

/** @brief The backend handed to every mount in this file. */
static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/** @brief Store a little-endian 16-bit BPB field. */
static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & (uint32_t)k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & (uint32_t)k_byte_mask);
}

/** @brief Allocate the RAM disk and hand-build a mountable FAT16 BPB in it. */
static void build_volume(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_disk_blocks * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = (uint8_t)k_num_fats;
  put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, (uint16_t)k_root_entries);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks);
  put16(bpb, (uint32_t)k_bpb_off_fat_sz16, (uint16_t)k_fat_sectors);
  bpb[k_bpb_off_sig_lo] = (uint8_t)k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = (uint8_t)k_bpb_sig_hi;
}

/** @brief Release the RAM disk. */
static void free_volume(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/**
 * @struct fake_lock_t
 * @brief What the fake mutex records so the bracketing can be asserted.
 */
typedef struct {
  uint32_t acquired;  /**< Times `acquire` ran.                        */
  uint32_t released;  /**< Times `release` ran.                        */
  int32_t  depth;     /**< Currently-held count (must never exceed 1). */
  int32_t  max_depth; /**< Deepest nesting observed.                   */
  uint32_t wrong_ctx; /**< Callbacks that saw the wrong cookie.        */
} fake_lock_t;

/** @brief The single fake lock instance; also its own cookie. */
static fake_lock_t s_fake = {};

/** @brief Fake `acquire`: count it and track nesting. */
static void fake_acquire(void* ctx)
{
  fake_lock_t* fl = (fake_lock_t*)ctx;
  if (fl != &s_fake) {
    s_fake.wrong_ctx++;
    return;
  }
  fl->acquired++;
  fl->depth++;
  if (fl->depth > fl->max_depth) {
    fl->max_depth = fl->depth;
  }
}

/** @brief Fake `release`: count it and unwind the nesting. */
static void fake_release(void* ctx)
{
  fake_lock_t* fl = (fake_lock_t*)ctx;
  if (fl != &s_fake) {
    s_fake.wrong_ctx++;
    return;
  }
  fl->released++;
  fl->depth--;
}

/** @brief The binding installed by every test that needs one. */
static const ra8_fs_lock_t s_lock_binding = {
  .acquire = fake_acquire,
  .release = fake_release,
  .ctx     = &s_fake,
};

/** @brief Zero the recorder so the next call is measured on its own. */
static void reset_counts(void)
{
  s_fake.acquired  = 0U;
  s_fake.released  = 0U;
  s_fake.depth     = 0;
  s_fake.max_depth = 0;
}

/**
 * @brief Assert that exactly @p want brackets ran and none of them leaked.
 *
 * @param[in] want  Expected acquire (and release) count.
 * @param[in] label Call being judged, for the failure message.
 */
static void expect_brackets(uint32_t want, const char* label)
{
  if (s_fake.acquired != want) {
    TEST_FAIL_FMT("%s: acquired %u, wanted %u", label, s_fake.acquired, want);
  }
  if (s_fake.released != s_fake.acquired) {
    TEST_FAIL_FMT("%s: acquired %u but released %u -- the lock leaked",
                  label,
                  s_fake.acquired,
                  s_fake.released);
  }
  if (s_fake.depth != 0) {
    TEST_FAIL_FMT("%s: still held on return (depth %d)", label, s_fake.depth);
  }
  if (s_fake.max_depth > 1) {
    TEST_FAIL_FMT("%s: took the lock %d deep -- a non-recursive mutex deadlocks here",
                  label,
                  s_fake.max_depth);
  }
  if (s_fake.wrong_ctx != 0U) {
    TEST_FAIL_FMT("%s: a callback saw a cookie other than the installed one", label);
  }
  reset_counts();
}

/** @brief Fill @p buf with a deterministic pattern. */
static void fill(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)((i * (uint32_t)k_payload_stride) + (uint32_t)k_payload_seed);
  }
}

/** @brief listdir callback that counts entries. */
static void count_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  (*(uint32_t*)ctx)++;
}

/**
 * @test test_mcdc_set_lock_validates
 *
 * @par MC/DC:
 * Decision: `if (lock->acquire == nullptr || lock->release == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_lock.c@ra8_fs_set_lock` (2 conditions). A
 * half-filled binding must be refused outright: installing one would leave the
 * library able to take a lock it has no way to drop.
 * - V1: acquire=fake_acquire, release=fake_release -> A=F, B=F -> installed
 *   (control: both conditions false).
 * - V2: acquire=NULL,        release=fake_release -> A=T -> invalid_arg
 *   (varies A only; short-circuits before B).
 * - V3: acquire=fake_acquire, release=NULL        -> A=F, B=T -> invalid_arg
 *   (varies B only).
 * V1+V2 prove `acquire` independently affects the outcome; V1+V3 prove the
 * same for `release`. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * A NULL @p lock is a separate single-condition guard on the line above, and
 * is asserted here too: it is the documented way to remove a binding.
 */
static void test_mcdc_set_lock_validates(void)
{
  TEST_BEGIN("ra8_fs_set_lock MC/DC: a half-filled binding is refused");

  const ra8_fs_lock_t no_acquire = {.acquire = nullptr, .release = fake_release, .ctx = &s_fake};
  const ra8_fs_lock_t no_release = {.acquire = fake_acquire, .release = nullptr, .ctx = &s_fake};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_set_lock(&no_acquire)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_set_lock(&no_release)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));          /* V1 */

  /* A rejected binding must not have been installed: the successful V1 install
   * is the first one, so a call now brackets exactly once. */
  reset_counts();
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(nullptr, "/", &st));
  expect_brackets((uint32_t)k_expected_brackets, "stat(null)");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr)); /* removal */
  TEST_END("ra8_fs_set_lock MC/DC: a half-filled binding is refused");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- with no binding installed the callbacks
 * must not run at all, which is the bare-metal default the seam must not cost)
 */
static void test_no_lock_is_the_default(void)
{
  TEST_BEGIN("no binding installed -> no callback");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  reset_counts();

  build_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  TEST_ASSERT_EQ(0U, s_fake.acquired);
  TEST_ASSERT_EQ(0U, s_fake.released);
  TEST_END("no binding installed -> no callback");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the volume- and directory-level entry
 * points each take the lock exactly once on their success path and give it back)
 */
static void test_volume_entry_points_bracket(void)
{
  TEST_BEGIN("volume + directory calls are bracketed exactly once");
  build_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "LOCKED"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  expect_brackets((uint32_t)k_expected_brackets, "format");

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  expect_brackets((uint32_t)k_expected_brackets, "mount");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/DIR"));
  expect_brackets((uint32_t)k_expected_brackets, "mkdir");

  uint8_t payload[k_payload_bytes] = {};
  fill(payload, (uint32_t)k_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/A.BIN", payload, (uint32_t)k_payload_bytes));
  reset_counts();

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/A.BIN", &st));
  expect_brackets((uint32_t)k_expected_brackets, "stat");

  uint32_t entries = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &entries));
  expect_brackets((uint32_t)k_expected_brackets, "listdir");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "/A.BIN", "/B.BIN"));
  expect_brackets((uint32_t)k_expected_brackets, "rename");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/B.BIN"));
  expect_brackets((uint32_t)k_expected_brackets, "unlink");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  expect_brackets((uint32_t)k_expected_brackets, "unmount");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  free_volume();
  TEST_END("volume + directory calls are bracketed exactly once");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the same claim for the per-file entry
 * points, which is where a leaked lock would be least visible)
 */
static void test_file_entry_points_bracket(void)
{
  TEST_BEGIN("file calls are bracketed exactly once");
  build_volume();
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "LOCKED"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  uint8_t payload[k_payload_bytes] = {};
  fill(payload, (uint32_t)k_payload_bytes);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/A.BIN", k_ra8_fs_mode_write, &f));
  expect_brackets((uint32_t)k_expected_brackets, "open");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, (uint32_t)k_payload_bytes));
  expect_brackets((uint32_t)k_expected_brackets, "write");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  expect_brackets((uint32_t)k_expected_brackets, "seek");

  uint64_t at = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &at));
  expect_brackets((uint32_t)k_expected_brackets, "tell");

  uint64_t sz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  expect_brackets((uint32_t)k_expected_brackets, "size");

  uint8_t  back[k_payload_bytes] = {};
  uint32_t got                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_payload_bytes, &got));
  expect_brackets((uint32_t)k_expected_brackets, "read");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  expect_brackets((uint32_t)k_expected_brackets, "close");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("file calls are bracketed exactly once");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- ra8_fs_write_file drives open/write/close
 * internally and must still take the lock exactly once, or a non-recursive
 * mutex deadlocks and the three steps stop being one atomic creation)
 */
static void test_write_file_takes_the_lock_once(void)
{
  TEST_BEGIN("write_file is one bracket, not four");
  build_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  uint8_t payload[k_payload_bytes] = {};
  fill(payload, (uint32_t)k_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/WHOLE.BIN", payload, (uint32_t)k_payload_bytes));
  expect_brackets((uint32_t)k_expected_brackets, "write_file");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("write_file is one bracket, not four");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the error return of every volume-level
 * entry point must still release; a lock leaked on the NULL-argument path is
 * the one that hangs an app on its first bad call)
 */
static void test_volume_error_paths_release(void)
{
  TEST_BEGIN("volume-call error returns release the lock");
  build_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_format(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "format(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "mount(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unmount(nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "unmount(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write_file(nullptr, nullptr, nullptr, 0U));
  expect_brackets((uint32_t)k_expected_brackets, "write_file(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(nullptr, nullptr, nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "listdir(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mkdir(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "mkdir(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unlink(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "unlink(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rename(nullptr, nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "rename(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(h, nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "stat(null)");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("volume-call error returns release the lock");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the same claim for the per-file entry
 * points, including a not-found open, which is an error the caller expects)
 */
static void test_file_error_paths_release(void)
{
  TEST_BEGIN("file-call error returns release the lock");
  build_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "/MISSING.BIN", k_ra8_fs_mode_read, &f));
  expect_brackets((uint32_t)k_expected_brackets, "open(missing)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_close(nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "close(null)");

  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(nullptr, nullptr, 0U, &got));
  expect_brackets((uint32_t)k_expected_brackets, "read(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write(nullptr, nullptr, 0U));
  expect_brackets((uint32_t)k_expected_brackets, "write(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_seek(nullptr, 0U));
  expect_brackets((uint32_t)k_expected_brackets, "seek(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_tell(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "tell(null)");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_size(nullptr, nullptr));
  expect_brackets((uint32_t)k_expected_brackets, "size(null)");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("file-call error returns release the lock");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- removing the binding stops the callbacks
 * for good, so a consumer can hand the filesystem back to a single-threaded
 * context without the seam still reaching for a mutex that may be gone)
 */
static void test_removing_the_binding_stops_the_calls(void)
{
  TEST_BEGIN("set_lock(nullptr) removes the binding");
  build_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(&s_lock_binding));
  reset_counts();

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_expected_brackets, s_fake.acquired);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_lock(nullptr));
  reset_counts();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(0U, s_fake.acquired);
  TEST_ASSERT_EQ(0U, s_fake.released);

  free_volume();
  TEST_END("set_lock(nullptr) removes the binding");
}

int32_t main(void)
{
  test_mcdc_set_lock_validates();
  test_no_lock_is_the_default();
  test_volume_entry_points_bracket();
  test_file_entry_points_bracket();
  test_write_file_takes_the_lock_once();
  test_volume_error_paths_release();
  test_file_error_paths_release();
  test_removing_the_binding_stops_the_calls();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_lock.c\n");
  return 0;
}
