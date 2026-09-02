/**
 * @file test_ra8_shelf_classify.c
 * @brief Host test: the ereader_shelf finds long-named books on the card (#633).
 *
 * @details
 * Before #600 gave `ra8_fs` VFAT long-name write, the shelf classified 8.3
 * truncations (`.RBK`, `.EPB`). #633 migrated it to the real long extensions
 * while keeping the legacy ones so existing cards still resolve.
 *
 * This exercises the SAME classifier the shelf ships
 * (`sh_book_classify`, shared through the board-free `sh_classify.h`) and proves
 * discovery round-trips through a mount: books written to a FAT volume under
 * their long names -- plus one legacy 8.3 name -- are enumerated by
 * `ra8_fs_listdir`, classified into the right format, and open by that name,
 * while a non-book file is skipped.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Test] {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_log.h"
#include "sh_classify.h"
#include "unity_minimal.h"

/** @brief No-op log sink so ra8_fs failure logs never touch host-unmapped ITM. */
static void test_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @enum shc_disk_t
 * @brief RAM block-device geometry (4 MiB -> FAT16).
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,  /**< Bytes per sector.                  */
  k_disk_blocks     = 8192U, /**< 4 MiB volume (auto-detects FAT16). */
} shc_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if ((lba + (uint64_t)count) > (uint64_t)disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &disk->bytes[(size_t)lba * (size_t)k_disk_block_size],
               (size_t)count * (size_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if ((lba + (uint64_t)count) > (uint64_t)disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(&disk->bytes[(size_t)lba * (size_t)k_disk_block_size],
               buf,
               (size_t)count * (size_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

/** @brief Format a fresh FAT16 RAM volume and return a mounted handle. */
static ra8_fs_mount_t* fresh_volume(void)
{
  /** FAT16 storage and backend retained for the mounted volume's lifetime. */
  static uint8_t                s_disk_bytes[(size_t)k_disk_blocks * (size_t)k_disk_block_size];
  static const ra8_fs_backend_t backend = {
    .read_block   = mem_read,
    .write_block  = mem_write,
    .get_capacity = mem_capacity,
    .ctx          = &s_disk,
  };
  (void)memset(s_disk_bytes, 0, sizeof(s_disk_bytes));
  s_disk.block_count = (uint32_t)k_disk_blocks;
  s_disk.bytes       = s_disk_bytes;

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "SHELF";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&backend, &opts));

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&backend, &mount));
  return mount;
}

static void teardown(ra8_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  s_disk.bytes = nullptr;
}

/* --- listdir collector mirroring the shelf's scan discipline --------------- */

/**
 * @enum shc_scan_cap_t
 * @brief Scan collector sizing.
 */
typedef enum : uint32_t {
  k_scan_max  = 16U,  /**< Max entries collected.      */
  k_scan_name = 128U, /**< Per-name buffer (>= shelf). */
} shc_scan_cap_t;

typedef struct {
  char          name[k_scan_max][k_scan_name]; /**< Classified book names. */
  sh_book_fmt_t fmt[k_scan_max];               /**< Their formats.         */
  uint32_t      count;                         /**< Books found.           */
} shc_scan_t;

/** @brief listdir callback: keep every entry the real shelf classifier accepts. */
static void shc_scan_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)size;
  shc_scan_t*   sc   = (shc_scan_t*)ctx;
  const uint8_t skip = (uint8_t)k_ra8_fs_attr_directory | (uint8_t)k_ra8_fs_attr_volume_id;
  sh_book_fmt_t fmt  = k_sh_fmt_rabook;
  if (((attr & skip) != 0U) || !sh_book_classify(name, &fmt) ||
      (strlen(name) >= (size_t)k_scan_name) || (sc->count >= (uint32_t)k_scan_max)) {
    return;
  }
  (void)strncpy(sc->name[sc->count], name, (size_t)k_scan_name - 1U);
  sc->fmt[sc->count] = fmt;
  sc->count++;
}

/** @brief Index of the collected entry named @p want, or -1 if not found. */
static int32_t shc_find(const shc_scan_t* sc, const char* want)
{
  for (uint32_t i = 0U; i < sc->count; ++i) {
    if (strcmp(sc->name[i], want) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

/** @brief Write @p body to @p name on @p mount. */
static void put_file(ra8_fs_mount_t* mount, const char* name, const char* body)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(mount, name, (const uint8_t*)body, (uint32_t)strlen(body)));
}

/* --- Tests ----------------------------------------------------------------- */

/**
 * @test test_classify_extensions
 * @brief The real classifier accepts the long forms + the legacy 8.3 forms,
 *        is case-insensitive, and rejects non-books.
 *
 * @par MC/DC:
 * `sh_book_classify` selects rabook/epub through `A || B` extension
 * alternatives (long form `|| ` legacy 8.3 form). Each arm is driven
 * independently -- `.rabook` and `.rbk`, `.epub` and `.epb` -- plus
 * non-matching names (`.txt`, extension-only, no-dot) for the all-false reject,
 * so each condition independently determines the classification.
 */
static void test_classify_extensions(void)
{
  TEST_BEGIN("shelf: classify long + legacy + comic extensions");
  sh_book_fmt_t fmt = k_sh_fmt_epub;

  /* Long forms the tools now write. */
  TEST_ASSERT(sh_book_classify("Meditations.rabook", &fmt) && (fmt == k_sh_fmt_rabook));
  TEST_ASSERT(sh_book_classify("Pride and Prejudice.epub", &fmt) && (fmt == k_sh_fmt_epub));

  /* Legacy 8.3 truncations still resolve (case-insensitive vs upper-cased 8.3). */
  TEST_ASSERT(sh_book_classify("BOOK01.RBK", &fmt) && (fmt == k_sh_fmt_rabook));
  TEST_ASSERT(sh_book_classify("BOOK01.EPB", &fmt) && (fmt == k_sh_fmt_epub));

  /* Comics -- never truncated (already 3-char), both cases. */
  TEST_ASSERT(sh_book_classify("Akira.cbz", &fmt) && (fmt == k_sh_fmt_cbz));
  TEST_ASSERT(sh_book_classify("SERIES.CBR", &fmt) && (fmt == k_sh_fmt_cbr));
  TEST_ASSERT(sh_book_classify("tape.cbt", &fmt) && (fmt == k_sh_fmt_cbt));

  /* Non-books and edge cases are rejected. */
  TEST_ASSERT(!sh_book_classify("README.txt", &fmt));
  TEST_ASSERT(!sh_book_classify(".rabook", &fmt)); /* extension only, no stem */
  TEST_ASSERT(!sh_book_classify("rabook", &fmt));  /* no dot                  */

  TEST_END("shelf: classify long + legacy + comic extensions");
}

/**
 * @test test_discovery_roundtrips_through_mount
 * @brief Long-named books written to the card are enumerated, classified, and
 *        open by the same name; a legacy name still resolves; a non-book is
 *        skipped.
 *
 * @par MC/DC:
 * No compound decision under test; this is an end-to-end discovery round-trip
 * (write -> listdir -> classify -> open) proving each on-card name resolves.
 */
static void test_discovery_roundtrips_through_mount(void)
{
  TEST_BEGIN("shelf: long book names discovered + openable through a mount");
  ra8_fs_mount_t* mount = fresh_volume();

  put_file(mount, "Meditations.rabook", "RABOOK1-body-a");
  put_file(mount, "Pride and Prejudice.epub", "epub-body-b");
  put_file(mount, "BOOK01.RBK", "legacy-body-c"); /* pre-#633 card content */
  put_file(mount, "notes.txt", "not-a-book");     /* must be skipped       */

  shc_scan_t sc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mount, "/", shc_scan_cb, &sc));

  /* Exactly the three books were classified; the .txt was skipped. */
  TEST_ASSERT_EQ(3U, sc.count);
  TEST_ASSERT(shc_find(&sc, "notes.txt") < 0);

  const int32_t i_rab = shc_find(&sc, "Meditations.rabook");
  const int32_t i_epb = shc_find(&sc, "Pride and Prejudice.epub");
  const int32_t i_leg = shc_find(&sc, "BOOK01.RBK");
  TEST_ASSERT(i_rab >= 0);
  TEST_ASSERT(i_epb >= 0);
  TEST_ASSERT(i_leg >= 0);
  TEST_ASSERT_EQ(k_sh_fmt_rabook, sc.fmt[i_rab]);
  TEST_ASSERT_EQ(k_sh_fmt_epub, sc.fmt[i_epb]);
  TEST_ASSERT_EQ(k_sh_fmt_rabook, sc.fmt[i_leg]);

  /* Every discovered name opens -- the discovery name is the on-card name. */
  for (uint32_t i = 0U; i < sc.count; ++i) {
    ra8_fs_file_t* file = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, sc.name[i], k_ra8_fs_mode_read, &file));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  }

  teardown(mount);
  TEST_END("shelf: long book names discovered + openable through a mount");
}

int main(void)
{
  ra8_log_set_byte_sink(test_log_sink, nullptr);
  test_classify_extensions();
  test_discovery_roundtrips_through_mount();
  return 0;
}
