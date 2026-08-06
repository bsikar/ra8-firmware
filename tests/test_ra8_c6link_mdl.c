/**
 * @file test_ra8_c6link_mdl.c
 * @brief Unit tests for the media download RPC stub client.
 *
 * @par Tag
 * [Ring 4 / TEST] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_c6link_mdl.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_link(void)
{
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_download(nullptr, "https://x", "/sd/x"));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_url(void)
{
  ra8_c6link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_download(&link, nullptr, "/sd/x"));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_path(void)
{
  ra8_c6link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_download(&link, "https://x", nullptr));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_ok(void)
{
  ra8_c6link_t link = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_download(&link, "https://x", "/sd/x"));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_null_link(void)
{
  ra8_mdl_download_progress_t p = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_poll(nullptr, &p));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_null_progress(void)
{
  ra8_c6link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_poll(&link, nullptr));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_ok(void)
{
  ra8_c6link_t                link = {};
  ra8_mdl_download_progress_t p    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_poll(&link, &p));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_cancel_null_link(void)
{
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_cancel(nullptr));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_cancel_ok(void)
{
  ra8_c6link_t link = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(&link));
}

int32_t main(void)
{
  test_mdl_download_null_link();
  test_mdl_download_null_url();
  test_mdl_download_null_path();
  test_mdl_download_ok();
  test_mdl_poll_null_link();
  test_mdl_poll_null_progress();
  test_mdl_poll_ok();
  test_mdl_cancel_null_link();
  test_mdl_cancel_ok();
  return g_unity_failures;
}
