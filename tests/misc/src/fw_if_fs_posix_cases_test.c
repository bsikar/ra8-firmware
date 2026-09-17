/**
 * @file fw_if_fs_posix_cases_test.c
 * @brief POSIX-specific filesystem security and transaction-retry vectors
 *
 * @details
 * Exercises public lifecycle guards and capability binding, root-alias
 * classification and canonical no-follow opening across hostile topologies,
 * native symlink confinement, rejection of a symlinked composition root, and
 * deterministic occupied transaction-stage handling inside a private fixture
 * root. Read-only `/tmp` and `/var` bindings prove exact system-root behavior;
 * Darwin additionally proves canonical descriptor identity. The companion
 * keeps hosted-only setup out of the portable conformance driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

/**
 * @def _GNU_SOURCE
 * @brief Request hosted POSIX extension declarations used by this test
 *
 * @details
 * Enables `openat()`, `symlink()`, and the related descriptor-relative POSIX
 * interfaces before any system header is parsed.
 *
 * @note This translation-unit feature selection has no runtime state.
 * @warning The definition must remain before every system header.
 * @par Usage:
 * @code
 * #define _GNU_SOURCE
 * #include <fcntl.h>
 * @endcode
 * @since 0.1.0
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_posix_internal.h"
#include "fw_if_fs_posix_root_test.h"
#include "fw_if_fs_posix_test_cases.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum posix_test_limits_t
 * @brief Fixed workspace, path, counter, and permission operands
 *
 * @details
 * Names every integral fixture operand used by the hosted security and
 * transaction-retry vectors. The largest value fits in `uint16_t`.
 *
 * @invariant Every value is representable in the declared underlying type.
 * @par Example:
 * @code
 * char path[k_posix_test_path_bytes];
 * @endcode
 * @see internal_check_stage_collision()
 * @see internal_check_symlinks()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_posix_test_work_bytes = 8192U,  /**< Maximum backend workspace.         */
  k_posix_test_path_bytes = 256U,   /**< Native path formatting capacity.   */
  k_posix_test_stage_seed = 0x100U, /**< Counter before a staged collision. */
  k_posix_test_stage_next = 0x102U, /**< Counter after collision and retry. */
  k_posix_test_file_mode  = 0600U,  /**< Owner-only fixture-file mode.      */
  k_posix_test_dir_mode   = 0700U,  /**< Owner-only fixture-directory mode. */
} posix_test_limits_t;

/**
 * @union posix_test_workspace
 * @brief Maximally aligned caller-owned backend workspace
 *
 * @details
 * Provides an opaque byte array with `max_align_t` alignment so the POSIX
 * backend may place its private directory or transaction state in
 * caller-owned storage.
 *
 * @invariant The byte array starts at offset zero and has maximum C alignment.
 * @par Example:
 * @code
 * posix_test_workspace_t work = {};
 * @endcode
 * @see fw_fs_transaction_begin()
 * @since 0.1.0
 */
typedef union posix_test_workspace {
  max_align_t alignment; /**< Force maximum C alignment. */
  /** @brief Opaque backend state storage. */
  uint8_t bytes[k_posix_test_work_bytes];
} posix_test_workspace_t;

/**
 * @struct posix_bind_observation
 * @brief Captured public-bind result and descriptor ownership state
 *
 * @details
 * Records adapter state both before and after defensive cleanup so namespace
 * fixture removal and every assertion can occur after descriptor ownership is
 * resolved.
 *
 * @invariant A conforming failed bind records false and -1 in both state pairs.
 * @par Example:
 * @code
 * const posix_bind_observation_t observation = internal_observe_bind(path);
 * @endcode
 * @see fw_fs_posix_init()
 * @since 0.1.0
 */
typedef struct posix_bind_observation {
  ra8_err_t status;                /**< Initialization result.               */
  ra8_err_t cleanup;               /**< Defensive descriptor cleanup result. */
  bool      attempted_initialized; /**< State immediately after init.        */
  int       attempted_root_fd;     /**< Descriptor immediately after init.   */
  bool      final_initialized;     /**< State after defensive cleanup.       */
  int       final_root_fd;         /**< Descriptor after defensive cleanup.  */
} posix_bind_observation_t;

/**
 * @struct posix_alias_open_observation
 * @brief Captured canonical alias-open result and cleanup state
 *
 * @details
 * Records descriptor publication before consuming any owned descriptor. This
 * lets callers remove all namespace fixtures before asserting outcomes.
 *
 * @invariant `final_fd` is -1 and the lowest free descriptor is reusable after cleanup.
 * @par Example:
 * @code
 * const posix_alias_open_observation_t result = internal_observe_alias_open(root_fd, alias);
 * @endcode
 * @see priv_fs_posix_root_alias_open()
 * @since 0.1.0
 */
typedef struct posix_alias_open_observation {
  ra8_err_t status;       /**< Canonical open result.                 */
  ra8_err_t cleanup;      /**< Published-descriptor cleanup result.   */
  bool      published;    /**< True when the opener published an fd.  */
  int       final_fd;     /**< Descriptor value after cleanup.        */
  int       before_fd;    /**< Lowest free descriptor before opening. */
  int       before_close; /**< Result from consuming @ref before_fd.  */
  int       after_fd;     /**< Lowest free descriptor after cleanup.  */
  int       after_close;  /**< Result from consuming @ref after_fd.   */
} posix_alias_open_observation_t;

/**
 * @struct posix_alias_classifier_vector
 * @brief One pure filesystem-root alias classification vector
 *
 * @details
 * Couples the component and raw target extent with a deliberately seeded
 * output alias and the exact expected result. Explicit byte counts exercise
 * non-terminated and truncated link targets without host filesystem behavior.
 *
 * @invariant Each target addresses at least `target_bytes` readable bytes.
 * @par Example:
 * @code
 * const posix_alias_classifier_vector_t vector = {
 *   .component = "tmp", .target = "private/tmp",
 *   .target_bytes = sizeof("private/tmp") - 1U,
 *   .initial_alias = k_posix_root_alias_var,
 *   .expected_status = k_ra8_ok, .expected_alias = k_posix_root_alias_tmp};
 * @endcode
 * @see priv_fs_posix_root_alias_classify()
 * @since 0.1.0
 */
typedef struct posix_alias_classifier_vector {
  const char*        component;       /**< Candidate alias basename.        */
  const char*        target;          /**< Raw candidate link-target bytes. */
  size_t             target_bytes;    /**< Readable target byte count.      */
  posix_root_alias_t initial_alias;   /**< Non-none output seed.            */
  ra8_err_t          expected_status; /**< Expected classification status.  */
  posix_root_alias_t expected_alias;  /**< Expected published alias.        */
} posix_alias_classifier_vector_t;

/**
 * @var s_posix_alias_classifier_vectors
 * @brief Exact-match and hostile root-alias classification matrix
 *
 * @details
 * Covers both approved pairs, both components' length and same-length content
 * mismatches, absolute targets, prefix and suffix targets, and an ordinary
 * component. Every rejection begins with a non-none output seed.
 *
 * @note Read-only and independent of host filesystem topology.
 * @warning Failure rows must expect ::k_posix_root_alias_none.
 * @since 0.1.0
 */
static const posix_alias_classifier_vector_t s_posix_alias_classifier_vectors[] = {
  {.component       = "tmp",
   .target          = "private/tmp",
   .target_bytes    = sizeof("private/tmp") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_ok,
   .expected_alias  = k_posix_root_alias_tmp},
  {.component       = "var",
   .target          = "private/var",
   .target_bytes    = sizeof("private/var") - 1U,
   .initial_alias   = k_posix_root_alias_tmp,
   .expected_status = k_ra8_ok,
   .expected_alias  = k_posix_root_alias_var},
  {.component       = "tmp",
   .target          = "private/tm",
   .target_bytes    = sizeof("private/tm") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "var",
   .target          = "private/va",
   .target_bytes    = sizeof("private/va") - 1U,
   .initial_alias   = k_posix_root_alias_tmp,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "tmp",
   .target          = "private/tmp/",
   .target_bytes    = sizeof("private/tmp/") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "var",
   .target          = "private/var/",
   .target_bytes    = sizeof("private/var/") - 1U,
   .initial_alias   = k_posix_root_alias_tmp,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "tmp",
   .target          = "/private/tmp",
   .target_bytes    = sizeof("/private/tmp") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "var",
   .target          = "/private/var",
   .target_bytes    = sizeof("/private/var") - 1U,
   .initial_alias   = k_posix_root_alias_tmp,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "tmp",
   .target          = "private/var",
   .target_bytes    = sizeof("private/var") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "var",
   .target          = "private/tmp",
   .target_bytes    = sizeof("private/tmp") - 1U,
   .initial_alias   = k_posix_root_alias_tmp,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
  {.component       = "ordinary",
   .target          = "private/tmp",
   .target_bytes    = sizeof("private/tmp") - 1U,
   .initial_alias   = k_posix_root_alias_var,
   .expected_status = k_ra8_err_access_denied,
   .expected_alias  = k_posix_root_alias_none},
};

/**
 * @struct posix_symlink_vector
 * @brief One hostile intermediate-symlink security vector
 *
 * @details
 * Couples a native fixture leaf and symlink target with the portable final and
 * intermediate paths used to exercise it. Keeping each case in one row
 * prevents target, component, and portable-path arrays from drifting apart.
 *
 * @invariant Every pointer addresses immutable NUL-terminated static storage.
 * @par Example:
 * @code
 * const posix_symlink_vector_t vector = {
 *   .component_name = "tmp", .symlink_target = "/",
 *   .directory_path = "/tmp", .intermediate_path = "/tmp/file"};
 * @endcode
 * @see internal_check_intermediate_components()
 * @since 0.1.0
 */
typedef struct posix_symlink_vector {
  const char* component_name;    /**< Native fixture-root symlink leaf.    */
  const char* symlink_target;    /**< Relative or absolute hostile target. */
  const char* directory_path;    /**< Portable final-component path.       */
  const char* intermediate_path; /**< Portable path traversing the link.   */
} posix_symlink_vector_t;

/**
 * @var s_posix_symlink_vectors
 * @brief Immutable intermediate-symlink security matrix
 *
 * @details
 * Exercises relative, absolute, and exact-looking nested targets for the
 * historically exceptional `tmp` and `var` spellings, plus an ordinary-name
 * control. Every row must be rejected by both intermediate stat and final
 * directory-open operations.
 *
 * @note Read-only table shared by one test helper.
 * @warning Keep each portable path aligned with its native component name.
 * @since 0.1.0
 */
static const posix_symlink_vector_t s_posix_symlink_vectors[] = {
  {.component_name    = "tmp",
   .symlink_target    = "target",
   .directory_path    = "/tmp",
   .intermediate_path = "/tmp/file"},
  {.component_name    = "tmp",
   .symlink_target    = "/",
   .directory_path    = "/tmp",
   .intermediate_path = "/tmp/file"},
  {.component_name    = "tmp",
   .symlink_target    = "private/tmp",
   .directory_path    = "/tmp",
   .intermediate_path = "/tmp/file"},
  {.component_name    = "var",
   .symlink_target    = "target",
   .directory_path    = "/var",
   .intermediate_path = "/var/file"},
  {.component_name    = "var",
   .symlink_target    = "/",
   .directory_path    = "/var",
   .intermediate_path = "/var/file"},
  {.component_name    = "var",
   .symlink_target    = "private/var",
   .directory_path    = "/var",
   .intermediate_path = "/var/file"},
  {.component_name    = "ordinary",
   .symlink_target    = "target",
   .directory_path    = "/ordinary",
   .intermediate_path = "/ordinary/file"},
};

/**
 * @brief Prove root-alias classification accepts only exact approved pairs
 *
 * @details
 * Runs the platform-neutral classifier over approved and hostile raw target
 * extents. Each vector begins with a non-none output so every failure also
 * proves the classifier clears stale alias state.
 *
 * @pre ::s_posix_alias_classifier_vectors contains immutable readable strings.
 * @pre Every vector's target extent satisfies its documented readable bound.
 * @post Both exact approved pairs publish their corresponding aliases.
 * @post Every rejected pair returns access denied and publishes alias none.
 * @note Pure, reentrant, and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_root_alias_classifier(void)
{
  for (size_t i = 0U;
       i < (sizeof(s_posix_alias_classifier_vectors) / sizeof(s_posix_alias_classifier_vectors[0]));
       ++i) {
    const posix_alias_classifier_vector_t* const vector = &s_posix_alias_classifier_vectors[i];
    posix_root_alias_t                           alias  = vector->initial_alias;
    const ra8_err_t status = priv_fs_posix_root_alias_classify(vector->component,
                                                               vector->target,
                                                               vector->target_bytes,
                                                               &alias);
    TEST_ASSERT_EQ(vector->expected_status, status);
    TEST_ASSERT_EQ(vector->expected_alias, alias);
  }
}

/**
 * @brief Observe one canonical alias open and consume any published descriptor
 *
 * @param[in] root_fd Open descriptor for a private root-like fixture.
 * @param[in] alias Canonical alias selection under test.
 * @return Captured open status, publication state, and cleanup result.
 * @pre @p root_fd remains open for the duration of the call.
 * @pre @p alias is any representable alias selection.
 * @post Any published descriptor is closed and final fd is -1.
 * @post No assertion occurs, allowing callers to remove fixtures first.
 * @note Reentrant for independent root descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL static posix_alias_open_observation_t
internal_observe_alias_open(int root_fd, posix_root_alias_t alias)
{
  const int                      before_fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int                      before_close = (before_fd >= 0) ? close(before_fd) : -1;
  int                            alias_fd     = -1;
  posix_alias_open_observation_t observation  = {
    .status       = priv_fs_posix_root_alias_open(root_fd, alias, &alias_fd),
    .cleanup      = k_ra8_ok,
    .before_fd    = before_fd,
    .before_close = before_close,
  };
  observation.published = alias_fd >= 0;
  if (alias_fd >= 0) {
    observation.cleanup = priv_fs_posix_close_fd(&alias_fd);
  }
  observation.final_fd    = alias_fd;
  observation.after_fd    = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  observation.after_close = (observation.after_fd >= 0) ? close(observation.after_fd) : -1;
  return observation;
}

/**
 * @brief Assert one canonical alias-open result and descriptor invariant
 *
 * @param[in] observation Completed and cleaned alias-open observation.
 * @param[in] expected Exact expected status.
 * @param[in] expected_publication Expected pre-cleanup publication state.
 * @pre @p observation was returned by ::internal_observe_alias_open.
 * @pre Associated namespace fixtures have already been removed.
 * @post Status, publication, cleanup, and final fd are asserted.
 * @post No descriptor or namespace state is changed.
 * @note Pure apart from test assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_alias_result(posix_alias_open_observation_t observation,
                                                      ra8_err_t                      expected,
                                                      bool expected_publication)
{
  TEST_ASSERT_EQ(expected, observation.status);
  TEST_ASSERT_EQ(expected_publication, observation.published);
  TEST_ASSERT_EQ(k_ra8_ok, observation.cleanup);
  TEST_ASSERT_EQ(-1, observation.final_fd);
  TEST_ASSERT(observation.before_fd >= 0);
  TEST_ASSERT_EQ(0, observation.before_close);
  TEST_ASSERT_EQ(observation.before_fd, observation.after_fd);
  TEST_ASSERT_EQ(0, observation.after_close);
}

/**
 * @brief Assert one host-specific no-follow symlink rejection
 *
 * @param[in] observation Completed and cleaned alias-open observation.
 * @pre @p observation was returned for a symlinked canonical component.
 * @pre Associated namespace fixtures have already been removed.
 * @post Access-denied or not-found status, no publication, cleanup, and fd -1
 *       are asserted.
 * @post No descriptor or namespace state is changed.
 * @note Hosts may map a no-follow directory symlink from `ELOOP` or `ENOTDIR`.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_assert_alias_symlink_rejected(posix_alias_open_observation_t observation)
{
  ra8_err_t allowed_status = k_ra8_fail;
  if (observation.status == k_ra8_err_access_denied) {
    allowed_status = observation.status;
  }
  if (observation.status == k_ra8_err_not_found) {
    allowed_status = observation.status;
  }
  TEST_ASSERT_EQ(allowed_status, observation.status);
  TEST_ASSERT(observation.published == false);
  TEST_ASSERT_EQ(k_ra8_ok, observation.cleanup);
  TEST_ASSERT_EQ(-1, observation.final_fd);
  TEST_ASSERT(observation.before_fd >= 0);
  TEST_ASSERT_EQ(0, observation.before_close);
  TEST_ASSERT_EQ(observation.before_fd, observation.after_fd);
  TEST_ASSERT_EQ(0, observation.after_close);
}

/**
 * @brief Cover missing and symlinked canonical alias parents
 *
 * @param[in] root_fd Open descriptor for an empty writable fixture root.
 * @pre @p root_fd identifies an owned private directory.
 * @pre The fixture contains no `private` entry.
 * @post The missing parent is rejected as not found.
 * @post The symlink parent is rejected and removed without being followed.
 * @note Not thread-safe; briefly creates `private` as a symlink.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_alias_parent_failures(int root_fd)
{
  const posix_alias_open_observation_t missing =
    internal_observe_alias_open(root_fd, k_posix_root_alias_tmp);
  TEST_ASSERT_EQ(0, symlinkat(".", root_fd, "private"));
  const posix_alias_open_observation_t symlinked =
    internal_observe_alias_open(root_fd, k_posix_root_alias_tmp);
  const int remove_private = unlinkat(root_fd, "private", 0);

  TEST_ASSERT_EQ(0, remove_private);
  internal_assert_alias_result(missing, k_ra8_err_not_found, false);
  internal_assert_alias_symlink_rejected(symlinked);
}

/**
 * @brief Cover missing and symlinked canonical alias children
 *
 * @param[in] root_fd Open descriptor for an empty writable fixture root.
 * @pre @p root_fd identifies an owned private directory.
 * @pre The fixture contains no `private` entry.
 * @post Missing `tmp` and `var` children are rejected as not found.
 * @post Symlink children are rejected and every fixture entry is removed.
 * @note Not thread-safe; mutates canonical-looking names in the fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_alias_child_failures(int root_fd)
{
  TEST_ASSERT_EQ(0, mkdirat(root_fd, "private", (mode_t)k_posix_test_dir_mode));
  const posix_alias_open_observation_t missing_tmp =
    internal_observe_alias_open(root_fd, k_posix_root_alias_tmp);
  const posix_alias_open_observation_t missing_var =
    internal_observe_alias_open(root_fd, k_posix_root_alias_var);
  TEST_ASSERT_EQ(0, symlinkat(".", root_fd, "private/tmp"));
  const posix_alias_open_observation_t symlinked_tmp =
    internal_observe_alias_open(root_fd, k_posix_root_alias_tmp);
  TEST_ASSERT_EQ(0, unlinkat(root_fd, "private/tmp", 0));
  TEST_ASSERT_EQ(0, symlinkat(".", root_fd, "private/var"));
  const posix_alias_open_observation_t symlinked_var =
    internal_observe_alias_open(root_fd, k_posix_root_alias_var);
  const int remove_var     = unlinkat(root_fd, "private/var", 0);
  const int remove_private = unlinkat(root_fd, "private", AT_REMOVEDIR);

  TEST_ASSERT_EQ(0, remove_var);
  TEST_ASSERT_EQ(0, remove_private);
  internal_assert_alias_result(missing_tmp, k_ra8_err_not_found, false);
  internal_assert_alias_result(missing_var, k_ra8_err_not_found, false);
  internal_assert_alias_symlink_rejected(symlinked_tmp);
  internal_assert_alias_symlink_rejected(symlinked_var);
}

/**
 * @brief Cover both successful canonical opens and invalid alias rejection
 *
 * @param[in] root_fd Open descriptor for an empty writable fixture root.
 * @pre @p root_fd identifies an owned private directory.
 * @pre The fixture contains no `private` entry.
 * @post Both canonical children are opened no-follow and their fds are closed.
 * @post Invalid alias output is -1 and every fixture directory is removed.
 * @note Not thread-safe; mutates canonical-looking names in the fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_alias_successes(int root_fd)
{
  TEST_ASSERT_EQ(0, mkdirat(root_fd, "private", (mode_t)k_posix_test_dir_mode));
  TEST_ASSERT_EQ(0, mkdirat(root_fd, "private/tmp", (mode_t)k_posix_test_dir_mode));
  TEST_ASSERT_EQ(0, mkdirat(root_fd, "private/var", (mode_t)k_posix_test_dir_mode));
  const posix_alias_open_observation_t opened_tmp =
    internal_observe_alias_open(root_fd, k_posix_root_alias_tmp);
  const posix_alias_open_observation_t opened_var =
    internal_observe_alias_open(root_fd, k_posix_root_alias_var);
  const posix_alias_open_observation_t invalid =
    internal_observe_alias_open(root_fd, k_posix_root_alias_none);
  const int remove_var     = unlinkat(root_fd, "private/var", AT_REMOVEDIR);
  const int remove_tmp     = unlinkat(root_fd, "private/tmp", AT_REMOVEDIR);
  const int remove_private = unlinkat(root_fd, "private", AT_REMOVEDIR);

  TEST_ASSERT_EQ(0, remove_var);
  TEST_ASSERT_EQ(0, remove_tmp);
  TEST_ASSERT_EQ(0, remove_private);
  internal_assert_alias_result(opened_tmp, k_ra8_ok, true);
  internal_assert_alias_result(opened_var, k_ra8_ok, true);
  internal_assert_alias_result(invalid, k_ra8_err_invalid_arg, false);
}

/**
 * @brief Observe one public bind and defensively consume any descriptor
 *
 * @param[in] path Existing or deliberately invalid native root path.
 * @return Captured bind status and state before and after cleanup.
 * @pre @p path is a non-null NUL-terminated string.
 * @pre No caller-owned state or descriptor is passed to this helper.
 * @post Any descriptor published by initialization is closed.
 * @post The returned final state records false and -1.
 * @note Reentrant for independent native paths.
 * @since 0.1.0
 */
RA8_INTERNAL static posix_bind_observation_t internal_observe_bind(const char* path)
{
  fw_fs_t                  fs          = {};
  fw_fs_posix_state_t      state       = {.root_fd = -1};
  const fw_fs_posix_cfg_t  cfg         = {.root_path = path, .removable_media = false};
  posix_bind_observation_t observation = {
    .status  = fw_fs_posix_init(&fs, &state, &cfg),
    .cleanup = k_ra8_ok,
  };
  observation.attempted_initialized = state.initialized;
  observation.attempted_root_fd     = state.root_fd;
  if (state.initialized) {
    observation.cleanup = fw_fs_posix_deinit(&state);
  } else if (state.root_fd >= 0) {
    observation.cleanup = priv_fs_posix_close_fd(&state.root_fd);
  }
  observation.final_initialized = state.initialized;
  observation.final_root_fd     = state.root_fd;
  return observation;
}

/**
 * @brief Assert one public bind failed without retaining adapter state
 *
 * @param[in] observation Fully captured and cleaned bind observation.
 * @param[in] expected Exact expected bind error.
 * @pre @p observation was returned by ::internal_observe_bind.
 * @pre Cleanup of any associated namespace fixture is already complete.
 * @post The bind error, cleanup result, and both state snapshots are asserted.
 * @post No descriptor ownership changes occur in this assertion helper.
 * @note Pure apart from the test framework's assertion reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_bind_failure(posix_bind_observation_t observation,
                                                      ra8_err_t                expected)
{
  TEST_ASSERT_EQ(expected, observation.status);
  TEST_ASSERT_EQ(k_ra8_ok, observation.cleanup);
  TEST_ASSERT(observation.attempted_initialized == false);
  TEST_ASSERT_EQ(-1, observation.attempted_root_fd);
  TEST_ASSERT(observation.final_initialized == false);
  TEST_ASSERT_EQ(-1, observation.final_root_fd);
}

/**
 * @brief Cover all public initialization pointer guards
 *
 * @param[in] root Existing writable native fixture root.
 * @pre @p root is a non-null NUL-terminated directory path.
 * @pre The local adapter state begins uninitialized with root fd -1.
 * @post Every invalid pointer combination returns null-pointer status.
 * @post The local adapter state remains uninitialized with root fd -1.
 * @note Pure with respect to the host filesystem namespace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_init_pointer_guards(const char* root)
{
  fw_fs_t                 fs         = {};
  fw_fs_posix_state_t     state      = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg        = {.root_path = root, .removable_media = false};
  const fw_fs_posix_cfg_t null_root  = {.root_path = nullptr, .removable_media = false};
  const ra8_err_t         null_out   = fw_fs_posix_init(nullptr, &state, &cfg);
  const ra8_err_t         null_state = fw_fs_posix_init(&fs, nullptr, &cfg);
  const ra8_err_t         null_cfg   = fw_fs_posix_init(&fs, &state, nullptr);
  const ra8_err_t         null_path  = fw_fs_posix_init(&fs, &state, &null_root);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, null_out);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, null_state);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, null_cfg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, null_path);
  TEST_ASSERT(state.initialized == false);
  TEST_ASSERT_EQ(-1, state.root_fd);
}

/**
 * @brief Cover public bind capability and lifecycle guards
 *
 * @param[in] root Existing writable native fixture root.
 * @pre @p root is a non-null NUL-terminated directory path.
 * @pre A second independent descriptor may be opened for @p root.
 * @post Removable-media capability is truthful and duplicate init is denied.
 * @post Deinit consumes the descriptor; repeated and null deinit are denied.
 * @note Namespace-read-only; opens and closes one root descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_public_lifecycle(const char* root)
{
  fw_fs_t                 fs          = {};
  fw_fs_posix_state_t     state       = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg         = {.root_path = root, .removable_media = true};
  fw_fs_caps_t            caps        = {};
  const ra8_err_t         opened      = fw_fs_posix_init(&fs, &state, &cfg);
  const bool              initialized = state.initialized;
  const int               root_fd     = state.root_fd;
  const ra8_err_t         caps_status = fw_fs_get_caps(&fs, &caps);
  const ra8_err_t         duplicate   = fw_fs_posix_init(&fs, &state, &cfg);
  ra8_err_t               closed      = k_ra8_ok;
  if (state.initialized) {
    closed = fw_fs_posix_deinit(&state);
  }
  const ra8_err_t repeated_close = fw_fs_posix_deinit(&state);
  const ra8_err_t null_close     = fw_fs_posix_deinit(nullptr);

  TEST_ASSERT_EQ(k_ra8_ok, opened);
  TEST_ASSERT(initialized);
  TEST_ASSERT(root_fd >= 0);
  TEST_ASSERT_EQ(k_ra8_ok, caps_status);
  TEST_ASSERT((caps.flags & (uint32_t)k_fw_fs_cap_removable_media) != 0U);
  TEST_ASSERT_EQ(k_ra8_err_exists, duplicate);
  TEST_ASSERT_EQ(k_ra8_ok, closed);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, repeated_close);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, null_close);
  TEST_ASSERT(state.initialized == false);
  TEST_ASSERT_EQ(-1, state.root_fd);
}

/**
 * @brief Cover missing and regular-file public root failures
 *
 * @param[in] root Existing writable native fixture root.
 * @pre @p root is a non-null NUL-terminated directory path.
 * @pre The two derived fixture leaves do not exist.
 * @post Both binds return not found with uninitialized state and root fd -1.
 * @post The temporary regular file is closed and removed before assertions.
 * @note Not thread-safe; briefly creates one owner-only regular file.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_public_root_failures(const char* root)
{
  char missing_path[k_posix_test_path_bytes];
  char file_path[k_posix_test_path_bytes];
  TEST_ASSERT((size_t)snprintf(missing_path, sizeof(missing_path), "%s/missing-root", root) <
              sizeof(missing_path));
  TEST_ASSERT((size_t)snprintf(file_path, sizeof(file_path), "%s/file-root", root) <
              sizeof(file_path));
  const posix_bind_observation_t missing = internal_observe_bind(missing_path);
  const int                      fd =
    open(file_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)k_posix_test_file_mode);
  int close_result = -1;
  if (fd >= 0) {
    close_result = close(fd);
  }
  const posix_bind_observation_t regular = internal_observe_bind(file_path);
  const int                      removed = unlink(file_path);

  internal_assert_bind_failure(missing, k_ra8_err_not_found);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(0, close_result);
  TEST_ASSERT_EQ(0, removed);
  internal_assert_bind_failure(regular, k_ra8_err_not_found);
}

/**
 * @brief Cover read-only bind/deinit of one exact system root
 *
 * @param[in] path Exact `/tmp` or `/var` public root path.
 * @param[in] canonical Darwin canonical directory used for identity checking.
 * @pre @p path and @p canonical are non-null terminated absolute paths.
 * @pre @p path names an existing readable directory or approved Darwin alias.
 * @post Initialization succeeds and deinit resets state to false and fd -1.
 * @post Darwin binds identify the same object as @p canonical.
 * @note Namespace-read-only; opens and closes descriptors only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_system_root(const char* path, const char* canonical)
{
  fw_fs_t                 fs          = {};
  fw_fs_posix_state_t     state       = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg         = {.root_path = path, .removable_media = false};
  const ra8_err_t         opened      = fw_fs_posix_init(&fs, &state, &cfg);
  const bool              initialized = state.initialized;
  const int               opened_fd   = state.root_fd;
#ifdef __APPLE__
  struct stat opened_meta    = {};
  struct stat canonical_meta = {};
  int         opened_stat    = -1;
  int         canonical_stat = -1;
  if (state.initialized) {
    opened_stat    = fstat(state.root_fd, &opened_meta);
    canonical_stat = stat(canonical, &canonical_meta);
  }
#else
  (void)canonical;
#endif
  ra8_err_t closed = k_ra8_ok;
  if (state.initialized) {
    closed = fw_fs_posix_deinit(&state);
  }

  TEST_ASSERT_EQ(k_ra8_ok, opened);
  TEST_ASSERT(initialized);
  TEST_ASSERT(opened_fd >= 0);
#ifdef __APPLE__
  TEST_ASSERT_EQ(0, opened_stat);
  TEST_ASSERT_EQ(0, canonical_stat);
  TEST_ASSERT_EQ(canonical_meta.st_dev, opened_meta.st_dev);
  TEST_ASSERT_EQ(canonical_meta.st_ino, opened_meta.st_ino);
#endif
  TEST_ASSERT_EQ(k_ra8_ok, closed);
  TEST_ASSERT(state.initialized == false);
  TEST_ASSERT_EQ(-1, state.root_fd);
}

/**
 * @brief Prove every fixture-root intermediate symlink is rejected
 *
 * @details
 * Creates table-driven symlinks named `tmp`, `var`, and `ordinary` under the
 * private fixture root. Each intermediate stat and final directory-open must
 * fail closed without following the link, independent of target form,
 * platform, or component spelling.
 *
 * @param[in] fs Initialized POSIX filesystem facade.
 * @param[in] root Native path backing `fs`.
 * @pre @p fs and @p root are non-null.
 * @pre `root/target` exists as a directory.
 * @post Every test symlink is removed.
 * @post Every stat and directory-open returns `k_ra8_err_access_denied`.
 * @note Not thread-safe; mutates names in the caller's private fixture root.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_intermediate_components(const fw_fs_t* fs, const char* root)
{
  char                   link_path[k_posix_test_path_bytes];
  fw_fs_stat_t           stat           = {};
  fw_fs_dir_t            directory      = {};
  posix_test_workspace_t directory_work = {};
  for (size_t i = 0U; i < (sizeof(s_posix_symlink_vectors) / sizeof(s_posix_symlink_vectors[0]));
       ++i) {
    const posix_symlink_vector_t* const vector = &s_posix_symlink_vectors[i];
    TEST_ASSERT(
      (size_t)snprintf(link_path, sizeof(link_path), "%s/%s", root, vector->component_name) <
      sizeof(link_path));
    TEST_ASSERT_EQ(0, symlink(vector->symlink_target, link_path));
    TEST_ASSERT_EQ(k_ra8_err_access_denied,
                   fw_fs_stat(&fs->names, vector->intermediate_path, &stat));
    directory      = (fw_fs_dir_t){};
    directory_work = (posix_test_workspace_t){};
    TEST_ASSERT_EQ(k_ra8_err_access_denied,
                   fw_fs_dir_open(&fs->names,
                                  vector->directory_path,
                                  &directory,
                                  directory_work.bytes,
                                  sizeof(directory_work.bytes)));
    TEST_ASSERT(directory.is_open == false);
    TEST_ASSERT_EQ(0, unlink(link_path));
  }
}

/**
 * @brief Prove an occupied transaction-stage name advances to the next ID
 *
 * @details
 * Pre-creates the exact stage leaf derived from the seeded counter,
 * then begins and aborts a transaction inside the fixture's private root.
 * This directly covers the retry path without depending on timing or shared
 * `/tmp` state from another test process.
 *
 * @param[in] fs Initialized POSIX filesystem facade.
 * @param[in,out] state Caller-owned POSIX adapter state.
 * @pre @p fs and @p state are non-null and refer to the same adapter.
 * @pre The fixture root contains neither named file used by this vector.
 * @post The transaction is inactive and both stage files are absent.
 * @post The transaction counter advances past the occupied stage name.
 * @note Not thread-safe; mutates the adapter counter and private fixture root.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_stage_collision(const fw_fs_t*       fs,
                                                        fw_fs_posix_state_t* state)
{
  static const uint8_t occupied[] = {'b', 'u', 's', 'y'};
  const int            fd         = openat(state->root_fd,
                                           "TX000101.TMP",
                                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                           (mode_t)k_posix_test_file_mode);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(sizeof(occupied), write(fd, occupied, sizeof(occupied)));
  TEST_ASSERT_EQ(0, close(fd));

  posix_test_workspace_t work = {};
  fw_fs_transaction_t    txn  = {};
  state->transaction_id       = k_posix_test_stage_seed;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/collision.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  TEST_ASSERT_EQ(k_posix_test_stage_next, state->transaction_id);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/TX000101.TMP"));
}

/**
 * @brief Prove final and intermediate symlinks are never followed
 *
 * @details
 * Builds one target directory and a symlink to it, verifies metadata remains
 * observable without traversal, then exercises final- and intermediate-link
 * rejection through the POSIX facade.
 *
 * @param[in] fs Initialized POSIX filesystem facade.
 * @param[in] root Native path backing @p fs.
 * @pre @p fs and @p root are non-null.
 * @pre @p root is an empty private test directory.
 * @post Every created symlink and directory is removed.
 * @post No stream or intermediate lookup follows a created symlink.
 * @note Not thread-safe; mutates names in the caller's private fixture root.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_symlinks(const fw_fs_t* fs, const char* root)
{
  char link_path[k_posix_test_path_bytes];
  char target_path[k_posix_test_path_bytes];
  TEST_ASSERT((size_t)snprintf(link_path, sizeof(link_path), "%s/link", root) < sizeof(link_path));
  TEST_ASSERT((size_t)snprintf(target_path, sizeof(target_path), "%s/target", root) <
              sizeof(target_path));
  TEST_ASSERT_EQ(0, mkdir(target_path, (mode_t)k_posix_test_dir_mode));
  TEST_ASSERT_EQ(0, symlink("target", link_path));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/link", &stat));
  TEST_ASSERT(stat.exists && stat.type == k_fw_fs_node_symlink);
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/link/file", &stat));
  posix_test_workspace_t work = {};
  fw_fs_file_t           file = {};
  TEST_ASSERT_EQ(
    k_ra8_err_access_denied,
    fw_fs_open(&fs->streams, "/link", k_fw_fs_open_read, &file, work.bytes, sizeof(work.bytes)));
  internal_check_intermediate_components(fs, root);
  TEST_ASSERT_EQ(0, unlink(link_path));
  TEST_ASSERT_EQ(0, rmdir(target_path));
}

RA8_TEST_HELPER void
ra8_test_fw_if_fs_posix_cases(const fw_fs_t* fs, fw_fs_posix_state_t* state, const char* root)
{
  TEST_ASSERT_NOT_NULL(fs);
  TEST_ASSERT_NOT_NULL(state);
  TEST_ASSERT_NOT_NULL(root);
  internal_check_init_pointer_guards(root);
  internal_check_public_lifecycle(root);
  internal_check_public_root_failures(root);
  internal_check_system_root("/tmp", "/private/tmp");
  internal_check_system_root("/var", "/private/var");
  internal_check_stage_collision(fs, state);
  internal_check_root_alias_classifier();
  internal_check_alias_parent_failures(state->root_fd);
  internal_check_alias_child_failures(state->root_fd);
  internal_check_alias_successes(state->root_fd);
  internal_check_symlinks(fs, root);
  ra8_test_fw_if_fs_posix_roots(root);
}
