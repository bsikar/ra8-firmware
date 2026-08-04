/**
 * @file board_periph_sd_image.c
 * @brief Backing-image lifecycle for the modelled SD card (see board_periph_sd.h)
 *
 * @details
 * Owns everything about where the modelled card's 512-byte sectors physically
 * live, leaving board_periph_sd.c to model the SD SPI protocol on top of them:
 * attaching a card from a host image file, synthesizing a freshly formatted
 * blank card over a sparse anonymous mmap (so a multi-GB card costs only the
 * sectors actually touched), saving the image back out, releasing the mapping,
 * and reporting the attached card's geometry.
 *
 * Split out of board_periph_sd.c along the same seam that already separates
 * board_periph_sd_format.c: image lifecycle, protocol engine, and FAT
 * formatting are three responsibilities, and the shared card state travels
 * through board_periph_sd_internal.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "board_console.h"
#include "board_periph_sd.h"
#include "board_periph_sd_internal.h"

/** @brief Release the current backing image (munmap a sparse card, else free). */
static void board_sd_release_image(void)
{
  if (s_sd.image != nullptr) {
    if (s_sd.mmapped) {
      (void)munmap(s_sd.image, (size_t)s_sd.image_len);
      if (s_sd.map_fd >= 0) {
        (void)close(s_sd.map_fd);
      }
    } else {
      free(s_sd.image);
    }
  }
  s_sd.image   = nullptr;
  s_sd.mmapped = false;
  s_sd.map_fd  = -1;
}

bool board_sd_attach(const char* path)
{
  if (path == nullptr) {
    return false;
  }
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    (void)fprintf(stderr, "ra8_emulator: --sd: cannot open '%s'\n", path);
    return false;
  }
  (void)fseek(fp, 0L, SEEK_END);
  const long size = ftell(fp);
  (void)fseek(fp, 0L, SEEK_SET);
  if (size <= 0L) {
    (void)fclose(fp);
    (void)fprintf(stderr, "ra8_emulator: --sd: empty image '%s'\n", path);
    return false;
  }
  board_sd_release_image();
  s_sd        = (board_sd_state_t){};
  s_sd.map_fd = -1;
  s_sd.image  = (uint8_t*)malloc((size_t)size);
  if (s_sd.image == nullptr) {
    (void)fclose(fp);
    return false;
  }
  const size_t got = fread(s_sd.image, 1U, (size_t)size, fp);
  (void)fclose(fp);
  if (got != (size_t)size) {
    board_sd_release_image();
    return false;
  }
  s_sd.image_len = (uint64_t)size;
  s_sd.attached  = true;
  (void)fprintf(stderr, "ra8_emulator: SD card attached (%ld bytes) from %s\n", size, path);
  return true;
}

bool board_sd_attached(void)
{
  return s_sd.attached;
}

/**
 * @brief Allocate a sparse, anonymous mmap-backed image buffer for a blank card.
 *
 * @details
 * Backs the card with a sparse mmap'd temp file so a multi-GB card only ever
 * materialises the few sectors the formatter + firmware actually touch (e.g. a
 * 30 GB card costs kilobytes of host RAM, not 30 GB). Creates an anonymous temp
 * file with `mkstemp` + `unlink`, sizes it with `ftruncate`, then maps it
 * read/write and shared. On any failure it emits the same diagnostic the caller
 * used to emit inline, closes the descriptor if one was opened, and returns
 * `nullptr`. Extracted verbatim from `board_sd_attach_blank()`.
 *
 * @param[in]  bytes  Card size in bytes to reserve for the mapping.
 * @param[out] out_fd Receives the backing file descriptor on success only.
 * @return Pointer to the mapped image buffer, or `nullptr` on failure.
 * @retval nullptr mkstemp, ftruncate, or mmap failed (descriptor already closed).
 * @retval non-null Mapped buffer of `bytes`; `*out_fd` holds its live descriptor.
 * @pre `out_fd` is non-null.
 * @pre `bytes` is non-zero.
 * @post On success `*out_fd` is an open descriptor owning the mapping's storage.
 * @post On failure no descriptor leaks and `*out_fd` is left unmodified.
 * @note Not thread-safe; intended for single-threaded card setup.
 * @since 0.1.0
 */
static uint8_t* board_sd_map_blank_image(uint64_t bytes, int* out_fd)
{
  char tmpl[] = "/tmp/ra8_emulator_sd.XXXXXX";
  int  fd     = mkstemp(tmpl);
  if (fd < 0) {
    (void)fprintf(stderr, "ra8_emulator: --sd-new: mkstemp failed\n");
    return nullptr;
  }
  (void)unlink(tmpl); /* anonymous: the storage lives until the fd is closed. */
  if (ftruncate(fd, (off_t)bytes) != 0) {
    (void)close(fd);
    (void)fprintf(stderr,
                  "ra8_emulator: --sd-new: ftruncate to %llu bytes failed\n",
                  (unsigned long long)bytes);
    return nullptr;
  }
  uint8_t* img = (uint8_t*)mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (img == MAP_FAILED) {
    (void)close(fd);
    (void)fprintf(stderr,
                  "ra8_emulator: --sd-new: mmap of %llu bytes failed\n",
                  (unsigned long long)bytes);
    return nullptr;
  }
  *out_fd = fd;
  return img;
}

/**
 * @brief Emit the "SD card created" diagnostic for a freshly formatted card.
 *
 * @details
 * Prints the card geometry to stderr in GiB when the card is at least 1 GiB and
 * in MiB otherwise, matching the two-branch message the caller used to emit
 * inline. Extracted verbatim from `board_sd_attach_blank()`.
 *
 * @param[in] bytes    Card size in bytes.
 * @param[in] fat_bits FAT width just applied (16 or 32).
 * @param[in] spc      Sectors-per-cluster chosen by the formatter.
 * @pre `bytes` is the size of an already-attached card.
 * @pre `spc` is the formatter's returned sectors-per-cluster.
 * @post Exactly one diagnostic line is written to stderr.
 * @post No program state other than the stderr stream is modified.
 * @note Not thread-safe; writes to the shared stderr stream.
 * @since 0.1.0
 */
static void board_sd_report_created(uint64_t bytes, uint8_t fat_bits, uint32_t spc)
{
  if (bytes >= ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib * (uint64_t)k_unit_kib)) {
    (void)fprintf(stderr,
                  "ra8_emulator: SD card created (%llu GiB FAT%u, %u sec/clus) sparse + attached\n",
                  (unsigned long long)(bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib *
                                                (uint64_t)k_unit_kib)),
                  (unsigned)fat_bits,
                  (unsigned)spc);
  } else {
    (void)fprintf(stderr,
                  "ra8_emulator: SD card created (%llu MiB FAT%u, %u sec/clus) sparse + attached\n",
                  (unsigned long long)(bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
                  (unsigned)fat_bits,
                  (unsigned)spc);
  }
}

bool board_sd_attach_blank(uint32_t total_sectors, uint8_t fat_bits, const char* label)
{
  if (total_sectors < (uint32_t)k_sd_min_sectors) {
    (void)fprintf(stderr, "ra8_emulator: --sd-new: size too small (need >= 32 KiB)\n");
    return false;
  }
  const uint64_t bytes = (uint64_t)total_sectors * (uint64_t)k_fmt_sec_bytes;
  int            fd    = -1;
  uint8_t*       img   = board_sd_map_blank_image(bytes, &fd);
  if (img == nullptr) {
    return false;
  }
  const uint32_t spc = (fat_bits == (uint8_t)k_fat32_bits)
                         ? sd_format_fat32(img, total_sectors, label)
                         : sd_format_fat16(img, total_sectors, label);
  board_sd_release_image();
  s_sd           = (board_sd_state_t){};
  s_sd.image     = img;
  s_sd.image_len = bytes;
  s_sd.mmapped   = true;
  s_sd.map_fd    = fd;
  s_sd.attached  = true;
  s_sd.fat_bits =
    (fat_bits == (uint8_t)k_fat32_bits) ? (uint8_t)k_fat32_bits : (uint8_t)k_fat16_bits;
  sd_label_field((uint8_t*)s_sd.label, label);
  s_sd.label[k_fmt_label_len] = '\0';
  board_sd_report_created(bytes, s_sd.fat_bits, spc);
  return true;
}

bool board_sd_save(const char* path)
{
  if ((path == nullptr) || !s_sd.attached || (s_sd.image == nullptr)) {
    return false;
  }
  /* A sparse multi-GB card would dump GBs of mostly-zeros; cap the dump so
   * --save-sd stays sane. Inspect a large card by its live mount instead. */
  if (s_sd.image_len > (uint64_t)k_sd_save_max_bytes) {
    (void)fprintf(
      stderr,
      "ra8_emulator: --save-sd: card is %llu MiB (> %u MiB cap) -- skipped\n",
      (unsigned long long)(s_sd.image_len / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
      (unsigned)((uint64_t)k_sd_save_max_bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)));
    return false;
  }
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr) {
    (void)fprintf(stderr, "ra8_emulator: --save-sd: cannot write '%s'\n", path);
    return false;
  }
  const size_t put = fwrite(s_sd.image, 1U, (size_t)s_sd.image_len, fp);
  (void)fclose(fp);
  if (put != (size_t)s_sd.image_len) {
    return false;
  }
  (void)fprintf(stderr,
                "ra8_emulator: SD card image saved (%llu bytes) to %s\n",
                (unsigned long long)s_sd.image_len,
                path);
  return true;
}

void board_sd_info(bool* attached, uint64_t* bytes, uint8_t* fat_bits, const char** label)
{
  if (attached != nullptr) {
    *attached = s_sd.attached;
  }
  if (bytes != nullptr) {
    *bytes = s_sd.image_len;
  }
  if (fat_bits != nullptr) {
    *fat_bits = s_sd.fat_bits;
  }
  if (label != nullptr) {
    *label = s_sd.label;
  }
}
