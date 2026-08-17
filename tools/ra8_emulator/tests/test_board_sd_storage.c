/**
 * @file test_board_sd_storage.c
 * @brief Focused tests for the emulator's sparse SD storage ownership domain.
 * @details Covers private-copy attachment, transactional saves, positioned-I/O
 * faults, FAT16/FAT32 serialization, sparse zeroing, bounds, and maximum geometry.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board_periph_sd.h"
#include "board_periph_sd_internal.h"
#include "emu_host_io_internal.h"

/** @brief Test-owned card state normally supplied by the protocol TU. */
board_sd_state_t g_board_sd = {.image_fd = -1};

/** @brief Force positioned writes to fail when set. */
RA8_INTERNAL static bool s_fail_pwrite;

/**
 * @brief Forward one sequential read through the real host syscall.
 * @details Supplies the unfaulted read operation to the emulator I/O seam.
 * @param[in] fd Open readable descriptor.
 * @param[out] dst Destination spanning @p count bytes.
 * @param[in] count Maximum byte count.
 * @return Host syscall result.
 * @retval nonnegative Bytes read, including zero at EOF.
 * @retval -1 Host read failed and set `errno`.
 * @pre @p fd and @p dst satisfy the host read contract.
 * @pre @p dst spans @p count writable bytes when count is non-zero.
 * @post Descriptor position advances by returned positive bytes.
 * @post Destination may contain only the returned byte count.
 * @note Test-only thin syscall adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_real_read(int fd, void* dst, size_t count)
{
  return read(fd, dst, count);
}

/**
 * @brief Forward one sequential write through the real host syscall.
 * @details Supplies the unfaulted write operation to the emulator I/O seam.
 * @param[in] fd Open writable descriptor.
 * @param[in] src Source spanning @p count bytes.
 * @param[in] count Maximum byte count.
 * @return Host syscall result.
 * @retval nonnegative Bytes written.
 * @retval -1 Host write failed and set `errno`.
 * @pre @p fd and @p src satisfy the host write contract.
 * @pre @p src spans @p count readable bytes when count is non-zero.
 * @post Descriptor position advances by returned positive bytes.
 * @post Source bytes are unchanged.
 * @note Test-only thin syscall adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_real_write(int fd, const void* src, size_t count)
{
  return write(fd, src, count);
}

/**
 * @brief Forward one positioned read through the real host syscall.
 * @details Supplies the unfaulted positioned-read operation to the I/O seam.
 * @param[in] fd Open readable descriptor.
 * @param[out] dst Destination spanning @p count bytes.
 * @param[in] count Maximum byte count.
 * @param[in] offset Absolute non-negative byte offset.
 * @return Host syscall result.
 * @retval nonnegative Bytes read, including zero at EOF.
 * @retval -1 Host positioned read failed and set `errno`.
 * @pre Arguments satisfy the host `pread` contract.
 * @pre @p dst spans @p count writable bytes when count is non-zero.
 * @post Descriptor stream position is unchanged.
 * @post Destination may contain only the returned byte count.
 * @note Test-only thin syscall adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_real_pread(int fd, void* dst, size_t count, off_t offset)
{
  return pread(fd, dst, count, offset);
}

/**
 * @brief Forward or deterministically fail one positioned write.
 * @details Returns `EIO` without host mutation while the test fault flag is set;
 * otherwise delegates to the real positioned-write syscall.
 * @param[in] fd Open writable descriptor.
 * @param[in] src Source spanning @p count bytes.
 * @param[in] count Maximum byte count.
 * @param[in] offset Absolute non-negative byte offset.
 * @return Injected or host syscall result.
 * @retval nonnegative Bytes written when the seam is healthy.
 * @retval -1 Injected `EIO` or real host failure.
 * @pre Arguments satisfy the host `pwrite` contract when fault is disabled.
 * @pre @p src spans @p count readable bytes when count is non-zero.
 * @post Descriptor position is unchanged; injected failure writes no bytes.
 * @post Source bytes and fault flag are unchanged.
 * @note Test-only global fault seam and therefore not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_faultable_pwrite(int fd, const void* src, size_t count, off_t offset)
{
  if (s_fail_pwrite) {
    errno = EIO;
    return -1;
  }
  return pwrite(fd, src, count, offset);
}

static const emu_io_ops_t s_k_real_ops = {
  .read_fn   = internal_real_read,
  .write_fn  = internal_real_write,
  .pread_fn  = internal_real_pread,
  .pwrite_fn = internal_faultable_pwrite,
};

/**
 * @brief Create a named test file with exact initial bytes.
 * @details Uses `mkstemp`, exact emulator I/O, and removes the name on write failure.
 * @param[in,out] path_template Writable `mkstemp` template, replaced with final path.
 * @param[in] bytes Initial byte span.
 * @param[in] count Exact byte count.
 * @return Owned open descriptor or failure sentinel.
 * @retval nonnegative Descriptor positioned after the complete fixture.
 * @retval -1 Creation or exact write failed without a surviving partial file.
 * @pre @p path_template is a valid writable template and @p bytes spans @p count.
 * @pre Configured emulator I/O seam is ready.
 * @post Success leaves caller owning one named open descriptor.
 * @post Failure closes the descriptor and unlinks any created name.
 * @note Test-only and not thread-safe for a shared template.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_make_file(char* path_template, const void* bytes, size_t count)
{
  const int fd = mkstemp(path_template);
  if (fd < 0) {
    return -1;
  }
  if (priv_emu_io_write_exact(fd, bytes, count).status != k_emu_io_ok) {
    (void)close(fd);
    (void)unlink(path_template);
    return -1;
  }
  return fd;
}

/**
 * @brief Verify attachment clones input and healthy save publishes exact changes.
 * @details Proves working writes do not mutate source and saved output contains the change.
 * @return Whether every clone, mutation-isolation, save, and cleanup observation passed.
 * @retval true Private-copy and transactional-save behavior matched the contract.
 * @retval false A host or emulator operation/byte comparison failed.
 * @pre Emulator I/O seam is configured with the healthy real operations.
 * @pre Global SD state may replace any prior attached test image.
 * @post Created source and save paths are unlinked on the normal vector path.
 * @post Successful vector leaves an attached private working image.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_attach_and_save(void)
{
  uint8_t input[1024];
  for (size_t i = 0U; i < sizeof(input); ++i) {
    input[i] = (uint8_t)(i & 0xFFU);
  }
  char input_path[] = "/tmp/ra8_sd_input.XXXXXX";
  int  input_fd     = internal_make_file(input_path, input, sizeof(input));
  if (input_fd < 0) {
    return false;
  }
  (void)close(input_fd);
  const uint8_t changed = 0xA5U;
  bool          ok = board_sd_attach(input_path) && priv_board_sd_storage_write(0U, &changed, 1U);
  input_fd         = open(input_path, O_RDONLY);
  uint8_t source_first = 0U;
  ok                   = ok && (input_fd >= 0) && (pread(input_fd, &source_first, 1U, 0) == 1) &&
                         (source_first == input[0]);
  if (input_fd >= 0) {
    (void)close(input_fd);
  }
  char save_path[] = "/tmp/ra8_sd_save.XXXXXX";
  int  save_fd     = mkstemp(save_path);
  ok               = ok && (save_fd >= 0);
  if (save_fd >= 0) {
    (void)close(save_fd);
  }
  ok                  = ok && board_sd_save(save_path);
  save_fd             = open(save_path, O_RDONLY);
  uint8_t saved_first = 0U;
  ok =
    ok && (save_fd >= 0) && (pread(save_fd, &saved_first, 1U, 0) == 1) && (saved_first == changed);
  if (save_fd >= 0) {
    (void)close(save_fd);
  }
  (void)unlink(input_path);
  (void)unlink(save_path);
  return ok;
}

/**
 * @brief Verify positioned faults surface and failed save preserves its target.
 * @details Injects `pwrite` failure into direct storage and transactional save,
 * then rereads the pre-existing destination sentinel.
 * @return Whether both failures surfaced and target bytes were preserved.
 * @retval true Direct write/save failed and sentinel remained exact.
 * @retval false Fault propagation, preservation, or host cleanup failed.
 * @pre A card remains attached from the preceding test vector.
 * @pre I/O seam uses ::internal_faultable_pwrite.
 * @post Fault flag is cleared before returning.
 * @post Named target is unlinked on the normal vector path.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_fault_preserves_target(void)
{
  static const char k_sentinel[] = "keep";
  char              target[]     = "/tmp/ra8_sd_fault.XXXXXX";
  int               fd           = internal_make_file(target, k_sentinel, sizeof(k_sentinel) - 1U);
  if (fd < 0) {
    return false;
  }
  (void)close(fd);
  s_fail_pwrite                     = true;
  const uint8_t value               = 0x5AU;
  const bool    write_rejected      = !priv_board_sd_storage_write(0U, &value, 1U);
  const bool    save_rejected       = !board_sd_save(target);
  s_fail_pwrite                     = false;
  char observed[sizeof(k_sentinel)] = {};
  fd                                = open(target, O_RDONLY);
  const bool preserved = (fd >= 0) && (read(fd, observed, sizeof(k_sentinel) - 1U) == 4) &&
                         (memcmp(observed, k_sentinel, sizeof(k_sentinel) - 1U) == 0);
  if (fd >= 0) {
    (void)close(fd);
  }
  (void)unlink(target);
  return write_rejected && save_rejected && preserved;
}

/**
 * @brief Verify FAT16/FAT32 sectors and sparse zeroing use bounded storage I/O.
 * @details Checks type/signature fields, writes and clears one sector, and proves EOF bounds.
 * @return Whether all format, zeroing, readback, and range checks passed.
 * @retval true Both formats and zero semantics matched their contracts.
 * @retval false Any attach, storage operation, field, or byte check failed.
 * @pre Emulator I/O seam is healthy and global SD state is single-thread owned.
 * @pre Host sparse file supports exact positioned access.
 * @post The final attached image is the FAT32 test card.
 * @post Temporary working files remain anonymous and emulator-owned.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_formats_and_zero(void)
{
  uint8_t boot[k_fmt_sec_bytes] = {};
  if (!board_sd_attach_blank(16384U, (uint8_t)k_fat16_bits, "FAT16TEST") ||
      !priv_board_sd_storage_read(0U, boot, sizeof(boot)) ||
      (memcmp(&boot[54], "FAT16   ", 8U) != 0) || (boot[510] != 0x55U) || (boot[511] != 0xAAU)) {
    return false;
  }
  if (!board_sd_attach_blank(131072U, (uint8_t)k_fat32_bits, "FAT32TEST") ||
      !priv_board_sd_storage_read(0U, boot, sizeof(boot)) ||
      (memcmp(&boot[82], "FAT32   ", 8U) != 0)) {
    return false;
  }
  uint8_t block[k_fmt_sec_bytes];
  (void)memset(block, 0xA5, sizeof(block));
  if (!priv_board_sd_storage_write(4096U, block, sizeof(block)) ||
      !priv_board_sd_storage_zero(4096U, sizeof(block)) ||
      !priv_board_sd_storage_read(4096U, block, sizeof(block))) {
    return false;
  }
  for (size_t i = 0U; i < sizeof(block); ++i) {
    if (block[i] != 0U) {
      return false;
    }
  }
  return !priv_board_sd_storage_read(g_board_sd.image_len, block, 1U);
}

/**
 * @brief Verify the original uint32-sector ceiling remains sparse and attachable.
 * @details Formats maximum advertised FAT32 geometry without materializing its ~2 TiB extent.
 * @return Whether attachment and exact 64-bit byte reporting succeeded.
 * @retval true Maximum sector count is attached with exact logical length.
 * @retval false Sparse creation, formatting, attachment, or size report failed.
 * @pre Host filesystem accepts a sparse length representable by `off_t`.
 * @pre Global SD state may replace the prior test card.
 * @post Success leaves the maximum-geometry anonymous image attached.
 * @post No named multi-terabyte file is created.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_maximum_geometry(void)
{
  if (!board_sd_attach_blank(UINT32_MAX, (uint8_t)k_fat32_bits, "MAXCARD")) {
    return false;
  }
  bool     attached = false;
  uint64_t bytes    = 0U;
  board_sd_info(&attached, &bytes, nullptr, nullptr);
  return attached && (bytes == ((uint64_t)UINT32_MAX * (uint64_t)k_fmt_sec_bytes));
}

int main(void)
{
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &s_k_real_ops);
  const bool passed = internal_test_attach_and_save() && internal_test_fault_preserves_target() &&
                      internal_test_formats_and_zero() && internal_test_maximum_geometry();
  if (!passed) {
    static const char k_failure[] = "test_board_sd_storage: failed\n";
    (void)write(STDERR_FILENO, k_failure, sizeof(k_failure) - 1U);
    return 1;
  }
  return 0;
}
