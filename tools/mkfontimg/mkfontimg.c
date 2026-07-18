/**
 * @file mkfontimg.c
 * @brief Build a FAT16 SD-card image carrying a single font file, written
 *        through the real ra8_fs so board_sim's app reads it back bit-for-bit.
 *
 * @details
 * board_sim's @c --sd flag attaches a raw FAT image to the modelled SD card.
 * The firmware app (@c sd_font_render) mounts that image with @ref ra8_fs and
 * reads a font off it. To guarantee the on-card layout is exactly what
 * ra8_fs expects, this host tool formats the image with the SAME ra8_fs code
 * path: it lays down a minimal FAT16 BPB, mounts it through a memory-backed
 * @ref ra8_fs_backend_t, writes the host font file as @c FONT.OTF, then dumps
 * the buffer to the output image. Mirrors the in-test image builder in
 * @c tests/test_ra8_sdmmc_card_reflow.c so the two stay in lock-step.
 *
 * Usage: @c mkfontimg <font-in> <image-out> [dest-name]
 *   - @c font-in   host path to the source font (.otf/.ttf)
 *   - @c image-out path of the raw FAT image to write
 *   - @c dest-name 8.3 name on the card (default @c FONT.OTF)
 *
 * Or:    @c mkfontimg --blank <image-out>
 *   Writes a formatted-but-empty FAT16 image (no font) -- the "random card"
 *   case used to exercise @ref ra8_sdfont_load's self-provisioning path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_fs.h"

/** @brief FAT16 geometry + BPB field offsets (mirror of the host test). */
typedef enum : uint32_t {
  k_block_size         = 512U,               /**< Bytes per sector.             */
  k_blocks_fat16       = 8U * 1024U,         /**< 4 MiB image (font + slack).   */
  k_byte_lo_mask       = 0xFFU,              /**< Low byte of a 16-bit value.   */
  k_byte_shift         = 8U,                 /**< Bits per byte.                */
  k_font_min_bytes     = 16U,                /**< Smallest plausible font file. */
  k_bpb_off_bytspersec = 11U,                /**< BPB_BytsPerSec offset.        */
  k_bpb_off_secperclus = 13U,                /**< BPB_SecPerClus offset.        */
  k_bpb_off_rsvdseccnt = 14U,                /**< BPB_RsvdSecCnt offset.        */
  k_bpb_off_numfats    = 16U,                /**< BPB_NumFATs offset.           */
  k_bpb_off_rootentcnt = 17U,                /**< BPB_RootEntCnt offset.        */
  k_bpb_off_totsec16   = 19U,                /**< BPB_TotSec16 offset.          */
  k_bpb_off_secfat     = 22U,                /**< BPB_FATSz16 offset.           */
  k_bpb_secperclus     = 1U,                 /**< 1 sector per cluster.         */
  k_bpb_rsvdseccnt     = 1U,                 /**< 1 reserved (boot) sector.     */
  k_bpb_numfats        = 2U,                 /**< Two FAT copies.               */
  k_bpb_rootentcnt     = 16U,                /**< 16 root-dir entries.          */
  k_bpb_fatsz16        = 32U,                /**< 32 sectors per FAT.           */
  k_bpb_sig_off_a      = 510U,               /**< Boot signature byte 0 (0x55). */
  k_bpb_sig_off_b      = 511U,               /**< Boot signature byte 1 (0xAA). */
  k_bpb_sig_a          = 0x55U,              /**< Boot signature low byte.      */
  k_bpb_sig_b          = 0xAAU,              /**< Boot signature high byte.     */
  k_font_cap           = 4U * 1024U * 1024U, /**< Max font we will embed.       */
} mkimg_const_t;

/** @brief Memory-backed disk handed to ra8_fs as a block device. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mem_disk_t;

static mem_disk_t s_disk;

/** @brief ra8_fs_backend_t::read_block over the flat sector store. */
static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_block_size],
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

/** @brief ra8_fs_backend_t::write_block over the flat sector store. */
static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_block_size],
         buf,
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

/** @brief ra8_fs_backend_t::get_capacity for the modelled disk. */
static ra8_err_t mem_cap(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_block_size;
  return k_ra8_ok;
}

/** @brief Write a little-endian uint16 into the BPB. */
static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]      = (uint8_t)(v & (uint16_t)k_byte_lo_mask);
  p[off + 1U] = (uint8_t)((v >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_lo_mask);
}

/** @brief Lay down a minimal FAT16 BPB on the zeroed image. */
static void build_fat16(uint8_t* b)
{
  put16(b, (uint32_t)k_bpb_off_bytspersec, (uint16_t)k_block_size);
  b[k_bpb_off_secperclus] = (uint8_t)k_bpb_secperclus;
  put16(b, (uint32_t)k_bpb_off_rsvdseccnt, (uint16_t)k_bpb_rsvdseccnt);
  b[k_bpb_off_numfats] = (uint8_t)k_bpb_numfats;
  put16(b, (uint32_t)k_bpb_off_rootentcnt, (uint16_t)k_bpb_rootentcnt);
  put16(b, (uint32_t)k_bpb_off_totsec16, (uint16_t)k_blocks_fat16);
  put16(b, (uint32_t)k_bpb_off_secfat, (uint16_t)k_bpb_fatsz16);
  b[k_bpb_sig_off_a] = (uint8_t)k_bpb_sig_a;
  b[k_bpb_sig_off_b] = (uint8_t)k_bpb_sig_b;
}

/**
 * @brief Format a 4 MiB FAT16 image, optionally write a font, dump to disk.
 *
 * @param[in] image_out Output path for the raw FAT image.
 * @param[in] font      Font bytes to write, or NULL for a blank card.
 * @param[in] font_len  Length of @p font (ignored when @p font is NULL).
 * @param[in] dest_name 8.3 name on the card (ignored when @p font is NULL).
 * @return 0 on success, 1 on any allocation / ra8_fs / I/O failure.
 */
static int
build_and_dump(const char* image_out, const uint8_t* font, size_t font_len, const char* dest_name)
{
  s_disk.block_count = (uint32_t)k_blocks_fat16;
  s_disk.bytes       = (uint8_t*)calloc(1U, (size_t)k_blocks_fat16 * (size_t)k_block_size);
  if (s_disk.bytes == nullptr) {
    (void)fprintf(stderr, "mkfontimg: out of memory (disk)\n");
    return 1;
  }
  build_fat16(s_disk.bytes);

  const ra8_fs_backend_t backend = {.read_block   = mem_read,
                                    .write_block  = mem_write,
                                    .get_capacity = mem_cap,
                                    .ctx          = &s_disk};

  /* Write the font (if any) through the real ra8_fs so the on-card layout
   * matches exactly what the firmware app will read. */
  ra8_fs_mount_t* mnt = nullptr;
  if (ra8_fs_mount(&backend, &mnt) != k_ra8_ok) {
    (void)fprintf(stderr, "mkfontimg: ra8_fs_mount failed\n");
    free(s_disk.bytes);
    return 1;
  }
  if (font != nullptr) {
    ra8_fs_file_t* f = nullptr;
    if (ra8_fs_open(mnt, dest_name, k_ra8_fs_mode_write, &f) != k_ra8_ok) {
      (void)fprintf(stderr, "mkfontimg: ra8_fs_open(%s) failed\n", dest_name);
      free(s_disk.bytes);
      return 1;
    }
    if (ra8_fs_write(f, font, (uint32_t)font_len) != k_ra8_ok) {
      (void)fprintf(stderr, "mkfontimg: ra8_fs_write failed\n");
      free(s_disk.bytes);
      return 1;
    }
    (void)ra8_fs_close(f);
  }
  (void)ra8_fs_unmount(mnt);

  FILE* fout = fopen(image_out, "wb");
  if (fout == nullptr) {
    (void)fprintf(stderr, "mkfontimg: cannot write %s\n", image_out);
    free(s_disk.bytes);
    return 1;
  }
  size_t wrote = fwrite(s_disk.bytes, 1U, (size_t)k_blocks_fat16 * (size_t)k_block_size, fout);
  (void)fclose(fout);
  free(s_disk.bytes);
  if (wrote != (size_t)k_blocks_fat16 * (size_t)k_block_size) {
    (void)fprintf(stderr, "mkfontimg: short write to %s\n", image_out);
    return 1;
  }
  return 0;
}

int main(int argc, char** argv)
{
  /* Blank-card mode: format an empty FAT16 image with no font. */
  if ((argc >= 2) && (strcmp(argv[1], "--blank") == 0)) {
    if (argc != 3) {
      (void)fprintf(stderr, "usage: %s --blank <image-out>\n", argv[0]);
      return 2;
    }
    if (build_and_dump(argv[2], nullptr, 0U, nullptr) != 0) {
      return 1;
    }
    (void)fprintf(stderr, "mkfontimg: wrote %s (blank FAT16, no font)\n", argv[2]);
    return 0;
  }

  if (argc < 3) {
    (void)fprintf(stderr, "usage: %s <font-in> <image-out> [dest-name]\n", argv[0]);
    return 2;
  }
  const char* font_in   = argv[1];
  const char* image_out = argv[2];
  const char* dest_name = (argc > 3) ? argv[3] : "FONT.OTF";

  /* Slurp the source font. */
  uint8_t* font = (uint8_t*)malloc((size_t)k_font_cap);
  if (font == nullptr) {
    (void)fprintf(stderr, "mkfontimg: out of memory\n");
    return 1;
  }
  FILE* fin = fopen(font_in, "rb");
  if (fin == nullptr) {
    (void)fprintf(stderr, "mkfontimg: cannot open %s\n", font_in);
    free(font);
    return 1;
  }
  size_t font_len = fread(font, 1U, (size_t)k_font_cap, fin);
  (void)fclose(fin);
  if (font_len < (size_t)k_font_min_bytes) {
    (void)fprintf(stderr, "mkfontimg: %s too small (%zu bytes)\n", font_in, font_len);
    free(font);
    return 1;
  }

  const int rc = build_and_dump(image_out, font, font_len, dest_name);
  free(font);
  if (rc != 0) {
    return 1;
  }
  (void)fprintf(stderr, "mkfontimg: wrote %s (%s = %zu bytes)\n", image_out, dest_name, font_len);
  return 0;
}
