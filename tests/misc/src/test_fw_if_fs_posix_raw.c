/**
 * @file test_fw_if_fs_posix_raw.c
 * @brief Raw hosted-directory and descriptor-cleanup qualification for `fw_if_fs`.
 *
 * @details Exercises a native bounded directory path and test-only raw read seam.
 * Injected records cover interrupted/short reads, bounded enumeration, callback
 * termination, backend errors, and malformed records. A Linux-only linker seam
 * injects one consumed-descriptor close failure without changing production code.
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
#include "fw_if_fs_backend.h"
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

#ifdef __linux__
/** @brief Linux `getdents64` fields used by the independent fixture encoder. */
typedef enum : uint8_t {
  k_test_reclen_offset = 16U, /**< Fixture offset of `d_reclen`. */
  k_test_name_offset   = 19U, /**< Fixture offset of `d_name`.   */
  k_test_record_align  = 8U,  /**< Fixture record alignment.     */
} test_dirent_layout_t;
#elifdef __APPLE__
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

#ifdef RA8_POSIX_CLOSE_WRAP_TEST
/**
 * @var s_close_failure_fd
 * @brief Descriptor whose next close reports a consumed-descriptor failure
 * @details The linker wrapper consumes this descriptor through the real host
 *          close, clears the selection, then reports `EIO` exactly once.
 * @note Test-binary state; only the single-threaded close-failure vector mutates it.
 * @warning Arm only after all fixture setup closes have completed.
 * @since 0.1.0
 */
static int s_close_failure_fd = -1;

/**
 * @brief Call the unwrapped host close selected by the GNU linker
 * @param[in] fd Open descriptor to consume.
 * @return Native close result.
 * @retval 0 The descriptor was consumed.
 * @retval -1 The host rejected the close and set `errno`.
 * @pre @p fd is any descriptor value accepted by `close()`.
 * @pre The test target links with `--wrap=close`.
 * @post The host applies normal close semantics to @p fd.
 * @post No test fault-selection state is changed.
 * @note This source name is bound to the linker's reserved real-symbol spelling.
 * @since 0.1.0
 */
extern int ra8_test_real_close(int fd) __asm__("__real_close");

/**
 * @brief Intercept hosted closes for one deterministic fault vector
 * @details Always consumes @p fd through the real host close. When @p fd is
 *          the armed descriptor, clears the arm and reports `EIO` so production
 *          cleanup must consume the already-opened sibling descriptor.
 * @param[in] fd Descriptor selected by a hosted close call.
 * @return Native close result or the injected failure.
 * @retval 0 The real close succeeded and no fault was armed for @p fd.
 * @retval -1 The real close failed or the selected one-shot fault fired.
 * @pre The test runs single-threaded and any selected descriptor is open.
 * @pre The test target links with `--wrap=close`.
 * @post The real close is invoked exactly once for @p fd.
 * @post A matching selection is cleared before returning `EIO`.
 * @note Test-only linker callback; production targets never compile this symbol.
 * @par MC/DC:
 * The one-condition selection decision is covered by ordinary fixture closes
 * (false) and the canonical alias parent close (true).
 * @since 0.1.0
 */
RA8_TEST_HELPER int ra8_test_close_intercept(int fd) __asm__("__wrap_close");

RA8_TEST_HELPER int ra8_test_close_intercept(int fd)
{
  const int result = ra8_test_real_close(fd);
  if (fd == s_close_failure_fd) {
    s_close_failure_fd = -1;
    errno              = EIO;
    return -1;
  }
  return result;
}
#endif

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
#ifdef __APPLE__
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
#ifdef __APPLE__
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

#ifdef RA8_POSIX_CLOSE_WRAP_TEST
/**
 * @brief Prove a canonical parent-close failure consumes the opened child
 * @details Builds a real `private/tmp` fixture, records the two lowest free
 *          descriptors, and injects `EIO` only after the canonical opener has
 *          opened both directories. The opener must return the close error,
 *          publish no child, and make both descriptor numbers reusable.
 * @pre GNU linker close wrapping is enabled for this Linux-only test target.
 * @pre No other thread allocates or closes descriptors during the vector.
 * @post The injected selection is consumed and both opened descriptors are closed.
 * @post Every namespace fixture is removed before assertions complete.
 * @note Not thread-safe because descriptor allocation and injection state are global.
 * @par MC/DC:
 * The production decisions are independent single-condition branches. A
 * successful child open followed by an injected parent-close failure takes the
 * cleanup-error and unpublished-child cleanup branches that normal opens do not.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_alias_parent_close_failure(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  TEST_BEGIN("fw_if_fs POSIX canonical alias parent-close failure");

  const int made_private = mkdirat(fixture.state.root_fd, "private", (mode_t)k_test_directory_mode);
  const int made_tmp = mkdirat(fixture.state.root_fd, "private/tmp", (mode_t)k_test_directory_mode);
  const int before_first  = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int before_second = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int closed_first  = close(before_first);
  const int closed_second = close(before_second);

  s_close_failure_fd       = before_first;
  int             alias_fd = -1;
  const ra8_err_t status =
    priv_fs_posix_root_alias_open(fixture.state.root_fd, k_posix_root_alias_tmp, &alias_fd);
  const int fault_selection    = s_close_failure_fd;
  const int after_first        = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int after_second       = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int after_close_first  = close(after_first);
  const int after_close_second = close(after_second);
  const int removed_tmp        = unlinkat(fixture.state.root_fd, "private/tmp", AT_REMOVEDIR);
  const int removed_private    = unlinkat(fixture.state.root_fd, "private", AT_REMOVEDIR);

  internal_fixture_deinit(&fixture);
  TEST_ASSERT_EQ(0, made_private);
  TEST_ASSERT_EQ(0, made_tmp);
  TEST_ASSERT(before_first >= 0);
  TEST_ASSERT(before_second >= 0);
  TEST_ASSERT_EQ(0, closed_first);
  TEST_ASSERT_EQ(0, closed_second);
  TEST_ASSERT_EQ(k_ra8_fail, status);
  TEST_ASSERT_EQ(-1, alias_fd);
  TEST_ASSERT_EQ(-1, fault_selection);
  TEST_ASSERT_EQ(before_first, after_first);
  TEST_ASSERT_EQ(before_second, after_second);
  TEST_ASSERT_EQ(0, after_close_first);
  TEST_ASSERT_EQ(0, after_close_second);
  TEST_ASSERT_EQ(0, removed_tmp);
  TEST_ASSERT_EQ(0, removed_private);
  TEST_END("fw_if_fs POSIX canonical alias parent-close failure");
}

/**
 * @brief Prove descriptor cleanup reports errors with deterministic precedence
 * @details Exercises a successful close after a primary failure, then injects
 *          close failures after both a primary failure and primary success.
 *          Every path must consume and invalidate the selected descriptor.
 * @pre GNU linker close wrapping is enabled for this Linux-only test target.
 * @pre No other thread allocates or closes descriptors during the vector.
 * @post Primary failures outrank cleanup failures; otherwise cleanup is visible.
 * @post Every descriptor opened by the vector is consumed exactly once.
 * @note Not thread-safe because close-failure selection is process-global.
 * @par MC/DC:
 * The helper's primary-status decision takes its failure branch with successful
 * and failed cleanup, and its success branch with failed cleanup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_close_preserve_precedence(void)
{
  TEST_BEGIN("fw_if_fs POSIX close error precedence");
  int primary_fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(primary_fd >= 0);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_fs_posix_close_fd_preserve(&primary_fd, k_ra8_err_invalid_arg));
  TEST_ASSERT_EQ(-1, primary_fd);

  int dual_failure_fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(dual_failure_fd >= 0);
  s_close_failure_fd = dual_failure_fd;
  TEST_ASSERT_EQ(k_ra8_err_busy, priv_fs_posix_close_fd_preserve(&dual_failure_fd, k_ra8_err_busy));
  TEST_ASSERT_EQ(-1, dual_failure_fd);
  TEST_ASSERT_EQ(-1, s_close_failure_fd);

  int cleanup_failure_fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(cleanup_failure_fd >= 0);
  s_close_failure_fd = cleanup_failure_fd;
  TEST_ASSERT_EQ(k_ra8_fail, priv_fs_posix_close_fd_preserve(&cleanup_failure_fd, k_ra8_ok));
  TEST_ASSERT_EQ(-1, cleanup_failure_fd);
  TEST_ASSERT_EQ(-1, s_close_failure_fd);
  TEST_END("fw_if_fs POSIX close error precedence");
}
#endif

/** @brief Fixed operands used by the portable filesystem facade guard matrix. */
typedef enum : uint16_t {
  k_guard_work_bytes    = 128U,                            /**< Guard workspace bytes.     */
  k_guard_need_bytes    = 32U,                             /**< Backend workspace demand.  */
  k_guard_good_align    = 4U,                              /**< Power-of-two alignment.    */
  k_guard_odd_align     = 3U,                              /**< Non power-of-two operand.  */
  k_guard_no_align      = 0U,                              /**< Zero alignment operand.    */
  k_guard_skew          = 1U,                              /**< Offset breaking alignment. */
  k_guard_bad_mode      = 9U,                              /**< Mode outside its enum.     */
  k_guard_bad_policy    = 7U,                              /**< Policy outside its enum.   */
  k_guard_one           = 1U,                              /**< One byte or one entry.     */
  k_guard_none          = 0U,                              /**< Zero bytes or entries.     */
  k_guard_short_path    = 1U,                              /**< Below the two-byte floor.  */
  k_guard_tiny_path     = 4U,                              /**< Too small for the vector.  */
  k_guard_name_cap      = 8U,                              /**< Component byte ceiling.    */
  k_guard_over_path_cap = (uint16_t)k_fw_fs_path_cap + 1U, /**< Above the interface cap.   */
} guard_limits_t;

/** @brief Accept any staged artifact; supplied only as a non-null validator. @details Implements the bounded guard validator fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in,out] staged Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_guard_validator(void* ctx, fw_fs_file_t* staged)
{
  (void)ctx;
  (void)staged;
  return k_ra8_ok;
}

/** @brief Prove the portable path grammar rejects every malformed operand. @details Runs the path grammar vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_path_grammar_guards(void)
{
  fw_fs_caps_t caps = {.path_max_bytes = (uint16_t)k_fw_fs_path_cap,
                       .name_max_bytes = (uint16_t)k_guard_name_cap};
  TEST_BEGIN("fw_if_fs path grammar guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_path_validate(nullptr, "/"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_path_validate(&caps, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_path_validate(&caps, "/a\x01"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_path_validate(&caps, "/a\x7f"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, fw_fs_path_validate(&caps, "/abcdefghi"));
  caps.path_max_bytes = (uint16_t)k_guard_tiny_path;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, fw_fs_path_validate(&caps, "/abcdef"));
  caps.path_max_bytes = (uint16_t)k_guard_short_path;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_path_validate(&caps, "/"));
  caps.path_max_bytes = (uint16_t)k_guard_over_path_cap;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_path_validate(&caps, "/"));
  caps.path_max_bytes = (uint16_t)k_fw_fs_path_cap;
  caps.name_max_bytes = (uint16_t)k_guard_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_path_validate(&caps, "/"));
  TEST_END("fw_if_fs path grammar guards");
}

/** @brief Prove every namespace call refuses an absent or unbound facade. @details Runs the namespace handle vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_namespace_handle_guards(void)
{
  const fw_fs_namespace_t unbound  = {};
  fw_fs_stat_t            stat     = {};
  fw_fs_space_t           space    = {};
  uint32_t                count    = 0U;
  bool                    complete = false;
  TEST_BEGIN("fw_if_fs namespace handle guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_stat(nullptr, "/", &stat));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_fs_stat(&unbound, "/", &stat));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_listdir(nullptr, "/", k_guard_one, internal_capture, nullptr, &count, &complete));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_mkdir(nullptr, "/x"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_unlink(nullptr, "/x"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_rmdir(nullptr, "/x"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_rename(nullptr, "/a", "/b", false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_space(nullptr, &space));
  TEST_END("fw_if_fs namespace handle guards");
}

/** @brief Prove every stream call refuses a closed or unbound file handle. @details Runs the stream handle vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_stream_handle_guards(void)
{
  fw_fs_file_t closed  = {};
  fw_fs_file_t unbound = {.is_open = true};
  uint8_t      byte    = 0U;
  uint32_t     moved   = 0U;
  uint64_t     scalar  = 0U;
  TEST_BEGIN("fw_if_fs stream handle guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_read(nullptr, &byte, k_guard_one, &moved));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_read(&closed, &byte, k_guard_one, &moved));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_fs_read(&unbound, &byte, k_guard_one, &moved));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_write(&closed, &byte, k_guard_one, &moved));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_seek(&closed, k_guard_none));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_tell(&closed, &scalar));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_file_size(&closed, &scalar));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_sync(&closed));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_close(&closed));
  TEST_END("fw_if_fs stream handle guards");
}

/** @brief Prove every transaction call refuses an inactive or unbound handle. @details Runs the transaction handle vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_transaction_handle_guards(void)
{
  fw_fs_transaction_t inactive  = {};
  fw_fs_transaction_t unbound   = {.active = true};
  uint8_t             byte      = 0U;
  uint32_t            written   = 0U;
  bool                published = false;
  TEST_BEGIN("fw_if_fs transaction handle guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_transaction_write(nullptr, &byte, k_guard_one, &written));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_write(&inactive, &byte, k_guard_one, &written));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 fw_fs_transaction_write(&unbound, &byte, k_guard_one, &written));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_seek(&inactive, k_guard_none));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_validate(&inactive, internal_guard_validator, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_commit(&inactive, &published));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_abort(&inactive));
  TEST_END("fw_if_fs transaction handle guards");
}

/** @brief Prove the workspace contract rejects every unusable caller buffer. @details Implements the bounded workspace guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_workspace_guards(const fw_fs_t* fs)
{
  alignas(uint64_t) uint8_t work[k_guard_work_bytes] = {};
  fw_fs_stream_port_t       port                     = fs->streams;
  fw_fs_file_t              file                     = {};
  port.caps.file_workspace_bytes                     = (uint32_t)k_guard_need_bytes;
  port.caps.file_workspace_align                     = (uint8_t)k_guard_good_align;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_open(&port, "/alpha", k_fw_fs_open_read, &file, nullptr, k_guard_need_bytes));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 fw_fs_open(&port, "/alpha", k_fw_fs_open_read, &file, work, k_guard_none));
  port.caps.file_workspace_align = (uint8_t)k_guard_no_align;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_open(&port, "/alpha", k_fw_fs_open_read, &file, work, k_guard_need_bytes));
  port.caps.file_workspace_align = (uint8_t)k_guard_odd_align;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_open(&port, "/alpha", k_fw_fs_open_read, &file, work, k_guard_need_bytes));
  port.caps.file_workspace_align = (uint8_t)k_guard_good_align;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    fw_fs_open(&port, "/alpha", k_fw_fs_open_read, &file, &work[k_guard_skew], k_guard_need_bytes));
}

/** @brief Prove open, caps, and listing refuse unusable ports and bounds. @details Implements the bounded open guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_open_guards(const fw_fs_t* fs)
{
  alignas(uint64_t) uint8_t work[k_guard_work_bytes] = {};
  const fw_fs_stream_port_t unbound                  = {};
  fw_fs_file_t              file                     = {};
  fw_fs_t                   blank                    = {};
  fw_fs_caps_t              caps                     = {};
  uint32_t                  count                    = 0U;
  bool                      complete                 = false;
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    fw_fs_open(&unbound, "/alpha", k_fw_fs_open_read, &file, work, k_guard_work_bytes));
  file.is_open = true;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    fw_fs_open(&fs->streams, "/alpha", k_fw_fs_open_read, &file, work, k_guard_work_bytes));
  file.is_open = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_open(&fs->streams,
                            "/alpha",
                            (fw_fs_open_mode_t)k_guard_bad_mode,
                            &file,
                            work,
                            k_guard_work_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_open(&fs->streams, "/", k_fw_fs_open_read, &file, work, k_guard_work_bytes));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_fs_get_caps(&blank, &caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_stat(&fs->names, "/alpha", nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    fw_fs_listdir(&fs->names, "/", k_guard_none, internal_capture, nullptr, &count, &complete));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_listdir(&fs->names,
                               "relative",
                               k_guard_one,
                               internal_capture,
                               nullptr,
                               &count,
                               &complete));
  file = (fw_fs_file_t){.iface = fs->streams.iface, .ctx = fs->streams.ctx, .is_open = true};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_tell(&file, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_file_size(&file, nullptr));
}

/** @brief Prove namespace policy fires before any backend operation runs. @details Implements the bounded namespace policy guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_namespace_policy_guards(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t iface = *fs->names.iface;
  fw_fs_namespace_t       port  = fs->names;
  fw_fs_space_t           space = {};
  iface.mkdir                   = nullptr;
  iface.space                   = nullptr;
  port.iface                    = &iface;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_mkdir(&port, "/x"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_mkdir(&fs->names, "relative"));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_mkdir(&fs->names, "/"));
  port.caps.flags |= (uint32_t)k_fw_fs_cap_space_query;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_space(&port, &space));
  port.iface = fs->names.iface;
  port.caps.flags &= ~(uint32_t)k_fw_fs_cap_space_query;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_space(&port, &space));
  port.caps.flags &= ~(uint32_t)k_fw_fs_cap_atomic_replace;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_rename(&port, "/a", "/b", true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_rename(&fs->names, "relative", "/b", false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_rename(&fs->names, "/a", "relative", false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_space(&fs->names, nullptr));
}

/** @brief Prove staged publication refuses unsupported and malformed starts. @details Implements the bounded transaction begin guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_transaction_begin_guards(const fw_fs_t* fs)
{
  alignas(uint64_t) uint8_t work[k_guard_work_bytes] = {};
  fw_fs_transaction_port_t  port                     = fs->transactions;
  fw_fs_transaction_t       txn                      = {};
  port.caps.flags &= ~(uint32_t)k_fw_fs_cap_transactions;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_fs_transaction_begin(&port,
                                         "/stage.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work,
                                         k_guard_work_bytes));
  txn.active = true;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/stage.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work,
                                         k_guard_work_bytes));
  txn.active = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/stage.bin",
                                         (fw_fs_transaction_policy_t)k_guard_bad_policy,
                                         &txn,
                                         work,
                                         k_guard_work_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "relative",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work,
                                         k_guard_work_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work,
                                         k_guard_work_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/stage.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         nullptr,
                                         k_guard_work_bytes));
}

/** @brief Prove validation and publication respect their required ordering. @details Implements the bounded transaction state guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_transaction_state_guards(const fw_fs_t* fs)
{
  fw_fs_transaction_t txn       = {.iface  = fs->transactions.iface,
                                   .ctx    = fs->transactions.ctx,
                                   .active = true};
  bool                published = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_transaction_validate(&txn, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_transaction_commit(&txn, nullptr));
  txn.validated = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_validate(&txn, internal_guard_validator, nullptr));
}

/** @brief Prove the binder refuses contradictory capability records. @details Implements the bounded bind guard fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_bind_guards(const fw_fs_t* fs)
{
  fw_fs_t      bound = {};
  fw_fs_caps_t caps  = fs->caps;
  caps.flags &= ~((uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
  caps                      = fs->caps;
  caps.file_workspace_align = (uint8_t)k_guard_no_align;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
  caps                             = fs->caps;
  caps.transaction_workspace_align = (uint8_t)k_guard_odd_align;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
  caps                = fs->caps;
  caps.path_max_bytes = (uint16_t)k_guard_short_path;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
}

/** @brief Run the portable facade guard matrix over a confined POSIX root. @details Runs the facade guard vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_test_facade_guards(void)
{
  posix_fixture_t fixture = {};
  internal_fixture_init(&fixture);
  TEST_BEGIN("fw_if_fs facade guards");
  internal_test_workspace_guards(&fixture.fs);
  internal_test_open_guards(&fixture.fs);
  internal_test_namespace_policy_guards(&fixture.fs);
  internal_test_transaction_begin_guards(&fixture.fs);
  internal_test_transaction_state_guards(&fixture.fs);
  internal_test_bind_guards(&fixture.fs);
  internal_fixture_deinit(&fixture);
  TEST_END("fw_if_fs facade guards");
}

int main(void)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_posix_test_set_directory_reader(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_posix_test_set_directory_reader(internal_script_read, nullptr));
  internal_test_path_grammar_guards();
  internal_test_namespace_handle_guards();
  internal_test_stream_handle_guards();
  internal_test_transaction_handle_guards();
  internal_test_facade_guards();
  internal_test_native_directory();
  internal_test_empty_mixed_interrupted();
  internal_test_callback_and_entry_bounds();
  internal_test_raw_failures();
#ifdef RA8_POSIX_CLOSE_WRAP_TEST
  internal_test_alias_parent_close_failure();
  internal_test_close_preserve_precedence();
#endif
  return 0;
}
#else
int main(void)
{
  return 0;
}
#endif
