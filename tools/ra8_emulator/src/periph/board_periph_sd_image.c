/**
 * @file board_periph_sd_image.c
 * @brief Sparse raw-file lifecycle for the modelled SD card.
 * @details Owns the private anonymous working descriptor used by the SD model.
 * Input images are copied with a fixed transfer buffer so firmware writes never
 * mutate the source. Blank cards remain sparse at their full advertised size,
 * and saves publish through a sibling transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifndef _GNU_SOURCE
/** @brief Request GNU descriptor-relative syscall declarations on Linux. */
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/falloc.h>
#endif

#include "board_periph_sd.h"
#include "board_periph_sd_internal.h"
#include "emu_host_io_internal.h"

/** @brief Fixed transfer size for card-image copies and portable zeroing. */
typedef enum : uint32_t {
  k_sd_io_chunk = 4096U, /**< Bounded stack scratch; independent of card size. */
} board_sd_io_const_t;

/**
 * @brief Close a descriptor when owned and invalidate the caller's slot.
 * @details Accepts null and already-invalid slots for idempotent unwind paths.
 * @param[in,out] fd Descriptor slot to release.
 * @pre @p fd is null or points to caller-owned descriptor state.
 * @pre No concurrent operation uses a non-negative descriptor in the slot.
 * @post A non-null slot contains -1.
 * @post Any formerly owned descriptor has been offered to `close` once.
 * @note Close errors are intentionally ignored during best-effort cleanup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_close_fd(int* fd)
{
  if ((fd != nullptr) && (*fd >= 0)) {
    (void)close(*fd);
    *fd = -1;
  }
}

/**
 * @brief Release the currently attached anonymous working file.
 * @details Closes the private descriptor and clears observable attachment geometry.
 * @pre Emulator SD state is single-thread owned.
 * @pre No protocol transfer concurrently uses the working descriptor.
 * @post Image descriptor is invalid, length is zero, and attachment is false.
 * @post No source or saved host image is changed.
 * @note Not thread-safe through global SD state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_release_image(void)
{
  internal_close_fd(&g_board_sd.image_fd);
  g_board_sd.image_len = 0U;
  g_board_sd.attached  = false;
}

/**
 * @brief Create, unlink, and size an anonymous sparse working file.
 * @details Uses a private temporary name only until open, then unlinks it and
 * sets the exact logical length without materializing zero-filled card bytes.
 * @param[in] bytes Required logical image length.
 * @param[out] out_fd Receives the owned anonymous descriptor.
 * @return Whether complete sparse setup succeeded.
 * @retval true Caller owns an unlinked descriptor of exact length.
 * @retval false Arguments, host creation, unlink-tolerant setup, or truncate failed.
 * @pre @p out_fd is non-null and writable.
 * @pre @p bytes is non-zero and representable by host `off_t`.
 * @post Success initializes @p out_fd with one owned descriptor.
 * @post Failure leaves no descriptor or named temporary owned.
 * @note Sparse allocation policy is supplied by the host filesystem.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_create_sparse(uint64_t bytes, int* out_fd)
{
  if ((out_fd == nullptr) || (bytes == 0U) || (bytes > (uint64_t)INT64_MAX)) {
    return false;
  }
  char path[] = "/tmp/ra8_emulator_sd.XXXXXX";
  int  fd     = mkstemp(path);
  if (fd < 0) {
    return false;
  }
  (void)unlink(path);
  if (ftruncate(fd, (off_t)bytes) != 0) {
    internal_close_fd(&fd);
    return false;
  }
  *out_fd = fd;
  return true;
}

/**
 * @brief Copy a descriptor range with one fixed scratch buffer.
 * @details Performs exact positioned reads and writes in bounded 4 KiB chunks.
 * @param[in] source_fd Open source descriptor.
 * @param[in] target_fd Open private target descriptor.
 * @param[in] bytes Exact byte count to copy from offset zero.
 * @return Whether the complete range was reproduced.
 * @retval true Every chunk was read and written exactly.
 * @retval false First exact host I/O operation failed.
 * @pre Both descriptors are open for their respective operations.
 * @pre Both files span at least @p bytes and offsets are host-representable.
 * @post Success makes target bytes `[0,bytes)` equal source bytes.
 * @post Descriptor stream positions remain unchanged.
 * @note Bounded stack use is independent of card size.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_copy_bytes(int source_fd, int target_fd, uint64_t bytes)
{
  uint8_t  scratch[k_sd_io_chunk];
  uint64_t offset = 0U;
  while (offset < bytes) {
    const uint64_t remaining = bytes - offset;
    const size_t   count =
      (remaining < (uint64_t)sizeof(scratch)) ? (size_t)remaining : sizeof(scratch);
    const emu_io_result_t read_result =
      priv_emu_io_pread_exact(source_fd, scratch, count, (off_t)offset);
    if (read_result.status != k_emu_io_ok) {
      return false;
    }
    const emu_io_result_t write_result =
      priv_emu_io_pwrite_exact(target_fd, scratch, count, (off_t)offset);
    if (write_result.status != k_emu_io_ok) {
      return false;
    }
    offset += (uint64_t)count;
  }
  return true;
}

/**
 * @brief Replace the active card with a prepared working file.
 * @details Releases prior private state before installing the fully prepared descriptor.
 * @param[in] image_fd Owned anonymous descriptor to adopt.
 * @param[in] bytes Exact logical card length.
 * @pre @p image_fd is open, private, and sized to @p bytes.
 * @pre No protocol operation concurrently accesses global SD state.
 * @post Prior working descriptor is closed exactly once.
 * @post New descriptor, length, and attached state are installed together.
 * @note Ownership of @p image_fd transfers to global SD state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_adopt_image(int image_fd, uint64_t bytes)
{
  internal_release_image();
  g_board_sd           = (board_sd_state_t){.image_fd = image_fd};
  g_board_sd.image_len = bytes;
  g_board_sd.attached  = true;
}

/**
 * @brief Validate a positioned operation against descriptor and card bounds.
 * @details Uses subtractive range checks after verifying attachment and host offset limits.
 * @param[in] offset First card byte.
 * @param[in] count Requested byte count.
 * @return Whether the complete half-open range is accessible.
 * @retval true Attached descriptor and geometry contain the request.
 * @retval false State, offset, or length is invalid.
 * @pre Global SD state is readable and single-thread owned.
 * @pre @p count may be zero through any `size_t` value.
 * @post No global or host state changes.
 * @post No host I/O is issued.
 * @note Not thread-safe with card replacement.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_range_valid(uint64_t offset, size_t count)
{
  if (!g_board_sd.attached || (g_board_sd.image_fd < 0) || (offset > g_board_sd.image_len) ||
      (offset > (uint64_t)INT64_MAX)) {
    return false;
  }
  return (uint64_t)count <= (g_board_sd.image_len - offset);
}

bool priv_board_sd_storage_read(uint64_t offset, void* dst, size_t count)
{
  if (((dst == nullptr) && (count != 0U)) || !internal_range_valid(offset, count)) {
    return false;
  }
  const emu_io_result_t result =
    priv_emu_io_pread_exact(g_board_sd.image_fd, dst, count, (off_t)offset);
  return result.status == k_emu_io_ok;
}

bool priv_board_sd_storage_write(uint64_t offset, const void* src, size_t count)
{
  if (((src == nullptr) && (count != 0U)) || !internal_range_valid(offset, count)) {
    return false;
  }
  const emu_io_result_t result =
    priv_emu_io_pwrite_exact(g_board_sd.image_fd, src, count, (off_t)offset);
  return result.status == k_emu_io_ok;
}

/**
 * @brief Portably zero a range with bounded memory when hole punching is unavailable.
 * @details Reuses one fixed zero buffer and exact positioned writes for the full range.
 * @param[in] offset First card byte to clear.
 * @param[in] count Number of bytes to clear.
 * @return Whether every chunk was written.
 * @retval true Complete range now reads as zero.
 * @retval false First exact storage write failed.
 * @pre Complete range passed the public zero-operation bounds checks.
 * @pre Working image descriptor remains attached and writable.
 * @post Success overwrites exactly `[offset, offset + count)` with zeroes.
 * @post Stack usage is constant independent of @p count.
 * @note Used after unsupported Linux hole punching and on other hosts.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_zeros(uint64_t offset, uint64_t count)
{
  const uint8_t zeros[k_sd_io_chunk] = {};
  uint64_t      done                 = 0U;
  while (done < count) {
    const uint64_t remaining = count - done;
    const size_t chunk = (remaining < (uint64_t)sizeof(zeros)) ? (size_t)remaining : sizeof(zeros);
    if (!priv_board_sd_storage_write(offset + done, zeros, chunk)) {
      return false;
    }
    done += (uint64_t)chunk;
  }
  return true;
}

bool priv_board_sd_storage_zero(uint64_t offset, uint64_t count)
{
  if (!g_board_sd.attached || (g_board_sd.image_fd < 0) || (offset > g_board_sd.image_len) ||
      (count > (g_board_sd.image_len - offset)) || (offset > (uint64_t)INT64_MAX) ||
      (count > (uint64_t)INT64_MAX)) {
    return false;
  }
  if (count == 0U) {
    return true;
  }
#if defined(__linux__)
  if (fallocate(g_board_sd.image_fd,
                FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                (off_t)offset,
                (off_t)count) == 0) {
    return true;
  }
  if ((errno != EOPNOTSUPP) && (errno != ENOSYS) && (errno != EINVAL)) {
    return false;
  }
#endif
  return internal_write_zeros(offset, count);
}

bool board_sd_attach(const char* path)
{
  if (path == nullptr) {
    return false;
  }
  emu_io_file_t source = {.fd = -1, .size = 0};
  if (priv_emu_io_open_read(path, &source).status != k_emu_io_ok) {
    (void)priv_emu_io_errf("ra8_emulator: --sd: cannot open '%s'\n", path);
    return false;
  }
  if (source.size <= 0) {
    (void)priv_emu_io_close(&source);
    (void)priv_emu_io_errf("ra8_emulator: --sd: empty image '%s'\n", path);
    return false;
  }
  const uint64_t bytes      = (uint64_t)source.size;
  int            working_fd = -1;
  const bool     prepared =
    internal_create_sparse(bytes, &working_fd) && internal_copy_bytes(source.fd, working_fd, bytes);
  (void)priv_emu_io_close(&source);
  if (!prepared) {
    internal_close_fd(&working_fd);
    (void)priv_emu_io_errf("ra8_emulator: --sd: cannot prepare private image '%s'\n", path);
    return false;
  }
  internal_adopt_image(working_fd, bytes);
  (void)priv_emu_io_errf("ra8_emulator: SD card attached (%llu bytes) from %s\n",
                         (unsigned long long)g_board_sd.image_len,
                         path);
  return true;
}

bool board_sd_attached(void)
{
  return g_board_sd.attached;
}

/**
 * @brief Emit the stable blank-card geometry diagnostic.
 * @details Selects GiB or MiB units while preserving the established CLI spelling.
 * @param[in] bytes Exact logical card length.
 * @param[in] fat_bits Selected FAT width.
 * @param[in] spc Chosen sectors per cluster.
 * @pre @p bytes came from a successfully formatted attached image.
 * @pre @p fat_bits and @p spc describe that image geometry.
 * @post One best-effort diagnostic line was attempted.
 * @post Card image and protocol state are unchanged.
 * @note Diagnostic failure does not invalidate the attached card.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_created(uint64_t bytes, uint8_t fat_bits, uint32_t spc)
{
  if (bytes >= ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib * (uint64_t)k_unit_kib)) {
    (void)priv_emu_io_errf(
      "ra8_emulator: SD card created (%llu GiB FAT%u, %u sec/clus) sparse + attached\n",
      (unsigned long long)(bytes /
                           ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
      (unsigned)fat_bits,
      (unsigned)spc);
  } else {
    (void)priv_emu_io_errf(
      "ra8_emulator: SD card created (%llu MiB FAT%u, %u sec/clus) sparse + attached\n",
      (unsigned long long)(bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
      (unsigned)fat_bits,
      (unsigned)spc);
  }
}

bool board_sd_attach_blank(uint32_t total_sectors, uint8_t fat_bits, const char* label)
{
  if (total_sectors < (uint32_t)k_sd_min_sectors) {
    (void)priv_emu_io_errf("ra8_emulator: --sd-new: size too small (need >= 32 KiB)\n");
    return false;
  }
  const uint64_t bytes    = (uint64_t)total_sectors * (uint64_t)k_fmt_sec_bytes;
  int            image_fd = -1;
  if (!internal_create_sparse(bytes, &image_fd)) {
    (void)priv_emu_io_errf("ra8_emulator: --sd-new: cannot create %llu-byte sparse image\n",
                           (unsigned long long)bytes);
    return false;
  }
  uint32_t   spc       = 0U;
  const bool formatted = (fat_bits == (uint8_t)k_fat32_bits)
                           ? priv_board_sd_format_fat32(image_fd, total_sectors, label, &spc)
                           : priv_board_sd_format_fat16(image_fd, total_sectors, label, &spc);
  if (!formatted) {
    internal_close_fd(&image_fd);
    (void)priv_emu_io_errf("ra8_emulator: --sd-new: FAT format write failed\n");
    return false;
  }
  internal_adopt_image(image_fd, bytes);
  g_board_sd.fat_bits =
    (fat_bits == (uint8_t)k_fat32_bits) ? (uint8_t)k_fat32_bits : (uint8_t)k_fat16_bits;
  priv_board_sd_label_field((uint8_t*)g_board_sd.label, label);
  g_board_sd.label[k_fmt_label_len] = '\0';
  internal_report_created(bytes, g_board_sd.fat_bits, spc);
  return true;
}

bool board_sd_save(const char* path)
{
  if ((path == nullptr) || !g_board_sd.attached || (g_board_sd.image_fd < 0)) {
    return false;
  }
  if (g_board_sd.image_len > (uint64_t)k_sd_save_max_bytes) {
    (void)priv_emu_io_errf(
      "ra8_emulator: --save-sd: card is %llu MiB (> %u MiB cap) -- skipped\n",
      (unsigned long long)(g_board_sd.image_len / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
      (unsigned)((uint64_t)k_sd_save_max_bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)));
    return false;
  }
  emu_io_txn_t txn = {.fd = -1};
  if (priv_emu_io_txn_begin(path, &txn).status != k_emu_io_ok) {
    (void)priv_emu_io_errf("ra8_emulator: --save-sd: cannot write '%s'\n", path);
    return false;
  }
  if (!internal_copy_bytes(g_board_sd.image_fd, txn.fd, g_board_sd.image_len) ||
      (priv_emu_io_txn_commit(&txn).status != k_emu_io_ok)) {
    priv_emu_io_txn_abort(&txn);
    return false;
  }
  (void)priv_emu_io_errf("ra8_emulator: SD card image saved (%llu bytes) to %s\n",
                         (unsigned long long)g_board_sd.image_len,
                         path);
  return true;
}

void board_sd_info(bool* attached, uint64_t* bytes, uint8_t* fat_bits, const char** label)
{
  if (attached != nullptr) {
    *attached = g_board_sd.attached;
  }
  if (bytes != nullptr) {
    *bytes = g_board_sd.image_len;
  }
  if (fat_bits != nullptr) {
    *fat_bits = g_board_sd.fat_bits;
  }
  if (label != nullptr) {
    *label = g_board_sd.label;
  }
}
