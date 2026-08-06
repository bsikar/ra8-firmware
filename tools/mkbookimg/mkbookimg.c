/**
 * @file mkbookimg.c
 * @brief Host tool: pack compiled .rabook files into a FAT32 SD-card image.
 *
 * @details
 * Builds a raw FAT32 disk image (512-byte sectors, no MBR) containing the given
 * .rabook files under 8.3 names BOOK01.RBK, BOOK02.RBK, ... It drives the SAME
 * first-party `ra8_fs` formatter/writer the firmware reads with, so the emulator
 * (`ra8_emulator --sd image.img`) and the on-device ra8_sdmmc_spi -> ra8_fs path see
 * a byte-identical layout. The firmware reads each book's title/author/cover
 * from the .rabook header, so the 8.3 names need carry no metadata -- which is
 * why they are still 8.3 now that `ra8_fs` can write long ones (#600). #633
 * revisits that.
 *
 * Usage: mkbookimg <out.img> <book1.rabook> [book2.rabook ...]
 *
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

enum : uint32_t { k_arena_bytes = 128U * 1024U * 1024U }; // 128 MiB - generous for disk images

enum : uint32_t {
  k_block_size  = 512U,    /**< FAT sector size.                       */
  k_img_sectors = 131072U, /**< 64 MiB image (FAT32 needs >= ~34 MiB). */
  k_max_books   = 32U,     /**< BOOK01..BOOK32.                        */
  k_name_len    = 16U,     /**< "BOOKNN.RABOOK" + NUL.                 */
};

/** @brief In-memory disk for the ra8_fs backend. */
typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk;

/**
 * @brief ra8_fs_backend_t::read_block over the in-memory disk.
 *
 * @details
 * Copies @p count contiguous 512-byte sectors starting at @p lba out of the
 * flat ::mem_disk_t byte store into @p buf. This is the read half of the
 * block-device facade handed to ra8_fs, so the host tool drives the SAME
 * formatter and FAT writer the firmware reads with rather than poking the
 * image directly.
 *
 * @param[in]  ctx   The ::mem_disk_t backing store, type-erased as ra8_fs wants.
 * @param[in]  lba   First logical block address to read.
 * @param[in]  count Number of 512-byte blocks to read.
 * @param[out] buf   Destination buffer for @p count * 512 bytes.
 *
 * @return Block-device status.
 * @retval k_ra8_ok               The requested sectors were copied out.
 * @retval k_ra8_err_out_of_range @p lba + @p count runs past the disk.
 *
 * @pre @p ctx points at an initialised ::mem_disk_t whose bytes are allocated.
 * @pre @p buf has room for @p count * 512 bytes.
 * @post On success @p buf holds the requested sectors.
 * @post The backing store is left unmodified on every path.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
// cppcheck-suppress constParameterCallback
static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  if ((lba + count) > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[(size_t)lba * k_block_size], (size_t)count * k_block_size);
  return k_ra8_ok;
}

/**
 * @brief ra8_fs_backend_t::write_block over the in-memory disk.
 *
 * @details
 * Copies @p count contiguous 512-byte sectors from @p buf into the flat
 * ::mem_disk_t byte store at @p lba. This is the write half of the block-device
 * facade ra8_fs formats and writes files through; the whole image is later
 * dumped verbatim by ::dump_image.
 *
 * @param[in]  ctx   The ::mem_disk_t backing store, type-erased as ra8_fs wants.
 * @param[in]  lba   First logical block address to write.
 * @param[in]  count Number of 512-byte blocks to write.
 * @param[in]  buf   Source of @p count * 512 bytes to store.
 *
 * @return Block-device status.
 * @retval k_ra8_ok               The sectors were stored.
 * @retval k_ra8_err_out_of_range @p lba + @p count runs past the disk.
 *
 * @pre @p ctx points at an initialised ::mem_disk_t whose bytes are allocated.
 * @pre @p buf holds at least @p count * 512 readable bytes.
 * @post On success the addressed sectors equal @p buf.
 * @post Out-of-range writes change nothing.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if ((lba + count) > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * k_block_size], buf, (size_t)count * k_block_size);
  return k_ra8_ok;
}

/**
 * @brief ra8_fs_backend_t::get_capacity for the modelled disk.
 *
 * @details
 * Reports the geometry ra8_fs needs to lay out a filesystem: the number of
 * 512-byte blocks in the ::mem_disk_t store and the fixed sector size. The
 * count is whatever ::main configured before formatting (::k_img_sectors).
 *
 * @param[in]  ctx         The ::mem_disk_t backing store, type-erased.
 * @param[out] block_count Receives the number of 512-byte blocks.
 * @param[out] block_size  Receives the sector size in bytes (::k_block_size).
 *
 * @return Block-device status.
 * @retval k_ra8_ok Geometry was reported (this shim cannot fail).
 *
 * @pre @p ctx points at an initialised ::mem_disk_t.
 * @pre @p block_count and @p block_size are non-NULL.
 * @post Both out-parameters are populated.
 * @post The backing store is left unmodified.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
// cppcheck-suppress constParameterCallback
static ra8_err_t mem_cap(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = k_block_size;
  return k_ra8_ok;
}

/** @brief Read a whole file into a freshly arena-allocated buffer; nullptr on failure. */
static uint8_t* read_file(ra8_arena_t* arena, const char* path, uint32_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return nullptr;
  }
  (void)fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    (void)fclose(f);
    return nullptr;
  }
  uint8_t* buf = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)n);
  if ((buf == nullptr) || (fread(buf, 1U, (size_t)n, f) != (size_t)n)) {
    (void)fclose(f);
    return nullptr;
  }
  (void)fclose(f);
  *out_len = (uint32_t)n;
  return buf;
}

/**
 * @brief Format the in-memory disk as FAT32 and mount it.
 *
 * @details
 * Runs the real ra8_fs FAT32 formatter over @p backend with the volume label
 * "RABOOKS", then mounts the freshly formatted volume. Both steps go through
 * first-party ra8_fs so the on-image layout matches what the firmware later
 * reads. Any failure is diagnosed on stderr before returning non-zero.
 *
 * @param[in]  backend  Block-device facade over the in-memory disk.
 * @param[out] out_mnt  Receives the mounted volume handle on success.
 *
 * @return 0 on success, 1 if formatting or mounting failed.
 * @retval 0 The disk is formatted FAT32 and mounted; @p out_mnt is valid.
 * @retval 1 ra8_fs_format or ra8_fs_mount failed (reported on stderr).
 *
 * @pre @p backend is fully populated with the mem_* callbacks and ctx.
 * @pre @p out_mnt is non-NULL.
 * @post On success @p out_mnt names a mounted volume the caller must unmount.
 * @post On failure @p out_mnt is left untouched and nothing is mounted.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int fs_format_mount(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_mnt)
{
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "RABOOKS";
  if (ra8_fs_format(backend, &opts) != k_ra8_ok) {
    (void)fprintf(stderr, "mkbookimg: ra8_fs_format failed\n");
    return 1;
  }
  if (ra8_fs_mount(backend, out_mnt) != k_ra8_ok) {
    (void)fprintf(stderr, "mkbookimg: ra8_fs_mount failed\n");
    return 1;
  }
  return 0;
}

/** @brief 8.3 extension for an input file: ".EPB" for *.epub, else ".RBK". */
static const char* book_ext(const char* path)
{
  const size_t n   = strlen(path);
  const size_t ext = strlen(".epub");
  return ((n > ext) && (strcmp(&path[n - ext], ".epub") == 0)) ? "EPB" : "RBK";
}

/**
 * @brief Write each input book onto the mounted card under an 8.3 name.
 *
 * @details
 * For every input path in @p argv[2 .. 2 + n_books) reads the whole file into
 * memory (::read_file), derives its 8.3 name BOOKNN.RBK or BOOKNN.EPB from the
 * one-based index and ::book_ext, and writes it through ra8_fs_write_file. The
 * firmware reads each book's title, author and cover from the .rabook header,
 * so the 8.3 names need carry no metadata. The first read or write failure
 * stops the run and returns non-zero.
 *
 * @param[in,out] mnt     Mounted FAT32 volume to create files on.
 * @param[in]     argv    Program argv; the books start at @p argv[2].
 * @param[in]     n_books Number of book paths, already capped at ::k_max_books.
 *
 * @return 0 when every book was written, 1 on the first failure.
 * @retval 0 All @p n_books files exist on the card.
 * @retval 1 A book could not be read or written (reported on stderr).
 *
 * @pre @p mnt is a mounted volume and @p argv holds @p n_books paths at [2..].
 * @pre @p n_books is in 0 .. ::k_max_books.
 * @post On success @p n_books files exist on the card and every buffer is freed.
 * @post On failure the per-book buffer is freed before returning.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int write_books(ra8_arena_t* arena, ra8_fs_mount_t* mnt, char** argv, int n_books)
{
  for (int i = 0; i < n_books; ++i) {
    uint32_t       len  = 0U;
    uint8_t* const data = read_file(arena, argv[2 + i], &len);
    if (data == nullptr) {
      (void)fprintf(stderr, "mkbookimg: cannot read %s\n", argv[2 + i]);
      return 1;
    }
    char name[k_name_len];
    (void)snprintf(name, sizeof name, "BOOK%02d.%s", i + 1, book_ext(argv[2 + i]));
    const ra8_err_t err = ra8_fs_write_file(mnt, name, data, len);
    if (err == k_ra8_ok) {
      (void)fprintf(stderr, "mkbookimg: + %s  (%u bytes)  <- %s\n", name, len, argv[2 + i]);
    }
    if (err != k_ra8_ok) {
      (void)fprintf(stderr, "mkbookimg: write %s failed\n", name);
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Write the whole in-memory disk out to @p path as a raw image.
 *
 * @details
 * Streams all ::k_img_sectors * ::k_block_size bytes of ::s_disk to @p path
 * with no MBR and no padding, producing the raw 64 MiB FAT32 image the
 * emulator attaches with @c --sd. A short write is treated as failure.
 *
 * @param[in] path Output path for the raw disk image.
 *
 * @return 0 on success, 1 if the file cannot be opened or the write is short.
 * @retval 0 The image bytes were fully written.
 * @retval 1 The output could not be opened, or fewer bytes than expected wrote.
 *
 * @pre ::s_disk.bytes holds a fully built image.
 * @pre @p path is a writable path.
 * @post On success @p path holds exactly the image bytes.
 * @post The output stream is closed on every path.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int dump_image(const char* path)
{
  FILE* out = fopen(path, "wb");
  if (out == NULL) {
    (void)fprintf(stderr, "mkbookimg: cannot write %s\n", path);
    return 1;
  }
  const size_t total = (size_t)k_img_sectors * k_block_size;
  const size_t wrote = fwrite(s_disk.bytes, 1U, total, out);
  (void)fclose(out);
  return (wrote == total) ? 0 : 1;
}

RA8_NASA_RULE_3_OK
int main(int argc, char** argv)
{
  uint8_t* arena_buf = (uint8_t*)malloc(k_arena_bytes);
  if (arena_buf == nullptr) {
    (void)fprintf(stderr, "mkbookimg: out of memory for arena\n");
    // cppcheck-suppress memleak  /* arena_buf is nullptr here */
    return 1;
  }
  ra8_arena_t arena;
  if (ra8_arena_init(&arena, arena_buf, k_arena_bytes) != k_ra8_ok) {
    free(arena_buf);
    return 1;
  }

  int rc = 0;
  if (argc < 3) {
    (void)fprintf(stderr, "usage: %s <out.img> <book1.rabook> [book2.rabook ...]\n", argv[0]);
    rc = 2;
  } else if ((argc - 2) > (int)k_max_books) {
    (void)fprintf(stderr, "mkbookimg: too many books (max %u)\n", k_max_books);
    rc = 2;
  } else {
    const int n_books  = argc - 2;
    s_disk.block_count = k_img_sectors;
    s_disk.bytes = (uint8_t*)ra8_arena_calloc(&arena, 1U, (uint32_t)k_img_sectors * k_block_size);
    if (s_disk.bytes == nullptr) {
      (void)fprintf(stderr, "mkbookimg: out of memory\n");
      rc = 1;
    } else {
      const ra8_fs_backend_t backend = {.read_block   = mem_read,
                                        .write_block  = mem_write,
                                        .get_capacity = mem_cap,
                                        .ctx          = &s_disk};
      ra8_fs_mount_t*        mnt     = nullptr;
      rc                             = fs_format_mount(&backend, &mnt);
      if (rc == 0) {
        rc = write_books(&arena, mnt, argv, n_books);
        (void)ra8_fs_unmount(mnt);
      }
      if (rc == 0) {
        rc = dump_image(argv[1]);
      }
      if (rc == 0) {
        (void)fprintf(stderr, "mkbookimg: wrote %s (%d book(s), 64 MiB FAT32)\n", argv[1], n_books);
      }
    }
  }
  free(arena_buf);
  return rc;
}
