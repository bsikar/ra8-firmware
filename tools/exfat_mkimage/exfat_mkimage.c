/**
 * @file tools/exfat_mkimage/exfat_mkimage.c
 * @brief Build an exFAT volume image with ra8_fs, for handing to a real OS
 *
 * @par Tag
 * [Ring 7 / TOOL] {World: NS}
 *
 * @details
 * Creates a RAM-backed card, lays an exFAT volume on it with the very same
 * `ra8_fs` code the firmware runs, writes a known set of files and directories
 * into it -- deliberately including non-ASCII names in four scripts and an
 * astral-plane emoji -- and writes the resulting bytes out as a `.img`.
 *
 * The point is that the image is not synthesised and not translated: it is a
 * verbatim copy of the block device our own driver just wrote. An operating
 * system asked to mount it is therefore judging `ra8_fs`, not a re-encoding of
 * it, which is the one question our own byte assertions cannot answer.
 *
 * This is a host tool with no MMIO, no RTOS and no board: `ra8_fs` is pure
 * logic over a block-device vtable, so this builds and runs anywhere a C23
 * compiler does -- including macOS, where the unit-test harness cannot run.
 * That is what lets the interop check be a local, one-command affair rather
 * than something borrowed from another machine.
 *
 * The file names are built from explicit UTF-8 byte escapes rather than
 * literals so that this source stays pure 7-bit ASCII, as every file in the
 * tree must.
 *
 * @par Example:
 * @code
 * exfat_mkimage /tmp/showcase.img
 * hdiutil attach -imagekey diskimage-class=CRawDiskImage /tmp/showcase.img
 * @endcode
 *
 * @author Brighton Sikarskie
 * @date 2026-08-10
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"

/** @brief Geometry of the RAM card this tool formats. */
typedef enum : uint32_t {
  k_mk_block_size  = 512U,    /**< Bytes per logical block.            */
  k_mk_block_count = 131072U, /**< 64 MiB of blocks.                   */
  k_mk_payload     = 32U,     /**< Bytes written into each file.       */
  k_mk_alphabet    = 26U,     /**< Letters cycled to fill the payload. */
} mk_geometry_t;

/** @brief The RAM card: one flat byte array plus its size. */
typedef struct {
  uint8_t* bytes;  /**< Whole-device storage, k_mk_block_count blocks. */
  uint64_t blocks; /**< Block count, mirrored for the vtable.          */
} mk_disk_t;

static mk_disk_t s_disk = {};

/**
 * @brief Read blocks out of the RAM card.
 * @param[in]  ctx   Unused; the card is a file-scope singleton.
 * @param[in]  lba   First logical block to read.
 * @param[in]  count Number of blocks.
 * @param[out] buf   Destination, `count * k_mk_block_size` bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Blocks copied out.
 * @retval k_ra8_err_out_of_range   The range leaves the card.
 * @pre `buf` is non-NULL and large enough for `count` blocks.
 * @post On success `buf` holds the requested blocks.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t mk_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if ((buf == nullptr) || ((lba + count) > s_disk.blocks)) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &s_disk.bytes[lba * k_mk_block_size], (size_t)count * k_mk_block_size);
  return k_ra8_ok;
}

/**
 * @brief Write blocks into the RAM card.
 * @param[in] ctx   Unused; the card is a file-scope singleton.
 * @param[in] lba   First logical block to write.
 * @param[in] count Number of blocks.
 * @param[in] buf   Source, `count * k_mk_block_size` bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Blocks copied in.
 * @retval k_ra8_err_out_of_range   The range leaves the card.
 * @pre `buf` is non-NULL and holds `count` blocks.
 * @post On success the card carries the written blocks.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t mk_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if ((buf == nullptr) || ((lba + count) > s_disk.blocks)) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&s_disk.bytes[lba * k_mk_block_size], buf, (size_t)count * k_mk_block_size);
  return k_ra8_ok;
}

/**
 * @brief Report the RAM card's geometry.
 * @param[in]  ctx         Unused; the card is a file-scope singleton.
 * @param[out] block_count Receives the block count.
 * @param[out] block_size  Receives the block size in bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Geometry reported.
 * @retval k_ra8_err_invalid_arg   Either output pointer is NULL.
 * @pre Both output pointers are non-NULL.
 * @post On success both outputs describe the card.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t mk_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == nullptr) || (block_size == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *block_count = s_disk.blocks;
  *block_size  = k_mk_block_size;
  return k_ra8_ok;
}

/**
 * @brief One entry this tool creates, named in explicit UTF-8 bytes.
 * @since 0.1.0
 */
typedef struct {
  const char* path;    /**< Absolute path inside the volume.             */
  bool        is_dir;  /**< True to mkdir, false to write a file.        */
  const char* meaning; /**< What the name should look like, for the log. */
} mk_entry_t;

/*
 * The volume's contents, spelled out so there is no mystery about what the
 * image holds. Non-ASCII is written as UTF-8 escapes to keep this file 7-bit
 * clean; the comment on each row says what a human should see.
 */
static const mk_entry_t k_entries[] = {
  {"/\xD0\x9F\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0", true, "Cyrillic directory (Papka)"},
  {"/\xE4\xBD\xA0\xE5\xA5\xBD", true, "CJK directory (ni hao)"},
  {"/Caf\xC3\xA9.txt", false, "Latin-1 accent (Cafe.txt)"},
  {"/\xE4\xBD\xA0\xE5\xA5\xBD.txt", false, "CJK filename"},
  {"/\xF0\x9F\x98\x80.txt", false, "astral-plane emoji, a surrogate pair on disk"},
  {"/\xD0\x9F\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0/n\xC3\xB6te.txt",
   false,
   "non-ASCII file inside a non-ASCII directory"},
  {"/README.txt", false, "plain ASCII control"},
};

/**
 * @brief Create every entry in ::k_entries on a mounted volume.
 * @param[in] mount Mounted volume to populate.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every directory and file was created.
 * @retval other    The first failure, already reported on stderr.
 * @pre `mount` is a live mount handle.
 * @post On success the volume holds the full entry table.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t mk_populate(ra8_fs_mount_t* mount)
{
  uint8_t payload[k_mk_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_mk_payload; i++) {
    payload[i] = (uint8_t)('A' + (i % (uint32_t)k_mk_alphabet));
  }

  for (size_t i = 0U; i < (sizeof(k_entries) / sizeof(k_entries[0])); i++) {
    const mk_entry_t* e   = &k_entries[i];
    const ra8_err_t   err = e->is_dir
                              ? ra8_fs_mkdir(mount, e->path)
                              : ra8_fs_write_file(mount, e->path, payload, (uint32_t)k_mk_payload);
    if (err != k_ra8_ok) {
      (void)fprintf(stderr, "exfat_mkimage: cannot create %s (%d)\n", e->meaning, (int)err);
      return err;
    }
    (void)printf("  created %-44s %s\n", e->path, e->meaning);
  }
  return k_ra8_ok;
}

/**
 * @brief Write the RAM card out verbatim as a disk image.
 * @param[in] out_path Destination file path.
 * @param[in] total    Bytes to write, the whole card.
 * @return bool True when the whole card reached the file.
 * @pre `s_disk.bytes` holds `total` bytes.
 * @post On true the file is a byte-for-byte copy of the card.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
static bool mk_dump(const char* out_path, size_t total)
{
  FILE* fp = fopen(out_path, "wb");
  if (fp == nullptr) {
    (void)fprintf(stderr, "exfat_mkimage: cannot open %s for writing\n", out_path);
    return false;
  }
  const size_t wrote = fwrite(s_disk.bytes, 1U, total, fp);
  (void)fclose(fp);
  if (wrote != total) {
    (void)fprintf(stderr, "exfat_mkimage: short write to %s\n", out_path);
    return false;
  }
  return true;
}

/**
 * @brief Format a RAM card, populate it and write the image out.
 * @param[in] argc Argument count; one optional output path.
 * @param[in] argv Argument vector; `argv[1]` is the image path.
 * @return int Process status.
 * @retval 0 The image was created.
 * @retval 1 Allocation, filesystem or output failure (message on stderr).
 * @pre A C23 host with enough memory for a 64 MiB card.
 * @post On success the named path holds a mountable exFAT image.
 * @note Not thread-safe; this tool is single-threaded.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  const char*  out_path = (argc > 1) ? argv[1] : "exfat_showcase.img";
  const size_t total    = (size_t)k_mk_block_count * (size_t)k_mk_block_size;

  s_disk.bytes  = calloc(1U, total);
  s_disk.blocks = k_mk_block_count;
  if (s_disk.bytes == nullptr) {
    (void)fprintf(stderr, "exfat_mkimage: cannot allocate a %zu byte card\n", total);
    return 1;
  }

  ra8_fs_backend_t backend = {};
  backend.read_block       = mk_read;
  backend.write_block      = mk_write;
  backend.get_capacity     = mk_capacity;

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "RA8IMAGE";

  ra8_fs_mount_t* mount = nullptr;
  ra8_err_t       err   = ra8_fs_format(&backend, &opts);
  if (err == k_ra8_ok) {
    err = ra8_fs_mount(&backend, &mount);
  }
  if (err != k_ra8_ok) {
    (void)fprintf(stderr, "exfat_mkimage: format/mount failed (%d)\n", (int)err);
    free(s_disk.bytes);
    return 1;
  }

  err = mk_populate(mount);
  (void)ra8_fs_unmount(mount);
  if ((err != k_ra8_ok) || !mk_dump(out_path, total)) {
    free(s_disk.bytes);
    return 1;
  }

  free(s_disk.bytes);
  (void)printf("wrote %s (%zu bytes) -- the card exactly as ra8_fs left it\n", out_path, total);
  return 0;
}
