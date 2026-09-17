/**
 * @file fw_if_fs_types.h
 * @brief Portable filesystem value types and caller-owned opaque handles.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * These types describe filesystem intent without naming POSIX, FAT, a block
 * device, an MCU, or an RTOS. Concrete ports bind private vtables and opaque
 * contexts into the small handles below. Backend-specific file and
 * transaction state is supplied by the caller at open/begin time; no handle
 * owns dynamically allocated memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Interface-wide hard bounds used before a backend is called. */
typedef enum : uint16_t {
  k_fw_fs_path_cap = 512U, /**< Largest portable path including its NUL. */
} fw_fs_limits_t;

/** @brief Filesystem node kind reported by ::fw_fs_stat. */
typedef enum : uint8_t {
  k_fw_fs_node_none      = 0U, /**< No node exists at the path.     */
  k_fw_fs_node_file      = 1U, /**< Regular byte stream.            */
  k_fw_fs_node_directory = 2U, /**< Directory.                      */
  k_fw_fs_node_symlink   = 3U, /**< Symbolic link, if observable.   */
  k_fw_fs_node_other     = 4U, /**< Backend-specific non-file node. */
} fw_fs_node_type_t;

/** @brief Open intent for ::fw_fs_open. */
typedef enum : uint8_t {
  k_fw_fs_open_read           = 0U, /**< Existing file, read-only.        */
  k_fw_fs_open_write_truncate = 1U, /**< Create/truncate, write-only.     */
  k_fw_fs_open_append         = 2U, /**< Create/append, write-only.       */
  k_fw_fs_open_create_new     = 3U, /**< Create only when leaf is absent. */
} fw_fs_open_mode_t;

/** @brief Destination policy for a staged transaction. */
typedef enum : uint8_t {
  k_fw_fs_txn_create_new     = 0U, /**< Commit only when destination is absent. */
  k_fw_fs_txn_replace_atomic = 1U, /**< Atomically replace an existing file.    */
} fw_fs_transaction_policy_t;

/** @brief Capability bits returned in ::fw_fs_caps_t.flags. */
typedef enum : uint32_t {
  /** @brief Metadata and namespace operations are available. */
  k_fw_fs_cap_namespace = 1UL << 0U,
  /** @brief Regular-file stream operations are available. */
  k_fw_fs_cap_stream = 1UL << 1U,
  /** @brief Volume capacity and available bytes can be queried. */
  k_fw_fs_cap_space_query = 1UL << 2U,
  /** @brief Rename is supported within one backend volume. */
  k_fw_fs_cap_same_volume_rename = 1UL << 3U,
  /** @brief Rename can atomically replace an existing destination. */
  k_fw_fs_cap_atomic_replace = 1UL << 4U,
  /** @brief Rename can atomically reject an existing destination. */
  k_fw_fs_cap_atomic_noreplace = 1UL << 5U,
  /** @brief Open can atomically require that its leaf be absent. */
  k_fw_fs_cap_create_exclusive = 1UL << 6U,
  /** @brief An explicit file-sync operation is available. */
  k_fw_fs_cap_file_sync = 1UL << 7U,
  /** @brief Successful file sync reaches durable media. */
  k_fw_fs_cap_durable_file_sync = 1UL << 8U,
  /** @brief Namespace changes can be made durable. */
  k_fw_fs_cap_durable_directory_sync = 1UL << 9U,
  /** @brief Staged publication operations are available. */
  k_fw_fs_cap_transactions = 1UL << 10U,
  /** @brief Symbolic links may be represented by the backend. */
  k_fw_fs_cap_symlinks = 1UL << 11U,
  /** @brief Path traversal refuses symbolic-link components. */
  k_fw_fs_cap_rejects_symlink_walk = 1UL << 12U,
  /** @brief Path-component comparisons are case-sensitive. */
  k_fw_fs_cap_case_sensitive = 1UL << 13U,
  /** @brief The backing volume may disappear at runtime. */
  k_fw_fs_cap_removable_media = 1UL << 14U,
  /** @brief Independent calls may execute concurrently. */
  k_fw_fs_cap_thread_safe = 1UL << 15U,
  /** @brief Creation timestamps may be reported as valid. */
  k_fw_fs_cap_created_time = 1UL << 16U,
  /** @brief Modification timestamps may be reported as valid. */
  k_fw_fs_cap_modified_time = 1UL << 17U,
  /** @brief Access timestamps may be reported as valid. */
  k_fw_fs_cap_accessed_time = 1UL << 18U,
} fw_fs_capability_t;

/** @brief Static properties and workspace requirements of one bound port. */
typedef struct {
  uint64_t max_file_bytes;              /**< Largest supported regular file.     */
  uint32_t flags;                       /**< OR of ::fw_fs_capability_t.         */
  uint32_t file_workspace_bytes;        /**< State bytes required by open.       */
  uint32_t directory_workspace_bytes;   /**< State bytes required by dir open.   */
  uint32_t transaction_workspace_bytes; /**< State bytes required by begin.      */
  uint16_t path_max_bytes;              /**< Path bytes including NUL.           */
  uint16_t name_max_bytes;              /**< Component bytes excluding NUL.      */
  uint16_t max_open_files;              /**< Concurrent backend file limit.      */
  uint16_t max_open_directories;        /**< Concurrent directory cursors.       */
  uint8_t  file_workspace_align;        /**< Required file-state alignment.      */
  uint8_t  directory_workspace_align;   /**< Required directory-state alignment. */
  uint8_t  transaction_workspace_align; /**< Required transaction alignment.     */
} fw_fs_caps_t;

/** @brief Backend-neutral civil time with nanosecond precision. */
typedef struct {
  uint32_t nanosecond;     /**< Fraction within second, 0..999,999,999.        */
  uint16_t year;           /**< Full civil year.                               */
  int16_t  utc_offset_min; /**< Offset from UTC when the validity flag is set. */
  uint8_t  month;          /**< Month, 1..12.                                  */
  uint8_t  day;            /**< Day, 1..31.                                    */
  uint8_t  hour;           /**< Hour, 0..23.                                   */
  uint8_t  minute;         /**< Minute, 0..59.                                 */
  uint8_t  second;         /**< Second, 0..59.                                 */
} fw_fs_datetime_t;

/**
 * @brief One portable timestamp and independent availability facts.
 * @details Civil time avoids requiring an epoch or 64-bit calendar conversion
 *          on firmware. `valid == false` means `value` is all zero. A valid
 *          timestamp may still have an unknown UTC offset (FAT is one example).
 */
typedef struct {
  fw_fs_datetime_t value;            /**< Decoded civil date and time.         */
  bool             valid;            /**< True when the field is meaningful.   */
  bool             utc_offset_valid; /**< True when `utc_offset_min` is known. */
} fw_fs_timestamp_t;

/** @brief Result of a portable metadata query. */
typedef struct {
  uint64_t          size_bytes; /**< File length; zero for a directory.         */
  fw_fs_timestamp_t created;    /**< Creation/birth time, when supported.       */
  fw_fs_timestamp_t modified;   /**< Content modification time, when supported. */
  fw_fs_timestamp_t accessed;   /**< Last access time, when supported.          */
  fw_fs_node_type_t type;       /**< Kind of node at the path.                  */
  bool              exists;     /**< False means a clean lookup miss.           */
} fw_fs_stat_t;

/** @brief One directory entry, valid only for the callback invocation. */
typedef struct {
  const char*       name;       /**< NUL-terminated leaf name.          */
  uint64_t          size_bytes; /**< File length; zero for directories. */
  uint16_t          name_bytes; /**< Bytes excluding the NUL.           */
  fw_fs_node_type_t type;       /**< Entry kind.                        */
} fw_fs_dirent_t;

/** @brief Stable caller-owned value returned by ::fw_fs_dir_next. */
typedef struct {
  char              name[k_fw_fs_path_cap]; /**< Copied NUL-terminated leaf. */
  uint64_t          size_bytes;             /**< File length; zero for dirs. */
  uint16_t          name_bytes;             /**< Bytes excluding the NUL.    */
  fw_fs_node_type_t type;                   /**< Entry kind.                 */
} fw_fs_dirent_value_t;

/** @brief Portable volume usage snapshot. */
typedef struct {
  uint64_t total_bytes; /**< Addressable data bytes.      */
  uint64_t free_bytes;  /**< Bytes available to new data. */
  uint64_t used_bytes;  /**< Allocated bytes.             */
} fw_fs_space_t;

/**
 * @brief Bounded list callback.
 * @param[in]     ctx          Caller cookie.
 * @param[in]     entry        Entry valid for this call only.
 * @param[in,out] out_continue Set false to stop delivering entries.
 * @return ::k_ra8_ok to continue/stop normally, or an error to abort delivery.
 */
typedef ra8_err_t (*fw_fs_list_fn_t)(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue);

typedef struct fw_fs_namespace_iface   fw_fs_namespace_iface_t;
typedef struct fw_fs_stream_iface      fw_fs_stream_iface_t;
typedef struct fw_fs_transaction_iface fw_fs_transaction_iface_t;

/** @brief Independently injectable path/namespace facade. */
typedef struct {
  const fw_fs_namespace_iface_t* iface; /**< Bound private vtable.   */
  void*                          ctx;   /**< Backend context.        */
  fw_fs_caps_t                   caps;  /**< Immutable capabilities. */
} fw_fs_namespace_t;

/** @brief Independently injectable open-stream facade. */
typedef struct {
  const fw_fs_stream_iface_t* iface; /**< Bound private vtable.   */
  void*                       ctx;   /**< Backend context.        */
  fw_fs_caps_t                caps;  /**< Immutable capabilities. */
} fw_fs_stream_port_t;

/** @brief Independently injectable staged-transaction facade. */
typedef struct {
  const fw_fs_transaction_iface_t* iface; /**< Bound private vtable.   */
  void*                            ctx;   /**< Backend context.        */
  fw_fs_caps_t                     caps;  /**< Immutable capabilities. */
} fw_fs_transaction_port_t;

/** @brief One complete composition-root filesystem binding. */
typedef struct {
  fw_fs_namespace_t        names;        /**< Namespace operations.   */
  fw_fs_stream_port_t      streams;      /**< Byte-stream operations. */
  fw_fs_transaction_port_t transactions; /**< Staged publication.     */
  fw_fs_caps_t             caps;         /**< Shared capabilities.    */
} fw_fs_t;

/** @brief Caller-owned open file; fields are private to the facade. */
typedef struct {
  const fw_fs_stream_iface_t* iface;       /**< Bound stream vtable.    */
  void*                       ctx;         /**< Adapter context.        */
  void*                       state;       /**< Caller workspace.       */
  uint32_t                    state_bytes; /**< Workspace extent.       */
  bool                        is_open;     /**< Facade lifecycle guard. */
} fw_fs_file_t;

/** @brief Caller-owned open directory cursor; fields are private to the facade. */
typedef struct {
  const fw_fs_namespace_iface_t* iface;       /**< Bound namespace vtable. */
  void*                          ctx;         /**< Adapter context.        */
  void*                          state;       /**< Caller workspace.       */
  uint32_t                       state_bytes; /**< Workspace extent.       */
  fw_fs_caps_t                   caps;        /**< Immutable port caps.    */
  bool                           is_open;     /**< Facade lifecycle guard. */
} fw_fs_dir_t;

/** @brief Caller-owned transaction; fields are private to the facade. */
typedef struct {
  const fw_fs_transaction_iface_t* iface;       /**< Bound vtable.       */
  void*                            ctx;         /**< Adapter context.    */
  void*                            state;       /**< Caller workspace.   */
  uint32_t                         state_bytes; /**< Workspace extent.   */
  bool                             active;      /**< Begin succeeded.    */
  bool                             validated;   /**< Stage was accepted. */
} fw_fs_transaction_t;

/**
 * @brief Validate staged bytes through a read-only generic file handle.
 * @param[in] ctx    Caller cookie.
 * @param[in] staged Open read-only view of the staged artifact.
 * @return ::k_ra8_ok only when the artifact is safe to publish.
 */
typedef ra8_err_t (*fw_fs_validate_fn_t)(void* ctx, fw_fs_file_t* staged);

#ifdef __cplusplus
}
#endif
