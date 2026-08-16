/**
 * @file fw_if_fs_cursor_test.c
 * @brief Shared incremental-directory conformance vectors.
 * @details Exercises independent cursor lifetimes, nested namespace calls,
 * stable copied entries, capacity reporting, and close/error semantics.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fw_if_fs_cursor_test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs_backend.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Fixed opaque workspace extents used by both test backends. */
typedef enum : uint32_t {
  k_cursor_test_directory_work = 8192U, /**< Cursor backend workspace. */
  k_cursor_test_file_work      = 64U,   /**< File backend workspace.   */
} cursor_test_limit_t;

/** @brief Maximally aligned directory workspace. */
typedef union {
  max_align_t alignment;                           /**< Force maximum alignment. */
  uint8_t     bytes[k_cursor_test_directory_work]; /**< Opaque cursor state.     */
} cursor_test_directory_work_t;

/** @brief Maximally aligned file workspace. */
typedef union {
  max_align_t alignment;                      /**< Force maximum alignment. */
  uint8_t     bytes[k_cursor_test_file_work]; /**< Opaque file state.       */
} cursor_test_file_work_t;

/** @brief Nested-stat callback state. */
typedef struct {
  const fw_fs_t* fs;    /**< Filesystem queried from callback. */
  uint32_t       count; /**< Successfully validated entries.   */
} cursor_test_nested_t;

/** @brief Caller-owned response for synthetic cursor faults. */
typedef struct {
  fw_fs_dirent_value_t entry;   /**< Candidate backend value. */
  ra8_err_t            status;  /**< Backend callback status. */
  bool                 present; /**< Backend presence result. */
} cursor_test_fault_t;

/**
 * @brief Return one configured directory-cursor result.
 * @details Copies status, presence, and candidate value independently.
 * @param[in,out] ctx Caller-owned ::cursor_test_fault_t configuration.
 * @param[in,out] state Unused cursor workspace.
 * @param[out] out Candidate value returned to the facade.
 * @param[out] out_entry Configured presence result.
 * @return Configured callback status.
 * @retval k_ra8_ok The configured successful status was returned.
 * @retval k_ra8_fail The configured failure status was returned.
 * @pre @p ctx, @p out, and @p out_entry designate live objects.
 * @pre The synthetic directory handle is open.
 * @post Outputs equal their configured values.
 * @post No configuration or filesystem state is changed.
 * @note Test-only injected backend seam.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cursor_fault_next(void* ctx, void* state, fw_fs_dirent_value_t* out, bool* out_entry)
{
  (void)state;
  const cursor_test_fault_t* fault = (const cursor_test_fault_t*)ctx;
  *out                             = fault->entry;
  *out_entry                       = fault->present;
  return fault->status;
}

/**
 * @brief Join the cursor fixture root with one bounded directory-entry name.
 * @details Builds the path without a formatted-I/O dependency.
 * @param[out] path Writable destination buffer.
 * @param[in] capacity Writable bytes in @p path.
 * @param[in] name Directory-entry name bytes.
 * @param[in] name_capacity Readable bound of @p name including its terminator.
 * @return True when the complete path and terminator fit.
 * @retval false An argument is NULL or the bounded name does not fit.
 * @pre @p name spans @p name_capacity readable bytes.
 * @pre @p path spans @p capacity writable bytes.
 * @post On success @p path contains `/cursor/` followed by @p name.
 * @post On failure @p path is unchanged.
 * @note File-local helper; no ownership escapes the test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
internal_cursor_path(char* path, size_t capacity, const char* name, size_t name_capacity)
{
  const char   k_prefix[] = "/cursor/";
  const size_t prefix_len = sizeof(k_prefix) - 1U;
  if ((path == nullptr) || (name == nullptr)) {
    return false;
  }
  size_t name_len = 0U;
  while ((name_len < name_capacity) && (name[name_len] != '\0')) {
    ++name_len;
  }
  if ((name_len == name_capacity) || (prefix_len + name_len + 1U > capacity)) {
    return false;
  }
  memcpy(path, k_prefix, prefix_len);
  memcpy(&path[prefix_len], name, name_len + 1U);
  return true;
}

/**
 * @brief Create one single-byte cursor fixture file.
 * @details Opens, writes, and closes the fixture through the portable facade.
 * @param[in] fs Initialized filesystem facade.
 * @pre @p fs is non-NULL and `/cursor` exists.
 * @pre The facade supports a file workspace no larger than the local fixture.
 * @post `/cursor/beta` contains exactly one byte on success.
 * @post No file handle or workspace ownership escapes.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_write(const fw_fs_t* fs)
{
  const uint8_t           k_value[] = {0xA5U};
  cursor_test_file_work_t work      = {};
  fw_fs_file_t            file      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            "/cursor/beta",
                            k_fw_fs_open_write_truncate,
                            &file,
                            work.bytes,
                            sizeof(work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, k_value, sizeof(k_value), &written));
  TEST_ASSERT_EQ(sizeof(k_value), written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/**
 * @brief Stat each callback entry to prove no backend lock spans delivery.
 * @details Re-enters the namespace facade from the list callback.
 * @param[in,out] ctx Bound ::cursor_test_nested_t state.
 * @param[in] entry Current copied directory entry.
 * @param[out] out_continue Set true after a matching nested stat.
 * @return RA8 status from path construction or the nested stat.
 * @retval k_ra8_ok The entry matched its nested stat result.
 * @retval k_ra8_err_invalid_size The bounded fixture path did not fit.
 * @retval k_ra8_err_invalid_state The stat result disagreed with @p entry.
 * @pre All pointers satisfy the list callback contract.
 * @pre The bound filesystem remains initialized throughout delivery.
 * @post Success increments the bound count exactly once.
 * @post No entry or filesystem storage is retained.
 * @note Implements the injected callback seam under test.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cursor_nested(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  cursor_test_nested_t* nested = (cursor_test_nested_t*)ctx;
  char                  path[k_fw_fs_path_cap];
  if (!internal_cursor_path(path, sizeof(path), entry->name, sizeof(entry->name))) {
    return k_ra8_err_invalid_size;
  }
  fw_fs_stat_t    stat = {};
  const ra8_err_t err  = fw_fs_stat(&nested->fs->names, path, &stat);
  if ((err != k_ra8_ok) || !stat.exists || (stat.type != entry->type)) {
    return (err == k_ra8_ok) ? k_ra8_err_invalid_state : err;
  }
  ++nested->count;
  *out_continue = true;
  return k_ra8_ok;
}

/**
 * @brief Reset a synthetic cursor result to one coherent file entry.
 * @details Produces the common all-false predicate vector used by fault cases.
 * @param[out] fault Caller-owned synthetic backend result.
 * @pre @p fault designates writable storage.
 * @pre The fixed name fits ::fw_fs_dirent_value_t.
 * @post @p fault returns file `ok`, status success, and presence true.
 * @post No filesystem or external state is changed.
 * @note Test-only fixture initializer.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_fault_reset(cursor_test_fault_t* fault)
{
  *fault = (cursor_test_fault_t){
    .entry   = {.name = "ok", .name_bytes = 2U, .type = k_fw_fs_node_file, .size_bytes = 1U},
    .status  = k_ra8_ok,
    .present = true};
}

/**
 * @brief Bind one open facade around the synthetic cursor callback.
 * @details Copies the truthful namespace table before replacing only dir-next.
 * @param[in] fs Truthful baseline filesystem binding.
 * @param[in,out] fault Caller-owned callback configuration.
 * @param[out] iface Mutable namespace table copy.
 * @param[out] directory Open synthetic directory facade.
 * @pre All pointers are non-null and remain live through the vector.
 * @pre The baseline namespace table is complete.
 * @post @p directory dispatches next calls through @p fault.
 * @post The baseline filesystem binding remains unchanged.
 * @note No real backend operation is invoked by the synthetic handle.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_fault_bind(const fw_fs_t*           fs,
                                                    cursor_test_fault_t*     fault,
                                                    fw_fs_namespace_iface_t* iface,
                                                    fw_fs_dir_t*             directory)
{
  *iface          = *fs->names.iface;
  iface->dir_next = internal_cursor_fault_next;
  *directory =
    (fw_fs_dir_t){.iface = iface, .ctx = fault, .state = fault, .caps = fs->caps, .is_open = true};
}

/**
 * @brief Assert one synthetic cursor-next outcome and safe output state.
 * @details Dispatches a configured result and checks publication or clearing.
 * @param[in,out] directory Open synthetic directory facade.
 * @param[in] expected_status Expected facade status.
 * @param[in] expected_present Expected published-entry flag.
 * @pre @p directory is bound through ::internal_cursor_fault_bind.
 * @pre The configured candidate is fully initialized.
 * @post Success/presence publishes the configured value.
 * @post Every other result leaves a cleared public value.
 * @note Assertions terminate the test on mismatch.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_cursor_expect(fw_fs_dir_t* directory, ra8_err_t expected_status, bool expected_present)
{
  fw_fs_dirent_value_t output  = {.name = "stale", .name_bytes = 5U};
  bool                 present = true;
  TEST_ASSERT_EQ(expected_status, fw_fs_dir_next(directory, &output, &present));
  TEST_ASSERT_EQ(expected_present, present);
  if (!expected_present) {
    TEST_ASSERT_EQ(0U, output.name_bytes);
    TEST_ASSERT_EQ(k_fw_fs_node_none, output.type);
  }
}

/**
 * @brief Toggle both cursor-workspace alignment predicates.
 * @details Uses one aligned full-size workspace with zero, non-power-two, and
 * truthful alignment metadata while preserving all other inputs.
 * @param[in] fs Truthful baseline filesystem binding.
 * @pre The advertised directory workspace fits the local fixture.
 * @pre The baseline alignment is a non-zero power of two.
 * @post Both invalid alignment operands report invalid state.
 * @post The real backend never observes either rejected workspace.
 * @note The normal cursor-open vector supplies the all-false result.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_workspace_mcdc(const fw_fs_t* fs)
{
  cursor_test_directory_work_t work      = {};
  fw_fs_dir_t                  directory = {};
  fw_fs_namespace_t            names     = fs->names;
  names.caps.directory_workspace_align   = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_dir_open(&names, "/", &directory, work.bytes, sizeof(work.bytes)));
  names.caps.directory_workspace_align = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_dir_open(&names, "/", &directory, work.bytes, sizeof(work.bytes)));
}

/**
 * @brief Exercise every bounded cursor-name predicate.
 * @details Isolates empty, over-limit, path-cap, suffix, and embedded-NUL
 * candidates against the same coherent file baseline.
 * @param[in] fs Truthful baseline filesystem binding.
 * @pre The baseline name limit accepts the fixed two-byte name.
 * @pre Synthetic callback storage remains live for every dispatch.
 * @post The coherent name is accepted and every malformed name is rejected.
 * @post Rejected candidates never escape through the public output.
 * @note Bad suffix and embedded NUL vectors cover the coupled terminator test.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_name_mcdc(const fw_fs_t* fs)
{
  cursor_test_fault_t     fault     = {};
  fw_fs_namespace_iface_t iface     = {};
  fw_fs_dir_t             directory = {};
  internal_cursor_fault_reset(&fault);
  internal_cursor_fault_bind(fs, &fault, &iface, &directory);
  internal_cursor_expect(&directory, k_ra8_ok, true);
  fault.entry.name_bytes = 0U;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  internal_cursor_fault_reset(&fault);
  directory.caps.name_max_bytes = 1U;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  internal_cursor_fault_reset(&fault);
  directory.caps.name_max_bytes = (uint16_t)k_fw_fs_path_cap;
  fault.entry.name_bytes        = (uint16_t)k_fw_fs_path_cap;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  internal_cursor_fault_reset(&fault);
  directory.caps      = fs->caps;
  fault.entry.name[2] = 'x';
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  internal_cursor_fault_reset(&fault);
  fault.entry.name[1] = '\0';
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
}

/**
 * @brief Exercise every cursor node-type predicate.
 * @details Isolates none, out-of-range, and sized-directory contradictions
 * while retaining coherent file and empty-directory false vectors.
 * @param[in] fs Truthful baseline filesystem binding.
 * @pre Synthetic callback storage remains live for every dispatch.
 * @pre The fixed name satisfies the portable leaf policy.
 * @post Coherent file/directory values pass and each bad type is rejected.
 * @post Rejected candidates never escape through the public output.
 * @note Supplies the N+1 vectors for the aggregate type decision.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_type_mcdc(const fw_fs_t* fs)
{
  cursor_test_fault_t     fault     = {};
  fw_fs_namespace_iface_t iface     = {};
  fw_fs_dir_t             directory = {};
  internal_cursor_fault_reset(&fault);
  internal_cursor_fault_bind(fs, &fault, &iface, &directory);
  fault.entry.type = k_fw_fs_node_none;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  fault.entry.type = (fw_fs_node_type_t)UINT32_MAX;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  fault.entry.type       = k_fw_fs_node_directory;
  fault.entry.size_bytes = 1U;
  internal_cursor_expect(&directory, k_ra8_err_invalid_state, false);
  fault.entry.size_bytes = 0U;
  internal_cursor_expect(&directory, k_ra8_ok, true);
  internal_cursor_fault_reset(&fault);
  internal_cursor_expect(&directory, k_ra8_ok, true);
}

/**
 * @brief Toggle cursor-next null, status, and presence operands.
 * @details Supplies one-null-at-a-time output tuples plus success/present,
 * success/absent, and failure/present backend results.
 * @param[in] fs Truthful baseline filesystem binding.
 * @pre Synthetic callback storage remains live for every dispatch.
 * @pre The configured candidate is coherent.
 * @post Null outputs fail before dispatch.
 * @post Status and presence independently control publication.
 * @note Supplies N+1 vectors for both public next decisions.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_next_mcdc(const fw_fs_t* fs)
{
  cursor_test_fault_t     fault     = {};
  fw_fs_namespace_iface_t iface     = {};
  fw_fs_dir_t             directory = {};
  internal_cursor_fault_reset(&fault);
  internal_cursor_fault_bind(fs, &fault, &iface, &directory);
  fw_fs_dirent_value_t output  = {};
  bool                 present = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_dir_next(&directory, nullptr, &present));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_dir_next(&directory, &output, nullptr));
  internal_cursor_expect(&directory, k_ra8_ok, true);
  fault.present = false;
  internal_cursor_expect(&directory, k_ra8_ok, false);
  fault.status  = k_ra8_fail;
  fault.present = true;
  internal_cursor_expect(&directory, k_ra8_fail, false);
}

/**
 * @brief Open two independent cursors after exact workspace rejection.
 * @details Proves capacity-minus-one fails before binding both exact workspaces.
 * @param[in] fs Initialized filesystem facade.
 * @param[in,out] first_work First caller-owned cursor workspace.
 * @param[in,out] second_work Second caller-owned cursor workspace.
 * @param[out] first First cursor binding.
 * @param[out] second Independent second cursor binding.
 * @pre `/cursor` exists and all pointers are non-NULL.
 * @pre Each workspace meets the facade's advertised capacity.
 * @post Both cursors are open on `/cursor`.
 * @post The rejected undersized attempt retains no backend state.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_open_pair(const fw_fs_t*                fs,
                                                   cursor_test_directory_work_t* first_work,
                                                   cursor_test_directory_work_t* second_work,
                                                   fw_fs_dir_t*                  first,
                                                   fw_fs_dir_t*                  second)
{
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 fw_fs_dir_open(&fs->names,
                                "/cursor",
                                first,
                                first_work->bytes,
                                fs->caps.directory_workspace_bytes - 1U));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_dir_open(&fs->names, "/cursor", first, first_work->bytes, sizeof(first_work->bytes)));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_dir_open(&fs->names, "/cursor", second, second_work->bytes, sizeof(second_work->bytes)));
}

/**
 * @brief Advance two cursors in lockstep with a nested stat between calls.
 * @details Confirms copied names remain stable and close invalidates a cursor.
 * @param[in] fs Initialized filesystem facade.
 * @param[in,out] first Open first cursor.
 * @param[in,out] second Open independent second cursor.
 * @pre Both cursors address the same two-entry fixture directory.
 * @pre @p fs remains initialized throughout traversal.
 * @post Both cursors are consumed and closed.
 * @post A next call on the closed first cursor reports invalid state.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_cursor_walk_pair(const fw_fs_t* fs, fw_fs_dir_t* first, fw_fs_dir_t* second)
{
  fw_fs_dirent_value_t first_entry    = {};
  fw_fs_dirent_value_t second_entry   = {};
  bool                 first_present  = false;
  bool                 second_present = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(first, &first_entry, &first_present));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(second, &second_entry, &second_present));
  TEST_ASSERT(first_present && second_present);
  TEST_ASSERT(strcmp(first_entry.name, second_entry.name) == 0);
  char path[k_fw_fs_path_cap];
  TEST_ASSERT(internal_cursor_path(path, sizeof(path), first_entry.name, sizeof(first_entry.name)));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, path, &stat));
  TEST_ASSERT(stat.exists && (stat.type == first_entry.type));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(first, &first_entry, &first_present));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(second, &second_entry, &second_present));
  TEST_ASSERT(first_present && second_present);
  TEST_ASSERT(strcmp(first_entry.name, second_entry.name) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(first, &first_entry, &first_present));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(second, &second_entry, &second_present));
  TEST_ASSERT(!first_present && !second_present);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_close(first));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_close(second));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_dir_next(first, &first_entry, &first_present));
}

/**
 * @brief Prove callback list permits nested stat and later mutation.
 * @details Traverses through the callback facade, then removes every fixture.
 * @param[in] fs Initialized filesystem facade.
 * @pre `/cursor` contains the two expected entries.
 * @pre @p fs supports callback listing and namespace mutation.
 * @post Both entries and `/cursor` are removed.
 * @post The callback observes exactly two matching nested stats.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_cursor_callback_and_cleanup(const fw_fs_t* fs)
{
  cursor_test_nested_t nested   = {.fs = fs};
  uint32_t             count    = 0U;
  bool                 complete = false;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_listdir(&fs->names, "/cursor", 2U, internal_cursor_nested, &nested, &count, &complete));
  TEST_ASSERT(complete && (count == 2U) && (nested.count == 2U));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/cursor/beta"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/cursor/alpha"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/cursor"));
}

/**
 * @brief Exercise independent cursors and lock-free nested namespace calls.
 * @details Creates two entries, traverses them through independent cursor and
 * callback APIs, and removes every fixture afterward.
 * @param[in] fs Initialized portable filesystem facade with an empty root.
 * @pre @p fs is non-NULL and supports files and directories.
 * @pre Its advertised workspaces fit the bounded local fixtures.
 * @post All cursor fixtures are removed and both cursors are consumed.
 * @post No backend handle or caller workspace ownership escapes.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER void fw_if_fs_test_directory_cursors(const fw_fs_t* fs)
{
  TEST_ASSERT(fs->caps.directory_workspace_bytes <= (uint32_t)k_cursor_test_directory_work);
  TEST_ASSERT(fs->caps.directory_workspace_align != 0U);
  TEST_ASSERT((fs->caps.directory_workspace_align & (fs->caps.directory_workspace_align - 1U)) ==
              0U);
  internal_cursor_workspace_mcdc(fs);
  internal_cursor_name_mcdc(fs);
  internal_cursor_type_mcdc(fs);
  internal_cursor_next_mcdc(fs);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs->names, "/cursor"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs->names, "/cursor/alpha"));
  internal_cursor_write(fs);
  cursor_test_directory_work_t first_work  = {};
  cursor_test_directory_work_t second_work = {};
  fw_fs_dir_t                  first       = {};
  fw_fs_dir_t                  second      = {};
  internal_cursor_open_pair(fs, &first_work, &second_work, &first, &second);
  internal_cursor_walk_pair(fs, &first, &second);
  internal_cursor_callback_and_cleanup(fs);
}
