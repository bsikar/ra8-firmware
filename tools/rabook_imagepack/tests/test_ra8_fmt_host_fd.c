/**
 * @file test_ra8_fmt_host_fd.c
 * @brief Raw-fd source and durable sibling-transaction integration tests.
 * @details Exercises exact reads, symlink rejection, atomic replacement, and
 * abort cleanup inside one private temporary directory.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

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

/** @brief Record one failed integration expectation without aborting cleanup. */
#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (false)

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
  (void)memcpy(out, root, root_len);
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

/**
 * @brief Exercise exact reads, symlink rejection, commit, replacement, and abort.
 * @details Drives raw-fd source and durable transaction seams against a private host directory.
 * @pre Static failure counter is initialized for this process.
 * @pre `/tmp` supports regular files, symlinks, and same-directory rename.
 * @post All created descriptors, files, link, and directory are removed on passing paths.
 * @post Every failed expectation increments only the CHECK counter.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test(void)
{
  static const uint8_t source_bytes[k_test_data_len] = {'J', 'O', 'F', '1', '!'};
  static const uint8_t output_bytes[k_test_data_len] = {'n', 'e', 'w', '!', '\n'};
  char                 root[]                        = "/tmp/ra8-fmt-fd.XXXXXX";
  CHECK(mkdtemp(root) == root);
  char input[k_test_path_cap];
  char output[k_test_path_cap];
  char link[k_test_path_cap];
  char aborted[k_test_path_cap];
  CHECK(internal_path(input, root, "/input.jof"));
  CHECK(internal_path(output, root, "/output.jof"));
  CHECK(internal_path(link, root, "/link.jof"));
  CHECK(internal_path(aborted, root, "/aborted.jof"));
  CHECK(internal_create(input, source_bytes, sizeof(source_bytes)));

  ra8_fmt_host_source_t source = {.fd = -1};
  CHECK(priv_fmt_host_source_open(input, sizeof(source_bytes), &source) == k_ra8_ok);
  uint8_t readback[k_test_data_len] = {};
  size_t  got                       = 0U;
  CHECK(source.source.read_at(source.source.ctx, 0U, readback, sizeof(readback), &got) == k_ra8_ok);
  CHECK((got == sizeof(readback)) && (memcmp(readback, source_bytes, sizeof(readback)) == 0));
  priv_fmt_host_source_close(&source);

  CHECK(symlink(input, link) == 0);
  CHECK(priv_fmt_host_source_open(link, sizeof(source_bytes), &source) == k_ra8_err_access_denied);
  CHECK(internal_create(output, source_bytes, sizeof(source_bytes)));
  ra8_fmt_host_transaction_t state;
  ra8_fmt_transaction_t      transaction;
  CHECK(priv_fmt_host_transaction_begin(output, &state, &transaction) == k_ra8_ok);
  CHECK(transaction.ops->append(transaction.ctx, output_bytes, sizeof(output_bytes)) == k_ra8_ok);
  CHECK(transaction.ops->commit(transaction.ctx) == k_ra8_ok);
  CHECK(priv_fmt_host_source_open(output, sizeof(output_bytes), &source) == k_ra8_ok);
  got = 0U;
  CHECK(source.source.read_at(source.source.ctx, 0U, readback, sizeof(readback), &got) == k_ra8_ok);
  CHECK((got == sizeof(readback)) && (memcmp(readback, output_bytes, sizeof(readback)) == 0));
  priv_fmt_host_source_close(&source);

  CHECK(priv_fmt_host_transaction_begin(aborted, &state, &transaction) == k_ra8_ok);
  CHECK(transaction.ops->append(transaction.ctx, output_bytes, sizeof(output_bytes)) == k_ra8_ok);
  transaction.ops->abort(transaction.ctx);
  CHECK(access(aborted, F_OK) != 0);

  CHECK(unlink(link) == 0);
  CHECK(unlink(input) == 0);
  CHECK(unlink(output) == 0);
  CHECK(rmdir(root) == 0);
}

int main(void)
{
  internal_test();
  return s_failures;
}
