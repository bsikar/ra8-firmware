/**
 * @file test_ra8_mdl_library.c
 * @brief Portable tracked-library enumeration and removal qualification.
 * @details Runs identical authentication, traversal-bound, and injected-fault
 *          vectors over POSIX and the real RAM blockdev to FAT to VFS stack.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "mdl_library.h"
#include "mdl_state_fs_fault.h"
#include "mdl_storage.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Fixed disk and portable-workspace capacities. */
typedef enum : uint32_t {
  k_library_disk_blocks    = 4096U, /**< RAM disk block count.           */
  k_library_work_bytes     = 8192U, /**< Backend-handle workspace bytes. */
  k_library_io_bytes       = 512U,  /**< State stream scratch bytes.     */
  k_library_entry_capacity = 16U,   /**< Explicit test entry ceiling.    */
} mdl_library_test_limit_t;

/** @brief Maximally aligned generic backend workspace. */
typedef union {
  max_align_t alignment;                   /**< Force maximum alignment. */
  uint8_t     bytes[k_library_work_bytes]; /**< Backend workspace bytes. */
} mdl_library_test_workspace_t;

/** @brief Enumeration callback observations and requested behavior. */
typedef struct {
  uint32_t  calls;      /**< Number of authenticated callbacks. */
  bool      saw_alpha;  /**< Alpha authenticated successfully.  */
  bool      saw_beta;   /**< Beta authenticated successfully.   */
  bool      stop_first; /**< Request clean stop after one call. */
  ra8_err_t error;      /**< Error returned by the callback.    */
} mdl_library_visit_t;

static uint8_t s_disk[(size_t)k_library_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t  s_ram_state;
static ra8_io_blockdev_t            s_blockdev;
static ra8_fs_backend_t             s_backend;
static ra8_fs_mount_t*              s_mount;
static mdl_library_test_workspace_t s_file_work;
static mdl_library_test_workspace_t s_transaction_work;
static uint8_t                      s_io[k_library_io_bytes];
static mdl_state_t                  s_state;
static mdl_state_t                  s_loaded;
static mdl_library_workspace_t      s_library_workspace;
static mdl_library_test_workspace_t s_directory_work;

/**
 * @brief Discard expected host-test log bytes.
 * @details Prevents expected rejection vectors from touching target-only ITM registers on a host.
 * @param[in] ctx Unused logger context.
 * @param[in] byte Unused emitted byte.
 * @pre Installed only for this single-threaded test process.
 * @pre Both arguments may contain arbitrary values.
 * @post No state is modified.
 * @post The byte is intentionally discarded.
 * @note Thread-safe because it performs no operation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Bind one backend through the deterministic fault facade.
 * @details Composes storage and traversal workspaces over the supplied real filesystem backend.
 * @param[in] inner Real portable filesystem backend.
 * @param[out] fault Fault wrapper initialized with no armed faults.
 * @param[out] storage Downloader storage binding.
 * @pre Required pointers are non-NULL and shared workspaces are idle.
 * @pre @p inner exposes namespace, stream, and transaction contracts.
 * @post The storage binding is ready without allocated memory.
 * @post No backend handle remains open.
 * @note Unity assertions terminate the active vector on failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_library_bind(const fw_fs_t* inner, mdl_state_fault_fs_t* fault, mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_fault_fs_init(fault, inner));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(storage,
                                  &fault->fs,
                                  s_file_work.bytes,
                                  sizeof(s_file_work.bytes),
                                  s_transaction_work.bytes,
                                  sizeof(s_transaction_work.bytes),
                                  s_io,
                                  sizeof(s_io)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_workspace_init(&s_library_workspace,
                                            s_directory_work.bytes,
                                            (uint32_t)sizeof(s_directory_work.bytes)));
}

/**
 * @brief Replace one portable file with exact fixture bytes.
 * @details Drives the injected stream facade until every requested fixture byte has been written.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Canonical destination path.
 * @param[in] bytes Fixture bytes.
 * @param[in] length Exact byte count.
 * @pre Required pointers are non-NULL and @p bytes covers @p length bytes.
 * @pre No other stream borrows the shared file workspace.
 * @post Exactly @p length bytes are present at @p path.
 * @post The stream is closed before return.
 * @note Short writes are retried.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_write(mdl_storage_t* storage,
                                                const char*    path,
                                                const uint8_t* bytes,
                                                uint32_t       length)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            path,
                            k_fw_fs_open_write_truncate,
                            &file,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint32_t offset = 0U;
  while (offset < length) {
    uint32_t count = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, bytes + offset, length - offset, &count));
    TEST_ASSERT(count > 0U);
    offset += count;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/**
 * @brief Publish one authenticated tracked-state generation.
 * @details Initializes a bounded state record and requires its transactional publication flag.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] state_path Canonical logical marker path.
 * @param[in] title Deterministic series title and identity selector.
 * @pre Required pointers are non-NULL and the containing directory exists.
 * @pre @p title fits the bounded state schema.
 * @post One authenticated generation is visible.
 * @post The shared state model contains the published identity.
 * @note Repeated calls publish alternating monotonically ordered generations.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_library_save(mdl_storage_t* storage, const char* state_path, const char* title)
{
  mdl_state_init(&s_state);
  mdl_state_set_series(&s_state, "https://example.test/series", title, "site", title, "/site.cfg");
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_save(storage, state_path, &s_state, &published));
  TEST_ASSERT(published);
}

/**
 * @brief Observe one authenticated series or request deterministic termination.
 * @details Records discovered titles while preserving callback cancellation and error semantics.
 * @param[in] series_dir Canonical series directory.
 * @param[in] state_path Canonical logical state path.
 * @param[in] state Authenticated state generation.
 * @param[in,out] ctx Mutable ::mdl_library_visit_t.
 * @param[out] out_continue Callback continuation choice.
 * @return Configured callback status.
 * @retval k_ra8_ok The observation completed.
 * @pre Required pointers are non-NULL and @p ctx has the documented type.
 * @pre The state and paths remain borrowed only for the call.
 * @post One call is recorded when no configured error is returned first.
 * @post Success initializes @p out_continue.
 * @note The callback never modifies the filesystem.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_visit(const char*        series_dir,
                                                     const char*        state_path,
                                                     const mdl_state_t* state,
                                                     void*              ctx,
                                                     bool*              out_continue)
{
  mdl_library_visit_t* visit = (mdl_library_visit_t*)ctx;
  (void)series_dir;
  (void)state_path;
  if (visit->error != k_ra8_ok) {
    return visit->error;
  }
  ++visit->calls;
  visit->saw_alpha = visit->saw_alpha || (strcmp(state->series_title, "alpha") == 0);
  visit->saw_beta  = visit->saw_beta || (strcmp(state->series_title, "beta") == 0);
  *out_continue    = !(visit->stop_first && (visit->calls == 1U));
  return k_ra8_ok;
}

/**
 * @brief Enumerate with explicit policy and a fresh observation context.
 * @details Routes one library walk through the shared authenticated-state and cursor workspaces.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] root Canonical library path.
 * @param[in] policy Explicit traversal limits.
 * @param[in,out] visit Callback behavior and observations.
 * @return Library enumeration status.
 * @retval k_ra8_ok Enumeration completed or stopped through the callback policy.
 * @retval other A storage, authentication, capacity, or callback error stopped enumeration.
 * @pre Required pointers are non-NULL.
 * @pre Shared state scratch is idle.
 * @post @p visit records every callback completed before return.
 * @post No namespace object is modified.
 * @note This helper preserves exact production callback semantics.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_library_enumerate(mdl_storage_t*              storage,
                                                         const char*                 root,
                                                         const mdl_library_policy_t* policy,
                                                         mdl_library_visit_t*        visit)
{
  return mdl_library_for_each(storage,
                              root,
                              &s_loaded,
                              &s_library_workspace,
                              policy,
                              internal_library_visit,
                              visit);
}

/**
 * @brief Assert a path's portable existence state.
 * @details Uses only the injected namespace stat contract to compare actual and expected presence.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Canonical path.
 * @param[in] expected Expected existence flag.
 * @pre Required pointers are non-NULL.
 * @pre No namespace fault is armed.
 * @post The stat operation succeeded and matched @p expected.
 * @post No namespace object is modified.
 * @note Node type is intentionally irrelevant to this helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_library_expect_exists(mdl_storage_t* storage, const char* path, bool expected)
{
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&storage->fs->names, path, &stat));
  TEST_ASSERT(stat.exists == expected);
}

/**
 * @brief Exercise absent, empty, mixed, authenticated, and callback outcomes.
 * @details Builds a mixed portable library and proves counting, iteration
 * order, early-stop, and callback-error propagation.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @pre @p storage is non-NULL and the vector paths are absent.
 * @pre No deterministic fault is armed.
 * @post Valid base/alternate generations are enumerated exactly once.
 * @post No corrupt-shape vector runs in this helper.
 * @note Runs identically on both real backends.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_enumeration_absent_and_mixed(mdl_storage_t* storage)
{
  mdl_library_policy_t policy = mdl_library_policy_default();
  policy.max_entries          = (uint32_t)k_library_entry_capacity;
  mdl_library_visit_t visit   = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_library_enumerate(storage, "/absent", &policy, &visit));
  TEST_ASSERT_EQ(0U, visit.calls);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/empty"));
  TEST_ASSERT_EQ(k_ra8_ok, internal_library_enumerate(storage, "/empty", &policy, &visit));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/mixed"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/mixed/untracked"));
  internal_library_write(storage, "/mixed/plain", (const uint8_t*)"x", 1U);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/mixed/alpha"));
  internal_library_save(storage, "/mixed/alpha/.mdl_state", "alpha");
  internal_library_save(storage, "/mixed/alpha/.mdl_state", "alpha");
  internal_library_write(storage, "/mixed/alpha/.mdl_state", (const uint8_t*)"broken", 6U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_state_load_authenticated(storage, "/mixed/alpha/.mdl_state", &s_loaded));
  TEST_ASSERT(strcmp(s_loaded.series_title, "alpha") == 0);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/mixed/beta"));
  internal_library_save(storage, "/mixed/beta/.mdl_state", "beta");

  visit = (mdl_library_visit_t){};
  TEST_ASSERT_EQ(k_ra8_ok, internal_library_enumerate(storage, "/mixed", &policy, &visit));
  TEST_ASSERT_EQ(2U, visit.calls);
  TEST_ASSERT(visit.saw_alpha);
  TEST_ASSERT(visit.saw_beta);
  visit = (mdl_library_visit_t){.stop_first = true};
  TEST_ASSERT_EQ(k_ra8_ok, internal_library_enumerate(storage, "/mixed", &policy, &visit));
  TEST_ASSERT_EQ(1U, visit.calls);
  visit = (mdl_library_visit_t){.error = k_ra8_err_cancelled};
  TEST_ASSERT_EQ(k_ra8_err_cancelled,
                 internal_library_enumerate(storage, "/mixed", &policy, &visit));
}

/**
 * @brief Exercise corrupt, legacy, marker-directory, and non-directory shapes.
 * @details Each vector proves one non-portable on-disk shape fails closed
 * with the documented error instead of being silently skipped or accepted.
 * @param[in,out] storage Initialized fault-wrapped storage; the `/mixed`
 * fixture from a prior enumeration call may already exist on it.
 * @pre @p storage is non-NULL.
 * @pre No deterministic fault is armed.
 * @post Every vectored bad shape returns its documented error.
 * @post No enumeration observes a partial or corrupted entry as valid.
 * @note Runs identically on both real backends.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_enumeration_bad_shapes(mdl_storage_t* storage)
{
  mdl_library_policy_t policy = mdl_library_policy_default();
  policy.max_entries          = (uint32_t)k_library_entry_capacity;
  mdl_library_visit_t visit   = {};

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/corrupt"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/corrupt/item"));
  internal_library_write(storage, "/corrupt/item/.mdl_state", (const uint8_t*)"broken", 6U);
  visit = (mdl_library_visit_t){};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 internal_library_enumerate(storage, "/corrupt", &policy, &visit));
  static const uint8_t legacy[] =
    "MDLSTATE\t2\nseries\thttps://old\tlegacy\tsite\tslug\t/site.cfg\n";
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/legacy"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/legacy/item"));
  internal_library_write(storage,
                         "/legacy/item/.mdl_state",
                         legacy,
                         (uint32_t)(sizeof(legacy) - 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 internal_library_enumerate(storage, "/legacy", &policy, &visit));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/marker-dir"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/marker-dir/item"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/marker-dir/item/.mdl_state"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 internal_library_enumerate(storage, "/marker-dir", &policy, &visit));
  internal_library_write(storage, "/root-file", (const uint8_t*)"x", 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_library_enumerate(storage, "/root-file", &policy, &visit));
}

/**
 * @brief Exercise absent, empty, mixed, authenticated, and callback outcomes.
 * @details Builds a mixed portable library and proves authenticated enumeration fails closed.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @pre @p storage is non-NULL and the vector paths are absent.
 * @pre No deterministic fault is armed.
 * @post Valid base/alternate generations are enumerated exactly once.
 * @post Corrupt, legacy, and non-file markers fail closed.
 * @note Runs identically on both real backends.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_enumeration_vectors(mdl_storage_t* storage)
{
  internal_library_enumeration_absent_and_mixed(storage);
  internal_library_enumeration_bad_shapes(storage);
}

/**
 * @brief Exercise exact entry boundaries and dependency failures.
 * @details Drives capacity, namespace, media, stream-read, and close failures one at a time.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Fault facade controlling exact errors.
 * @pre Both pointers are non-NULL and enumeration fixtures already exist.
 * @pre No stream or namespace handle is live.
 * @post Exact cap succeeds while cap+1 fails closed.
 * @post Stat, list, media, read, and close errors propagate exactly.
 * @note Armed faults are cleared before return.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_enumeration_faults(mdl_storage_t*        storage,
                                                             mdl_state_fault_fs_t* fault)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/cap"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/cap/a"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/cap/b"));
  mdl_library_policy_t policy = mdl_library_policy_default();
  policy.max_entries          = 2U;
  mdl_library_visit_t visit   = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_library_enumerate(storage, "/cap", &policy, &visit));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/cap/c"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_library_enumerate(storage, "/cap", &policy, &visit));
  TEST_ASSERT_EQ(3U, s_library_workspace.required_entries);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/fault-enum"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/fault-enum/item"));
  internal_library_save(storage, "/fault-enum/item/.mdl_state", "fault");

  policy             = mdl_library_policy_default();
  policy.max_entries = (uint32_t)k_library_entry_capacity;
  fault->flags       = (uint32_t)k_mdl_state_fault_stat;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_library_enumerate(storage, "/mixed", &policy, &visit));
  fault->flags = (uint32_t)k_mdl_state_fault_dir_open;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_library_enumerate(storage, "/mixed", &policy, &visit));
  fault->flags = (uint32_t)k_mdl_state_fault_dir_next;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_library_enumerate(storage, "/mixed", &policy, &visit));
  fault->flags = (uint32_t)k_mdl_state_fault_media;
  TEST_ASSERT_EQ(k_ra8_err_hw_not_ready,
                 internal_library_enumerate(storage, "/mixed", &policy, &visit));
  fault->flags = (uint32_t)k_mdl_state_fault_read;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_library_enumerate(storage, "/fault-enum", &policy, &visit));
  fault->flags = (uint32_t)k_mdl_state_fault_close;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_library_enumerate(storage, "/cap", &policy, &visit));
  fault->flags = 0U;
}

/**
 * @brief Create one nested removal fixture with optional leaf file.
 * @details Constructs a bounded directory chain solely through portable namespace and stream calls.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] root Canonical absent root path.
 * @param[in] depth Number of child-directory levels to create.
 * @param[in] leaf_file Whether to create a file in the deepest directory.
 * @pre Required pointers are non-NULL and @p depth does not exceed three.
 * @pre @p root is absent and fits the fixed path buffer.
 * @post The complete directory chain exists.
 * @post When requested, the deepest directory contains `page.bin`.
 * @note This fixture helper uses only portable namespace and streams.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_library_make_tree(mdl_storage_t* storage, const char* root, uint16_t depth, bool leaf_file)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, root));
  char path[k_fw_fs_path_cap];
  (void)snprintf(path, sizeof(path), "%s", root);
  for (uint16_t i = 0U; i < depth; ++i) {
    const size_t used = strlen(path);
    TEST_ASSERT((used + 3U) < sizeof(path));
    (void)snprintf(path + used, sizeof(path) - used, "/d%u", (unsigned)i);
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, path));
  }
  if (leaf_file) {
    const size_t used = strlen(path);
    TEST_ASSERT((used + sizeof("/page.bin")) <= sizeof(path));
    (void)memcpy(path + used, "/page.bin", sizeof("/page.bin"));
    internal_library_write(storage, path, (const uint8_t*)"page", 4U);
  }
}

/**
 * @brief Exercise injected list, media, unlink, and rmdir delete errors.
 * @details Verifies every removal dependency error propagates before a clean retry removes the tree.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Fault facade controlling exact errors.
 * @param[in] policy Complete production removal policy.
 * @pre Required pointers are non-NULL and the fault root is absent.
 * @pre No deterministic fault is armed.
 * @post Every injected error propagated and the fixture was then removed.
 * @post Fault flags are cleared before return.
 * @note Failure assertions verify undeleted content remains visible.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_removal_faults(mdl_storage_t*              storage,
                                                         mdl_state_fault_fs_t*       fault,
                                                         const mdl_library_policy_t* policy)
{
  internal_library_make_tree(storage, "/fault-remove", 0U, true);
  fault->flags = (uint32_t)k_mdl_state_fault_unlink;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
  fault->flags = 0U;
  internal_library_expect_exists(storage, "/fault-remove/page.bin", true);
  fault->flags = (uint32_t)k_mdl_state_fault_dir_open;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
  fault->flags = (uint32_t)k_mdl_state_fault_dir_next;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
  fault->flags = (uint32_t)k_mdl_state_fault_media;
  TEST_ASSERT_EQ(k_ra8_err_hw_not_ready,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
  fault->flags = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage->fs->names, "/fault-remove/page.bin"));
  fault->flags = (uint32_t)k_mdl_state_fault_rmdir;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
  fault->flags = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/fault-remove", policy, &s_library_workspace));
}

/**
 * @brief Exercise iterative bounds, exact budgets, and partial-delete state.
 * @details Proves root protection, depth, operation, and entry limits around successful removals.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Fault facade controlling exact errors.
 * @pre Both pointers are non-NULL and removal vector roots are absent.
 * @pre Shared removal workspace is idle.
 * @post Successful vectors leave no root; failures leave their remainder visible.
 * @post List, stat/media, unlink, and rmdir faults propagate exactly.
 * @note Cleanup after each fault uses the same portable removal algorithm.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_removal_vectors(mdl_storage_t*        storage,
                                                          mdl_state_fault_fs_t* fault)
{
  mdl_library_policy_t policy = mdl_library_policy_default();
  internal_library_write(storage, "/root-sentinel", (const uint8_t*)"safe", 4U);
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 mdl_library_remove_tree(storage, "/", &policy, &s_library_workspace));
  internal_library_expect_exists(storage, "/root-sentinel", true);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage->fs->names, "/root-sentinel"));

  internal_library_make_tree(storage, "/tree", 2U, true);
  internal_library_write(storage, "/tree/root.bin", (const uint8_t*)"root", 4U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/tree", &policy, &s_library_workspace));
  internal_library_expect_exists(storage, "/tree", false);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/tree", &policy, &s_library_workspace));

  internal_library_make_tree(storage, "/depth-ok", 2U, false);
  policy.max_depth = 2U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/depth-ok", &policy, &s_library_workspace));
  internal_library_make_tree(storage, "/depth-over", 3U, false);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_library_remove_tree(storage, "/depth-over", &policy, &s_library_workspace));
  internal_library_expect_exists(storage, "/depth-over/d0/d1/d2", true);
  policy = mdl_library_policy_default();
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/depth-over", &policy, &s_library_workspace));

  internal_library_make_tree(storage, "/ops-ok", 0U, false);
  policy.max_operations = 5U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/ops-ok", &policy, &s_library_workspace));
  internal_library_make_tree(storage, "/ops-over", 0U, false);
  policy.max_operations = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_library_remove_tree(storage, "/ops-over", &policy, &s_library_workspace));
  internal_library_expect_exists(storage, "/ops-over", true);
  policy = mdl_library_policy_default();
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/ops-over", &policy, &s_library_workspace));

  internal_library_make_tree(storage, "/entry-over", 0U, false);
  internal_library_write(storage, "/entry-over/a", (const uint8_t*)"a", 1U);
  internal_library_write(storage, "/entry-over/b", (const uint8_t*)"b", 1U);
  policy.max_entries = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_library_remove_tree(storage, "/entry-over", &policy, &s_library_workspace));
  TEST_ASSERT_EQ(1U, s_library_workspace.entries);
  TEST_ASSERT_EQ(2U, s_library_workspace.required_entries);
  internal_library_expect_exists(storage, "/entry-over", true);
  policy = mdl_library_policy_default();
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/entry-over", &policy, &s_library_workspace));

  internal_library_removal_faults(storage, fault, &policy);
}

/**
 * @brief Remove every common-vector root through the portable algorithm.
 * @details Cleans all shared test roots so the backend can be reused or torn down deterministically.
 * @param[in,out] storage Initialized storage binding.
 * @pre @p storage is non-NULL and no fault is armed.
 * @pre No vector path is held open.
 * @post Every common vector root is absent.
 * @post Removal used no host-specific namespace operation.
 * @note Missing roots are accepted by the production remover.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_cleanup(mdl_storage_t* storage)
{
  static const char* const roots[] =
    {"/empty", "/mixed", "/corrupt", "/legacy", "/marker-dir", "/cap", "/fault-enum"};
  const mdl_library_policy_t policy = mdl_library_policy_default();
  for (size_t i = 0U; i < (sizeof(roots) / sizeof(roots[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok,
                   mdl_library_remove_tree(storage, roots[i], &policy, &s_library_workspace));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage->fs->names, "/root-file"));
}

/**
 * @brief Run every backend-independent library vector.
 * @details Binds fault injection, executes enumeration/removal coverage, and performs exact cleanup.
 * @param[in] label Unity label for this backend.
 * @param[in] inner Live real filesystem facade.
 * @pre Required pointers are non-NULL and the backend is empty and writable.
 * @pre Shared global workspaces are idle.
 * @post All common fixtures are removed.
 * @post Every injected fault is cleared.
 * @note Backend setup and teardown remain outside this function.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_run_backend(const char* label, const fw_fs_t* inner)
{
  TEST_BEGIN(label);
  mdl_state_fault_fs_t fault   = {};
  mdl_storage_t        storage = {};
  internal_library_bind(inner, &fault, &storage);
  internal_library_enumeration_vectors(&storage);
  internal_library_enumeration_faults(&storage, &fault);
  internal_library_removal_vectors(&storage, &fault);
  internal_library_cleanup(&storage);
  TEST_END(label);
}

/**
 * @brief Qualify symlink refusal and destination containment on POSIX.
 * @details Creates one host-only symlink and proves the portable remover cannot escape its root.
 * @param[in,out] storage Initialized root-bound POSIX storage.
 * @param[in] host_root Host path backing the portable root.
 * @pre Both pointers are non-NULL and the root is writable.
 * @pre The host supports symbolic links.
 * @post The outside sentinel survives a refused tree removal.
 * @post All symlink-specific fixtures are removed.
 * @note Raw symlink creation is test setup; production traversal remains portable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_symlink_vector(mdl_storage_t* storage,
                                                         const char*    host_root)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/outside"));
  internal_library_write(storage, "/outside/sentinel", (const uint8_t*)"safe", 4U);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/symlink-tree"));
  char link_path[512];
  (void)snprintf(link_path, sizeof(link_path), "%s/symlink-tree/link", host_root);
  TEST_ASSERT_EQ(0, symlink("../outside", link_path));
  const mdl_library_policy_t policy = mdl_library_policy_default();
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 mdl_library_remove_tree(storage, "/symlink-tree", &policy, &s_library_workspace));
  internal_library_expect_exists(storage, "/outside/sentinel", true);
  TEST_ASSERT_EQ(0, unlink(link_path));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/symlink-tree", &policy, &s_library_workspace));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_library_remove_tree(storage, "/outside", &policy, &s_library_workspace));
}

/**
 * @brief Run common and symlink vectors through the POSIX adapter.
 * @details Qualifies portable behavior on the hosted backend and removes every temporary object.
 * @pre The process may create one temporary directory below `/tmp`.
 * @pre The POSIX adapter is available.
 * @post Every fixture and the temporary directory are removed.
 * @post The adapter is deinitialized.
 * @note This is the hosted reference backend.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_test_posix(void)
{
  char root[] = "/tmp/mdl_library_port_XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(root));
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     posix = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix, &cfg));
  internal_library_run_backend("media library POSIX vectors", &fs);
  mdl_state_fault_fs_t fault   = {};
  mdl_storage_t        storage = {};
  internal_library_bind(&fs, &fault, &storage);
  internal_library_symlink_vector(&storage, root);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix));
  TEST_ASSERT_EQ(0, rmdir(root));
}

/**
 * @brief Mount the formatted RAM/FAT volume through VFS and `fw_fs`.
 * @details Connects the real block, FAT, VFS, and portable-facade layers without a mock filesystem.
 * @param[out] fs Portable filesystem facade.
 * @param[out] state VFS adapter state.
 * @pre Required pointers are non-NULL and the volume is unmounted.
 * @pre The shared filesystem backend is formatted.
 * @post The RAM volume is mounted under `ram`.
 * @post @p fs is ready for portable library calls.
 * @note The caller owns teardown.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_mount_vfs(fw_fs_t* fs, fw_fs_ra8_vfs_state_t* state)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
  const fw_fs_ra8_vfs_cfg_t cfg = {.mount_name = "ram", .mount = s_mount, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(fs, state, &cfg));
}

/**
 * @brief Run common vectors through real RAM blockdev/FAT/VFS layers.
 * @details Formats and mounts the device-equivalent backend before executing the shared suite.
 * @pre Shared RAM disk and mount objects are idle.
 * @pre No VFS mount named `ram` exists.
 * @post The common vector completed and the volume is unmounted.
 * @post No vector fixture remains on the volume.
 * @note This is the device-equivalent backend qualification.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_library_test_vfs(void)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_library_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));
  const ra8_fs_format_opts_t format = {.type = k_ra8_fs_type_fat12, .label = "MDLLIB"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  fw_fs_t               fs  = {};
  fw_fs_ra8_vfs_state_t vfs = {};
  internal_library_mount_vfs(&fs, &vfs);
  internal_library_run_backend("media library RAM/FAT/VFS vectors", &fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("ram"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_library_log_sink, nullptr);
  internal_library_test_posix();
  internal_library_test_vfs();
  return 0;
}
