/**
 * @file test_media_dl_fetch_body.c
 * @brief Fault qualification for the streamed fetch publication boundary.
 * @details Qualifies body streaming, hashing, and transactional publication
 *          across success and injected storage-failure paths.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_fetch_body_internal.h"
#include "mdl_hash.h"
#include "mdl_state_fs_fault.h"
#include "unity_minimal.h"

/** @brief Fixed capacities and fixture byte counts. */
typedef enum : uint16_t {
  k_body_workspace_bytes = 8192U, /**< Opaque facade workspace extent. */
  k_body_io_bytes        = 256U,  /**< Validation scratch extent.      */
  k_body_fixture_bytes   = 24U,   /**< Captured PNG-like body extent.  */
} body_test_limit_t;

/** @brief Maximally aligned storage-facade workspace. */
typedef union {
  max_align_t alignment;                     /**< Enforce natural alignment. */
  uint8_t     bytes[k_body_workspace_bytes]; /**< Opaque backend state.      */
} body_test_workspace_t;

/** @brief One root-confined POSIX backend with an injected-fault facade. */
typedef struct {
  char                  root[PATH_MAX];        /**< Temporary root path.       */
  fw_fs_t               posix_fs;              /**< Real hosted backend.       */
  fw_fs_posix_state_t   posix_state;           /**< POSIX adapter state.       */
  mdl_state_fault_fs_t  fault;                 /**< Fault-injecting wrapper.   */
  mdl_storage_t         storage;               /**< Downloader storage bundle. */
  body_test_workspace_t file_workspace;        /**< Stream state.              */
  body_test_workspace_t transaction_workspace; /**< Transaction state.         */
  uint8_t               io[k_body_io_bytes];   /**< Validation scratch.        */
} body_test_fixture_t;

/** @brief Valid PNG signature plus distinctive captured payload bytes. */
static const uint8_t s_png[k_body_fixture_bytes] = {
  0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
  0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
};

/** @brief Bytes that must survive every prepublication failure. */
static const uint8_t s_old[] = {'O', 'L', 'D', '-', 'P', 'A', 'G', 'E'};

/**
 * @brief Initialize one isolated POSIX/fault/storage binding.
 * @details Creates a private root, binds POSIX, then wraps it for fault injection.
 * @param[out] fixture Fixture to initialize.
 * @return Whether all three bindings initialized.
 * @retval true The complete storage fixture is live.
 * @retval false Initialization failed and acquired resources were released.
 * @pre @p fixture is writable and inactive.
 * @pre The caller has exclusive ownership of @p fixture.
 * @post Success leaves an empty confined root and initialized storage.
 * @post Failure leaves no live root descriptor.
 * @note Test-only composition root; no production singleton is introduced.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_body_fixture_init(body_test_fixture_t* fixture)
{
  *fixture = (body_test_fixture_t){.posix_state = {.root_fd = -1}};
  (void)__builtin_snprintf(fixture->root, sizeof(fixture->root), "%s", "/tmp/mdl_body_XXXXXX");
  if (mkdtemp(fixture->root) == nullptr) {
    return false;
  }
  const fw_fs_posix_cfg_t cfg   = {.root_path = fixture->root, .removable_media = false};
  ra8_err_t               error = fw_fs_posix_init(&fixture->posix_fs, &fixture->posix_state, &cfg);
  if (error == k_ra8_ok) {
    error = mdl_state_fault_fs_init(&fixture->fault, &fixture->posix_fs);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_init(&fixture->storage,
                             &fixture->fault.fs,
                             fixture->file_workspace.bytes,
                             sizeof(fixture->file_workspace.bytes),
                             fixture->transaction_workspace.bytes,
                             sizeof(fixture->transaction_workspace.bytes),
                             fixture->io,
                             sizeof(fixture->io));
  }
  if (error != k_ra8_ok) {
    if (fixture->posix_state.root_fd >= 0) {
      (void)fw_fs_posix_deinit(&fixture->posix_state);
    }
    (void)rmdir(fixture->root);
  }
  return error == k_ra8_ok;
}

/**
 * @brief Release one fixture and require its root to contain no stage debris.
 * @details Deinitializes the adapter before asserting that its root is empty.
 * @param[in,out] fixture Initialized fixture.
 * @pre No live transaction remains unless its backend already consumed it.
 * @pre @p fixture was successfully initialized.
 * @post The POSIX root descriptor and empty temporary directory are removed.
 * @post No fixture resource remains usable.
 * @note Assertions make leaked anonymous stages visible.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_body_fixture_deinit(body_test_fixture_t* fixture)
{
  TEST_ASSERT(fw_fs_posix_deinit(&fixture->posix_state) == k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(fixture->root));
}

/**
 * @brief Write exact fixture bytes relative to the confined root.
 * @details Uses bounded retry-on-EINTR descriptor I/O followed by synchronization.
 * @param[in] fixture Initialized fixture.
 * @param[in] name Single-component relative path.
 * @param[in] bytes Source bytes.
 * @param[in] length Source extent.
 * @return Whether every byte was durably closed.
 * @retval true Exact bytes were written, synchronized, and closed.
 * @retval false An open, write, sync, or close operation failed.
 * @pre Inputs are valid and @p name is not a symlink.
 * @pre @p bytes covers @p length readable bytes.
 * @post Success replaces the named regular file with exact bytes.
 * @post The descriptor is closed on every path after a successful open.
 * @note Raw descriptors are used only at this test composition edge.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_body_file_write(const body_test_fixture_t* fixture,
                                                  const char*                name,
                                                  const uint8_t*             bytes,
                                                  size_t                     length)
{
  int fd = openat(fixture->posix_state.root_fd,
                  name,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  0600);
  if (fd < 0) {
    return false;
  }
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(fd, bytes + offset, length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  const bool synced = (offset == length) && (fsync(fd) == 0);
  return (close(fd) == 0) && synced;
}

/**
 * @brief Compare one confined regular file with exact expected bytes.
 * @details Reads the expected extent plus one EOF probe through a raw descriptor.
 * @param[in] fixture Initialized fixture.
 * @param[in] name Single-component relative path.
 * @param[in] bytes Expected bytes.
 * @param[in] length Exact expected extent.
 * @return Whether the file contains exactly the expectation and EOF.
 * @retval true Contents, extent, EOF, and close all matched.
 * @retval false The file or any comparison condition differed.
 * @pre Inputs are valid and length fits this test's bounded comparison.
 * @pre @p bytes covers @p length readable bytes.
 * @post The inspected descriptor is closed.
 * @post Neither the file nor expected bytes are modified.
 * @note Symlinks are rejected by the raw descriptor flags.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_body_file_equals(const body_test_fixture_t* fixture,
                                                   const char*                name,
                                                   const uint8_t*             bytes,
                                                   size_t                     length)
{
  uint8_t actual[k_body_fixture_bytes] = {};
  if (length > sizeof(actual)) {
    return false;
  }
  const int fd = openat(fixture->posix_state.root_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t got = read(fd, actual + offset, length - offset);
    if (got > 0) {
      offset += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  uint8_t       trailing = 0U;
  const ssize_t end      = read(fd, &trailing, 1U);
  const bool    closed   = close(fd) == 0;
  return closed && (offset == length) && (end == 0) && (memcmp(actual, bytes, length) == 0);
}

/**
 * @brief Reset and feed a response through arbitrary bounded chunk splits.
 * @details Invokes exactly the same reset/write callbacks exposed to networking.
 * @param[in,out] body Initialized body state.
 * @param[in] bytes Response bytes.
 * @param[in] length Response extent.
 * @param[in] chunk Maximum bytes per callback.
 * @return First reset/write/progress error.
 * @retval k_ra8_ok Every source byte was accepted exactly once.
 * @retval other Reset, write, or progress validation failed.
 * @pre @p chunk is nonzero and source bytes cover @p length.
 * @pre @p body is initialized and exclusively owned.
 * @post Success means the sink accepted exactly @p length bytes.
 * @post Failure publishes no destination.
 * @note Models libcurl's unconstrained callback chunk boundaries.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_body_feed(mdl_fetch_body_t* body, const uint8_t* bytes, uint32_t length, uint32_t chunk)
{
  mdl_net_body_sink_t sink   = priv_mdl_fetch_body_sink(body);
  ra8_err_t           error  = sink.reset(sink.ctx);
  uint32_t            offset = 0U;
  while ((error == k_ra8_ok) && (offset < length)) {
    const uint32_t count   = ((length - offset) < chunk) ? (length - offset) : chunk;
    uint32_t       written = 0U;
    error                  = sink.write(sink.ctx, bytes + offset, count, &written);
    if ((error == k_ra8_ok) && (written != count)) {
      error = k_ra8_err_invalid_state;
    }
    offset += (error == k_ra8_ok) ? written : 0U;
  }
  return error;
}

/**
 * @test internal_test_body_chunk_splits_short_writes_and_instances
 * @brief Prove byte identity across one-byte network and backend writes.
 * @details Uses two independently bound roots/body contexts, with the first
 *          backend forced to accept one byte per transaction write.
 * @pre Hosted POSIX temporary directories are available.
 * @pre Each fixture binding has independent caller-owned workspaces.
 * @post Both final files are exact and neither binding crosses into the other.
 * @post Both temporary roots and their files are removed.
 * @note Exercises the boundary around the sixteen-byte signature threshold.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_body_chunk_splits_short_writes_and_instances(void)
{
  TEST_BEGIN("fetch body chunks + two instances");
  body_test_fixture_t first;
  body_test_fixture_t second;
  TEST_ASSERT(internal_body_fixture_init(&first));
  TEST_ASSERT(internal_body_fixture_init(&second));
  first.fault.flags = (uint32_t)k_mdl_state_fault_short_write;

  mdl_fetch_body_t body_a = {};
  mdl_fetch_body_t body_b = {};
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body_a, &first.storage, "/page.jpg", "page.jpg") ==
              k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body_b, &second.storage, "/other.jpg", "other.jpg") ==
              k_ra8_ok);
  TEST_ASSERT(internal_body_feed(&body_a, s_png, sizeof(s_png), 1U) == k_ra8_ok);
  TEST_ASSERT(internal_body_feed(&body_b, s_png, sizeof(s_png), 7U) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_prepare(&body_a) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_prepare(&body_b) == k_ra8_ok);
  TEST_ASSERT(strcmp(body_a.actual_abs, "/page.png") == 0);
  TEST_ASSERT(strcmp(body_a.actual_rel, "page.png") == 0);
  TEST_ASSERT(strcmp(body_b.actual_abs, "/other.png") == 0);
  TEST_ASSERT(priv_mdl_fetch_body_commit(&body_a) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_commit(&body_b) == k_ra8_ok);
  TEST_ASSERT(internal_body_file_equals(&first, "page.png", s_png, sizeof(s_png)));
  TEST_ASSERT(internal_body_file_equals(&second, "other.png", s_png, sizeof(s_png)));

  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlinkat(first.posix_state.root_fd, "page.png", 0));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlinkat(second.posix_state.root_fd, "other.png", 0));
  internal_body_fixture_deinit(&first);
  internal_body_fixture_deinit(&second);
  TEST_END("fetch body chunks + two instances");
}

/**
 * @test internal_test_body_zero_and_magic_fail_closed
 * @brief Reject empty and unsupported response bodies without transaction begin.
 * @details Arms begin failure to prove neither rejection reaches transaction begin.
 * @pre A good prior destination exists and begin-fault injection is armed.
 * @pre The body state and storage binding are exclusively owned.
 * @post Both invalid responses preserve the prior bytes exactly.
 * @post No stage debris remains.
 * @note The empty vector models a successful 304/no-body sink lifecycle.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_body_zero_and_magic_fail_closed(void)
{
  TEST_BEGIN("fetch body empty + bad magic");
  body_test_fixture_t fixture;
  TEST_ASSERT(internal_body_fixture_init(&fixture));
  TEST_ASSERT(internal_body_file_write(&fixture, "page.png", s_old, sizeof(s_old)));
  fixture.fault.flags = (uint32_t)k_mdl_state_fault_begin;

  mdl_fetch_body_t body = {};
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body, &fixture.storage, "/page.jpg", "page.jpg") ==
              k_ra8_ok);
  TEST_ASSERT(internal_body_feed(&body, nullptr, 0U, 1U) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_prepare(&body) == k_ra8_err_invalid_size);
  TEST_ASSERT(!body.writer.transaction.active);

  static const uint8_t bad[k_mdl_fetch_magic_bytes] =
    {'n', 'o', 't', '-', 'a', 'n', '-', 'i', 'm', 'a', 'g', 'e', '-', 'x', 'x', 'x'};
  TEST_ASSERT(internal_body_feed(&body, bad, sizeof(bad), sizeof(bad)) ==
              k_ra8_err_validation_failed);
  TEST_ASSERT(!body.writer.transaction.active);
  TEST_ASSERT(internal_body_file_equals(&fixture, "page.png", s_old, sizeof(s_old)));

  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlinkat(fixture.posix_state.root_fd, "page.png", 0));
  internal_body_fixture_deinit(&fixture);
  TEST_END("fetch body empty + bad magic");
}

/**
 * @brief Run one prepublication transaction fault and prove preservation.
 * @details Selects one phase, streams a valid image, and observes exact cleanup.
 * @param[in,out] fixture Initialized fixture containing the old page.
 * @param[in] flag Independently selected transaction fault.
 * @param[in] expected Exact surfaced status.
 * @return Whether the vector surfaced @p expected and preserved old bytes.
 * @retval true Status, cleanup, and preservation all matched.
 * @retval false At least one required property differed.
 * @pre No transaction is active on entry.
 * @pre @p fixture contains the exact old-page fixture.
 * @post No transaction or changed destination remains on successful check.
 * @post The selected fault is cleared before return.
 * @note Begin/write faults occur during feed; validate/commit faults occur at commit.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_body_prepublication_fault(body_test_fixture_t*   fixture,
                                                            mdl_state_fault_flag_t flag,
                                                            ra8_err_t              expected)
{
  mdl_fetch_body_t body = {};
  if (priv_mdl_fetch_body_init_image(&body, &fixture->storage, "/page.jpg", "page.jpg") !=
      k_ra8_ok) {
    return false;
  }
  fixture->fault.flags = (uint32_t)flag;
  ra8_err_t error      = internal_body_feed(&body, s_png, sizeof(s_png), sizeof(s_png));
  if (error == k_ra8_ok) {
    error = priv_mdl_fetch_body_prepare(&body);
  }
  if (error == k_ra8_ok) {
    error = priv_mdl_fetch_body_commit(&body);
  } else {
    fixture->fault.flags    = 0U;
    const ra8_err_t aborted = priv_mdl_fetch_body_abort(&body);
    if (aborted != k_ra8_ok) {
      return false;
    }
  }
  fixture->fault.flags = 0U;
  return (error == expected) && !body.writer.transaction.active &&
         internal_body_file_equals(fixture, "page.png", s_old, sizeof(s_old));
}

/**
 * @test internal_test_body_transaction_faults_preserve_destination
 * @brief Qualify every prepublication transaction phase and cleanup failure.
 * @details Runs begin, write, validate, commit-before, and abort fault vectors.
 * @pre A valid existing destination is present in the isolated root.
 * @pre Fault flags are clear on entry.
 * @post Begin, write, validate, and precommit faults preserve it exactly.
 * @post Abort failure remains visible after the underlying stage is cleaned.
 * @note Each normal fault is independently selected.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_body_transaction_faults_preserve_destination(void)
{
  TEST_BEGIN("fetch body transaction faults");
  body_test_fixture_t fixture;
  TEST_ASSERT(internal_body_fixture_init(&fixture));
  TEST_ASSERT(internal_body_file_write(&fixture, "page.png", s_old, sizeof(s_old)));

  TEST_ASSERT(
    internal_body_prepublication_fault(&fixture, k_mdl_state_fault_begin, k_ra8_err_hw_error));
  TEST_ASSERT(internal_body_prepublication_fault(&fixture,
                                                 k_mdl_state_fault_transaction_write,
                                                 k_ra8_err_hw_error));
  TEST_ASSERT(internal_body_prepublication_fault(&fixture,
                                                 k_mdl_state_fault_validate,
                                                 k_ra8_err_validation_failed));
  TEST_ASSERT(internal_body_prepublication_fault(&fixture,
                                                 k_mdl_state_fault_commit_before,
                                                 k_ra8_err_hw_timeout));

  mdl_fetch_body_t body = {};
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body, &fixture.storage, "/page.jpg", "page.jpg") ==
              k_ra8_ok);
  fixture.fault.flags = (uint32_t)k_mdl_state_fault_transaction_write;
  TEST_ASSERT(internal_body_feed(&body, s_png, sizeof(s_png), sizeof(s_png)) == k_ra8_err_hw_error);
  fixture.fault.flags = (uint32_t)k_mdl_state_fault_abort;
  TEST_ASSERT(priv_mdl_fetch_body_abort(&body) == k_ra8_err_cancelled);
  TEST_ASSERT(internal_body_file_equals(&fixture, "page.png", s_old, sizeof(s_old)));
  fixture.fault.flags = 0U;

  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlinkat(fixture.posix_state.root_fd, "page.png", 0));
  internal_body_fixture_deinit(&fixture);
  TEST_END("fetch body transaction faults");
}

/**
 * @test internal_test_body_postcommit_truth_and_nonregular_rejection
 * @brief Preserve truthful postpublication status and reject unsafe targets.
 * @details Separates postcommit publication truth from pre-begin type rejection.
 * @pre The fault facade and isolated root are initialized.
 * @pre Its transaction workspace is inactive on entry.
 * @post Postcommit failure reports failure while retaining published exact bytes.
 * @post A directory destination is rejected before staging or mutation.
 * @note Publication truth is deliberately distinct from preservation faults.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_body_postcommit_truth_and_nonregular_rejection(void)
{
  TEST_BEGIN("fetch body commit truth + nonregular");
  body_test_fixture_t fixture;
  TEST_ASSERT(internal_body_fixture_init(&fixture));
  TEST_ASSERT(internal_body_file_write(&fixture, "page.png", s_old, sizeof(s_old)));

  mdl_fetch_body_t body = {};
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body, &fixture.storage, "/page.jpg", "page.jpg") ==
              k_ra8_ok);
  fixture.fault.flags = (uint32_t)k_mdl_state_fault_commit_after;
  TEST_ASSERT(internal_body_feed(&body, s_png, sizeof(s_png), 3U) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_prepare(&body) == k_ra8_ok);
  TEST_ASSERT(priv_mdl_fetch_body_commit(&body) == k_ra8_err_hw_timeout);
  TEST_ASSERT(!body.writer.transaction.active);
  TEST_ASSERT(internal_body_file_equals(&fixture, "page.png", s_png, sizeof(s_png)));
  fixture.fault.flags = 0U;
  TEST_ASSERT_EQ((int64_t)0, (int64_t)unlinkat(fixture.posix_state.root_fd, "page.png", 0));

  TEST_ASSERT_EQ((int64_t)0, (int64_t)mkdirat(fixture.posix_state.root_fd, "page.png", 0700));
  body = (mdl_fetch_body_t){};
  TEST_ASSERT(priv_mdl_fetch_body_init_image(&body, &fixture.storage, "/page.jpg", "page.jpg") ==
              k_ra8_ok);
  TEST_ASSERT(internal_body_feed(&body, s_png, sizeof(s_png), sizeof(s_png)) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(!body.writer.transaction.active);
  TEST_ASSERT_EQ((int64_t)0,
                 (int64_t)unlinkat(fixture.posix_state.root_fd, "page.png", AT_REMOVEDIR));

  internal_body_fixture_deinit(&fixture);
  TEST_END("fetch body commit truth + nonregular");
}

int main(void)
{
  internal_test_body_chunk_splits_short_writes_and_instances();
  internal_test_body_zero_and_magic_fail_closed();
  internal_test_body_transaction_faults_preserve_destination();
  internal_test_body_postcommit_truth_and_nonregular_rejection();
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_fetch_body.c\n",
              sizeof("[OK  ] test_media_dl_fetch_body.c\n") - 1U);
  return 0;
}
