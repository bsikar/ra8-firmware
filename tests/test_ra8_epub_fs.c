/**
 * @file test_ra8_epub_fs.c
 * @brief End-to-end host test for the ra8_fs -> ra8_epub bridge (#71).
 *
 * @details
 * Proves the storage-stack acceptance for #71/#151/#230 on the host: a real
 * `.epub` is assembled in memory with miniz, *written to a FAT16 volume through
 * ra8_fs* (over a RAM block backend -- the same mem-disk pattern as
 * `tests/test_ra8_fs_fat.c`), and then opened end to end with the STREAMED
 * `ra8_epub_open_streamed_fs()` -- the sole production `ra8_fs` open path since
 * #230 retired the whole-file `ra8_epub_open_fs()`: the source file stays open
 * and every ZIP entry is seek+read on demand, with no whole-file buffer. On
 * target the only difference is the block backend (`ra8_sdmmc_spi` over the SD
 * card instead of RAM), which is independently bench-validated.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_epub_fs.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum epub_fs_fixture_t
 * @brief Protocol and on-disk field offsets, plus the byte-level helpers.
 */
typedef enum : uint8_t {
  k_bpb_sig_lo            = 0x55U, /**< That signature's low byte.                    */
  k_bpb_sig_hi            = 0xAAU, /**< Its high byte.                                */
  k_bpb_off_bytes_per_sec = 11U,   /**< BPB_BytsPerSec: bytes per sector.             */
  k_bpb_off_rsvd_sec_cnt  = 14U,   /**< BPB_RsvdSecCnt: sectors before the first FAT. */
  k_bpb_off_root_ent_cnt  = 17U,   /**< BPB_RootEntCnt: root-directory entries.       */
  k_bpb_off_tot_sec16     = 19U,   /**< BPB_TotSec16: total sectors.                  */
  k_bpb_off_fat_sz16      = 22U,   /**< BPB_FATSz16: sectors per FAT.                 */
  k_byte_mask             = 0xFFU, /**< Low-byte mask used by the put16 helper.       */
  k_bpb_off_sec_per_clus  = 13,    /**< BPB_SecPerClus: sectors per cluster.          */
} epub_fs_fixture_t;

/**
 * @enum epub_fs_fixture2_t
 * @brief Protocol and on-disk field offsets, plus buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_epub_chapter_buf_bytes =
    2048, /**< Chapter read-back buffer; larger than any fixture chapter, truncation would show. */
  /** Offset of the 0xAA55 boot signature's low byte. */
  k_bpb_off_sig_lo = 510,
  /** Offset of its high byte. */
  k_bpb_off_sig_hi = 511,
} epub_fs_fixture2_t;

/* --- RAM block device + minimal FAT16 volume (mirrors test_ra8_fs_fat.c) --- */

typedef enum : uint32_t {
  k_disk_block_size = 512U,       /**< Disk block size.    */
  k_disk_blocks     = 8U * 1024U, /**< 4 MiB FAT16 volume. */
} epub_fs_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

static mem_disk_t s_disk = {};

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

static ra8_err_t mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
}

static void build_fat16_volume(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_disk_blocks * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1U, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks;
  TEST_ASSERT(s_disk.bytes != nullptr);
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size); /* bytes/sector     */
  bpb[k_bpb_off_sec_per_clus] = 1U;                                 /* sectors/cluster  */
  put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);                           /* reserved sectors */
  bpb[16] = 2U;                                                     /* number of FATs   */
  put16(bpb, k_bpb_off_root_ent_cnt, 16U);                          /* root dir entries */
  put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks);         /* total sectors    */
  put16(bpb, k_bpb_off_fat_sz16, 32U);                              /* sectors/FAT      */
  bpb[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = k_bpb_sig_hi;
}

/* --- A real (minimal) EPUB assembled in memory with miniz --- */

typedef enum : size_t {
  k_epub_cap = 8U * 1024U, /**< EPUB cap. */
  k_read_cap = 8U * 1024U, /**< Read cap. */
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
    const char* path;  /**< Path.  */
    const char* data;  /**< Data.  */
    mz_uint     flags; /**< Flags. */
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
static void write_epub(ra8_fs_mount_t* mount, const char* path)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, path, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_epub, (uint32_t)s_epub_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
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
 * @brief A corrupt FAT chain makes the on-demand stream reads fault mid-parse;
 *        ra8_epub_open_streamed_fs fails cleanly and closes the file handle.
 *
 * @details
 * Cluster 2's FAT16 entry is rewritten (in both FAT copies) to 0xF000, a
 * normal (non-EOC) pointer whose data LBA is past the 8192-sector disk. The
 * streamed open's first ZIP-tail read walks the chain 2 -> 0xF000, whose LBA
 * (61504) trips the mem_read `lba + count > block_count` guard ->
 * k_ra8_err_out_of_range inside the stream callback, which reports 0 bytes;
 * miniz treats the short read as a reader-init failure, so the open returns
 * k_ra8_err_validation_failed, the adapter closes the file (io.file == NULL),
 * and the book is left unopened.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- the stream callback's guards are
 * independent single-condition early returns; this drives the seek/read error
 * leg the happy-path roundtrip never reaches.)
 */
static void test_epub_fs_read_error_corrupt_fat(void)
{
  TEST_BEGIN("ra8_epub_fs: corrupt FAT chain -> streamed open fails, file closed");
  build_fat16_volume();
  build_epub();
  TEST_ASSERT(s_epub_len > (size_t)k_disk_block_size); /* multi-cluster chain */
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");
  /* Unmount before corrupting, and mount again after. Corruption arrives on
   * real media between sessions, not under a live mount, and the driver caches
   * one FAT sector (#607) -- so poking the FAT behind a mounted volume would
   * be masked by the copy already in memory and prove nothing. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  mount = nullptr;

  /* Point cluster 2 at an off-disk cluster in both FAT copies so the chain walk
   * past the first sector faults. FAT-relative byte = cluster*2 = 4. */
  put16(s_disk.bytes,
        (k_fat16_fat0_lba * (uint32_t)k_disk_block_size) + (uint32_t)k_fat16_clus2_ent_byte,
        (uint16_t)k_fat16_offdisk_clus);
  put16(s_disk.bytes,
        (k_fat16_fat1_lba * (uint32_t)k_disk_block_size) + (uint32_t)k_fat16_clus2_ent_byte,
        (uint16_t)k_fat16_offdisk_clus);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  ra8_epub_stream_fs_ctx_t io   = {};
  ra8_epub_book_t          book = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_open_streamed_fs(mount, "BOOK.EPB", &io, &book));
  TEST_ASSERT(io.file == nullptr); /* the failed open released the handle */
  TEST_ASSERT_EQ(0, book.in_use);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra8_epub_fs: corrupt FAT chain -> streamed open fails, file closed");
}

/**
 * @brief Prove the ra8_fs layer returns the .epub bytes exactly as written.
 *
 * @details
 * Runs before the adapter parses anything, so a later parse failure can never
 * be blamed on storage: size and content are both compared against the source
 * blob the fixture wrote.
 *
 * @param[in,out] mount Mounted volume holding BOOK.EPB.
 *
 * @pre @p mount is a mounted volume and BOOK.EPB has been written to it.
 * @pre `s_epub` / `s_epub_len` describe the bytes that were written.
 * @post The file handle is closed again.
 * @post Every assertion has run; a mismatch aborts the process.
 *
 * @note Not thread-safe; the fixtures are file-scope state.
 */
static void check_fs_byte_roundtrip(ra8_fs_mount_t* mount)
{
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "BOOK.EPB", k_ra8_fs_mode_read, &rf));
  uint64_t sz = 0U;
  uint32_t gt = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(rf, &sz));
  TEST_ASSERT_EQ(s_epub_len, sz);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(rf, s_read, sz, &gt));
  TEST_ASSERT_EQ(sz, gt);
  TEST_ASSERT_EQ(0, memcmp(s_read, s_epub, (size_t)sz));
  (void)ra8_fs_close(rf);
}

/**
 * @test test_epub_fs_streamed_roundtrip
 * @brief A .epub on ra8_fs opens end to end through ra8_epub_open_streamed_fs with
 *        no whole-file buffer (#151): the spine count and both chapter bodies come
 *        back intact, the source file stays open for on-demand reads, and close
 *        releases it.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- the happy path; the streamed-open
 * NULL-OR guard is covered by test_epub_fs_streamed_guards.)
 */
static void test_epub_fs_streamed_roundtrip(void)
{
  TEST_BEGIN("ra8_epub_fs: streamed open roundtrip (no whole-file residency)");
  build_fat16_volume();
  build_epub();

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");

  check_fs_byte_roundtrip(mount);

  ra8_epub_stream_fs_ctx_t io   = {};
  ra8_epub_book_t          book = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed_fs(mount, "BOOK.EPB", &io, &book));
  TEST_ASSERT(io.file != nullptr); /* file kept open for on-demand reads */
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT(book.zip_bytes == nullptr); /* streamed: no resident blob */

  uint16_t chapters = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(&book, &chapters));
  TEST_ASSERT_EQ(2U, chapters);

  /* Each chapter is inflated on demand straight off the volume. */
  uint8_t chbuf[k_epub_chapter_buf_bytes] = {};
  size_t  got                             = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(&book, 0U, chbuf, sizeof(chbuf) - 1U, &got));
  TEST_ASSERT(got > 0U);
  chbuf[got] = (uint8_t)'\0';
  TEST_ASSERT(strstr((const char*)chbuf, "rejoice") != nullptr);

  size_t got2 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(&book, 1U, chbuf, sizeof(chbuf) - 1U, &got2));
  TEST_ASSERT(got2 > 0U);
  chbuf[got2] = (uint8_t)'\0';
  TEST_ASSERT(strstr((const char*)chbuf, "Genevese") != nullptr);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close_streamed_fs(&io, &book));
  TEST_ASSERT(io.file == nullptr); /* file released on close */
  TEST_ASSERT_EQ(0, book.in_use);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra8_epub_fs: streamed open roundtrip (no whole-file residency)");
}

/**
 * @test test_epub_fs_streamed_guards
 * @brief ra8_epub_open_streamed_fs rejects NULL args and a missing file, closes the
 *        source file on a parse failure, and ra8_epub_close_streamed_fs is a safe
 *        no-op after a failed open.
 *
 * @par MC/DC:
 * Decision: `mount==NULL || path==NULL || io==NULL || out_book==NULL`
 * (4 conditions, OR). Control (all non-NULL) is exercised by the roundtrip test;
 * here each operand is independently flipped to NULL -> N+1 = 5 vectors.
 */
static void test_epub_fs_streamed_guards(void)
{
  TEST_BEGIN("ra8_epub_fs: streamed open guards");
  build_fat16_volume();
  build_epub();
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));
  write_epub(mount, "BOOK.EPB");

  /* A small non-ZIP file: it opens + sizes fine but ra8_epub_open_streamed rejects
   * it, driving the adapter's parse-failure (file-close) branch. */
  {
    static const char k_garbage[] = "not a zip archive at all, just text";
    ra8_fs_file_t*    gf          = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "BAD.EPB", k_ra8_fs_mode_write, &gf));
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_fs_write(gf, (const uint8_t*)k_garbage, (uint32_t)(sizeof(k_garbage) - 1U)));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(gf));
  }

  ra8_epub_stream_fs_ctx_t io   = {};
  ra8_epub_book_t          book = {};

  /* NULL-OR guard MC/DC: each operand independently flipped to NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed_fs(nullptr, "BOOK.EPB", &io, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed_fs(mount, nullptr, &io, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed_fs(mount, "BOOK.EPB", nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed_fs(mount, "BOOK.EPB", &io, nullptr));

  /* Missing file -> propagated open error; no file left open. */
  TEST_ASSERT(ra8_epub_open_streamed_fs(mount, "NOPE.EPB", &io, &book) != k_ra8_ok);
  TEST_ASSERT(io.file == nullptr);

  /* Non-ZIP file -> parse failure; the adapter closes the file it opened. */
  TEST_ASSERT(ra8_epub_open_streamed_fs(mount, "BAD.EPB", &io, &book) != k_ra8_ok);
  TEST_ASSERT(io.file == nullptr);

  /* close_streamed_fs guards: NULL args, then a safe no-op after the failed open
   * (io.file already NULL -> the file-close branch is skipped). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_close_streamed_fs(nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_close_streamed_fs(&io, nullptr));
  TEST_ASSERT(ra8_epub_close_streamed_fs(&io, &book) != k_ra8_ok); /* book not in_use */
  TEST_ASSERT(io.file == nullptr);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  TEST_END("ra8_epub_fs: streamed open guards");
}

int32_t main(void)
{
  test_epub_fs_streamed_roundtrip();
  test_epub_fs_streamed_guards();
  test_epub_fs_read_error_corrupt_fat();
  return 0;
}
