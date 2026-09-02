/**
 * @file test_mdl_net_curl.c
 * @brief Curl failure publication-preservation regression tests.
 * @details Owns the host-file fixture and bounded body sink used to prove a
 *          failed curl transfer preserves an existing destination.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "test_mdl_net_curl_internal.h"
#include "unity_minimal.h"

/** @brief Opaque POSIX-directory workspace capacity. */
typedef enum : uint16_t {
  k_atom_directory_work_bytes = 8192U, /**< Hosted cursor backend state. */
} mdl_atom_directory_limit_t;

/** @brief Maximally aligned storage for one injected directory cursor. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_atom_directory_work_bytes]; /**< Backend-private bytes. */
} mdl_atom_directory_workspace_t;

/* ---- a failed re-fetch must not destroy the file already on disk --------- */

/** @brief Fixture sizes for the atomic-write regression tests. */
typedef enum : uint16_t {
  k_atom_path_max   = 256,  /**< Fixture path buffer bytes.                  */
  k_atom_body_max   = 128,  /**< Fixture file-content buffer bytes.          */
  k_atom_timeout_ms = 5000, /**< Budget for the fetch that is meant to fail. */
} mdl_atom_test_const_t;

/**
 * @var s_atom_good
 * @brief The bytes a previously-downloaded, still-good page holds.
 * @details Distinctive so a truncation to zero bytes, a partial write, or a
 *          deletion are all distinguishable from "survived intact".
 * @note Read-only fixture data.
 * @since 0.1.0
 */
static const char s_atom_good[] = "GOOD-PAGE-BYTES";

/** @brief Minimal body sink state for a transfer expected to fail. */
typedef struct {
  uint32_t length; /**< Bytes observed before transfer failure. */
} atom_body_t;

/**
 * @brief Reset the atomic-publication test body sink.
 * @details Validates the callback context and clears the observed byte count
 * before the transfer-under-test begins.
 * @param[in,out] context Test-owned ::atom_body_t callback context.
 * @return Canonical callback status.
 * @retval k_ra8_ok The observed length was reset.
 * @retval k_ra8_err_invalid_arg @p context was null.
 * @pre A non-null context points to writable test state.
 * @pre The test owns the sink exclusively.
 * @post Success leaves the observed length at zero.
 * @post Failure does not access storage through the null context.
 * @note Host-test callback with no filesystem side effects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_atom_body_reset(void* context)
{
  atom_body_t* body = (atom_body_t*)context;
  if (body == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  body->length = 0U;
  return k_ra8_ok;
}

/**
 * @brief Count bytes accepted by the atomic-publication test sink.
 * @details Validates the callback tuple, advances the bounded observation
 * count, and truthfully reports the entire offered span as consumed.
 * @param[in,out] context Test-owned ::atom_body_t callback context.
 * @param[in] bytes Response bytes supplied by the network seam.
 * @param[in] length Number of offered bytes.
 * @param[out] out_written Number of bytes accepted by this invocation.
 * @return Canonical callback status.
 * @retval k_ra8_ok The complete offered span was counted.
 * @retval k_ra8_err_invalid_arg A callback argument was invalid.
 * @pre Nonzero @p length requires a readable @p bytes span.
 * @pre @p context and @p out_written point to writable test storage.
 * @post Success reports @p length through @p out_written.
 * @post Failure does not advance the observation count.
 * @note Payload contents are intentionally irrelevant to this test seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_atom_body_write(void*          context,
                                                       const uint8_t* bytes,
                                                       uint32_t       length,
                                                       uint32_t*      out_written)
{
  atom_body_t* body = (atom_body_t*)context;
  if ((body == nullptr) || ((bytes == nullptr) && (length > 0U)) || (out_written == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  body->length += length;
  *out_written = length;
  return k_ra8_ok;
}

/**
 * @brief Bind the local body state to the production sink contract.
 * @details Produces a non-owning callback table that accepts exact byte counts
 *          into the caller's failure-observation state.
 * @param[in,out] body Caller-owned body state.
 * @return Complete sink value borrowing @p body.
 * @retval nonzero A sink with reset and exact-write callbacks.
 * @pre @p body is non-NULL and exclusively owned by the caller.
 * @pre @p body remains live through every dispatched callback.
 * @post The returned sink borrows rather than owns @p body.
 * @post No callback has run when this helper returns.
 * @note Test-only stack binding.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_net_body_sink_t internal_atom_body_sink(atom_body_t* body)
{
  return (mdl_net_body_sink_t){.reset = internal_atom_body_reset,
                               .write = internal_atom_body_write,
                               .ctx   = body};
}

/** @brief Write `body` to `path`, returning whether every byte landed.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in] path Filesystem path used by this fixture operation.
 * @param[in] body Immutable fixture body bytes.
 * @return True when the helper condition succeeds; otherwise false.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_atom_write(const char* path, const char* body)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  const size_t length = strlen(body);
  size_t       offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &body[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  return (close(descriptor) == 0) && (offset == length);
}

/** @brief Read `path` into `out`; returns the byte count, or -1 when absent.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in] path Filesystem path used by this fixture operation.
 * @param[out] out Caller-owned destination for the helper result.
 * @param[in] cap Supplied capacity of the destination buffer, in bytes.
 * @return Value produced by the bounded test helper.
 * @retval 0 The helper produced its zero-valued boundary result.
 * @retval nonzero The helper produced its documented nonzero result.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static long internal_atom_read(const char* path, char* out, size_t cap)
{
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if ((descriptor < 0) || (cap == 0U)) {
    return -1;
  }
  size_t total = 0U;
  while (total < (cap - 1U)) {
    const ssize_t got = read(descriptor, &out[total], cap - 1U - total);
    if (got > 0) {
      total += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  out[total] = '\0';
  return (close(descriptor) == 0) ? (long)total : -1L;
}

/** @brief Whether `dir` still holds any `.mdl-tmp-` debris from a failed write.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in] dir Directory inspected for transaction debris.
 * @return True when the helper condition succeeds; otherwise false.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_atom_has_debris(const char* dir)
{
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = "/", .removable_media = false};
  if (fw_fs_posix_init(&fs, &state, &cfg) != k_ra8_ok) {
    return false;
  }
  mdl_atom_directory_workspace_t workspace;
  fw_fs_dir_t                    directory = {};
  bool                           found =
    fw_fs_dir_open(&fs.names, dir, &directory, workspace.bytes, sizeof(workspace.bytes)) !=
    k_ra8_ok;
  bool                 present = false;
  fw_fs_dirent_value_t entry   = {};
  while (!found && (fw_fs_dir_next(&directory, &entry, &present) == k_ra8_ok) && present) {
    if (strncmp(entry.name, ".mdl-tmp-", strlen(".mdl-tmp-")) == 0) {
      found = true;
    }
  }
  if (directory.is_open && (fw_fs_dir_close(&directory) != k_ra8_ok)) {
    found = true;
  }
  if (fw_fs_posix_deinit(&state) != k_ra8_ok) {
    found = true;
  }
  return found;
}

/**
 * @test internal_test_curl_get_body_failure_keeps_existing
 *
 * @par MC/DC:
 * Decision: `rc != k_ra8_ok` on the curl body transfer-result path (single
 * condition, N+1 = 2 vectors). This test drives the TRUE arm and asserts that
 * an unrelated existing destination is untouched; successful transactional
 * publication is covered by the integration suite.
 *
 * @details
 * The historical regression this exists for: `curl_get_file` opened `out_path`
 * directly with `fopen(..., "wb")`, so the previously-downloaded page was
 * TRUNCATED the instant the open succeeded, and the `remove(out_path)` on the
 * failure path then deleted the remains. A re-fetch that hit a connection error
 * therefore destroyed data the user already had. The fetch below is guaranteed
 * to fail (loopback port 1 refuses the connection, so no network is touched)
 * and the good file must come through it byte-for-byte, with no temp debris
 * left.
 * @brief Exercise the curl get body failure keeps existing media-downloader scenario.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_curl_get_body_failure_keeps_existing(void)
{
  TEST_BEGIN("curl get_body failure keeps existing");
  char tmpl[k_atom_path_max];
  (void)__builtin_snprintf(tmpl, sizeof(tmpl), "%s", "/tmp/mdl_refetch_XXXXXX");
  const char* dir = mkdtemp(tmpl);
  TEST_ASSERT(dir != nullptr);

  char dst[k_atom_path_max];
  char body[k_atom_body_max];
  (void)__builtin_snprintf(dst, sizeof(dst), "%s/page_0001.jpg", dir);
  TEST_ASSERT(internal_atom_write(dst, s_atom_good));

  /* allow_private_hosts so the SSRF guard does not reject the loopback URL
   * before libcurl ever runs -- the failure under test must be the TRANSFER
   * failing, not the request being refused up front. */
  const mdl_net_policy_t pol     = {.allow_private_hosts       = true,
                                    .allow_cross_host_redirect = false,
                                    .max_response_bytes        = 0U};
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  TEST_ASSERT(mdl_net_curl_init(&net, &storage, &pol) == k_ra8_ok);

  const mdl_net_req_t req      = {.user_agent = "mdl-test",
                                  .referer    = nullptr,
                                  .timeout_ms = (uint32_t)k_atom_timeout_ms};
  size_t              len      = 0U;
  mdl_net_resp_t      resp     = {};
  atom_body_t         received = {};
  mdl_net_body_sink_t sink     = internal_atom_body_sink(&received);
  /* Port 1 on loopback refuses immediately: deterministic, offline, fast. */
  const ra8_err_t rc =
    mdl_net_get_body(&net, "http://127.0.0.1:1/page.jpg", &req, &sink, &len, &resp);
  TEST_ASSERT(rc != k_ra8_ok);

  /* THE POINT: the file the user already had is still there, intact. */
  TEST_ASSERT_EQ((long)strlen(s_atom_good), internal_atom_read(dst, body, sizeof(body)));
  TEST_ASSERT(strcmp(body, s_atom_good) == 0);
  /* ...and the failed attempt left no half-downloaded sibling behind. */
  TEST_ASSERT(!internal_atom_has_debris(dir));

  mdl_net_destroy(&net);
  TEST_ASSERT(net.vtable == nullptr);
  TEST_ASSERT(net.ctx == nullptr);
  (void)unlink(dst);
  (void)rmdir(dir);
  TEST_END("curl get_body failure keeps existing");
}

/**
 * @brief Run the curl failed-publication preservation test group.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_net_curl_run(void)
{
  internal_test_curl_get_body_failure_keeps_existing();
}
