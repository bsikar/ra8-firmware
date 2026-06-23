/**
 * @file test_ra_epub_fs.c
 * @brief End-to-end host test for the ra_fs -> ra_epub bridge (#71).
 *
 * @details
 * Proves the storage-stack acceptance for #71 on the host: a real `.epub` is
 * assembled in memory with miniz, *written to a FAT16 volume through ra_fs*
 * (over a RAM block backend -- the same mem-disk pattern as
 * `tests/test_ra_fs_fat.c`), and then opened end to end with
 * `ra_epub_open_fs()`, which reads the file back off `ra_fs` and parses it. On
 * target the only difference is the block backend (`ra_sdmmc_spi` over the SD
 * card instead of RAM), which is independently bench-validated.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra_epub.h"
#include "ra_epub_fs.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "unity_minimal.h"

/* --- RAM block device + minimal FAT16 volume (mirrors test_ra_fs_fat.c) --- */

typedef enum : uint32_t {
  k_disk_block_size = 512U,
  k_disk_blocks     = 8U * 1024U, /**< 4 MiB FAT16 volume. */
} epub_fs_disk_t;

typedef struct {
  uint8_t* bytes;
  uint32_t block_count;
  uint32_t byte_count;
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_disk_block_size], count * (uint32_t)k_disk_block_size);
  return k_ra_ok;
}

static ra_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_disk_block_size], buf, count * (uint32_t)k_disk_block_size);
  return k_ra_ok;
}

static ra_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra_ok;
}

static const ra_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void build_fat16_volume(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_disk_blocks * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1U, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks;
  TEST_ASSERT(s_disk.bytes != nullptr);
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, 11U, (uint16_t)k_disk_block_size); /* bytes/sector       */
  bpb[13] = 1U;                                 /* sectors/cluster    */
  put16(bpb, 14U, 1U);                          /* reserved sectors   */
  bpb[16] = 2U;                                 /* number of FATs     */
  put16(bpb, 17U, 16U);                         /* root dir entries   */
  put16(bpb, 19U, (uint16_t)k_disk_blocks);     /* total sectors      */
  put16(bpb, 22U, 32U);                         /* sectors/FAT        */
  bpb[510] = 0x55U;
  bpb[511] = 0xAAU;
}

/* --- A real (minimal) EPUB assembled in memory with miniz --- */

typedef enum : size_t {
  k_epub_cap = 8U * 1024U,
  k_read_cap = 8U * 1024U,
} epub_fs_cap_t;

static uint8_t s_epub[k_epub_cap];
static size_t  s_epub_len;
static uint8_t s_read[k_read_cap];

static const char* const k_mimetype = "application/epub+zip";
static const char* const k_container =
  "<?xml version=\"1.0\"?><container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Frankenstein</dc:title><dc:creator>Mary Shelley</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:gutenberg:84</dc:identifier>"
  "</metadata><manifest>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>";
static const char* const k_c1 =
  "<?xml version=\"1.0\"?><html><body><h1>Letter 1</h1>"
  "<p>You will rejoice to hear that no disaster has accompanied the "
  "commencement of an enterprise which you have regarded with such evil "
  "forebodings.</p></body></html>";
static const char* const k_c2 = "<?xml version=\"1.0\"?><html><body><h1>Chapter 1</h1>"
                                "<p>I am by birth a Genevese, and my family is one of the most "
                                "distinguished of that republic.</p></body></html>";

static void build_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_epub_cap) == MZ_TRUE);
  struct {
    const char* path;
    const char* data;
    mz_uint     flags;
  } e[] = {
    {"mimetype", k_mimetype, MZ_NO_COMPRESSION},
    {"META-INF/container.xml", k_container, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/content.opf", k_opf, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/c1.xhtml", k_c1, MZ_DEFAULT_COMPRESSION},
    {"OEBPS/c2.xhtml", k_c2, MZ_DEFAULT_COMPRESSION},
  };
  for (size_t i = 0U; i < (sizeof(e) / sizeof(e[0])); ++i) {
    TEST_ASSERT(mz_zip_writer_add_mem(&zip, e[i].path, e[i].data, strlen(e[i].data), e[i].flags) ==
                MZ_TRUE);
  }
  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > 0U) && (hsz <= sizeof(s_epub)));
  memcpy(s_epub, heap, hsz);
  s_epub_len = hsz;
  mz_zip_writer_end(&zip);
}

/** @brief Write the built EPUB to @p path on the mounted volume. */
static void write_epub(ra_fs_mount_t* mount, const char* path)
{
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(mount, path, k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write(f, s_epub, (uint32_t)s_epub_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
}

/**
 * @test test_epub_fs_roundtrip
 * @brief A .epub written through ra_fs is opened end to end by ra_epub_open_fs:
 *        the spine count and a chapter body come back intact.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- the happy path; the open_fs guard
 * decision is covered by test_epub_fs_guards.)
 */
static void test_epub_fs_roundtrip(void)
{
  TEST_BEGIN("ra_epub_fs: write -> ra_fs -> open_fs roundtrip");
  build_fat16_volume();
  build_epub();

  ra_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");

  /* The ra_fs layer round-trips the exact bytes (size + content) before the
   * adapter parses them -- so a later parse failure is never a storage bug. */
  {
    ra_fs_file_t* rf = nullptr;
    TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(mount, "BOOK.EPB", k_ra_fs_mode_read, &rf));
    uint32_t sz = 0U;
    uint32_t gt = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_fs_size(rf, &sz));
    TEST_ASSERT_EQ((uint32_t)s_epub_len, sz);
    TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(rf, s_read, sz, &gt));
    TEST_ASSERT_EQ(sz, gt);
    TEST_ASSERT_EQ(0, memcmp(s_read, s_epub, (size_t)sz));
    (void)ra_fs_close(rf);
  }

  ra_epub_book_t book = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open_fs(mount, "BOOK.EPB", s_read, (size_t)k_read_cap, &book));

  uint16_t chapters = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_get_chapter_count(&book, &chapters));
  TEST_ASSERT_EQ(2U, chapters);

  uint8_t chbuf[2048] = {};
  size_t  got         = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_load_chapter(&book, 0U, chbuf, sizeof(chbuf) - 1U, &got));
  TEST_ASSERT(got > 0U);
  chbuf[got] = (uint8_t)'\0';
  TEST_ASSERT(strstr((const char*)chbuf, "rejoice") != nullptr); /* real Frankenstein prose */

  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra_epub_fs: write -> ra_fs -> open_fs roundtrip");
}

/**
 * @test test_epub_fs_guards
 * @brief ra_epub_open_fs rejects bad arguments, a missing file, and a buffer
 *        too small for the book -- without leaking the file handle.
 *
 * @par MC/DC:
 * Decision: `mount==NULL || path==NULL || buf==NULL || out_book==NULL`
 * (4 conditions, OR). One control vector (all non-NULL -> passes the guard)
 * plus one vector flipping each operand to NULL -> N+1 = 5 vectors.
 */
static void test_epub_fs_guards(void)
{
  TEST_BEGIN("ra_epub_fs: open_fs guards (MC/DC)");
  build_fat16_volume();
  build_epub();
  ra_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");

  ra_epub_book_t book = {};

  /* NULL-OR guard MC/DC: control (all set) is exercised by the roundtrip test;
   * here each operand is independently flipped to NULL. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_open_fs(nullptr, "BOOK.EPB", s_read, (size_t)k_read_cap, &book));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_open_fs(mount, nullptr, s_read, (size_t)k_read_cap, &book));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_open_fs(mount, "BOOK.EPB", nullptr, (size_t)k_read_cap, &book));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epub_open_fs(mount, "BOOK.EPB", s_read, (size_t)k_read_cap, nullptr));

  /* cap == 0 -> invalid_size. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_epub_open_fs(mount, "BOOK.EPB", s_read, 0U, &book));

  /* Missing file -> propagated open error (not k_ra_ok). */
  TEST_ASSERT(ra_epub_open_fs(mount, "NOPE.EPB", s_read, (size_t)k_read_cap, &book) != k_ra_ok);

  /* Buffer smaller than the book -> no_mem (file does not fit). */
  TEST_ASSERT_EQ(k_ra_err_no_mem,
                 ra_epub_open_fs(mount, "BOOK.EPB", s_read, s_epub_len - 1U, &book));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra_epub_fs: open_fs guards (MC/DC)");
}

/**
 * @enum epub_fs_fat16_off_t
 * @brief Byte offsets into the FAT16 image for cluster-chain corruption.
 *
 * @details
 * Geometry of the volume built by ::build_fat16_volume (512 B/sector,
 * 1 sector/cluster, 1 reserved sector, 2 FATs of 32 sectors each, 16 root
 * entries). The single file `BOOK.EPB` occupies the first data cluster
 * (cluster 2), chained 2 -> 3 -> ... -> EOC. FAT16 entry for cluster N lives at
 * FAT-relative byte `N * 2`.
 */
typedef enum : uint32_t {
  k_fat16_fat0_lba       = 1U,     /**< Reserved sectors = 1 -> FAT0 at LBA 1.    */
  k_fat16_fat1_lba       = 33U,    /**< FAT0 + 32 sectors/FAT -> FAT1 at LBA 33.  */
  k_fat16_clus2_ent_byte = 4U,     /**< Cluster 2 entry at FAT-relative byte 2*2. */
  k_fat16_offdisk_clus   = 0xF000U /**< Off-disk next-cluster ptr (< 0xFFF8 EOC). */
} epub_fs_fat16_off_t;

/**
 * @test test_epub_fs_read_error_corrupt_fat
 * @brief A corrupt FAT chain makes the whole-file read fault; ra_epub_open_fs
 *        propagates the backend error and still closes the file handle.
 *
 * @par MC/DC:
 * Drives the *production* decision in ra_epub_fs.c ra_epub_open_fs():
 *   `if ((err == k_ra_ok) && (got != size))` (2 conditions, AND) -- the
 * left-operand-false arm. Cluster 2's FAT16 entry is rewritten (in both FAT
 * copies) to 0xF000, a normal (non-EOC) pointer whose data LBA is past the
 * 8192-sector disk. The first 512-byte chunk reads cluster 2 fine, then the
 * chain walk follows 2 -> 0xF000, whose LBA (61504) trips the mem_read
 * `lba + count > block_count` guard -> k_ra_err_out_of_range. So ra_fs_read
 * returns non-ok, making `(err == k_ra_ok)` FALSE; the AND short-circuits and
 * the error is propagated.
 *
 * - This vector: err != k_ra_ok -> C1=F -> overall F via the left operand.
 * - The control vector (err==k_ra_ok, got==size -> C1=T, C2=F -> overall F) is
 *   exercised by test_epub_fs_roundtrip.
 * Pair (control, this) isolates the left condition.
 *
 * @note The right-operand-true arm (err==k_ra_ok && got!=size) is structurally
 *       unreachable here: ra_fs_read returns k_ra_ok only after producing
 *       exactly `remaining == size` bytes (offset starts at 0, max_len==size),
 *       so on the success path got==size is invariant. The short-read guard is
 *       defensive against a future backend that violates that contract.
 */
static void test_epub_fs_read_error_corrupt_fat(void)
{
  TEST_BEGIN("ra_epub_fs: corrupt FAT chain -> read error propagated");
  build_fat16_volume();
  build_epub();
  TEST_ASSERT(s_epub_len > (size_t)k_disk_block_size); /* multi-cluster chain */
  ra_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");

  /* Point cluster 2 at an off-disk cluster in both FAT copies so the chain walk
   * past the first sector faults. FAT-relative byte = cluster*2 = 4. */
  put16(s_disk.bytes,
        (k_fat16_fat0_lba * (uint32_t)k_disk_block_size) + (uint32_t)k_fat16_clus2_ent_byte,
        (uint16_t)k_fat16_offdisk_clus);
  put16(s_disk.bytes,
        (k_fat16_fat1_lba * (uint32_t)k_disk_block_size) + (uint32_t)k_fat16_clus2_ent_byte,
        (uint16_t)k_fat16_offdisk_clus);

  ra_epub_book_t book = {};
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_epub_open_fs(mount, "BOOK.EPB", s_read, (size_t)k_read_cap, &book));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra_epub_fs: corrupt FAT chain -> read error propagated");
}

int32_t main(void)
{
  test_epub_fs_roundtrip();
  test_epub_fs_guards();
  test_epub_fs_read_error_corrupt_fat();
  return 0;
}
