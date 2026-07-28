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
 * from the .rabook header, so the 8.3 names need carry no metadata.
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

#include "ra8_fs.h"

enum : uint32_t {
  k_block_size  = 512U,    /**< FAT sector size.                       */
  k_img_sectors = 131072U, /**< 64 MiB image (FAT32 needs >= ~34 MiB). */
  k_max_books   = 32U,     /**< BOOK01..BOOK32.                        */
  k_name_len    = 12U,     /**< "BOOKNN.RBK" + NUL.                    */
};

/** @brief In-memory disk for the ra8_fs backend. */
typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk;

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if ((lba + count) > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[(size_t)lba * k_block_size], (size_t)count * k_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if ((lba + count) > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * k_block_size], buf, (size_t)count * k_block_size);
  return k_ra8_ok;
}

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

/** @brief Format + mount the in-memory disk as FAT32; 0 on success. */
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

/** @brief Write each argv[2..] book as BOOKNN.RBK / BOOKNN.EPB; 0 on success. */
static int write_books(ra8_fs_mount_t* mnt, char** argv, int n_books)
{
  for (int i = 0; i < n_books; ++i) {
    uint32_t       len  = 0U;
    uint8_t* const data = read_file(argv[2 + i], &len);
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
    free(data);
    if (err != k_ra8_ok) {
      (void)fprintf(stderr, "mkbookimg: write %s failed\n", name);
      return 1;
    }
  }
  return 0;
}

/** @brief Dump the in-memory disk to @p path; 0 on success. */
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
