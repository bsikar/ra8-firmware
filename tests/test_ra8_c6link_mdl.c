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

#include "ra8_c6link_mdl.h"
#include "ra8_test.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_link(void)
{
  ra8_err_t rc = ra8_c6link_mdl_download(nullptr, "https://x", "/sd/x");
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_url(void)
{
  ra8_c6link_t link = {0};
  ra8_err_t    rc   = ra8_c6link_mdl_download(&link, nullptr, "/sd/x");
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_null_path(void)
{
  ra8_c6link_t link = {0};
  ra8_err_t    rc   = ra8_c6link_mdl_download(&link, "https://x", nullptr);
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_download_ok(void)
{
  ra8_c6link_t link = {0};
  ra8_err_t    rc   = ra8_c6link_mdl_download(&link, "https://x", "/sd/x");
  RA8_ASSERT_EQ(rc, k_ra8_ok);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_null_link(void)
{
  ra8_mdl_download_progress_t p  = {0};
  ra8_err_t                   rc = ra8_c6link_mdl_poll(nullptr, &p);
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_null_progress(void)
{
  ra8_c6link_t link = {0};
  ra8_err_t    rc   = ra8_c6link_mdl_poll(&link, nullptr);
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_poll_ok(void)
{
  ra8_c6link_t                link = {0};
  ra8_mdl_download_progress_t p    = {0};
  ra8_err_t                   rc   = ra8_c6link_mdl_poll(&link, &p);
  RA8_ASSERT_EQ(rc, k_ra8_ok);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_cancel_null_link(void)
{
  ra8_err_t rc = ra8_c6link_mdl_cancel(nullptr);
  RA8_ASSERT_EQ(rc, k_ra8_err_invalid_arg);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_mdl_cancel_ok(void)
{
  ra8_c6link_t link = {0};
  ra8_err_t    rc   = ra8_c6link_mdl_cancel(&link);
  RA8_ASSERT_EQ(rc, k_ra8_ok);
}

int main(void)
{
  RA8_TEST(test_mdl_download_null_link);
  RA8_TEST(test_mdl_download_null_url);
  RA8_TEST(test_mdl_download_null_path);
  RA8_TEST(test_mdl_download_ok);
  RA8_TEST(test_mdl_poll_null_link);
  RA8_TEST(test_mdl_poll_null_progress);
  RA8_TEST(test_mdl_poll_ok);
  RA8_TEST(test_mdl_cancel_null_link);
  RA8_TEST(test_mdl_cancel_ok);
  return ra8_test_summary();
}
