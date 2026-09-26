/**
 * @file test_ra8_net_urlguard.c
 * @brief Unit tests for the URL and peer-address safety policy.
 * @details Exercises the scheme allowlist, the in-tree address literal parser
 *          and its classification, the private-space policy, the overflow-safe
 *          size cap, and the authority / path split.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_net_urlguard.h"
#include "unity_minimal.h"

/**
 * @enum urlguard_test_size_t
 * @brief Buffer sizes the fixtures use.
 */
typedef enum : uint16_t {
  k_urlguard_test_buf = 64U, /**< Roomy destination for a host or path. */
} urlguard_test_size_t;

/**
 * @brief Only http and https pass the scheme allowlist.
 * @details A refusal here is the first gate, ahead of any transport.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_scheme_allowlist(void)
{
  TEST_BEGIN("urlguard: only http and https pass the allowlist");
  TEST_ASSERT(ra8_net_urlguard_scheme_allowed("http://example.com/a"));
  TEST_ASSERT(ra8_net_urlguard_scheme_allowed("https://example.com/a"));
  TEST_ASSERT(ra8_net_urlguard_scheme_allowed("HTTPS://EXAMPLE.COM/a"));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed("file:///etc/passwd"));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed("ftp://example.com"));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed("data:text/plain,hi"));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed(""));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed(nullptr));
  TEST_ASSERT(!ra8_net_urlguard_scheme_allowed("http:/example.com"));
  TEST_END("urlguard: only http and https pass the allowlist");
}

/**
 * @brief IPv4 literals land in the right reachability bucket.
 * @details Covers every non-public range the guard names, and the boundaries
 *          on each side of the 172.16/12 and 100.64/10 windows.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_classify_v4(void)
{
  TEST_BEGIN("urlguard: IPv4 literals land in the right class");
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("93.184.216.34"));
  TEST_ASSERT_EQ(k_ra8_net_addr_loopback, ra8_net_urlguard_classify_ip("127.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_loopback, ra8_net_urlguard_classify_ip("127.255.255.254"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("10.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("172.16.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("172.31.255.254"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("172.15.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("172.32.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("192.168.1.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("192.167.1.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("100.64.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("100.127.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("100.63.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("100.128.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_linklocal, ra8_net_urlguard_classify_ip("169.254.169.254"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("169.253.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("0.0.0.0"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("224.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("255.255.255.255"));
  TEST_END("urlguard: IPv4 literals land in the right class");
}

/**
 * @brief Malformed address literals are unknown, never public.
 * @details The parser replaces the host's inet_pton, so the shapes it used to
 *          reject have to stay rejected: a leading zero, an over-range octet,
 *          a short quad, trailing text, and a zone identifier.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_classify_rejects_malformed(void)
{
  TEST_BEGIN("urlguard: malformed literals are unknown");
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip(nullptr));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip(""));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("example.com"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1.2.3"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1.2.3.4.5"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1.2.3.256"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1.2.3.4 "));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1.2.3.04"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("0177.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("fe80::1%eth0"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("12345::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1::2::3"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1:2:3:4:5:6:7"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1:2:3:4:5:6:7:8:9"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip(":1:2:3:4:5:6:7:8"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("1:2:3:4:5:6:7:8:"));
  TEST_END("urlguard: malformed literals are unknown");
}

/**
 * @brief IPv6 literals, including the mapped IPv4 form, classify correctly.
 * @details A mapped loopback must not reach public: that unwrap is the whole
 *          reason the mapped prefix is inspected before anything else.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_classify_v6(void)
{
  TEST_BEGIN("urlguard: IPv6 and mapped IPv4 classify correctly");
  TEST_ASSERT_EQ(k_ra8_net_addr_loopback, ra8_net_urlguard_classify_ip("::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("::"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public,
                 ra8_net_urlguard_classify_ip("2606:2800:220:1:248:1893:25c8:1946"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("2001:db8::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_linklocal, ra8_net_urlguard_classify_ip("fe80::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_linklocal, ra8_net_urlguard_classify_ip("febf::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("fc00::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("fd12:3456::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_unknown, ra8_net_urlguard_classify_ip("ff02::1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_loopback, ra8_net_urlguard_classify_ip("::ffff:127.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_private, ra8_net_urlguard_classify_ip("::ffff:10.0.0.1"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("::ffff:93.184.216.34"));
  TEST_ASSERT_EQ(k_ra8_net_addr_public, ra8_net_urlguard_classify_ip("::FFFF:93.184.216.34"));
  TEST_END("urlguard: IPv6 and mapped IPv4 classify correctly");
}

/**
 * @brief The private-space opt-in never makes an unknown address fetchable.
 * @details Public is always fetchable, the three private-space classes follow
 *          the flag, and unknown is refused under both settings.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_fetchable_policy(void)
{
  TEST_BEGIN("urlguard: unknown is never fetchable");
  TEST_ASSERT(ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_public, false));
  TEST_ASSERT(ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_public, true));
  TEST_ASSERT(!ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_loopback, false));
  TEST_ASSERT(ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_loopback, true));
  TEST_ASSERT(!ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_private, false));
  TEST_ASSERT(ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_private, true));
  TEST_ASSERT(!ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_linklocal, false));
  TEST_ASSERT(ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_linklocal, true));
  TEST_ASSERT(!ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_unknown, false));
  TEST_ASSERT(!ra8_net_urlguard_addr_fetchable(k_ra8_net_addr_unknown, true));
  TEST_END("urlguard: unknown is never fetchable");
}

/**
 * @brief The size cap is overflow-safe and treats zero as unlimited.
 * @details The running total never wraps, even when both operands are near
 *          the width of the type.
 * @pre The policy is stateless, so no fixture is required.
 * @post No state is modified.
 * @since 0.1.0
 */
static void test_size_cap(void)
{
  TEST_BEGIN("urlguard: the size cap cannot overflow");
  TEST_ASSERT(!ra8_net_urlguard_size_exceeds(0U, 0U, 0U));
  TEST_ASSERT(!ra8_net_urlguard_size_exceeds(UINT64_MAX, UINT64_MAX, 0U));
  TEST_ASSERT(!ra8_net_urlguard_size_exceeds(10U, 90U, 100U));
  TEST_ASSERT(ra8_net_urlguard_size_exceeds(10U, 91U, 100U));
  TEST_ASSERT(ra8_net_urlguard_size_exceeds(101U, 0U, 100U));
  TEST_ASSERT(ra8_net_urlguard_size_exceeds(UINT64_MAX - 1U, 2U, UINT64_MAX));
  TEST_ASSERT(!ra8_net_urlguard_size_exceeds(UINT64_MAX - 1U, 1U, UINT64_MAX));
  TEST_END("urlguard: the size cap cannot overflow");
}

/**
 * @brief The authority is lower-cased, keeps its port, and drops userinfo.
 * @details The port is part of the origin identity, so it survives; the
 *          userinfo is attacker-supplied decoration, so it does not.
 * @pre The policy is stateless, so no fixture is required.
 * @post Only caller-owned storage is written.
 * @since 0.1.0
 */
static void test_host_extraction(void)
{
  TEST_BEGIN("urlguard: authority keeps its port, drops userinfo");
  char out[k_urlguard_test_buf] = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_host("https://Example.COM/a/b?q=1", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "example.com") == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_host("http://example.com:8080/a", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "example.com:8080") == 0);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_net_urlguard_host("https://user:pw@evil.example/a", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "evil.example") == 0);

  /* An '@' after the path start belongs to the path, not the authority. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_host("https://example.com/a@b", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "example.com") == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_host("https://example.com", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "example.com") == 0);
  TEST_END("urlguard: authority keeps its port, drops userinfo");
}

/**
 * @brief Each authority refusal is distinguishable and empties the buffer.
 * @details The three refusals are the lift over the bool predicate this
 *          replaced: a bad call, no authority, and no room are different facts.
 * @pre The policy is stateless, so no fixture is required.
 * @post Only caller-owned storage is written.
 * @since 0.1.0
 */
static void test_host_refusals(void)
{
  TEST_BEGIN("urlguard: each authority refusal is distinct");
  char out[k_urlguard_test_buf] = {};
  char tiny[4]                  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_net_urlguard_host(nullptr, out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0');
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_net_urlguard_host("https://a.example", nullptr, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_net_urlguard_host("https://a.example", out, 0U));

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_net_urlguard_host("example.com/a", out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0');
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_net_urlguard_host("https:///a", out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0');

  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_net_urlguard_host("https://example.com/a", tiny, sizeof(tiny)));
  TEST_ASSERT(tiny[0] == '\0');
  TEST_END("urlguard: each authority refusal is distinct");
}

/**
 * @brief The path drops the query and fragment and defaults to a slash.
 * @details A rule set matches on the path only, so everything after it goes.
 * @pre The policy is stateless, so no fixture is required.
 * @post Only caller-owned storage is written.
 * @since 0.1.0
 */
static void test_path_extraction(void)
{
  TEST_BEGIN("urlguard: path drops query and fragment");
  char out[k_urlguard_test_buf] = {};
  char tiny[4]                  = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_path("https://example.com/a/b?q=1#f", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "/a/b") == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_path("https://example.com", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "/") == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_urlguard_path("https://example.com/", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "/") == 0);

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_net_urlguard_path("example.com/a", out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0');
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_net_urlguard_path(nullptr, out, sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_net_urlguard_path("https://example.com/abcdef", tiny, sizeof(tiny)));
  TEST_ASSERT(tiny[0] == '\0');
  TEST_END("urlguard: path drops query and fragment");
}

/**
 * @brief Test binary entry point.
 * @return 0 on success; a failing assertion exits non-zero first.
 * @pre None.
 * @post Every case above has run in order.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  test_scheme_allowlist();
  test_classify_v4();
  test_classify_rejects_malformed();
  test_classify_v6();
  test_fetchable_policy();
  test_size_cap();
  test_host_extraction();
  test_host_refusals();
  test_path_extraction();
  return 0;
}
