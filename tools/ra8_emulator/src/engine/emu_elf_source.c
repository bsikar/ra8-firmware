/**
 * @file emu_elf_source.c
 * @brief Allocation-free raw-descriptor ELF source ownership and bounded views
 * @details Each caller owns an independent descriptor context. Parsing code
 * acquires only exact transient views in caller-provided scratch memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "emu_elf.h"
#include "emu_elf_source_internal.h"
#include "emu_host_io_internal.h"

/**
 * @brief Construct one fully initialized ELF I/O result.
 * @details Copies semantic status, exact byte counts, and captured errno by value.
 * @param[in] status Semantic operation status.
 * @param[in] required Exact source or range byte requirement.
 * @param[in] supplied Caller-provided scratch bytes.
 * @param[in] os_error Captured host error, or zero.
 * @return Complete result value.
 * @retval emu_elf_io_result_t A fully initialized result.
 * @pre @p status is a valid ::emu_elf_io_status_t.
 * @pre Byte counts describe the current operation.
 * @post No caller or host state changes.
 * @post Every result field is initialized.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_elf_io_result_t
internal_elf_result(emu_elf_io_status_t status, uint64_t required, uint64_t supplied, int os_error)
{
  return (emu_elf_io_result_t){.status         = status,
                               .required_bytes = required,
                               .supplied_bytes = supplied,
                               .os_error       = os_error};
}

/**
 * @brief Translate a raw host-I/O result without losing progress metadata.
 * @details Maps raw invalid and EOF states while preserving host faults.
 * @param[in] io Raw host operation result.
 * @param[in] required Exact ELF operation requirement.
 * @param[in] supplied Caller-provided scratch bytes.
 * @return Corresponding ELF invalid, EOF, or host-error result.
 * @retval emu_elf_io_result_t The mapped result with exact byte metadata.
 * @pre @p io does not report success.
 * @pre Byte counts describe the current operation.
 * @post No state changes.
 * @post Captured errno is retained for host errors.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_elf_io_result_t
internal_from_host(emu_io_result_t io, uint64_t required, uint64_t supplied)
{
  emu_elf_io_status_t status = k_emu_elf_io_error;
  if (io.status == k_emu_io_invalid) {
    status = k_emu_elf_io_invalid;
  } else if (io.status == k_emu_io_eof) {
    status = k_emu_elf_io_eof;
  }
  return internal_elf_result(status, required, supplied, io.os_error);
}

emu_elf_io_result_t priv_emu_elf_source_open(const char* path, emu_elf_source_t* source)
{
  if (source == nullptr) {
    return internal_elf_result(k_emu_elf_io_invalid, 0U, 0U, 0);
  }
  emu_io_file_t         file   = {.fd = -1, .size = 0};
  const emu_io_result_t opened = priv_emu_io_open_read(path, &file);
  if (opened.status != k_emu_io_ok) {
    return internal_from_host(opened, 0U, 0U);
  }
  const emu_elf_source_t ready = {.fd = file.fd, .length = (uint64_t)file.size};
  *source                      = ready;
  return internal_elf_result(k_emu_elf_io_ok, ready.length, 0U, 0);
}

emu_elf_io_result_t priv_emu_elf_source_close(emu_elf_source_t* source)
{
  if ((source == nullptr) || (source->fd < 0)) {
    return internal_elf_result(k_emu_elf_io_invalid, 0U, 0U, 0);
  }
  const uint64_t length        = source->length;
  emu_io_file_t  file          = {.fd = source->fd, .size = (int64_t)source->length};
  source->fd                   = -1;
  source->length               = 0U;
  const emu_io_result_t closed = priv_emu_io_close(&file);
  return (closed.status == k_emu_io_ok) ? internal_elf_result(k_emu_elf_io_ok, length, 0U, 0)
                                        : internal_from_host(closed, length, 0U);
}

emu_elf_io_result_t priv_emu_elf_read(const emu_elf_source_t* source,
                                      uint64_t                offset,
                                      size_t                  required_bytes,
                                      void*                   scratch,
                                      size_t                  supplied_bytes,
                                      emu_elf_view_t*         view)
{
  if ((source == nullptr) || (source->fd < 0) || (scratch == nullptr) || (view == nullptr) ||
      (required_bytes == 0U)) {
    return internal_elf_result(k_emu_elf_io_invalid, required_bytes, supplied_bytes, 0);
  }
  if ((offset > source->length) || ((uint64_t)required_bytes > (source->length - offset))) {
    return internal_elf_result(k_emu_elf_io_eof, required_bytes, supplied_bytes, 0);
  }
  if (required_bytes > supplied_bytes) {
    return internal_elf_result(k_emu_elf_io_capacity, required_bytes, supplied_bytes, 0);
  }
  const emu_io_result_t read =
    priv_emu_io_pread_exact(source->fd, scratch, required_bytes, (off_t)offset);
  if (read.status != k_emu_io_ok) {
    return internal_from_host(read, required_bytes, supplied_bytes);
  }
  *view =
    (emu_elf_view_t){.bytes = (const uint8_t*)scratch, .length = required_bytes, .offset = offset};
  return internal_elf_result(k_emu_elf_io_ok, required_bytes, supplied_bytes, 0);
}
