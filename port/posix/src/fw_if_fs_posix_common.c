/**
 * @file fw_if_fs_posix_common.c
 * @brief Shared descriptor, raw-directory, and path helpers for the POSIX port.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Centralizes errno mapping, descriptor invalidation, bounded raw directory
 * record decoding, civil timestamp conversion, path copying, and sibling
 * stage-name construction. Keeping these operations shared gives every
 * backend path the same failure vocabulary and close-once lifecycle behavior.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef _GNU_SOURCE
/** @brief Request GNU raw-directory syscall declarations on Linux. */
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ra8_attributes.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/syscall.h>
#endif

#include "fw_if_fs_posix_internal.h"
#include "ra8_err.h"

RA8_PRIV ra8_err_t priv_fs_posix_errno(int value)
{
  switch (value) {
    case 0:
      return k_ra8_ok;
    case ENOENT:
    case ENOTDIR:
      return k_ra8_err_not_found;
    case EEXIST:
      return k_ra8_err_exists;
    case ENOSPC:
    case EDQUOT:
    case EMFILE:
    case ENFILE:
      return k_ra8_err_no_mem;
    case ENOTEMPTY:
      return k_ra8_err_not_empty;
    case EACCES:
    case EPERM:
    case ELOOP:
      return k_ra8_err_access_denied;
    case EINVAL:
    case EXDEV:
    case ENAMETOOLONG:
    case EISDIR:
      return k_ra8_err_invalid_arg;
    case EFBIG:
    case EOVERFLOW:
      return k_ra8_err_invalid_size;
    case EBADF:
      return k_ra8_err_invalid_state;
    case EBUSY:
      return k_ra8_err_busy;
#if defined(ENOTSUP)
    case ENOTSUP:
      return k_ra8_err_not_supported;
#endif
    default:
      return k_ra8_fail;
  }
}

RA8_PRIV ra8_err_t priv_fs_posix_close_fd(int* fd)
{
  const int value = *fd;
  *fd             = -1;
  if (value < 0) {
    return k_ra8_err_invalid_state;
  }
  if (close(value) != 0) {
    return priv_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

#if defined(RA8_POSIX_TEST)
/** @brief Test-only injected raw directory reader. */
static fw_fs_posix_test_dir_read_fn_t s_test_directory_reader;

/** @brief Caller context paired with ::s_test_directory_reader. */
static void* s_test_directory_reader_ctx;

RA8_TEST_HELPER ra8_err_t
ra8_fs_posix_test_set_directory_reader(fw_fs_posix_test_dir_read_fn_t reader, void* ctx)
{
  if (reader == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  s_test_directory_reader     = reader;
  s_test_directory_reader_ctx = ctx;
  return k_ra8_ok;
}
#endif

/**
 * @brief Read one raw directory batch without C-runtime stream state.
 * @details Invokes the platform descriptor syscall directly and reports errno
 *          separately so the bounded retry layer can distinguish interrupts.
 * @param[in] ctx Unused production reader context.
 * @param[in] fd Open directory descriptor.
 * @param[out] buffer Destination for raw native records.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_errno Receives errno for a negative syscall result, else zero.
 * @return Raw syscall byte count, zero at EOF, or a negative failure result.
 * @retval 0 Native directory enumeration reached EOF.
 * @retval -1 The syscall failed or the host layout is unsupported.
 * @pre @p buffer addresses @p capacity writable bytes.
 * @pre @p out_errno addresses one writable integer and @p fd is an owned directory.
 * @post A non-negative result never claims more than the host syscall wrote.
 * @post Filesystem contents and descriptor ownership are unchanged.
 * @note Not thread-safe for concurrent reads using one directory descriptor offset.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static int64_t internal_directory_read_native(void*    ctx,
                                              int      fd,
                                              uint8_t* buffer,
                                              uint32_t capacity,
                                              int*     out_errno)
{
  (void)ctx;
#if defined(__linux__) && defined(SYS_getdents64)
  const long result = syscall(SYS_getdents64, fd, buffer, (size_t)capacity);
  *out_errno        = (result < 0L) ? errno : 0;
  return (int64_t)result;
#elif defined(__APPLE__) && defined(SYS_getdirentries64)
  off_t position = 0;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  const int result = syscall(SYS_getdirentries64, fd, (char*)buffer, (size_t)capacity, &position);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  *out_errno = (result < 0) ? errno : 0;
  return (int64_t)result;
#else
  (void)fd;
  (void)buffer;
  (void)capacity;
  *out_errno = ENOTSUP;
  return -1;
#endif
}

/**
 * @brief Dispatch one raw read through the production or test-only reader.
 * @details Selects the injected fault-vector reader in test builds, otherwise
 *          calls the exact production raw-syscall adapter.
 * @param[in] fd Open directory descriptor.
 * @param[out] buffer Destination for raw native records.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_errno Receives the raw failure errno or zero.
 * @return Selected reader's signed byte-count result.
 * @retval 0 The selected reader reported EOF.
 * @retval -1 The selected reader reported failure.
 * @pre Buffer and errno outputs satisfy ::internal_directory_read_native.
 * @pre Any installed test callback and context remain valid for the call.
 * @post Exactly one selected reader is invoked.
 * @post Production builds cannot dispatch through test-only global state.
 * @note Test injection is not thread-safe; production follows descriptor semantics.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static int64_t
internal_directory_read_once(int fd, uint8_t* buffer, uint32_t capacity, int* out_errno)
{
#if defined(RA8_POSIX_TEST)
  if (s_test_directory_reader != nullptr) {
    return s_test_directory_reader(s_test_directory_reader_ctx, fd, buffer, capacity, out_errno);
  }
#endif
  return internal_directory_read_native(nullptr, fd, buffer, capacity, out_errno);
}

/**
 * @brief Retry a bounded number of interrupted raw directory reads.
 * @details Accepts bounded non-negative byte counts, retries only `EINTR`, maps
 *          other errno values, and stops after a fixed interrupt budget.
 * @param[in] fd Open directory descriptor.
 * @param[out] buffer Destination for raw native records.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_bytes Receives the accepted byte count including zero EOF.
 * @return Bounded raw-read status.
 * @retval k_ra8_ok @p out_bytes contains a count no greater than @p capacity.
 * @retval k_ra8_err_invalid_state Reader count or errno contract was violated.
 * @retval k_ra8_err_busy Every bounded attempt was interrupted.
 * @retval k_ra8_err_* Mapped non-interrupt reader failure.
 * @pre @p buffer addresses @p capacity writable bytes.
 * @pre @p out_bytes addresses one writable uint32_t object.
 * @post Success initializes @p out_bytes.
 * @post The retry count never exceeds ::k_posix_directory_read_retries.
 * @note Not thread-safe for concurrent use of one directory descriptor offset.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_directory_fill(int fd, uint8_t* buffer, uint32_t capacity, uint32_t* out_bytes)
{
  for (uint16_t attempt = 0U; attempt < (uint16_t)k_posix_directory_read_retries; ++attempt) {
    int           read_errno = 0;
    const int64_t result     = internal_directory_read_once(fd, buffer, capacity, &read_errno);
    if (result >= 0) {
      if ((uint64_t)result > (uint64_t)capacity) {
        return k_ra8_err_invalid_state;
      }
      *out_bytes = (uint32_t)result;
      return k_ra8_ok;
    }
    if (read_errno != EINTR) {
      return (read_errno == 0) ? k_ra8_err_invalid_state : priv_fs_posix_errno(read_errno);
    }
  }
  return k_ra8_err_busy;
}

#if defined(__linux__)
/**
 * @brief Validate one Linux `getdents64` record without unaligned loads.
 * @details Copies the record length safely, proves size/alignment, and locates
 *          a non-empty bounded NUL-terminated name inside the record.
 * @param[in] bytes Candidate record beginning.
 * @param[in] available Bytes remaining in the raw batch.
 * @param[out] out Receives a borrowed name view and exact record extent.
 * @return Native-record validation status.
 * @retval k_ra8_ok @p out describes one complete safe record.
 * @retval k_ra8_err_invalid_state Record layout, alignment, or termination is invalid.
 * @retval k_ra8_err_invalid_size The name exceeds portable component capacity.
 * @pre @p bytes addresses @p available readable bytes.
 * @pre @p out addresses one writable record-view object.
 * @post Success publishes only pointers and extents within @p bytes.
 * @post Failure leaves raw source bytes unchanged.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_directory_decode(const uint8_t* bytes, uint32_t available, posix_directory_record_t* out)
{
  const uint32_t minimum = (uint32_t)k_posix_linux_name_offset + 1U;
  if (available < minimum) {
    return k_ra8_err_invalid_state;
  }
  uint16_t record_bytes = 0U;
  (void)memcpy(&record_bytes, &bytes[(uint8_t)k_posix_linux_reclen_offset], sizeof(record_bytes));
  if ((uint32_t)record_bytes < minimum) {
    return k_ra8_err_invalid_state;
  }
  if ((uint32_t)record_bytes > available) {
    return k_ra8_err_invalid_state;
  }
  if ((record_bytes & ((uint16_t)k_posix_linux_record_align - 1U)) != 0U) {
    return k_ra8_err_invalid_state;
  }
  const uint16_t name_capacity = (uint16_t)(record_bytes - (uint16_t)k_posix_linux_name_offset);
  const char*    name          = (const char*)&bytes[(uint8_t)k_posix_linux_name_offset];
  const char*    terminator    = (const char*)memchr(name, '\0', (size_t)name_capacity);
  if (terminator == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (terminator == name) {
    return k_ra8_err_invalid_state;
  }
  const size_t name_bytes = (size_t)(terminator - name);
  if (name_bytes >= (size_t)k_posix_component_cap) {
    return k_ra8_err_invalid_size;
  }
  out->name         = name;
  out->name_bytes   = (uint16_t)name_bytes;
  out->record_bytes = record_bytes;
  return k_ra8_ok;
}
#elif defined(__APPLE__)
/**
 * @brief Validate one Darwin `getdirentries64` record without unaligned loads.
 * @details Copies length fields safely, proves extent/alignment, and requires a
 *          non-empty name with exactly one terminator at its declared end.
 * @param[in] bytes Candidate record beginning.
 * @param[in] available Bytes remaining in the raw batch.
 * @param[out] out Receives a borrowed name view and exact record extent.
 * @return Native-record validation status.
 * @retval k_ra8_ok @p out describes one complete safe record.
 * @retval k_ra8_err_invalid_state Record layout, alignment, or termination is invalid.
 * @retval k_ra8_err_invalid_size The name exceeds portable component capacity.
 * @pre @p bytes addresses @p available readable bytes.
 * @pre @p out addresses one writable record-view object.
 * @post Success publishes only pointers and extents within @p bytes.
 * @post Failure leaves raw source bytes unchanged.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_directory_decode(const uint8_t* bytes, uint32_t available, posix_directory_record_t* out)
{
  const uint32_t minimum = (uint32_t)k_posix_darwin_name_offset + 1U;
  if (available < minimum) {
    return k_ra8_err_invalid_state;
  }
  uint16_t record_bytes = 0U;
  uint16_t name_bytes   = 0U;
  (void)memcpy(&record_bytes, &bytes[(uint8_t)k_posix_darwin_reclen_offset], sizeof(record_bytes));
  (void)memcpy(&name_bytes, &bytes[(uint8_t)k_posix_darwin_namlen_offset], sizeof(name_bytes));
  if ((uint32_t)record_bytes < minimum) {
    return k_ra8_err_invalid_state;
  }
  if ((uint32_t)record_bytes > available) {
    return k_ra8_err_invalid_state;
  }
  if ((record_bytes & ((uint16_t)k_posix_darwin_record_align - 1U)) != 0U) {
    return k_ra8_err_invalid_state;
  }
  const uint16_t name_capacity = (uint16_t)(record_bytes - (uint16_t)k_posix_darwin_name_offset);
  if (name_bytes == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (name_bytes >= (uint16_t)k_posix_component_cap) {
    return k_ra8_err_invalid_size;
  }
  if (name_bytes >= name_capacity) {
    return k_ra8_err_invalid_state;
  }
  const char* name = (const char*)&bytes[(uint8_t)k_posix_darwin_name_offset];
  if (name[name_bytes] != '\0') {
    return k_ra8_err_invalid_state;
  }
  if (memchr(name, '\0', (size_t)name_bytes) != nullptr) {
    return k_ra8_err_invalid_state;
  }
  out->name         = name;
  out->name_bytes   = name_bytes;
  out->record_bytes = record_bytes;
  return k_ra8_ok;
}
#else
#error "fw_if_fs_posix requires Linux getdents64 or Darwin getdirentries64"
#endif

RA8_PRIV ra8_err_t priv_fs_posix_directory_next(int                       fd,
                                                posix_directory_reader_t* reader,
                                                posix_directory_record_t* out,
                                                bool*                     out_end)
{
  if (fd < 0) {
    return k_ra8_err_invalid_state;
  }
  if (reader == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_end == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (reader->valid_bytes > (uint32_t)sizeof(reader->buffer)) {
    return k_ra8_err_invalid_state;
  }
  if (reader->cursor > reader->valid_bytes) {
    return k_ra8_err_invalid_state;
  }
  *out_end = false;
  if (reader->cursor == reader->valid_bytes) {
    const ra8_err_t filled = internal_directory_fill(fd,
                                                     reader->buffer,
                                                     (uint32_t)sizeof(reader->buffer),
                                                     &reader->valid_bytes);
    reader->cursor         = 0U;
    if (filled != k_ra8_ok) {
      return filled;
    }
    if (reader->valid_bytes == 0U) {
      *out_end = true;
      return k_ra8_ok;
    }
  }
  const ra8_err_t decoded = internal_directory_decode(&reader->buffer[reader->cursor],
                                                      reader->valid_bytes - reader->cursor,
                                                      out);
  if (decoded != k_ra8_ok) {
    return decoded;
  }
  reader->cursor += (uint32_t)out->record_bytes;
  return k_ra8_ok;
}

RA8_PRIV fw_fs_timestamp_t priv_fs_posix_timestamp(time_t seconds, long nanoseconds)
{
  fw_fs_timestamp_t portable = {};
  struct tm         utc      = {};
  if (nanoseconds < 0L) {
    return portable;
  }
  if (nanoseconds > (long)k_posix_nanosecond_max) {
    return portable;
  }
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return portable;
  }
  const int64_t year = (int64_t)utc.tm_year + (int64_t)k_posix_epoch_year_offset;
  if (year < 0) {
    return portable;
  }
  if (year > (int64_t)UINT16_MAX) {
    return portable;
  }
  portable.value.nanosecond     = (uint32_t)nanoseconds;
  portable.value.year           = (uint16_t)year;
  portable.value.utc_offset_min = 0;
  portable.value.month          = (uint8_t)(utc.tm_mon + 1);
  portable.value.day            = (uint8_t)utc.tm_mday;
  portable.value.hour           = (uint8_t)utc.tm_hour;
  portable.value.minute         = (uint8_t)utc.tm_min;
  portable.value.second         = (uint8_t)utc.tm_sec;
  portable.valid                = true;
  portable.utc_offset_valid     = true;
  return portable;
}

RA8_PRIV ra8_err_t priv_fs_posix_copy_path(char* out, const char* path)
{
  for (uint16_t i = 0U; i < (uint16_t)k_fw_fs_path_cap; ++i) {
    out[i] = path[i];
    if (path[i] == '\0') {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/**
 * @brief Render the bounded six-digit transaction suffix.
 * @details Emits lowercase nibbles from most to least significant without a NUL;
 *          the caller inserts the exact field into a bounded stage leaf.
 * @param[out] out Six-byte hexadecimal field.
 * @param[in] value Transaction identifier whose low 24 bits are rendered.
 * @pre @p out addresses ::k_posix_stage_hex_digits writable bytes.
 * @pre The destination does not overlap read-only digit storage.
 * @post Exactly six lowercase hexadecimal characters are written.
 * @post Bytes outside the six-byte field are unchanged.
 * @note Pure apart from caller output and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_hex6(char out[k_posix_stage_hex_digits], uint32_t value)
{
  static const char digits[] = "0123456789abcdef";
  for (uint8_t i = 0U; i < (uint8_t)k_posix_stage_hex_digits; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_posix_hex_last_digit - i) * (uint8_t)k_posix_hex_nibble_bits);
    out[i] = digits[(value >> shift) & (uint32_t)k_posix_hex_nibble_mask];
  }
}

RA8_PRIV ra8_err_t priv_fs_posix_stage_path(const char* destination, uint32_t id, char* out)
{
  uint16_t last_slash = 0U;
  uint16_t length     = 0U;
  while (length < (uint16_t)k_fw_fs_path_cap) {
    if (destination[length] == '\0') {
      break;
    }
    if (destination[length] == '/') {
      last_slash = length;
    }
    ++length;
  }
  if (length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  if ((uint16_t)(last_slash + k_posix_stage_leaf_span) >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i <= last_slash; ++i) {
    out[i] = destination[i];
  }
  uint16_t cursor = (uint16_t)(last_slash + 1U);
  out[cursor++]   = 'T';
  out[cursor++]   = 'X';
  internal_hex6(&out[cursor], id & k_posix_transaction_id_mask);
  cursor        = (uint16_t)(cursor + k_posix_stage_hex_digits);
  out[cursor++] = '.';
  out[cursor++] = 'T';
  out[cursor++] = 'M';
  out[cursor++] = 'P';
  out[cursor]   = '\0';
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_fs_posix_listdir(void*           ctx,
                                         const char*     path,
                                         uint32_t        max_entries,
                                         fw_fs_list_fn_t callback,
                                         void*           callback_ctx,
                                         uint32_t*       out_count,
                                         bool*           out_complete)
{
  posix_directory_state_t directory = {.fd = -1};
  const ra8_err_t         opened =
    priv_fs_posix_dir_open(ctx, path, &directory, (uint32_t)sizeof(directory));
  if (opened != k_ra8_ok) {
    return opened;
  }
  ra8_err_t result = k_ra8_ok;
  *out_complete    = false;
  for (;;) {
    fw_fs_dirent_value_t value   = {};
    bool                 present = false;
    result                       = priv_fs_posix_dir_next(ctx, &directory, &value, &present);
    if (result != k_ra8_ok) {
      break;
    }
    if (!present) {
      *out_complete = true;
      break;
    }
    if (*out_count >= max_entries) {
      break;
    }
    const fw_fs_dirent_t entry      = {.name       = value.name,
                                       .size_bytes = value.size_bytes,
                                       .name_bytes = value.name_bytes,
                                       .type       = value.type};
    bool                 keep_going = true;
    result                          = callback(callback_ctx, &entry, &keep_going);
    ++(*out_count);
    if (result != k_ra8_ok) {
      break;
    }
    if (!keep_going) {
      break;
    }
  }
  const ra8_err_t closed = priv_fs_posix_dir_close(ctx, &directory);
  return (result == k_ra8_ok) ? closed : result;
}
