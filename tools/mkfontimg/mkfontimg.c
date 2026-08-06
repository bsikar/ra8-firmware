/**
 * @file mkfontimg.c
 * @brief Build a FAT16 SD-card image carrying a single font file, written
 *        through the real ra8_fs so ra8_emulator's app reads it back bit-for-bit.
 *
 * @details
 * ra8_emulator's @c --sd flag attaches a raw FAT image to the modelled SD card.
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
 * @since 0.1.0
 *
 *

 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_host_arena.h"

enum : uint32_t { k_arena_bytes = 128U * 1024U * 1024U }; // 128 MiB - generous for disk images

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

/**
 * @brief ra8_fs_backend_t::read_block over the flat sector store.
 *
 * @details
 * Copies @p count contiguous 512-byte sectors starting at @p lba out of the
 * ::mem_disk_t byte store. ra8_fs reads through this facade while mounting and
 * reading the image, so the host tool and the firmware agree on the on-card
 * layout byte for byte.
 *
 * @param[in]  ctx   The ::mem_disk_t backing store, type-erased as ra8_fs wants.
 * @param[in]  lba   First logical block address to read.
 * @param[in]  count Number of 512-byte blocks to read.
 * @param[out] buf   Destination for @p count * 512 bytes.
 *
 * @return Block-device status.
 * @retval k_ra8_ok               The sectors were copied out.
 * @retval k_ra8_err_out_of_range @p lba + @p count runs past the disk.
 *
 * @pre @p ctx points at an initialised ::mem_disk_t whose bytes are allocated.
 * @pre @p buf has room for @p count * 512 bytes.
 * @post On success @p buf holds the requested sectors.
 * @post The backing store is left unmodified.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
// cppcheck-suppress constParameterCallback
static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_block_size],
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

/**
 * @brief ra8_fs_backend_t::write_block over the flat sector store.
 *
 * @details
 * Copies @p count contiguous 512-byte sectors from @p buf into the
 * ::mem_disk_t byte store at @p lba. ra8_fs writes the font and directory
 * entries through this facade; the whole image is dumped verbatim afterwards.
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
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_block_size],
         buf,
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

/**
 * @brief ra8_fs_backend_t::get_capacity for the modelled disk.
 *
 * @details
 * Reports the geometry ra8_fs needs: the number of 512-byte blocks in the
 * ::mem_disk_t store (::k_blocks_fat16) and the fixed sector size
 * (::k_block_size).
 *
 * @param[in]  ctx         The ::mem_disk_t backing store, type-erased.
 * @param[out] block_count Receives the number of 512-byte blocks.
 * @param[out] block_size  Receives the sector size in bytes.
 *
 * @return Block-device status.
 * @retval k_ra8_ok Geometry was reported (this shim cannot fail).
 *
 * @pre @p ctx points at an initialised ::mem_disk_t.
 * @pre @p block_count and @p block_size are non-nullptr.
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
  *block_size         = (uint32_t)k_block_size;
  return k_ra8_ok;
}

/**
 * @brief Write a little-endian uint16 into the BPB at a byte offset.
 *
 * @details
 * Stores @p v as two bytes, low byte first, at @p p[@p off]. FAT BPB fields are
 * little-endian regardless of host endianness, so the bytes are assembled by
 * hand rather than through a struct overlay.
 *
 * @param[out] p   Base of the image buffer being built.
 * @param[in]  off Byte offset of the field within @p p.
 * @param[in]  v   The 16-bit value to store little-endian.
 *
 * @pre @p p is non-nullptr with at least @p off + 2 bytes.
 * @pre @p off is a valid BPB field offset.
 * @post @p p[@p off] and @p p[@p off + 1] hold @p v low byte first.
 * @post No other bytes of @p p are touched.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]      = (uint8_t)(v & (uint16_t)k_byte_lo_mask);
  p[off + 1U] = (uint8_t)((v >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_lo_mask);
}

/**
 * @brief Lay down a minimal FAT16 BPB on the zeroed image.
 *
 * @details
 * Writes just the BPB fields ra8_fs needs to recognise and mount the volume --
 * bytes-per-sector, sectors-per-cluster, reserved count, FAT count, root-entry
 * count, total sectors and FAT size -- plus the 0x55AA boot signature. Mirrors
 * the in-test image builder in tests/test_ra8_sdmmc_card_reflow.c so the two
 * stay in lock-step. The buffer must already be zeroed by the caller.
 *
 * @param[in,out] b Image buffer, pre-zeroed; the boot sector is filled in place.
 *
 * @pre @p b is non-nullptr with at least one 512-byte sector.
 * @pre @p b is zero-initialised before the call.
 * @post @p b carries a mountable FAT16 BPB and boot signature.
 * @post Only BPB fields and the signature bytes are set; the rest stays zero.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
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
 * @brief Write @p font onto the mounted card under @p dest_name.
 *
 * @details
 * Goes through the real `ra8_fs` writer rather than poking the image directly,
 * so the on-card layout is exactly what the firmware app will later read.
 * A nullptr @p font is the blank-card case and succeeds without writing.
 *
 * @param[in,out] mnt       Mounted volume to write into.
 * @param[in]     font      Font bytes, or nullptr to leave the card empty.
 * @param[in]     font_len  Length of @p font (ignored when @p font is nullptr).
 * @param[in]     dest_name 8.3 name to create on the card.
 *
 * @return 0 on success, 1 on any ra8_fs failure (diagnosed on stderr).
 * @retval 0 The font was written and closed, or @p font was nullptr (blank card).
 * @retval 1 ra8_fs_open or ra8_fs_write failed (reported on stderr).
 *
 * @pre @p mnt is a successfully mounted volume.
 * @pre @p dest_name is a valid 8.3 name when @p font is non-nullptr.
 * @post On success the file exists on the card and is closed.
 * @post On failure the caller still owns the image buffer and must free it.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int
write_font_file(ra8_fs_mount_t* mnt, const uint8_t* font, size_t font_len, const char* dest_name)
{
  if (font == nullptr) {
    return 0;
  }
  ra8_fs_file_t* f = nullptr;
  if (ra8_fs_open(mnt, dest_name, k_ra8_fs_mode_write, &f) != k_ra8_ok) {
    (void)fprintf(stderr, "mkfontimg: ra8_fs_open(%s) failed\n", dest_name);
    return 1;
  }
  if (ra8_fs_write(f, font, (uint32_t)font_len) != k_ra8_ok) {
    (void)fprintf(stderr, "mkfontimg: ra8_fs_write failed\n");
    return 1;
  }
  (void)ra8_fs_close(f);
  return 0;
}

/**
 * @brief Write the whole in-memory card image out to @p image_out.
 *
 * @details
 * Streams all ::k_blocks_fat16 * ::k_block_size bytes of ::s_disk to
 * @p image_out with no MBR and no padding, producing the raw FAT16 image the
 * emulator attaches with @c --sd. A short write is treated as failure.
 *
 * @param[in] image_out Output path for the raw FAT image.
 *
 * @return 0 on success, 1 if the file cannot be opened or the write is short.
 * @retval 0 The image bytes were fully written.
 * @retval 1 The output could not be opened, or fewer bytes than expected wrote.
 *
 * @pre `s_disk.bytes` holds a fully built image.
 * @pre @p image_out is non-nullptr.
 * @post On success @p image_out holds exactly the image bytes.
 * @post The output stream is closed on every path.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int dump_image(const char* image_out)
{
  const size_t image_bytes = (size_t)k_blocks_fat16 * (size_t)k_block_size;
  FILE*        fout        = fopen(image_out, "wb");
  if (fout == nullptr) {
    (void)fprintf(stderr, "mkfontimg: cannot write %s\n", image_out);
    return 1;
  }
  const size_t wrote = fwrite(s_disk.bytes, 1U, image_bytes, fout);
  (void)fclose(fout);
  if (wrote != image_bytes) {
    (void)fprintf(stderr, "mkfontimg: short write to %s\n", image_out);
    return 1;
  }
  return 0;
}

/**
 * @brief Format a 4 MiB FAT16 image, optionally write a font, dump to disk.
 *
 * @details
 * Allocates the zeroed sector store, lays the BPB (::build_fat16), mounts it
 * through the memory-backed ra8_fs backend, writes @p font as @p dest_name when
 * one is supplied (::write_font_file), unmounts, and dumps the buffer to
 * @p image_out (::dump_image). The disk buffer is freed on every path. A nullptr
 * @p font produces a formatted-but-empty card.
 *
 * @param[in] image_out Output path for the raw FAT image.
 * @param[in] font      Font bytes to write, or nullptr for a blank card.
 * @param[in] font_len  Length of @p font (ignored when @p font is nullptr).
 * @param[in] dest_name 8.3 name on the card (ignored when @p font is nullptr).
 *
 * @return 0 on success, 1 on any allocation / ra8_fs / I/O failure.
 * @retval 0 The image was formatted, populated and written.
 * @retval 1 Allocation, mount, font write, or dump failed (reported on stderr).
 *
 * @pre @p image_out is a writable path.
 * @pre @p font_len describes @p font when @p font is non-nullptr.
 * @post The disk buffer is allocated and freed within this call.
 * @post On success @p image_out holds the raw FAT16 image.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int build_and_dump(ra8_arena_t*   arena,
                          const char*    image_out,
                          const uint8_t* font,
                          size_t         font_len,
                          const char*    dest_name)
{
  s_disk.block_count = (uint32_t)k_blocks_fat16;
  s_disk.bytes =
    (uint8_t*)ra8_arena_calloc(arena, 1U, (uint32_t)k_blocks_fat16 * (uint32_t)k_block_size);
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
    return 1;
  }
  if (write_font_file(mnt, font, font_len, dest_name) != 0) {
    return 1;
  }
  (void)ra8_fs_unmount(mnt);

  const int rc = dump_image(image_out);
  return rc;
}

/**
 * @brief Read a source font into a freshly allocated buffer.
 *
 * @details
 * Reads at most ::k_font_cap bytes and rejects anything shorter than
 * ::k_font_min_bytes, which is the smallest input that could plausibly be a
 * font rather than a truncated file. The caller owns the returned buffer.
 *
 * @param[in]  font_in  Path to the source font.
 * @param[out] font_len Receives the byte count read, on success only.
 * @return Malloc'd font bytes, or nullptr when the font cannot be read.
 * @retval nullptr Allocation failed, the file is missing, or it is too short.
 *
 * @pre @p font_in and @p font_len are non-nullptr.
 * @pre The process may allocate ::k_font_cap bytes.
 * @post On success the caller owns the buffer and must `free()` it.
 * @post On failure nothing is allocated and the input stream is closed.
 *
 * @note Not thread-safe; the tool is single-threaded.
 */
static uint8_t* slurp_font(ra8_arena_t* arena, const char* font_in, size_t* font_len)
{
  uint8_t* font = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)k_font_cap);
  if (font == nullptr) {
    (void)fprintf(stderr, "mkfontimg: out of memory\n");
    return nullptr;
  }
  FILE* fin = fopen(font_in, "rb");
  if (fin == nullptr) {
    (void)fprintf(stderr, "mkfontimg: cannot open %s\n", font_in);
    return nullptr;
  }
  const size_t len = fread(font, 1U, (size_t)k_font_cap, fin);
  (void)fclose(fin);
  if (len < (size_t)k_font_min_bytes) {
    (void)fprintf(stderr, "mkfontimg: %s too small (%zu bytes)\n", font_in, len);
    return nullptr;
  }
  *font_len = len;
  return font;
}

/**
 * @brief Blank-card mode: format an empty FAT16 image with no font.
 *
 * @details
 * The "random card" case: writes a formatted-but-empty FAT16 image so the
 * firmware app can exercise ::ra8_sdfont_load's self-provisioning path against
 * a card that carries no font. Delegates to ::build_and_dump with a nullptr font.
 *
 * @param[in] argc Argument count, as handed to `main()`.
 * @param[in] argv Argument vector, with `argv[1]` already known to be --blank.
 * @return Process exit status.
 * @retval 0 The image was written.
 * @retval 1 The image could not be built or written.
 * @retval 2 Wrong argument count (usage was printed).
 *
 * @pre @p argv is non-nullptr and `argv[1]` is "--blank".
 * @pre @p argc is at least 2.
 * @post On success the output path holds a blank FAT16 image.
 * @post Nothing is allocated on return.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int run_blank(ra8_arena_t* arena, int argc, char** argv)
{
  if (argc != 3) {
    (void)fprintf(stderr, "usage: %s --blank <image-out>\n", argv[0]);
    return 2;
  }
  if (build_and_dump(arena, argv[2], nullptr, 0U, nullptr) != 0) {
    return 1;
  }
  (void)fprintf(stderr, "mkfontimg: wrote %s (blank FAT16, no font)\n", argv[2]);
  return 0;
}

/**
 * @brief Font mode: slurp a font and write it onto a fresh FAT16 image.
 *
 * @details
 * Reads the source font (::slurp_font), then formats a fresh FAT16 image and
 * writes the font onto it (::build_and_dump) under @p argv[3] or the default
 * @c FONT.OTF. The font buffer is freed on every return path.
 *
 * @param[in] argc Argument count, as handed to `main()`.
 * @param[in] argv Argument vector: font-in, image-out, optional dest-name.
 * @return Process exit status.
 * @retval 0 The image was written with the font on it.
 * @retval 1 The font could not be read, or the image could not be built.
 * @retval 2 Too few arguments (usage was printed).
 *
 * @pre @p argv is non-nullptr.
 * @pre @p argc reflects the length of @p argv.
 * @post The font buffer is freed on every path.
 * @post On success the output path holds a FAT16 image carrying the font.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static int run_font(ra8_arena_t* arena, int argc, char** argv)
{
  if (argc < 3) {
    (void)fprintf(stderr, "usage: %s <font-in> <image-out> [dest-name]\n", argv[0]);
    return 2;
  }
  const char* font_in   = argv[1];
  const char* image_out = argv[2];
  const char* dest_name = (argc > 3) ? argv[3] : "FONT.OTF";

  size_t         font_len = 0U;
  const uint8_t* font     = slurp_font(arena, font_in, &font_len);
  if (font == nullptr) {
    return 1;
  }
  const int rc = build_and_dump(arena, image_out, font, font_len, dest_name);
  if (rc != 0) {
    return 1;
  }
  (void)fprintf(stderr, "mkfontimg: wrote %s (%s = %zu bytes)\n", image_out, dest_name, font_len);
  return 0;
}

RA8_NASA_RULE_3_OK
int main(int argc, char** argv)
{
  uint8_t* arena_buf = (uint8_t*)malloc(k_arena_bytes);
  if (arena_buf == nullptr) {
    (void)fprintf(stderr, "mkfontimg: out of memory for arena\n");
    // cppcheck-suppress memleak  /* arena_buf is nullptr here */
    return 1;
  }
  ra8_arena_t arena;
  if (ra8_arena_init(&arena, arena_buf, k_arena_bytes) != k_ra8_ok) {
    free(arena_buf);
    return 1;
  }

  int rc = 0;
  if ((argc >= 2) && (strcmp(argv[1], "--blank") == 0)) {
    rc = run_blank(&arena, argc, argv);
  } else {
    rc = run_font(&arena, argc, argv);
  }
  free(arena_buf);
  return rc;
}
