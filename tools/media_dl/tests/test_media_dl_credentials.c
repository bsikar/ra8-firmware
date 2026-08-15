/**
 * @file test_media_dl_credentials.c
 * @brief Host tests for path-free cookie/CA policy and stable snapshots.
 * @details Exercises strict cookie grammar, hostile byte rejection, custom CA
 *          view validation, and same-size same-second mutation detection.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_host_credentials_internal.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"

/** @brief Deterministic metadata values for the mutation fixture. */
typedef enum : uint16_t {
  k_stat_second     = 10U, /**< Shared whole-second timestamp. */
  k_stat_mtime_nsec = 20U, /**< Initial modification nanosecond. */
  k_stat_ctime_nsec = 30U, /**< Initial change nanosecond. */
} credential_stat_fixture_t;

/**
 * @brief Initialise one real curl backend and verify its exact status.
 * @param[in] policy Caller-owned policy retained for the call.
 * @param[in] expected Expected canonical result.
 * @return Nothing.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 */
RA8_INTERNAL static void internal_expect_curl_policy(const mdl_net_policy_t* policy,
                                                     ra8_err_t               expected)
{
  mdl_net_iface_t        net     = {};
  mdl_net_curl_storage_t storage = {};
  TEST_ASSERT(mdl_net_curl_init(&net, &storage, policy) == expected);
  if (expected == k_ra8_ok) {
    mdl_net_destroy(&net);
  } else {
    TEST_ASSERT(net.vtable == nullptr);
    TEST_ASSERT(net.ctx == nullptr);
  }
}

/**
 * @test Caller cookie bytes accept the two explicitly supported row formats.
 * @brief Verify path-free cookie ingestion initializes a reusable curl handle.
 * @since 0.1.0
 * @details Exercises the cookie rows accepted scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 */
RA8_INTERNAL static void internal_test_cookie_rows_accepted(void)
{
  TEST_BEGIN("cookie rows accepted");
  static const uint8_t netscape[] = "# Netscape HTTP Cookie File\n"
                                    ".example.com\tTRUE\t/\tFALSE\t0\tsession\tvalue\n";
  static const uint8_t set_cookie[] =
    "Set-Cookie: session=value; Path=/; Domain=example.com; Secure\n";
  mdl_net_policy_t policy = {.cookies = {.data = netscape, .length = sizeof(netscape) - 1U}};
  internal_expect_curl_policy(&policy, k_ra8_ok);
  policy.cookies = (mdl_net_bytes_t){.data = set_cookie, .length = sizeof(set_cookie) - 1U};
  internal_expect_curl_policy(&policy, k_ra8_ok);
  TEST_END("cookie rows accepted");
}

/**
 * @brief Assert that one caller cookie byte sequence fails closed.
 * @param[in] data Hostile bounded bytes.
 * @param[in] length Readable byte count.
 * @return Nothing.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 */
RA8_INTERNAL static void internal_expect_cookie_rejected(const uint8_t* data, size_t length)
{
  const mdl_net_policy_t policy = {.cookies = {.data = data, .length = length}};
  internal_expect_curl_policy(&policy, k_ra8_err_invalid_arg);
}

/**
 * @test Commands, CTLs, non-ASCII, malformed names, and ambiguous CA fail closed.
 * @brief Cover credential validation before any network transfer begins.
 * @since 0.1.0
 * @details Exercises the credential rejection scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 */
RA8_INTERNAL static void internal_test_credential_rejection(void)
{
  TEST_BEGIN("credential rejection");
  static const uint8_t command[]      = "ALL\n";
  static const uint8_t no_domain[]    = "Set-Cookie: session=value; Path=/\n";
  static const uint8_t empty_name[]   = "Set-Cookie: =value; Domain=example.com\n";
  static const uint8_t embedded_cr[]  = "Set-Cookie: name=x;\r Domain=example.com\n";
  static const uint8_t embedded_nul[] = {'x', 0U, 'y'};
  static const uint8_t delete_row[]   = {'S', 'e', 't', '-', 'C',   'o', 'o', 'k', 'i',
                                         'e', ':', ' ', 'n', 0x7FU, '=', 'x', ';', ' ',
                                         'D', 'o', 'm', 'a', 'i',   'n', '=', 'x'};
  internal_expect_cookie_rejected(command, sizeof(command) - 1U);
  internal_expect_cookie_rejected(no_domain, sizeof(no_domain) - 1U);
  internal_expect_cookie_rejected(empty_name, sizeof(empty_name) - 1U);
  internal_expect_cookie_rejected(embedded_cr, sizeof(embedded_cr) - 1U);
  internal_expect_cookie_rejected(embedded_nul, sizeof(embedded_nul));
  internal_expect_cookie_rejected(delete_row, sizeof(delete_row));

  static const uint8_t empty_ca_marker = 1U;
  static const uint8_t nul_ca[]        = {'-', '-', 0U, 'x'};
  mdl_net_policy_t     policy          = {.ca_pem = {.data = &empty_ca_marker, .length = 0U}};
  internal_expect_curl_policy(&policy, k_ra8_err_invalid_arg);
  policy.ca_pem = (mdl_net_bytes_t){.data = nul_ca, .length = sizeof(nul_ca)};
  internal_expect_curl_policy(&policy, k_ra8_err_invalid_arg);
  TEST_END("credential rejection");
}

/**
 * @test Same-size same-second credential mutation is rejected at nanoseconds.
 * @brief Prove both mtime and ctime subsecond fields participate in stability.
 * @since 0.1.0
 * @details Exercises the subsecond mutation rejected scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 */
RA8_INTERNAL static void internal_test_subsecond_mutation_rejected(void)
{
  TEST_BEGIN("subsecond credential mutation");
  struct stat before = {.st_dev = 1, .st_ino = 2, .st_mode = S_IFREG, .st_size = 4};
#if defined(__APPLE__)
  before.st_mtimespec = (struct timespec){.tv_sec = k_stat_second, .tv_nsec = k_stat_mtime_nsec};
  before.st_ctimespec = (struct timespec){.tv_sec = k_stat_second, .tv_nsec = k_stat_ctime_nsec};
#else
  before.st_mtim = (struct timespec){.tv_sec = k_stat_second, .tv_nsec = k_stat_mtime_nsec};
  before.st_ctim = (struct timespec){.tv_sec = k_stat_second, .tv_nsec = k_stat_ctime_nsec};
#endif
  struct stat after = before;
  TEST_ASSERT(priv_mdl_host_credential_stat_unchanged(&before, &after));
#if defined(__APPLE__)
  ++after.st_mtimespec.tv_nsec;
#else
  ++after.st_mtim.tv_nsec;
#endif
  TEST_ASSERT(!priv_mdl_host_credential_stat_unchanged(&before, &after));
  after = before;
#if defined(__APPLE__)
  ++after.st_ctimespec.tv_nsec;
#else
  ++after.st_ctim.tv_nsec;
#endif
  TEST_ASSERT(!priv_mdl_host_credential_stat_unchanged(&before, &after));
  TEST_END("subsecond credential mutation");
}

int32_t main(void)
{
  internal_test_cookie_rows_accepted();
  internal_test_credential_rejection();
  internal_test_subsecond_mutation_rejected();
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_credentials.c\n",
              sizeof("[OK  ] test_media_dl_credentials.c\n") - 1U);
  return 0;
}
