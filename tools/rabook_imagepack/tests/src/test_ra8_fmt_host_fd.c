/**
 * @file test_ra8_fmt_host_fd.c
 * @brief Raw-fd source and durable sibling-transaction integration tests.
 * @details Exercises exact reads, symlink rejection, atomic replacement, and
 * abort cleanup inside one private temporary directory.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

/** @brief Request the POSIX.1-2008 declarations this test calls. */
/* glibc fixes the spelling of its feature-test macros, so the
 * reserved-identifier rule cannot apply: openat() and friends are only
 * declared when _POSIX_C_SOURCE precedes the first system header. */
#define _POSIX_C_SOURCE (200809L)

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"

/** @brief Test capacities and file modes. */
typedef enum : uint32_t {
  k_test_path_cap = 128U,  /**< Complete temporary path capacity. */
  k_test_data_len = 5U,    /**< Fixture artifact length.          */
  k_test_mode     = 0600U, /**< Test-file permissions.            */
} test_const_t;

static int s_failures;

/**
 * @brief Record one integration expectation without aborting cleanup.
 * @details A failed expectation only advances the counter `main` returns, so
 *          every remaining step still runs and still removes its fixtures.
 * @param[in] condition Result of the expectation.
 * @pre The failure counter is initialized for this process.
 * @pre The caller is the single test thread.
 * @post A false condition has advanced the counter by exactly one.
 * @post A true condition has changed nothing.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_check(bool condition)
{
  if (!condition) {
    s_failures++;
  }
}

/**
 * @brief Join a temporary root and constant suffix without formatting APIs.
 * @details Checks the complete NUL-terminated result before copying either slice.
 * @param[out] out Fixed-capacity path buffer.
 * @param[in] root NUL-terminated temporary directory.
 * @param[in] suffix NUL-terminated suffix beginning with slash.
 * @return Whether the complete joined path fit.
 * @retval true @p out contains the complete joined path.
 * @retval false Fixed capacity was insufficient.
 * @pre All pointers are non-null and input strings are NUL-terminated.
 * @pre @p out spans ::k_test_path_cap bytes.
 * @post Success initializes one NUL-terminated path.
 * @post Failure leaves output bytes unchanged.
 * @note Test-only and thread-safe for independent outputs.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_path(char out[k_test_path_cap], const char* root, const char* suffix)
{
  const size_t root_len   = strlen(root);
  const size_t suffix_len = strlen(suffix);
  if ((root_len + suffix_len + 1U) > k_test_path_cap) {
    return false;
  }
  (void)memcpy(out, root, root_len + 1U);
  (void)memcpy(&out[root_len], suffix, suffix_len + 1U);
  return true;
}

/**
 * @brief Write an exact fixture file through a raw descriptor.
 * @details Creates/truncates the path, requires one complete small write, and closes it.
 * @param[in] path NUL-terminated fixture path.
 * @param[in] bytes Fixture byte span.
 * @param[in] len Exact fixture length.
 * @return Whether open, complete write, and close succeeded.
 * @retval true File contains exactly the requested fixture bytes.
 * @retval false A host operation failed.
 * @pre @p path and @p bytes are non-null and valid for @p len.
 * @pre Parent directory exists and is writable.
 * @post Descriptor opened here is closed on every path.
 * @post Success replaces any prior fixture at @p path.
 * @note Test-only helper for small single-write fixtures.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_create(const char* path, const uint8_t* bytes, size_t len)
{
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)k_test_mode);
  if (fd < 0) {
    return false;
  }
  const bool okay = write(fd, bytes, len) == (ssize_t)len;
  return (close(fd) == 0) && okay;
}

/** @brief The five fixture bytes every temporary source artifact is built from. */
static const uint8_t s_source_bytes[k_test_data_len] = {'J', 'O', 'F', '1', '!'};

/** @brief The five bytes one committed transaction must publish. */
static const uint8_t s_output_bytes[k_test_data_len] = {'n', 'e', 'w', '!', '\n'};

/**
 * @brief Prove an exact raw-fd read succeeds and a symlink source is refused.
 * @details Opens the fixture through the raw-descriptor source seam, requires
 *          the whole artifact back in one read, then proves the same open
 *          refuses a symlink pointing at that very file.
 * @param[in] input NUL-terminated path of the created fixture.
 * @param[in] link NUL-terminated path the symlink is created at.
 * @pre @p input names an existing fixture of ::s_source_bytes.
 * @pre No file exists at @p link.
 * @post Every descriptor opened here is closed.
 * @post A symlink exists at @p link for the caller to remove.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_check_source_reads(const char* input, const char* link)
{
  ra8_fmt_host_source_t source                    = {.fd = -1};
  uint8_t               readback[k_test_data_len] = {};
  size_t                got                       = 0U;
  internal_check(priv_fmt_host_source_open(input, sizeof(s_source_bytes), &source) == k_ra8_ok);
  internal_check(source.source.read_at(source.source.ctx, 0U, readback, sizeof(readback), &got) ==
                 k_ra8_ok);
  internal_check(got == sizeof(readback));
  internal_check(memcmp(readback, s_source_bytes, sizeof(readback)) == 0);
  priv_fmt_host_source_close(&source);

  internal_check(symlink(input, link) == 0);
  internal_check(priv_fmt_host_source_open(link, sizeof(s_source_bytes), &source) ==
                 k_ra8_err_access_denied);
}

/**
 * @brief Prove a committed transaction replaces an existing artifact exactly.
 * @details Creates the destination with the source fixture, appends the output
 *          fixture through one transaction, commits it, and reads the whole
 *          published artifact back through the raw-descriptor source seam.
 * @param[in] output NUL-terminated destination path.
 * @pre The parent directory exists and is writable.
 * @pre No transaction is open on @p output.
 * @post @p output holds exactly ::s_output_bytes.
 * @post Every descriptor opened here is closed.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_check_commit_replaces(const char* output)
{
  ra8_fmt_host_transaction_t state;
  ra8_fmt_transaction_t      transaction;
  internal_check(internal_create(output, s_source_bytes, sizeof(s_source_bytes)));
  internal_check(priv_fmt_host_transaction_begin(output, &state, &transaction) == k_ra8_ok);
  internal_check(transaction.ops->append(transaction.ctx, s_output_bytes, sizeof(s_output_bytes)) ==
                 k_ra8_ok);
  internal_check(transaction.ops->commit(transaction.ctx) == k_ra8_ok);

  ra8_fmt_host_source_t source                    = {.fd = -1};
  uint8_t               readback[k_test_data_len] = {};
  size_t                got                       = 0U;
  internal_check(priv_fmt_host_source_open(output, sizeof(s_output_bytes), &source) == k_ra8_ok);
  internal_check(source.source.read_at(source.source.ctx, 0U, readback, sizeof(readback), &got) ==
                 k_ra8_ok);
  internal_check(got == sizeof(readback));
  internal_check(memcmp(readback, s_output_bytes, sizeof(readback)) == 0);
  priv_fmt_host_source_close(&source);
}

/**
 * @brief Prove an aborted transaction publishes nothing at its destination.
 * @details Begins a transaction, appends the whole output fixture, aborts, and
 *          requires the destination path still not to exist.
 * @param[in] aborted NUL-terminated destination path that must stay absent.
 * @pre No file exists at @p aborted.
 * @pre The parent directory exists and is writable.
 * @post No file exists at @p aborted.
 * @post Every temporary object the transaction created is destroyed.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_check_abort_publishes_nothing(const char* aborted)
{
  ra8_fmt_host_transaction_t state;
  ra8_fmt_transaction_t      transaction;
  internal_check(priv_fmt_host_transaction_begin(aborted, &state, &transaction) == k_ra8_ok);
  internal_check(transaction.ops->append(transaction.ctx, s_output_bytes, sizeof(s_output_bytes)) ==
                 k_ra8_ok);
  transaction.ops->abort(transaction.ctx);
  internal_check(access(aborted, F_OK) != 0);
}

/**
 * @brief Exercise exact reads, symlink rejection, commit, replacement, and abort.
 * @details Creates one private host directory, derives every fixture path in
 *          it, and drives the three scenario steps against them in order.
 * @pre The failure counter is initialized for this process.
 * @pre `/tmp` supports regular files, symlinks, and same-directory rename.
 * @post All created descriptors, files, link, and directory are removed on
 *       passing paths.
 * @post Every failed expectation only advances the failure counter.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test(void)
{
  static const char root_template[] = "/tmp/ra8-fmt-fd.XXXXXX";
  char              root[sizeof(root_template)];
  (void)memcpy(root, root_template, sizeof(root));
  internal_check(mkdtemp(root) == root);
  char input[k_test_path_cap];
  char output[k_test_path_cap];
  char link[k_test_path_cap];
  char aborted[k_test_path_cap];
  internal_check(internal_path(input, root, "/input.jof"));
  internal_check(internal_path(output, root, "/output.jof"));
  internal_check(internal_path(link, root, "/link.jof"));
  internal_check(internal_path(aborted, root, "/aborted.jof"));
  internal_check(internal_create(input, s_source_bytes, sizeof(s_source_bytes)));

  internal_check_source_reads(input, link);
  internal_check_commit_replaces(output);
  internal_check_abort_publishes_nothing(aborted);

  internal_check(unlink(link) == 0);
  internal_check(unlink(input) == 0);
  internal_check(unlink(output) == 0);
  internal_check(rmdir(root) == 0);
}

int main(void)
{
  internal_test();
  return s_failures;
}
