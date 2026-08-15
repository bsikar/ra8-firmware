/**
 * @file fw_if_fs_posix_internal.h
 * @brief Shared errno/descriptor helpers for the POSIX filesystem port.
 * @ingroup grp_io
 *
 * @details
 * Defines the fixed caller-workspace layouts and bounded helper contracts used
 * across the POSIX adapter translation units. These declarations keep hosted
 * descriptor, timestamp, path, and transaction-name policy private to the
 * port while avoiding duplicated platform logic.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <time.h>

#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local component and stage-search bounds. */
typedef enum : uint16_t {
  k_posix_stage_leaf_span = 13U,  /**< Stage leaf plus terminating NUL.     */
  k_posix_max_open_files  = 64U,  /**< Truthful hosted descriptor capacity. */
  k_posix_component_cap   = 256U, /**< Component buffer including NUL.      */
  k_posix_file_mode       = 0600, /**< Owner-only created-file mode.        */
  k_posix_directory_mode  = 0700, /**< Owner-only created-directory mode.   */
  k_posix_stage_attempts  = 64U,  /**< Collision-search attempt cap.        */
} posix_limits_t;

/** @brief Hexadecimal stage-name encoding constants. */
typedef enum : uint8_t {
  k_posix_hex_nibble_bits  = 4U, /**< Bits represented by one hex digit. */
  k_posix_stage_hex_digits = 6U, /**< Hex digits in a stage identifier.  */
  /** @brief Highest valid index in a stage identifier. */
  k_posix_hex_last_digit  = k_posix_stage_hex_digits - 1U,
  k_posix_hex_nibble_mask = 0x0FU, /**< Low-nibble mask. */
} posix_hex_limits_t;

/** @brief POSIX timestamp and transaction arithmetic constants. */
typedef enum : uint32_t {
  k_posix_epoch_year_offset   = 1900U,       /**< `struct tm` year origin. */
  k_posix_nanosecond_max      = 999999999U,  /**< Largest valid subsecond. */
  k_posix_transaction_id_mask = 0x00FFFFFFU, /**< Six hexadecimal digits.  */
} posix_numeric_limits_t;

/** @brief Raw directory buffer and retry bounds. */
typedef enum : uint16_t {
  k_posix_directory_buffer_bytes = 4096U, /**< Fixed local kernel buffer. */
  k_posix_directory_read_retries = 16U,   /**< Maximum interrupted reads. */
} posix_directory_limits_t;

/** @brief Linux `getdents64` wire-record offsets and alignment. */
typedef enum : uint8_t {
  k_posix_linux_reclen_offset = 16U, /**< Offset of `d_reclen`.    */
  k_posix_linux_name_offset   = 19U, /**< Offset of `d_name`.      */
  k_posix_linux_record_align  = 8U,  /**< Kernel record alignment. */
} posix_linux_dirent_layout_t;

/** @brief Darwin `getdirentries64` wire-record offsets and alignment. */
typedef enum : uint8_t {
  k_posix_darwin_reclen_offset = 16U, /**< Offset of `d_reclen`.    */
  k_posix_darwin_namlen_offset = 18U, /**< Offset of `d_namlen`.    */
  k_posix_darwin_name_offset   = 21U, /**< Offset of `d_name`.      */
  k_posix_darwin_record_align  = 4U,  /**< Kernel record alignment. */
} posix_darwin_dirent_layout_t;

/** @brief Validated view over one raw host directory record. */
typedef struct {
  const char* name;         /**< NUL-terminated name inside the read buffer. */
  uint16_t    name_bytes;   /**< Name length excluding the terminator.       */
  uint16_t    record_bytes; /**< Complete aligned record length.             */
} posix_directory_record_t;

/** @brief Caller-owned cursor and fixed storage for raw directory batches. */
typedef struct {
  uint8_t  buffer[k_posix_directory_buffer_bytes]; /**< Kernel record bytes.  */
  uint32_t valid_bytes;                            /**< Bytes in @ref buffer. */
  uint32_t cursor;                                 /**< Next record offset.   */
} posix_directory_reader_t;

/** @brief POSIX state placed in caller directory workspace. */
typedef struct {
  posix_directory_reader_t reader; /**< Bounded raw-record buffer and cursor. */
  int                      fd;     /**< Owned directory descriptor.          */
} posix_directory_state_t;

/** @brief POSIX state placed in caller file workspace. */
typedef struct {
  /** @brief Owned descriptor, or -1 while closed. */
  int fd;
} posix_file_state_t;

/** @brief POSIX state placed in caller transaction workspace. */
typedef struct {
  /** @brief Final publication path. */
  char destination[k_fw_fs_path_cap];
  /** @brief Private sibling stage path. */
  char stage[k_fw_fs_path_cap];
  /** @brief Open descriptor state for the stage. */
  posix_file_state_t file_state;
  /** @brief Requested destination collision policy. */
  fw_fs_transaction_policy_t policy;
  /** @brief True while the stage descriptor is owned. */
  bool writer_open;
  /** @brief True while the stage leaf needs cleanup. */
  bool stage_exists;
} posix_transaction_state_t;

#if defined(RA8_POSIX_TEST)
/** @brief Injected raw-directory read used only by hosted fault tests. */
typedef int64_t (*fw_fs_posix_test_dir_read_fn_t)(void*    ctx,
                                                  int      fd,
                                                  uint8_t* buffer,
                                                  uint32_t capacity,
                                                  int*     out_errno);

/** @brief Select a caller-owned raw-directory reader for the test build. */
RA8_TEST_HELPER [[nodiscard]] ra8_err_t
ra8_fs_posix_test_set_directory_reader(fw_fs_posix_test_dir_read_fn_t reader, void* ctx);
#endif

/** @brief Map one captured errno value into ::ra8_err_t. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_errno(int value);

/** @brief Close exactly once and invalidate the caller's descriptor. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_close_fd(int* fd);

/** @brief Read and validate the next raw hosted directory record. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_directory_next(int                       fd,
                                                              posix_directory_reader_t* reader,
                                                              posix_directory_record_t* out,
                                                              bool*                     out_end);

/** @brief Convert a POSIX UTC instant into the portable civil representation. */
RA8_PRIV [[nodiscard]] fw_fs_timestamp_t priv_fs_posix_timestamp(time_t seconds, long nanoseconds);

/** @brief Copy one bounded portable path. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_copy_path(char* out, const char* path);

/** @brief Build an 8.3-compatible sibling transaction path. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fs_posix_stage_path(const char* destination, uint32_t id, char* out);

/** @brief Open a raw hosted directory cursor in caller workspace. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fs_posix_dir_open(void* ctx, const char* path, void* directory_state, uint32_t state_bytes);

/** @brief Copy the next visible hosted directory entry. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_dir_next(void*                 ctx,
                                                        void*                 directory_state,
                                                        fw_fs_dirent_value_t* out,
                                                        bool*                 out_entry);

/** @brief Close and consume one hosted directory cursor. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_dir_close(void* ctx, void* directory_state);

/** @brief Adapt the incremental cursor to the legacy bounded callback contract. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_listdir(void*           ctx,
                                                       const char*     path,
                                                       uint32_t        max_entries,
                                                       fw_fs_list_fn_t callback,
                                                       void*           callback_ctx,
                                                       uint32_t*       out_count,
                                                       bool*           out_complete);

/** @brief Bind the immutable POSIX operation tables to initialized state. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fs_posix_bind_interfaces(fw_fs_t* out, fw_fs_posix_state_t* state, const fw_fs_caps_t* caps);
