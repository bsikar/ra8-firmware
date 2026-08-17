/**
 * @file test_emu_elf_source.c
 * @brief Raw-fd ELF ownership, bounds, parser, and fault-injection tests
 * @details Proves independent source lifetimes, output commit semantics,
 * bounded capacity reporting, symbol streaming, and PT_LOAD behavior.
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

#include "emu_elf.h"
#include "emu_elf_source_internal.h"
#include "emu_host_io_internal.h"

/** @brief Minimal ELF32 ARM fixture dimensions and offsets. */
typedef enum : uint32_t {
  k_test_elf_bytes      = 264U,    /**< Complete fixture extent.            */
  k_test_ph_offset      = 52U,     /**< Program-header file offset.         */
  k_test_sh_offset      = 84U,     /**< Section-table file offset.          */
  k_test_sym_offset     = 204U,    /**< Symbol-table file offset.           */
  k_test_str_offset     = 236U,    /**< String-table file offset.           */
  k_test_segment_offset = 256U,    /**< PT_LOAD payload file offset.        */
  k_test_segment_paddr  = 0x2000U, /**< PT_LOAD physical destination.       */
  k_test_segment_vaddr  = 0x1000U, /**< PT_LOAD executable virtual address. */
} test_elf_layout_t;

/**
 * @brief Write one little-endian 16-bit fixture field.
 * @details Encodes each byte explicitly for host-endian independence.
 * @param[out] bytes Writable fixture bytes.
 * @param[in] offset First destination byte.
 * @param[in] value Field value to encode.
 * @pre @p bytes is non-null.
 * @pre Two bytes remain writable from @p offset.
 * @post Exactly two bytes encode @p value in little-endian order.
 * @post Bytes outside the field remain unchanged.
 * @note Pure apart from caller-owned fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put16(uint8_t* bytes, uint32_t offset, uint16_t value)
{
  bytes[offset]      = (uint8_t)value;
  bytes[offset + 1U] = (uint8_t)(value >> 8U);
}

/**
 * @brief Write one little-endian 32-bit fixture field.
 * @details Encodes each byte explicitly for host-endian independence.
 * @param[out] bytes Writable fixture bytes.
 * @param[in] offset First destination byte.
 * @param[in] value Field value to encode.
 * @pre @p bytes is non-null.
 * @pre Four bytes remain writable from @p offset.
 * @post Exactly four bytes encode @p value in little-endian order.
 * @post Bytes outside the field remain unchanged.
 * @note Pure apart from caller-owned fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put32(uint8_t* bytes, uint32_t offset, uint32_t value)
{
  bytes[offset]      = (uint8_t)value;
  bytes[offset + 1U] = (uint8_t)(value >> 8U);
  bytes[offset + 2U] = (uint8_t)(value >> 16U);
  bytes[offset + 3U] = (uint8_t)(value >> 24U);
}

/**
 * @brief Build one bounded ELF32 ARM fixture with load/symbol/string tables.
 * @details Encodes fixed little-endian headers, one load payload, and one symbol.
 * @param[out] bytes Exact fixture destination.
 * @pre @p bytes spans ::k_test_elf_bytes writable bytes.
 * @pre The fixture constants describe non-overlapping bounded ranges.
 * @post One executable PT_LOAD and `test_symbol` are fully encoded.
 * @post All unspecified fixture bytes are zero.
 * @note Pure fixture construction with no host resources.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_elf(uint8_t bytes[k_test_elf_bytes])
{
  /* Binary ELF content, not C strings: EI_MAG is immediately followed by
   * EI_CLASS, the string table carries its own two terminators inside the
   * bytes below, and the PT_LOAD payload is exactly p_filesz long. */
  static const uint8_t elf_magic[] = {0x7FU, 'E', 'L', 'F'};
  static const uint8_t str_table[] =
    {0U, 't', 'e', 's', 't', '_', 's', 'y', 'm', 'b', 'o', 'l', 0U};
  static const uint8_t load_payload[] = {'L', 'O', 'A', 'D', 'D', 'A', 'T', 'A'};
  (void)memset(bytes, 0, k_test_elf_bytes);
  (void)memcpy(bytes, elf_magic, sizeof(elf_magic));
  bytes[4] = 1U;
  bytes[5] = 1U;
  internal_put16(bytes, k_elf_e_machine_off, k_elf_em_arm);
  internal_put32(bytes, k_elf_e_phoff_off, k_test_ph_offset);
  internal_put16(bytes, k_elf_e_phentsize_off, 32U);
  internal_put16(bytes, k_elf_e_phnum_off, 1U);
  internal_put32(bytes, 32U, k_test_sh_offset);
  internal_put16(bytes, 46U, 40U);
  internal_put16(bytes, 48U, 3U);
  internal_put32(bytes, k_test_ph_offset, k_elf_pt_load);
  internal_put32(bytes, k_test_ph_offset + 4U, k_test_segment_offset);
  internal_put32(bytes, k_test_ph_offset + 8U, k_test_segment_vaddr);
  internal_put32(bytes, k_test_ph_offset + 12U, k_test_segment_paddr);
  internal_put32(bytes, k_test_ph_offset + 16U, 8U);
  internal_put32(bytes, k_test_ph_offset + 24U, k_elf_pf_x);
  const uint32_t sym_section = k_test_sh_offset + 40U;
  internal_put32(bytes, sym_section + 4U, 2U);
  internal_put32(bytes, sym_section + 16U, k_test_sym_offset);
  internal_put32(bytes, sym_section + 20U, 32U);
  internal_put32(bytes, sym_section + 24U, 2U);
  internal_put32(bytes, sym_section + 36U, 16U);
  const uint32_t str_section = k_test_sh_offset + 80U;
  internal_put32(bytes, str_section + 4U, 3U);
  internal_put32(bytes, str_section + 16U, k_test_str_offset);
  internal_put32(bytes, str_section + 20U, 20U);
  internal_put32(bytes, k_test_sym_offset + 16U, 1U);
  internal_put32(bytes, k_test_sym_offset + 20U, 0x1235U);
  internal_put32(bytes, k_test_sym_offset + 24U, 4U);
  bytes[k_test_sym_offset + 28U] = 2U;
  (void)memcpy(&bytes[k_test_str_offset], str_table, sizeof(str_table));
  (void)memcpy(&bytes[k_test_segment_offset], load_payload, sizeof(load_payload));
}

/**
 * @brief Write all fixture bytes through bounded raw syscalls.
 * @details Repeats short writes until the exact request completes or fails.
 * @param[in] fd Open writable descriptor.
 * @param[in] bytes Fixture bytes.
 * @param[in] length Exact byte count.
 * @return Whether every byte was written.
 * @retval true The complete request was transferred.
 * @retval false A syscall failed or made no progress.
 * @pre @p fd is writable.
 * @pre @p bytes spans @p length readable bytes.
 * @post Success advances the cursor by exactly @p length.
 * @post Failure reports no uncompleted bytes as written.
 * @note Test helper; errno is not translated.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_all(int fd, const uint8_t* bytes, size_t length)
{
  size_t done = 0U;
  while (done < length) {
    const ssize_t amount = write(fd, &bytes[done], length - done);
    if (amount <= 0) {
      return false;
    }
    done += (size_t)amount;
  }
  return true;
}

/**
 * @brief Create one private fixture path and close its initial descriptor.
 * @details Uses mkstemp, exact raw writes, and removes a partial fixture on failure.
 * @param[in] bytes Fixture contents.
 * @param[in] length Fixture byte count.
 * @param[out] path Writable 64-byte path buffer.
 * @return Whether a closed complete fixture was created.
 * @retval true @p path names the complete fixture.
 * @retval false Creation, transfer, or close failed.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p path spans 64 writable bytes.
 * @post Success leaves no owned descriptor.
 * @post Failure removes any named partial fixture.
 * @note The caller unlinks a successful fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_fixture(const uint8_t* bytes, size_t length, char path[64])
{
  static const char k_template[] = "/tmp/emu_elf_source.XXXXXX";
  (void)memcpy(path, k_template, sizeof(k_template));
  const int fd = mkstemp(path);
  if (fd < 0) {
    return false;
  }
  const bool ok = internal_write_all(fd, bytes, length) && (close(fd) == 0);
  if (!ok) {
    (void)unlink(path);
  }
  return ok;
}

/**
 * @brief Pass production sequential read through the test operation table.
 * @details Adapts the raw syscall to the injectable seam signature.
 * @param[in] fd Open readable descriptor.
 * @param[out] bytes Destination bytes.
 * @param[in] length Requested byte count.
 * @return Raw read result.
 * @retval -1 The syscall failed and set errno.
 * @pre @p bytes spans @p length writable bytes when nonzero.
 * @pre @p fd is valid for reading.
 * @post Positive success advances the descriptor cursor.
 * @post Failure preserves syscall errno.
 * @note Thread safety follows the descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_read(int fd, void* bytes, size_t length)
{
  return read(fd, bytes, length);
}

/**
 * @brief Pass production sequential write through the test operation table.
 * @details Adapts the raw syscall to the injectable seam signature.
 * @param[in] fd Open writable descriptor.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @return Raw write result.
 * @retval -1 The syscall failed and set errno.
 * @pre @p bytes spans @p length readable bytes when nonzero.
 * @pre @p fd is valid for writing.
 * @post Positive success advances the descriptor cursor.
 * @post Failure preserves syscall errno.
 * @note Thread safety follows the descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_write(int fd, const void* bytes, size_t length)
{
  return write(fd, bytes, length);
}

/**
 * @brief Inject a deterministic positioned-read EIO fault.
 * @details Ignores every transfer argument and fails before modifying output.
 * @param[in] fd Ignored descriptor.
 * @param[out] bytes Ignored destination.
 * @param[in] length Ignored byte count.
 * @param[in] offset Ignored source offset.
 * @return Always -1.
 * @retval -1 Deterministic injected EIO.
 * @pre Arguments may hold any values accepted by the operation seam.
 * @pre The caller expects a host I/O fault.
 * @post errno equals EIO.
 * @post Destination bytes remain unchanged.
 * @note Deterministic and thread-local only through errno.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_fault_pread(int fd, void* bytes, size_t length, off_t offset)
{
  (void)fd;
  (void)bytes;
  (void)length;
  (void)offset;
  errno = EIO;
  return -1;
}

/**
 * @brief Pass production positioned write through the test operation table.
 * @details Adapts pwrite to the injectable seam signature.
 * @param[in] fd Open writable descriptor.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @param[in] offset Destination offset.
 * @return Raw pwrite result.
 * @retval -1 The syscall failed and set errno.
 * @pre @p bytes spans @p length readable bytes when nonzero.
 * @pre @p fd is valid for positioned writes.
 * @post The descriptor cursor remains unchanged.
 * @post Failure preserves syscall errno.
 * @note Thread safety follows the descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_pwrite(int fd, const void* bytes, size_t length, off_t offset)
{
  return pwrite(fd, bytes, length, offset);
}

static const emu_io_ops_t s_k_fault_ops = {.read_fn   = internal_read,
                                           .write_fn  = internal_write,
                                           .pread_fn  = internal_fault_pread,
                                           .pwrite_fn = internal_pwrite};

/**
 * @brief Verify one streamed executable segment and its source bytes.
 * @details Checks metadata and acquires the exact payload through bounded scratch.
 * @param[in] segment Bounds-checked executable segment.
 * @param[out] opaque Writable boolean result.
 * @return Whether segment iteration should continue.
 * @retval false The single-segment fixture walk always stops here.
 * @pre @p segment is non-null and its source remains open.
 * @pre @p opaque points to a writable bool.
 * @post The result is true exactly when metadata and payload match.
 * @post No source bytes are retained.
 * @note Uses eight bytes of caller-local scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_segment(const elf_exec_segment_t* segment, void* opaque)
{
  bool* const    ok       = (bool*)opaque;
  uint8_t        bytes[8] = {};
  emu_elf_view_t view     = {};
  *ok =
    (segment->offset == k_test_segment_offset) && (segment->vaddr == k_test_segment_vaddr) &&
    (segment->paddr == k_test_segment_paddr) && (segment->filesz == sizeof(bytes)) &&
    (priv_emu_elf_read(segment->source, segment->offset, sizeof(bytes), bytes, sizeof(bytes), &view)
       .status == k_emu_elf_io_ok) &&
    (memcmp(bytes, "LOADDATA", sizeof(bytes)) == 0);
  return false;
}

/**
 * @brief Prove parser/vector/symbol behavior over one raw descriptor fixture.
 * @details Exercises vector, symbol, segment, and Unicorn PT_LOAD paths end to end.
 * @return Whether the complete parser matrix passed.
 * @retval true Every parser and load assertion passed.
 * @retval false Fixture setup or any assertion failed.
 * @pre The host permits private temporary files.
 * @pre Unicorn supports an ARM M-profile engine.
 * @post Any opened engine and source descriptor are closed.
 * @post Any named fixture is unlinked.
 * @note Self-contained focused unit test.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_parser(void)
{
  uint8_t fixture[k_test_elf_bytes];
  internal_build_elf(fixture);
  char path[64] = {};
  if (!internal_fixture(fixture, sizeof(fixture), path)) {
    return false;
  }
  emu_elf_source_t source = {.fd = -1};
  bool             ok     = priv_emu_elf_source_open(path, &source).status == k_emu_elf_io_ok;
  uint32_t         size   = 0U;
  ok                      = (elf_vector_base(&source) == k_test_segment_vaddr) && ok;
  ok              = (elf_sym_addr(&source, "test_symbol", &size) == 0x1234U) && (size == 4U) && ok;
  bool segment_ok = false;
  ok = (elf_foreach_exec_segment(&source, internal_segment, &segment_ok) == 1U) && segment_ok && ok;
  uc_engine* uc        = nullptr;
  uint8_t    loaded[8] = {};
  ok = (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &uc) == UC_ERR_OK) && ok;
  ok = (uc != nullptr) && (uc_mem_map(uc, k_test_segment_paddr, 4096U, UC_PROT_ALL) == UC_ERR_OK) &&
       ok;
  ok = (uc != nullptr) && (load_elf(uc, &source) == 0) && ok;
  ok = (uc != nullptr) &&
       (uc_mem_read(uc, k_test_segment_paddr, loaded, sizeof(loaded)) == UC_ERR_OK) &&
       (memcmp(loaded, "LOADDATA", sizeof(loaded)) == 0) && ok;
  if (uc != nullptr) {
    (void)uc_close(uc);
  }
  ok = (priv_emu_elf_source_close(&source).status == k_emu_elf_io_ok) && ok;
  (void)unlink(path);
  return ok;
}

/**
 * @brief Prove two descriptors and their caller scratch remain independent.
 * @details Unlinks both paths, closes one source, then reads the other successfully.
 * @return Whether independent lifetime and payload assertions passed.
 * @retval true Both source contexts remained independent.
 * @retval false Fixture setup, read, close, or comparison failed.
 * @pre The host permits two private temporary files.
 * @pre Raw positioned reads are available.
 * @post Both source descriptors are closed.
 * @post Both fixture paths are unlinked.
 * @note Models simultaneous Secure and Non-Secure ELF sources.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_two_instances(void)
{
  const uint8_t first_data[6]   = {'a', 'b', 'c', 'd', 'e', 'f'};
  const uint8_t second_data[6]  = {'u', 'v', 'w', 'x', 'y', 'z'};
  char          first_path[64]  = {};
  char          second_path[64] = {};
  if (!internal_fixture(first_data, sizeof(first_data), first_path) ||
      !internal_fixture(second_data, sizeof(second_data), second_path)) {
    return false;
  }
  emu_elf_source_t first  = {.fd = -1};
  emu_elf_source_t second = {.fd = -1};
  bool             ok = priv_emu_elf_source_open(first_path, &first).status == k_emu_elf_io_ok &&
                        priv_emu_elf_source_open(second_path, &second).status == k_emu_elf_io_ok;
  (void)unlink(first_path);
  (void)unlink(second_path);
  uint8_t        one[6]   = {};
  uint8_t        two[6]   = {};
  emu_elf_view_t one_view = {};
  emu_elf_view_t two_view = {};
  ok = (priv_emu_elf_read(&first, 0U, sizeof(one), one, sizeof(one), &one_view).status ==
        k_emu_elf_io_ok) &&
       ok;
  ok = (priv_emu_elf_source_close(&first).status == k_emu_elf_io_ok) && ok;
  ok = (priv_emu_elf_read(&second, 0U, sizeof(two), two, sizeof(two), &two_view).status ==
        k_emu_elf_io_ok) &&
       ok;
  ok = (memcmp(one, first_data, sizeof(one)) == 0) &&
       (memcmp(two, second_data, sizeof(two)) == 0) && ok;
  ok = (priv_emu_elf_source_close(&second).status == k_emu_elf_io_ok) && ok;
  return ok;
}

/**
 * @brief Prove capacity/fault failures preserve the published output view.
 * @details Checks exact required/supplied metadata, scratch bounds, and injected EIO.
 * @return Whether every failure-contract assertion passed.
 * @retval true Capacity and fault results preserved the prior published view.
 * @retval false Fixture setup or any contract assertion failed.
 * @pre The host permits a private temporary file.
 * @pre The raw-I/O operation seam can be reconfigured before active reads.
 * @post Production I/O operations are restored.
 * @post The source is closed and its fixture path unlinked.
 * @note Covers oversize workspace requests and deterministic host faults.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_fault_and_capacity(void)
{
  const uint8_t data[6]  = {'s', 'o', 'u', 'r', 'c', 'e'};
  char          path[64] = {};
  if (!internal_fixture(data, sizeof(data), path)) {
    return false;
  }
  emu_elf_source_t     source   = {.fd = -1};
  bool                 ok       = priv_emu_elf_source_open(path, &source).status == k_emu_elf_io_ok;
  uint8_t              sentinel = 0xA5U;
  uint8_t              scratch[5] = {0xA5U, 0xA5U, 0xA5U, 0xA5U, 0xA5U};
  const emu_elf_view_t original   = {.bytes = &sentinel, .length = 77U, .offset = 88U};
  emu_elf_view_t       view       = original;
  emu_elf_io_result_t  result = priv_emu_elf_read(&source, 0U, 6U, scratch, sizeof(scratch), &view);
  ok              = (result.status == k_emu_elf_io_capacity) && (result.required_bytes == 6U) &&
                    (result.supplied_bytes == sizeof(scratch)) &&
                    (memcmp(&view, &original, sizeof(view)) == 0) && (scratch[0] == 0xA5U) && ok;
  uint8_t full[6] = {};
  view            = original;
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &s_k_fault_ops);
  result = priv_emu_elf_read(&source, 0U, sizeof(full), full, sizeof(full), &view);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  ok = (result.status == k_emu_elf_io_error) && (result.os_error == EIO) &&
       (memcmp(&view, &original, sizeof(view)) == 0) && ok;
  ok = (priv_emu_elf_source_close(&source).status == k_emu_elf_io_ok) && ok;
  (void)unlink(path);
  return ok;
}

/** @brief Run the complete allocation-free ELF source contract matrix. */
int main(void)
{
  const bool ok =
    internal_test_parser() && internal_test_two_instances() && internal_test_fault_and_capacity();
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  return ok ? 0 : 1;
}
