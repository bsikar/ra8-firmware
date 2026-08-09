/**
 * @file mkbookimg.c
 * @brief Host tool: pack compiled .rabook files into a FAT32 SD-card image.
 *
 * @details
 * Builds a raw FAT32 disk image (512-byte sectors, no MBR) containing the given
 * books, each stored under its own basename (e.g. `Meditations.rabook`) now that
 * `ra8_fs` writes VFAT long names (#600/#633). It drives the SAME first-party
 * `ra8_fs` formatter/writer the firmware reads with, so the emulator
 * (`ra8_emulator --sd image.img`) and the on-device ra8_sdmmc_spi -> ra8_fs path see
 * a byte-identical layout. The firmware still reads each book's title/author/cover
 * from its header; the card now also carries the human-readable file name.
 *
 * Usage: mkbookimg <out.img> <book1.rabook> [book2.rabook ...]
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkbookimg_names.h"
#include "ra8_fs.h"

enum : uint32_t {
  k_block_size  = 512U,    /**< FAT sector size.                       */
  k_img_sectors = 131072U, /**< 64 MiB image (FAT32 needs >= ~34 MiB). */
  k_max_books   = 32U,     /**< Max books packed into one image.       */
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
static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
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
static ra8_err_t mem_cap(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = k_block_size;
  return k_ra8_ok;
}

/** @brief Read a whole file into a freshly malloc'd buffer; nullptr on failure. */
static uint8_t* read_file(const char* path, uint32_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return nullptr;
  }
  (void)fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    (void)fclose(f);
    return nullptr;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)n);
  if ((buf == nullptr) || (fread(buf, 1U, (size_t)n, f) != (size_t)n)) {
    free(buf);
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

/**
 * @brief Report whether input @p i's card name repeats an earlier input's.
 *
 * @details Two sources with the same basename (from different directories)
 *          would land on the same card name and the second would replace the
 *          first (`ra8_fs_write_file` overwrites by name). This scans the inputs
 *          before @p i for a matching basename so a collision is refused rather
 *          than silently dropping a book.
 *
 * @param[in] argv Program argv; the books start at @p argv[2].
 * @param[in] i    Zero-based index of the input being checked (0 .. n_books-1).
 *
 * @return Whether an earlier input shares input @p i's basename.
 * @retval true  Some @p j in [0, @p i) has the same basename as input @p i.
 * @retval false Input @p i's basename is unique among the inputs before it.
 *
 * @pre @p argv holds valid book paths at indices [2, 2 + i].
 * @pre @p i is a non-negative index within the input range.
 * @post No state is modified (pure scan over @p argv).
 * @post @p argv strings are read only, never written.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static bool dup_dest_name(char** argv, int i)
{
  const char* const name = mkbookimg_basename(argv[2 + i]);
  for (int j = 0; j < i; ++j) {
    if (strcmp(name, mkbookimg_basename(argv[2 + j])) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Write each input book onto the mounted card under its own basename.
 *
 * @details
 * For every input path in @p argv[2 .. 2 + n_books) derives the on-card name
 * from the source's basename (::mkbookimg_dest_name), refuses a name that
 * collides with an earlier input (::dup_dest_name), reads the whole file into
 * memory (::read_file), and writes it through ra8_fs_write_file under the
 * human-readable long name. The first bad name, collision, read or write
 * failure stops the run and returns non-zero.
 *
 * @param[in,out] mnt     Mounted FAT32 volume to create files on.
 * @param[in]     argv    Program argv; the books start at @p argv[2].
 * @param[in]     n_books Number of book paths, already capped at ::k_max_books.
 *
 * @return 0 when every book was written, 1 on the first failure.
 * @retval 0 All @p n_books files exist on the card under their basenames.
 * @retval 1 A name was unusable/duplicate or a book could not be read/written.
 *
 * @pre @p mnt is a mounted volume and @p argv holds @p n_books paths at [2..].
 * @pre @p n_books is in 0 .. ::k_max_books.
 * @post On success @p n_books files exist on the card and every buffer is freed.
 * @post On failure the per-book buffer is freed before returning.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int write_books(ra8_fs_mount_t* mnt, char** argv, int n_books)
{
  for (int i = 0; i < n_books; ++i) {
    char name[k_mkbookimg_name_cap];
    if (!mkbookimg_dest_name(argv[2 + i], name, sizeof name)) {
      (void)fprintf(stderr, "mkbookimg: unusable card name for %s\n", argv[2 + i]);
      return 1;
    }
    if (dup_dest_name(argv, i)) {
      (void)fprintf(stderr, "mkbookimg: duplicate card name %s (from %s)\n", name, argv[2 + i]);
      return 1;
    }
    uint32_t       len  = 0U;
    uint8_t* const data = read_file(argv[2 + i], &len);
    if (data == nullptr) {
      (void)fprintf(stderr, "mkbookimg: cannot read %s\n", argv[2 + i]);
      return 1;
    }
    const ra8_err_t err = ra8_fs_write_file(mnt, name, data, len);
    if (err == k_ra8_ok) {
      (void)fprintf(stderr, "mkbookimg: + %s  (%u bytes)  <- %s\n", name, len, argv[2 + i]);
    }
    free(data);
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
  if (out == nullptr) {
    (void)fprintf(stderr, "mkbookimg: cannot write %s\n", path);
    return 1;
  }
  const size_t total = (size_t)k_img_sectors * k_block_size;
  const size_t wrote = fwrite(s_disk.bytes, 1U, total, out);
  (void)fclose(out);
  return (wrote == total) ? 0 : 1;
}

int main(int argc, char** argv)
{
  if (argc < 3) {
    (void)fprintf(stderr, "usage: %s <out.img> <book1.rabook> [book2.rabook ...]\n", argv[0]);
    return 2;
  }
  const int n_books = argc - 2;
  if (n_books > (int)k_max_books) {
    (void)fprintf(stderr, "mkbookimg: too many books (max %u)\n", k_max_books);
    return 2;
  }
  s_disk.block_count = k_img_sectors;
  s_disk.bytes       = (uint8_t*)calloc(1U, (size_t)k_img_sectors * k_block_size);
  if (s_disk.bytes == nullptr) {
    (void)fprintf(stderr, "mkbookimg: out of memory\n");
    return 1;
  }
  const ra8_fs_backend_t backend = {.read_block   = mem_read,
                                    .write_block  = mem_write,
                                    .get_capacity = mem_cap,
                                    .ctx          = &s_disk};
  ra8_fs_mount_t*        mnt     = nullptr;
  int                    rc      = fs_format_mount(&backend, &mnt);
  if (rc == 0) {
    rc = write_books(mnt, argv, n_books);
    (void)ra8_fs_unmount(mnt);
  }
  if (rc == 0) {
    rc = dump_image(argv[1]);
  }
  free(s_disk.bytes);
  if (rc == 0) {
    (void)fprintf(stderr, "mkbookimg: wrote %s (%d book(s), 64 MiB FAT32)\n", argv[1], n_books);
  }
  return rc;
}
