/**
 * @file test_fw_if_fs_posix_raw.c
 * @brief Raw hosted-directory enumeration qualification for `fw_if_fs`.
 *
 * @details Exercises the native bounded directory path and a test-only raw
 * read seam. Injected records cover interrupted/short reads, exact and capped
 * enumeration, callback termination, backend errors, and malformed kernel
 * records without introducing a production fallback or allocator.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/** @brief Request hosted extension declarations used by the fixture. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "fw_if_fs_posix_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Fixed fixture, script, and raw-record bounds. */
typedef enum : uint16_t {
  k_test_root_bytes        = 64U,   /**< Temporary root path capacity. */
  k_test_record_bytes      = 512U,  /**< Injected record batch size.   */
  k_test_reader_capacity   = 4096U, /**< Production reader capacity.   */
  k_test_capture_entries   = 8U,    /**< Captured callback entries.    */
  k_test_script_steps      = 20U,   /**< Maximum injected read steps.  */
  k_test_interrupted_reads = 16U,   /**< Production retry ceiling.     */
} test_limits_t;

/** @brief File and directory creation modes for the private fixture. */
typedef enum : uint16_t {
  k_test_file_mode      = 0600, /**< Owner-only fixture-file permissions.      */
  k_test_directory_mode = 0700, /**< Owner-only fixture-directory permissions. */
} test_modes_t;

#if defined(__linux__)
/** @brief Linux `getdents64` fields used by the independent fixture encoder. */
typedef enum : uint8_t {
  k_test_reclen_offset = 16U, /**< Fixture offset of `d_reclen`. */
  k_test_name_offset   = 19U, /**< Fixture offset of `d_name`.   */
  k_test_record_align  = 8U,  /**< Fixture record alignment.     */
} test_dirent_layout_t;
#elif defined(__APPLE__)
/** @brief Darwin `getdirentries64` fields used by the fixture encoder. */
typedef enum : uint8_t {
  k_test_reclen_offset = 16U, /**< Fixture offset of `d_reclen`. */
  k_test_namlen_offset = 18U, /**< Fixture offset of `d_namlen`. */
  k_test_name_offset   = 21U, /**< Fixture offset of `d_name`.   */
  k_test_record_align  = 4U,  /**< Fixture record alignment.     */
} test_dirent_layout_t;
#endif

/** @brief One scripted low-level directory read result. */
typedef struct {
  const uint8_t* bytes;  /**< Bytes copied for a successful result. */
  int64_t        result; /**< Read count or negative failure.       */
  int            error;  /**< Captured errno for a failure.         */
} reader_step_t;

/** @brief Caller-owned sequence consumed by the injected reader. */
typedef struct {
  reader_step_t steps[k_test_script_steps]; /**< Ordered raw read results. */
  uint16_t      step_count;                 /**< Populated step count.     */
  uint16_t      next_step;                  /**< Next result to consume.   */
} reader_script_t;

/** @brief Confined hosted filesystem and native nodes used by every vector. */
typedef struct {
  char                root[k_test_root_bytes]; /**< Temporary root path. */
  fw_fs_t             fs;                      /**< Portable binding.    */
  fw_fs_posix_state_t state;                   /**< POSIX adapter state. */
} posix_fixture_t;

/** @brief Captured list callback results and termination policy. */
typedef struct {
  /** @brief NUL-terminated names copied during each callback. */
  char              names[k_test_capture_entries][k_posix_component_cap];
  fw_fs_node_type_t types[k_test_capture_entries]; /**< Captured node kinds.         */
  uint64_t          sizes[k_test_capture_entries]; /**< Captured node sizes.         */
  uint32_t          count;                         /**< Callback invocation count.   */
  uint32_t          stop_after;                    /**< Zero or callback stop limit. */
  ra8_err_t         forced_error;                  /**< Optional callback failure.   */
} list_capture_t;

/** @brief Canonical bytes stored in each regular fixture file. */
static const uint8_t s_file_bytes[] = {'r', 'a', '8'};

#if defined(__linux__) || defined(__APPLE__)
/** @brief Align one raw record length to the qualified host ABI. @details Implements the bounded record align fixture step using caller-owned state. @param[in] bytes Caller-supplied bounded extent or quantity. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static uint16_t internal_record_align(uint16_t bytes)
{
  const uint16_t mask = (uint16_t)k_test_record_align - 1U;
  return (uint16_t)((bytes + mask) & (uint16_t)~mask);
}

/** @brief Append one independently encoded raw directory record. @details Implements the bounded record append fixture step using caller-owned state. @param[in,out] buffer Caller-owned bounded byte storage. @param[in,out] cursor Value required by this filesystem vector. @param[in] name Validated fixture path or name value. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_record_append(uint8_t* buffer, uint16_t* cursor, const char* name)
{
  const size_t   name_bytes   = strlen(name);
  const uint16_t unaligned    = (uint16_t)((uint16_t)k_test_name_offset + name_bytes + 1U);
  const uint16_t record_bytes = internal_record_align(unaligned);
  TEST_ASSERT(name_bytes < (size_t)k_posix_component_cap);
  TEST_ASSERT((uint32_t)*cursor + (uint32_t)record_bytes <= (uint32_t)k_test_record_bytes);
  (void)memset(&buffer[*cursor], 0, (size_t)record_bytes);
  (void)memcpy(&buffer[(uint16_t)(*cursor + (uint16_t)k_test_reclen_offset)],
               &record_bytes,
               sizeof(record_bytes));
#if defined(__APPLE__)
  const uint16_t darwin_name_bytes = (uint16_t)name_bytes;
  (void)memcpy(&buffer[(uint16_t)(*cursor + (uint16_t)k_test_namlen_offset)],
               &darwin_name_bytes,
               sizeof(darwin_name_bytes));
#endif
  (void)memcpy(&buffer[(uint16_t)(*cursor + (uint16_t)k_test_name_offset)], name, name_bytes + 1U);
  *cursor = (uint16_t)(*cursor + record_bytes);
}

/** @brief Produce one scripted raw read without touching the directory fd. @details Implements the bounded script read fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] fd Value required by this filesystem vector. @param[in,out] buffer Caller-owned bounded byte storage. @param[in] capacity Caller-supplied bounded extent or quantity. @param[out] out_errno Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static int64_t
internal_script_read(void* ctx, int fd, uint8_t* buffer, uint32_t capacity, int* out_errno)
{
  reader_script_t* script = (reader_script_t*)ctx;
  if (script == nullptr || buffer == nullptr || out_errno == nullptr) {
    return -1;
  }
  if (fd < 0 || capacity == 0U || script->next_step >= script->step_count) {
    *out_errno = EIO;
    return -1;
  }
  const reader_step_t* step = &script->steps[script->next_step++];
  *out_errno                = step->error;
  if (step->result > 0 && (uint64_t)step->result <= (uint64_t)capacity) {
    if (step->bytes == nullptr) {
      *out_errno = EINVAL;
      return -1;
    }
    (void)memcpy(buffer, step->bytes, (size_t)step->result);
  }
  return step->result;
}

/** @brief Create one regular file relative to the confined root descriptor. @details Implements the bounded create file fixture step using caller-owned state. @param[in] root_fd Value required by this filesystem vector. @param[in] name Validated fixture path or name value. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_create_file(int root_fd, const char* name)
{
  const int fd =
    openat(root_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)k_test_file_mode);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(sizeof(s_file_bytes), write(fd, s_file_bytes, sizeof(s_file_bytes)));
  TEST_ASSERT_EQ(0, close(fd));
}

/** @brief Create and bind the confined native fixture. @details Implements the bounded fixture init fixture step using caller-owned state. @param[in,out] fixture Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_fixture_init(posix_fixture_t* fixture)
{
  static const char root_template[] = "/tmp/fw_fs_raw_XXXXXX";
  TEST_ASSERT_NOT_NULL(fixture);
  (void)memset(fixture, 0, sizeof(*fixture));
  (void)memcpy(fixture->root, root_template, sizeof(root_template));
  TEST_ASSERT_NOT_NULL(mkdtemp(fixture->root));
  fixture->state.root_fd      = -1;
  const fw_fs_posix_cfg_t cfg = {.root_path = fixture->root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fixture->fs, &fixture->state, &cfg));
  internal_create_file(fixture->state.root_fd, "alpha");
  internal_create_file(fixture->state.root_fd, "beta");
  TEST_ASSERT_EQ(0, mkdirat(fixture->state.root_fd, "shelf", (mode_t)k_test_directory_mode));
  TEST_ASSERT_EQ(0, symlinkat("alpha", fixture->state.root_fd, "link"));
}

/** @brief Tear down every native fixture node and the adapter binding. @details Implements the bounded fixture deinit fixture step using caller-owned state. @param[in,out] fixture Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_fixture_deinit(posix_fixture_t* fixture)
{
  TEST_ASSERT_NOT_NULL(fixture);
  TEST_ASSERT(fixture->state.initialized);
  TEST_ASSERT_EQ(0, unlinkat(fixture->state.root_fd, "alpha", 0));
  TEST_ASSERT_EQ(0, unlinkat(fixture->state.root_fd, "beta", 0));
  TEST_ASSERT_EQ(0, unlinkat(fixture->state.root_fd, "link", 0));
  TEST_ASSERT_EQ(0, unlinkat(fixture->state.root_fd, "shelf", AT_REMOVEDIR));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&fixture->state));
  TEST_ASSERT_EQ(0, rmdir(fixture->root));
}

/** @brief Capture one portable entry and apply the selected callback policy. @details Implements the bounded capture fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] entry Value required by this filesystem vector. @param[out] out_continue Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_capture(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  list_capture_t* capture = (list_capture_t*)ctx;
  if (capture == nullptr || entry == nullptr || out_continue == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (capture->count >= (uint32_t)k_test_capture_entries ||
      entry->name_bytes >= (uint16_t)k_posix_component_cap) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t index = capture->count++;
  (void)memcpy(capture->names[index], entry->name, (size_t)entry->name_bytes + 1U);
  capture->types[index] = entry->type;
  capture->sizes[index] = entry->size_bytes;
  *out_continue         = capture->stop_after == 0U || capture->count < capture->stop_after;
  return capture->forced_error;
}

/** @brief Find one captured name, or return the capture capacity. @details Implements the bounded find fixture step using caller-owned state. @param[in] capture Value required by this filesystem vector. @param[in] name Validated fixture path or name value. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static uint32_t internal_find(const list_capture_t* capture, const char* name)
{
  TEST_ASSERT_NOT_NULL(capture);
  TEST_ASSERT_NOT_NULL(name);
  for (uint32_t i = 0U; i < capture->count; ++i) {
    if (strcmp(capture->names[i], name) == 0) {
      return i;
    }
  }
  return (uint32_t)k_test_capture_entries;
}

/** @brief Install and run one caller-owned raw-read script. @details Implements the bounded list script fixture step using caller-owned state. @param[in] fixture Value required by this filesystem vector. @param[in,out] script Value required by this filesystem vector. @param[in] max_entries Value required by this filesystem vector. @param[in,out] capture Value required by this filesystem vector. @param[out] out_count Caller-owned output populated on success. @param[out] out_complete Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_list_script(const posix_fixture_t* fixture,
                                      reader_script_t*       script,
                                      uint32_t               max_entries,
                                      list_capture_t*        capture,
                                      uint32_t*              out_count,
                                      bool*                  out_complete)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_posix_test_set_directory_reader(internal_script_read, script));
  return fw_fs_listdir(&fixture->fs.names,
                       "/",
                       max_entries,
                       internal_capture,
                       capture,
                       out_count,
                       out_complete);
}

/** @brief Prove the real host primitive reports every fixture node exactly once. @details Runs the native directory vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_native_directory(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  list_capture_t capture  = {};
  uint32_t       count    = 0U;
  bool           complete = false;
  TEST_BEGIN("fw_if_fs POSIX raw native directory");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_listdir(&fixture.fs.names, "/", 8U, internal_capture, &capture, &count, &complete));
  TEST_ASSERT_EQ(4U, count);
  TEST_ASSERT(complete);
  const uint32_t alpha = internal_find(&capture, "alpha");
  const uint32_t beta  = internal_find(&capture, "beta");
  const uint32_t shelf = internal_find(&capture, "shelf");
  const uint32_t link  = internal_find(&capture, "link");
  TEST_ASSERT(alpha < (uint32_t)k_test_capture_entries);
  TEST_ASSERT(beta < (uint32_t)k_test_capture_entries);
  TEST_ASSERT(shelf < (uint32_t)k_test_capture_entries);
  TEST_ASSERT(link < (uint32_t)k_test_capture_entries);
  TEST_ASSERT_EQ(k_fw_fs_node_file, capture.types[alpha]);
  TEST_ASSERT_EQ(sizeof(s_file_bytes), capture.sizes[alpha]);
  TEST_ASSERT_EQ(k_fw_fs_node_directory, capture.types[shelf]);
  TEST_ASSERT_EQ(0U, capture.sizes[shelf]);
  TEST_ASSERT_EQ(k_fw_fs_node_symlink, capture.types[link]);
  internal_fixture_deinit(&fixture);
  TEST_END("fw_if_fs POSIX raw native directory");
}

/** @brief Prove empty, interrupted, and short raw batches preserve list semantics. @details Runs the empty mixed interrupted vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_empty_mixed_interrupted(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  uint8_t  records[k_test_record_bytes] = {};
  uint16_t cursor                       = 0U;
  internal_record_append(records, &cursor, ".");
  internal_record_append(records, &cursor, "..");
  reader_script_t empty    = {.steps      = {{.bytes = records, .result = cursor, .error = 0},
                                             {.bytes = nullptr, .result = 0, .error = 0}},
                              .step_count = 2U};
  list_capture_t  capture  = {};
  uint32_t        count    = UINT32_MAX;
  bool            complete = false;
  TEST_BEGIN("fw_if_fs POSIX raw empty/mixed/EINTR");
  TEST_ASSERT_EQ(k_ra8_ok, internal_list_script(&fixture, &empty, 8U, &capture, &count, &complete));
  TEST_ASSERT_EQ(0U, count);
  TEST_ASSERT(complete);

  cursor = 0U;
  internal_record_append(records, &cursor, "alpha");
  internal_record_append(records, &cursor, "shelf");
  internal_record_append(records, &cursor, "link");
  reader_script_t mixed = {.steps      = {{.bytes = nullptr, .result = -1, .error = EINTR},
                                          {.bytes = records, .result = cursor, .error = 0},
                                          {.bytes = nullptr, .result = 0, .error = 0}},
                           .step_count = 3U};
  capture               = (list_capture_t){};
  count                 = 0U;
  complete              = false;
  TEST_ASSERT_EQ(k_ra8_ok, internal_list_script(&fixture, &mixed, 8U, &capture, &count, &complete));
  TEST_ASSERT_EQ(3U, count);
  TEST_ASSERT(complete);
  TEST_ASSERT_EQ(3U, mixed.next_step);
  TEST_ASSERT(strcmp(capture.names[0], "alpha") == 0);
  TEST_ASSERT_EQ(k_fw_fs_node_file, capture.types[0]);
  TEST_ASSERT_EQ(k_fw_fs_node_directory, capture.types[1]);
  TEST_ASSERT_EQ(k_fw_fs_node_symlink, capture.types[2]);
  internal_fixture_deinit(&fixture);
  TEST_END("fw_if_fs POSIX raw empty/mixed/EINTR");
}

/** @brief Prove callback stop/error and exact/capped list bounds. @details Runs the callback and entry bounds vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_callback_and_entry_bounds(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  uint8_t  records[k_test_record_bytes] = {};
  uint16_t cursor                       = 0U;
  internal_record_append(records, &cursor, "alpha");
  internal_record_append(records, &cursor, "beta");
  internal_record_append(records, &cursor, "shelf");
  TEST_BEGIN("fw_if_fs POSIX raw callback/cap bounds");

  reader_script_t script   = {.steps      = {{.bytes = records, .result = cursor, .error = 0}},
                              .step_count = 1U};
  list_capture_t  capture  = {.stop_after = 1U};
  uint32_t        count    = 0U;
  bool            complete = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_list_script(&fixture, &script, 8U, &capture, &count, &complete));
  TEST_ASSERT_EQ(1U, count);
  TEST_ASSERT(!complete);

  script  = (reader_script_t){.steps      = {{.bytes = records, .result = cursor, .error = 0}},
                              .step_count = 1U};
  capture = (list_capture_t){.forced_error = k_ra8_fail};
  count   = 0U;
  TEST_ASSERT_EQ(k_ra8_fail,
                 internal_list_script(&fixture, &script, 8U, &capture, &count, &complete));
  TEST_ASSERT_EQ(1U, count);
  TEST_ASSERT(!complete);

  script  = (reader_script_t){.steps      = {{.bytes = records, .result = cursor, .error = 0}},
                              .step_count = 1U};
  capture = (list_capture_t){};
  count   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_list_script(&fixture, &script, 2U, &capture, &count, &complete));
  TEST_ASSERT_EQ(2U, count);
  TEST_ASSERT(!complete);

  script   = (reader_script_t){.steps      = {{.bytes = records, .result = cursor, .error = 0},
                                              {.bytes = nullptr, .result = 0, .error = 0}},
                               .step_count = 2U};
  capture  = (list_capture_t){};
  count    = 0U;
  complete = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_list_script(&fixture, &script, 3U, &capture, &count, &complete));
  TEST_ASSERT_EQ(3U, count);
  TEST_ASSERT(complete);
  internal_fixture_deinit(&fixture);
  TEST_END("fw_if_fs POSIX raw callback/cap bounds");
}

/** @brief Run one malformed or failed raw-read vector. @details Implements the bounded expect read error fixture step using caller-owned state. @param[in] fixture Value required by this filesystem vector. @param[in,out] script Value required by this filesystem vector. @param[in] expected Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_expect_read_error(const posix_fixture_t* fixture,
                                       reader_script_t*       script,
                                       ra8_err_t              expected)
{
  list_capture_t capture  = {};
  uint32_t       count    = UINT32_MAX;
  bool           complete = true;
  TEST_ASSERT_EQ(expected, internal_list_script(fixture, script, 8U, &capture, &count, &complete));
  TEST_ASSERT_EQ(0U, count);
  TEST_ASSERT(!complete);
}

/** @brief Prove backend failures and malformed raw records fail closed. @details Runs the raw failures vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_raw_failures(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  TEST_BEGIN("fw_if_fs POSIX raw malformed/error records");

  reader_script_t script = {.steps      = {{.bytes = nullptr, .result = -1, .error = EIO}},
                            .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_fail);
  script =
    (reader_script_t){.steps = {{.bytes = nullptr, .result = -1, .error = 0}}, .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);
  script = (reader_script_t){
    .steps      = {{.bytes = nullptr, .result = (int64_t)k_test_reader_capacity + 1, .error = 0}},
    .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);

  uint8_t malformed[k_test_record_bytes];
  (void)memset(malformed, 0, sizeof(malformed));
  script = (reader_script_t){
    .steps      = {{.bytes = malformed, .result = (int64_t)k_test_name_offset, .error = 0}},
    .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);

  uint16_t record_bytes = 0U;
  (void)memcpy(&malformed[k_test_reclen_offset], &record_bytes, sizeof(record_bytes));
  script = (reader_script_t){
    .steps      = {{.bytes = malformed, .result = (int64_t)internal_record_align(32U), .error = 0}},
    .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);

  record_bytes = internal_record_align(32U);
  (void)memcpy(&malformed[k_test_reclen_offset], &record_bytes, sizeof(record_bytes));
#if defined(__APPLE__)
  const uint16_t malformed_name_bytes = 3U;
  (void)memcpy(&malformed[k_test_namlen_offset],
               &malformed_name_bytes,
               sizeof(malformed_name_bytes));
#endif
  (void)memset(&malformed[k_test_name_offset],
               'x',
               (size_t)(record_bytes - (uint16_t)k_test_name_offset));
  script = (reader_script_t){.steps = {{.bytes = malformed, .result = record_bytes, .error = 0}},
                             .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);

  (void)memset(malformed, 0, sizeof(malformed));
  record_bytes = internal_record_align((uint16_t)k_test_name_offset + 1U);
  (void)memcpy(&malformed[k_test_reclen_offset], &record_bytes, sizeof(record_bytes));
  script = (reader_script_t){.steps = {{.bytes = malformed, .result = record_bytes, .error = 0}},
                             .step_count = 1U};
  internal_expect_read_error(&fixture, &script, k_ra8_err_invalid_state);

  reader_script_t interrupted = {.step_count = (uint16_t)k_test_interrupted_reads};
  for (uint16_t i = 0U; i < (uint16_t)k_test_interrupted_reads; ++i) {
    interrupted.steps[i] = (reader_step_t){.bytes = nullptr, .result = -1, .error = EINTR};
  }
  internal_expect_read_error(&fixture, &interrupted, k_ra8_err_busy);
  internal_fixture_deinit(&fixture);
  TEST_END("fw_if_fs POSIX raw malformed/error records");
}

int main(void)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_posix_test_set_directory_reader(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_posix_test_set_directory_reader(internal_script_read, nullptr));
  internal_test_native_directory();
  internal_test_empty_mixed_interrupted();
  internal_test_callback_and_entry_bounds();
  internal_test_raw_failures();
  return 0;
}
#else
int main(void)
{
  return 0;
}
#endif
