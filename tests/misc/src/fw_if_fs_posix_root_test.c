/**
 * @file fw_if_fs_posix_root_test.c
 * @brief POSIX composition-root traversal security vectors
 *
 * @details
 * Exercises initialization roots through the same descriptor-relative,
 * no-follow traversal used in production. Every observation consumes a
 * successful binding and verifies that the lowest available descriptor is
 * reusable afterward, including failure paths with transient parent opens.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

/**
 * @def _GNU_SOURCE
 * @brief Request hosted descriptor-relative test declarations
 * @details Enables the POSIX interfaces used to construct private fixtures.
 * @note This translation-unit feature selection has no runtime state.
 * @warning Keep this definition before every system header.
 * @since 0.1.0
 */
#define _GNU_SOURCE

#include "fw_if_fs_posix_root_test.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "fw_if_fs_posix_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum posix_root_test_limits_t
 * @brief Fixed fixture path and permission operands
 * @details Names bounded storage and owner-only modes used by root fixtures.
 * @invariant Every value is representable in the `uint16_t` underlying type.
 * @par Example:
 * @code
 * char path[k_posix_root_test_path_bytes];
 * @endcode
 * @see internal_format_path()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_posix_root_test_path_bytes = 512U,  /**< Native path formatting capacity. */
  k_posix_root_test_file_mode  = 0600U, /**< Owner-only fixture-file mode.    */
  k_posix_root_test_dir_mode   = 0700U, /**< Owner-only fixture-dir mode.     */
} posix_root_test_limits_t;

/**
 * @struct posix_fd_probe
 * @brief One open/close sample of the process's lowest free descriptor
 * @details Couples the descriptor selected by `open` with its consumption result.
 * @invariant A successful probe has a non-negative descriptor and close result zero.
 * @par Example:
 * @code
 * const posix_fd_probe_t probe = internal_probe_fd();
 * @endcode
 * @see internal_probe_fd()
 * @since 0.1.0
 */
typedef struct posix_fd_probe {
  int descriptor;   /**< Descriptor returned by opening `/dev/null`. */
  int close_result; /**< Result from consuming @ref descriptor.      */
} posix_fd_probe_t;

/**
 * @struct posix_root_observation
 * @brief Complete initialization and descriptor-ownership observation
 * @details Records public state around defensive cleanup and brackets it with
 *          lowest-free-descriptor samples so transient descriptor leaks are visible.
 * @invariant Final state is inactive with descriptor -1 after observation.
 * @par Example:
 * @code
 * const posix_root_observation_t observed = internal_observe_root(path);
 * @endcode
 * @see internal_observe_root()
 * @since 0.1.0
 */
typedef struct posix_root_observation {
  ra8_err_t        status;                /**< Public initialization status.         */
  ra8_err_t        cleanup;               /**< Defensive adapter cleanup status.     */
  posix_fd_probe_t before;                /**< Lowest free descriptor before init.   */
  posix_fd_probe_t after;                 /**< Lowest free descriptor after cleanup. */
  bool             attempted_initialized; /**< State immediately after init.         */
  int              attempted_root_fd;     /**< Root fd immediately after init.       */
  bool             final_initialized;     /**< State after defensive cleanup.        */
  int              final_root_fd;         /**< Root fd after defensive cleanup.      */
} posix_root_observation_t;

/**
 * @struct posix_relative_fixture
 * @brief Relative-root namespace paths and lifecycle results
 * @details Keeps fixture creation and removal accounting outside the focused
 *          cwd traversal test so cleanup remains explicit on partial setup.
 * @invariant Every successful creation has one corresponding removal result.
 * @par Example:
 * @code
 * posix_relative_fixture_t fixture = {};
 * @endcode
 * @see internal_relative_fixture_create()
 * @since 0.1.0
 */
typedef struct posix_relative_fixture {
  char parent_path[k_posix_root_test_path_bytes];       /**< Real parent directory path.  */
  char child_path[k_posix_root_test_path_bytes];        /**< Real selected child path.    */
  char target_path[k_posix_root_test_path_bytes];       /**< Link-target directory path.  */
  char target_child_path[k_posix_root_test_path_bytes]; /**< Link-target child path.      */
  char link_path[k_posix_root_test_path_bytes];         /**< Relative symlink path.       */
  int  made_parent;                                     /**< Parent mkdir result.         */
  int  made_child;                                      /**< Child mkdir result.          */
  int  made_target;                                     /**< Target mkdir result.         */
  int  made_target_child;                               /**< Target-child mkdir result.   */
  int  made_link;                                       /**< Symlink creation result.     */
  int  removed_link;                                    /**< Symlink removal result.      */
  int  removed_target_child;                            /**< Target-child removal result. */
  int  removed_target;                                  /**< Target removal result.       */
  int  removed_child;                                   /**< Child removal result.        */
  int  removed_parent;                                  /**< Parent removal result.       */
} posix_relative_fixture_t;

/**
 * @brief Sample and release the process's lowest available descriptor
 * @return Opened descriptor number and its close result.
 * @pre `/dev/null` is readable by the hosted test process.
 * @pre The caller does not change descriptors concurrently.
 * @post Any successfully opened descriptor is closed.
 * @post The returned descriptor number remains available to a later open.
 * @note Not thread-safe because descriptor allocation is process-wide.
 * @since 0.1.0
 */
RA8_INTERNAL static posix_fd_probe_t internal_probe_fd(void)
{
  posix_fd_probe_t probe = {
    .descriptor   = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW),
    .close_result = -1,
  };
  if (probe.descriptor >= 0) {
    probe.close_result = close(probe.descriptor);
  }
  return probe;
}

/**
 * @brief Observe one root bind and consume every descriptor it publishes
 * @param[in] path Existing or deliberately invalid native root path.
 * @return Bind result, state snapshots, cleanup, and descriptor probes.
 * @pre @p path is a non-null NUL-terminated string.
 * @pre No other thread allocates descriptors during this call.
 * @post Any initialized adapter is deinitialized.
 * @post The final state is inactive with root descriptor -1.
 * @note Reentrant only across processes because descriptor allocation is shared.
 * @since 0.1.0
 */
RA8_INTERNAL static posix_root_observation_t internal_observe_root(const char* path)
{
  fw_fs_t                  fs          = {};
  fw_fs_posix_state_t      state       = {.root_fd = -1};
  const fw_fs_posix_cfg_t  cfg         = {.root_path = path, .removable_media = false};
  posix_root_observation_t observation = {
    .cleanup = k_ra8_ok,
    .before  = internal_probe_fd(),
  };
  observation.status                = fw_fs_posix_init(&fs, &state, &cfg);
  observation.attempted_initialized = state.initialized;
  observation.attempted_root_fd     = state.root_fd;
  if (state.initialized) {
    observation.cleanup = fw_fs_posix_deinit(&state);
  } else if (state.root_fd >= 0) {
    observation.cleanup = priv_fs_posix_close_fd(&state.root_fd);
  }
  observation.final_initialized = state.initialized;
  observation.final_root_fd     = state.root_fd;
  observation.after             = internal_probe_fd();
  return observation;
}

/**
 * @brief Assert descriptor reuse and final inactive state
 * @param[in] observation Completed bind observation.
 * @pre @p observation was returned by ::internal_observe_root.
 * @pre Associated namespace fixtures have already been removed.
 * @post Both probes, cleanup, and final state are asserted.
 * @post No descriptor or namespace ownership changes occur.
 * @note Pure apart from test assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_common(posix_root_observation_t observation)
{
  TEST_ASSERT(observation.before.descriptor >= 0);
  TEST_ASSERT_EQ(0, observation.before.close_result);
  TEST_ASSERT_EQ(observation.before.descriptor, observation.after.descriptor);
  TEST_ASSERT_EQ(0, observation.after.close_result);
  TEST_ASSERT_EQ(k_ra8_ok, observation.cleanup);
  TEST_ASSERT(observation.final_initialized == false);
  TEST_ASSERT_EQ(-1, observation.final_root_fd);
}

/**
 * @brief Assert one exact root-bind rejection and clean state
 * @param[in] observation Completed bind observation.
 * @param[in] expected Exact public initialization error.
 * @pre Namespace cleanup associated with @p observation is complete.
 * @pre @p expected is a non-success error.
 * @post The exact error, inactive attempt, and descriptor reuse are asserted.
 * @post No ownership changes occur.
 * @note Pure apart from test assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_failure(posix_root_observation_t observation,
                                                 ra8_err_t                expected)
{
  TEST_ASSERT_EQ(expected, observation.status);
  TEST_ASSERT(observation.attempted_initialized == false);
  TEST_ASSERT_EQ(-1, observation.attempted_root_fd);
  internal_assert_common(observation);
}

/**
 * @brief Assert one successful root bind was completely consumed
 * @param[in] observation Completed bind observation.
 * @pre Namespace cleanup associated with @p observation is complete.
 * @pre @p observation captured an existing real directory root.
 * @post Success, initial ownership, cleanup, and descriptor reuse are asserted.
 * @post No ownership changes occur.
 * @note Pure apart from test assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_success(posix_root_observation_t observation)
{
  TEST_ASSERT_EQ(k_ra8_ok, observation.status);
  TEST_ASSERT(observation.attempted_initialized);
  TEST_ASSERT(observation.attempted_root_fd >= 0);
  internal_assert_common(observation);
}

/**
 * @brief Format one fixture-root path and assert bounded termination
 * @param[out] out Fixed native path destination.
 * @param[in] root Existing terminated native path prefix.
 * @param[in] suffix Terminated suffix beginning with `/` when required.
 * @pre @p out has ::k_posix_root_test_path_bytes writable bytes.
 * @pre @p root and @p suffix are non-null terminated strings.
 * @post @p out contains the concatenated path and a NUL.
 * @post A capacity overflow is reported through the test framework.
 * @note Pure apart from caller-owned output and assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_format_path(char* out, const char* root, const char* suffix)
{
  TEST_ASSERT((size_t)snprintf(out, (size_t)k_posix_root_test_path_bytes, "%s%s", root, suffix) <
              (size_t)k_posix_root_test_path_bytes);
}

/**
 * @brief Create every relative-root fixture and report complete readiness
 * @param[in] root Existing writable private fixture root.
 * @param[out] fixture Receives formatted paths and each creation result.
 * @return Whether every required fixture was created.
 * @retval true All directories and the relative symlink exist.
 * @retval false At least one creation failed; cleanup accounting remains valid.
 * @pre @p root and @p fixture are non-NULL.
 * @pre None of the named relative fixture leaves exists.
 * @post Every attempted result is recorded in @p fixture.
 * @post A false result retains enough state for defensive removal.
 * @note Not thread-safe; mutates the private fixture namespace.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_relative_fixture_create(const char*               root,
                                                          posix_relative_fixture_t* fixture)
{
  internal_format_path(fixture->parent_path, root, "/relative-parent");
  internal_format_path(fixture->child_path, root, "/relative-child");
  internal_format_path(fixture->target_path, root, "/relative-target");
  internal_format_path(fixture->target_child_path, root, "/relative-target/child");
  internal_format_path(fixture->link_path, root, "/relative-link");
  fixture->made_parent = mkdir(fixture->parent_path, (mode_t)k_posix_root_test_dir_mode);
  fixture->made_child  = mkdir(fixture->child_path, (mode_t)k_posix_root_test_dir_mode);
  fixture->made_target = mkdir(fixture->target_path, (mode_t)k_posix_root_test_dir_mode);
  fixture->made_target_child =
    (fixture->made_target == 0)
      ? mkdir(fixture->target_child_path, (mode_t)k_posix_root_test_dir_mode)
      : -1;
  fixture->made_link =
    (fixture->made_target_child == 0) ? symlink("relative-target", fixture->link_path) : -1;
  bool ready = true;
  if (fixture->made_parent != 0) {
    ready = false;
  }
  if (fixture->made_child != 0) {
    ready = false;
  }
  if (fixture->made_link != 0) {
    ready = false;
  }
  return ready;
}

/**
 * @brief Remove every successfully created relative-root fixture
 * @param[in,out] fixture Creation state and removal-result destination.
 * @pre @p fixture is non-NULL and was populated by the creation helper.
 * @pre Restoration of the process cwd has been attempted before cleanup.
 * @post Every successfully created entry has one recorded removal attempt.
 * @post Removal proceeds from symlink and child leaves toward parents.
 * @note Not thread-safe; mutates the private fixture namespace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_relative_fixture_remove(posix_relative_fixture_t* fixture)
{
  fixture->removed_link = (fixture->made_link == 0) ? unlink(fixture->link_path) : 0;
  fixture->removed_target_child =
    (fixture->made_target_child == 0) ? rmdir(fixture->target_child_path) : 0;
  fixture->removed_target = (fixture->made_target == 0) ? rmdir(fixture->target_path) : 0;
  fixture->removed_child  = (fixture->made_child == 0) ? rmdir(fixture->child_path) : 0;
  fixture->removed_parent = (fixture->made_parent == 0) ? rmdir(fixture->parent_path) : 0;
}

/**
 * @brief Assert complete relative fixture creation and defensive removal
 * @param[in] fixture Completed fixture lifecycle results.
 * @pre @p fixture has been through creation and removal helpers.
 * @pre No fixture entry is still needed by a root observation.
 * @post Every creation and removal result is asserted as zero.
 * @post No namespace or descriptor ownership changes occur.
 * @note Pure apart from test assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_relative_fixture(const posix_relative_fixture_t* fixture)
{
  TEST_ASSERT_EQ(0, fixture->made_parent);
  TEST_ASSERT_EQ(0, fixture->made_child);
  TEST_ASSERT_EQ(0, fixture->made_target);
  TEST_ASSERT_EQ(0, fixture->made_target_child);
  TEST_ASSERT_EQ(0, fixture->made_link);
  TEST_ASSERT_EQ(0, fixture->removed_link);
  TEST_ASSERT_EQ(0, fixture->removed_target_child);
  TEST_ASSERT_EQ(0, fixture->removed_target);
  TEST_ASSERT_EQ(0, fixture->removed_child);
  TEST_ASSERT_EQ(0, fixture->removed_parent);
}

/**
 * @brief Cover absolute repeated, trailing, dot, and parent components
 * @param[in] root Existing writable private fixture root.
 * @pre @p root is an absolute path with room for test leaves.
 * @pre The `absolute-parent` and `absolute-child` leaves do not exist.
 * @post Both bindings succeed and every fixture directory is removed.
 * @post No descriptor is retained by either binding.
 * @note Not thread-safe; mutates the private fixture namespace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_absolute_paths(const char* root)
{
  char parent_path[k_posix_root_test_path_bytes];
  char child_path[k_posix_root_test_path_bytes];
  char normalized[k_posix_root_test_path_bytes];
  char parent_walk[k_posix_root_test_path_bytes];
  internal_format_path(parent_path, root, "/absolute-parent");
  internal_format_path(child_path, root, "/absolute-child");
  internal_format_path(normalized, root, "//./absolute-child///");
  internal_format_path(parent_walk, root, "/absolute-parent/../absolute-child");
  const int                made_parent = mkdir(parent_path, (mode_t)k_posix_root_test_dir_mode);
  const int                made_child  = mkdir(child_path, (mode_t)k_posix_root_test_dir_mode);
  posix_root_observation_t normalized_result = {};
  posix_root_observation_t parent_result     = {};
  if (made_parent == 0) {
    if (made_child == 0) {
      normalized_result = internal_observe_root(normalized);
      parent_result     = internal_observe_root(parent_walk);
    }
  }
  const int removed_child  = (made_child == 0) ? rmdir(child_path) : 0;
  const int removed_parent = (made_parent == 0) ? rmdir(parent_path) : 0;

  TEST_ASSERT_EQ(0, made_parent);
  TEST_ASSERT_EQ(0, made_child);
  TEST_ASSERT_EQ(0, removed_child);
  TEST_ASSERT_EQ(0, removed_parent);
  if (made_parent == 0) {
    if (made_child == 0) {
      internal_assert_success(normalized_result);
      internal_assert_success(parent_result);
    }
  }
}

/**
 * @brief Cover relative dot, parent, and intermediate-link traversal
 * @param[in] root Existing writable private fixture root.
 * @pre @p root contains none of the named relative-case leaves.
 * @pre The current directory can be opened and restored with `fchdir`.
 * @post Real relative roots succeed and the linked root is denied exactly.
 * @post The original cwd is restored and every fixture is removed.
 * @note Not thread-safe because cwd is process-wide.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_relative_paths(const char* root)
{
  posix_relative_fixture_t fixture    = {};
  const bool               ready      = internal_relative_fixture_create(root, &fixture);
  const int                cwd_fd     = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const int                changed    = (cwd_fd >= 0) ? chdir(root) : -1;
  posix_root_observation_t dot_result = {};
  posix_root_observation_t parent_result = {};
  posix_root_observation_t link_result   = {};
  if (changed == 0) {
    if (ready) {
      dot_result    = internal_observe_root("./relative-child///");
      parent_result = internal_observe_root("relative-parent/../relative-child");
      link_result   = internal_observe_root("relative-link/child");
    }
  }
  const int restored   = (changed == 0) ? fchdir(cwd_fd) : 0;
  const int cwd_closed = (cwd_fd >= 0) ? close(cwd_fd) : -1;
  internal_relative_fixture_remove(&fixture);

  internal_assert_relative_fixture(&fixture);
  TEST_ASSERT(cwd_fd >= 0);
  TEST_ASSERT_EQ(0, changed);
  TEST_ASSERT_EQ(0, restored);
  TEST_ASSERT_EQ(0, cwd_closed);
  if (changed == 0) {
    if (ready) {
      internal_assert_success(dot_result);
      internal_assert_success(parent_result);
      internal_assert_failure(link_result, k_ra8_err_access_denied);
    }
  }
}

/**
 * @brief Cover final, trailing-slash, and absolute intermediate links
 * @param[in] root Existing writable private fixture root.
 * @pre The `root-link-target` and `root-link` leaves do not exist.
 * @pre @p root is an absolute path with room for test leaves.
 * @post All three binds return exact access-denied status.
 * @post The link and both target directories are removed without fd leakage.
 * @note Not thread-safe; mutates the private fixture namespace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_absolute_symlink(const char* root)
{
  char target_path[k_posix_root_test_path_bytes];
  char child_path[k_posix_root_test_path_bytes];
  char link_path[k_posix_root_test_path_bytes];
  char trailing_path[k_posix_root_test_path_bytes];
  char intermediate_path[k_posix_root_test_path_bytes];
  internal_format_path(target_path, root, "/root-link-target");
  internal_format_path(child_path, root, "/root-link-target/child");
  internal_format_path(link_path, root, "/root-link");
  internal_format_path(trailing_path, link_path, "/");
  internal_format_path(intermediate_path, link_path, "/child");
  const int made_target = mkdir(target_path, (mode_t)k_posix_root_test_dir_mode);
  const int made_child =
    (made_target == 0) ? mkdir(child_path, (mode_t)k_posix_root_test_dir_mode) : -1;
  const int made_link = (made_child == 0) ? symlink("root-link-target", link_path) : -1;
  posix_root_observation_t final_result        = {};
  posix_root_observation_t trailing_result     = {};
  posix_root_observation_t intermediate_result = {};
  if (made_link == 0) {
    final_result        = internal_observe_root(link_path);
    trailing_result     = internal_observe_root(trailing_path);
    intermediate_result = internal_observe_root(intermediate_path);
  }
  const int removed_link   = (made_link == 0) ? unlink(link_path) : 0;
  const int removed_child  = (made_child == 0) ? rmdir(child_path) : 0;
  const int removed_target = (made_target == 0) ? rmdir(target_path) : 0;

  TEST_ASSERT_EQ(0, made_target);
  TEST_ASSERT_EQ(0, made_child);
  TEST_ASSERT_EQ(0, made_link);
  TEST_ASSERT_EQ(0, removed_link);
  TEST_ASSERT_EQ(0, removed_child);
  TEST_ASSERT_EQ(0, removed_target);
  if (made_link == 0) {
    internal_assert_failure(final_result, k_ra8_err_access_denied);
    internal_assert_failure(trailing_result, k_ra8_err_access_denied);
    internal_assert_failure(intermediate_result, k_ra8_err_access_denied);
  }
}

/**
 * @brief Cover missing and non-directory intermediate root components
 * @param[in] root Existing writable private fixture root.
 * @pre The derived missing path is absent and the file leaf does not exist.
 * @pre @p root is an absolute path with room for test leaves.
 * @post Both binds return exact not-found status.
 * @post The temporary file is closed and removed without fd leakage.
 * @note Not thread-safe; briefly creates one private fixture file.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_invalid_intermediate(const char* root)
{
  char missing_path[k_posix_root_test_path_bytes];
  char file_path[k_posix_root_test_path_bytes];
  char file_child_path[k_posix_root_test_path_bytes];
  internal_format_path(missing_path, root, "/missing-parent/child");
  internal_format_path(file_path, root, "/file-parent");
  internal_format_path(file_child_path, file_path, "/child");
  const posix_root_observation_t missing_result = internal_observe_root(missing_path);
  const int                file_fd     = open(file_path,
                                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                              (mode_t)k_posix_root_test_file_mode);
  const int                file_closed = (file_fd >= 0) ? close(file_fd) : -1;
  posix_root_observation_t file_result = {};
  if (file_closed == 0) {
    file_result = internal_observe_root(file_child_path);
  }
  const int removed_file = (file_fd >= 0) ? unlink(file_path) : 0;

  TEST_ASSERT(file_fd >= 0);
  TEST_ASSERT_EQ(0, file_closed);
  TEST_ASSERT_EQ(0, removed_file);
  internal_assert_failure(missing_result, k_ra8_err_not_found);
  if (file_closed == 0) {
    internal_assert_failure(file_result, k_ra8_err_not_found);
  }
}

/**
 * @brief Cover failed initialization from a zero-initialized state object
 * @param[in] root Existing writable private fixture root.
 * @pre The derived missing root path does not exist.
 * @pre No other thread allocates descriptors during this call.
 * @post Failure publishes the complete inactive sentinel state.
 * @post The lowest available descriptor is unchanged.
 * @note Namespace-read-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_zero_state_failure(const char* root)
{
  char missing_path[k_posix_root_test_path_bytes];
  internal_format_path(missing_path, root, "/zero-missing");
  fw_fs_t                 fs     = {};
  fw_fs_posix_state_t     state  = {};
  const fw_fs_posix_cfg_t cfg    = {.root_path = missing_path, .removable_media = true};
  const posix_fd_probe_t  before = internal_probe_fd();
  const ra8_err_t         status = fw_fs_posix_init(&fs, &state, &cfg);
  const posix_fd_probe_t  after  = internal_probe_fd();

  TEST_ASSERT_EQ(k_ra8_err_not_found, status);
  TEST_ASSERT(state.initialized == false);
  TEST_ASSERT_EQ(-1, state.root_fd);
  TEST_ASSERT_EQ(0U, state.transaction_id);
  TEST_ASSERT(state.removable_media == false);
  TEST_ASSERT(state.atomic_noreplace == false);
  TEST_ASSERT(before.descriptor >= 0);
  TEST_ASSERT_EQ(0, before.close_result);
  TEST_ASSERT_EQ(before.descriptor, after.descriptor);
  TEST_ASSERT_EQ(0, after.close_result);
}

/**
 * @brief Cover empty, overlong-component, and overlong-path root guards
 * @pre The process working directory remains readable during the observations.
 * @pre No other thread allocates descriptors during this call.
 * @post Empty input returns invalid argument and both bounds return invalid size.
 * @post Every transient anchor descriptor is closed and reusable.
 * @note Namespace-read-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_root_bounds(void)
{
  char overlong_component[k_posix_component_cap + 1U];
  char overlong_path[k_fw_fs_path_cap + 1U];
  RA8_LOOP_BOUND(k_posix_component_cap);
  for (uint16_t index = 0U; index < (uint16_t)k_posix_component_cap; ++index) {
    overlong_component[index] = 'a';
  }
  overlong_component[k_posix_component_cap] = '\0';
  RA8_LOOP_BOUND(k_fw_fs_path_cap);
  for (uint16_t index = 0U; index < (uint16_t)k_fw_fs_path_cap; ++index) {
    overlong_path[index] = '/';
  }
  overlong_path[k_fw_fs_path_cap]          = '\0';
  const posix_root_observation_t empty     = internal_observe_root("");
  const posix_root_observation_t component = internal_observe_root(overlong_component);
  const posix_root_observation_t path      = internal_observe_root(overlong_path);

  internal_assert_failure(empty, k_ra8_err_invalid_arg);
  internal_assert_failure(component, k_ra8_err_invalid_size);
  internal_assert_failure(path, k_ra8_err_invalid_size);
}

RA8_TEST_HELPER void ra8_test_fw_if_fs_posix_roots(const char* root)
{
  TEST_ASSERT_NOT_NULL(root);
  internal_check_absolute_paths(root);
  internal_check_relative_paths(root);
  internal_check_absolute_symlink(root);
  internal_check_invalid_intermediate(root);
  internal_check_zero_state_failure(root);
  internal_check_root_bounds();
}
