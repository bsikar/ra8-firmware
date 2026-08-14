/**
 * @file test_media_dl_cli.c
 * @brief Standalone command grammar and mode-matrix qualification.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "mdl_cli.h"
#include "unity_minimal.h"

/** @brief Parse one vector and require its expected validated mode. */
static void expect_mode(int argc, char** argv, mdl_cli_mode_t expected)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  mdl_cli_parse(argc, argv, &args);
  TEST_ASSERT(mdl_cli_validate(&args, &mode));
  TEST_ASSERT_EQ((uint8_t)expected, (uint8_t)mode);
  TEST_ASSERT(strcmp(mdl_cli_mode_name(mode), "invalid") != 0);
}

/** @brief Parse one vector and require a usage rejection. */
static void expect_invalid(int argc, char** argv)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_page;
  mdl_cli_parse(argc, argv, &args);
  TEST_ASSERT(!mdl_cli_validate(&args, &mode));
  TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_invalid, (uint8_t)mode);
}

/** @test Every primary command has exactly one accepted minimal spelling. */
static void test_all_modes(void)
{
  TEST_BEGIN("CLI all primary modes");
  char* series[] = {"media_dl", "--config", "site.conf", "--series", "https://x/book"};
  expect_mode(5, series, k_mdl_cli_mode_series);
  char* search[] = {"media_dl", "--config", "site.conf", "--search", "title"};
  expect_mode(5, search, k_mdl_cli_mode_search);
  char* browse[] = {"media_dl", "--config", "site.conf", "--browse"};
  expect_mode(4, browse, k_mdl_cli_mode_browse);
  char* list[] = {"media_dl", "--list"};
  expect_mode(2, list, k_mdl_cli_mode_list);
  char* update_all[] = {"media_dl", "--update-all", "--config", "site.conf"};
  expect_mode(4, update_all, k_mdl_cli_mode_update_all);
  char* remove[] = {"media_dl", "--remove", "slug"};
  expect_mode(3, remove, k_mdl_cli_mode_remove);
  char* verify[] = {"media_dl", "--verify"};
  expect_mode(2, verify, k_mdl_cli_mode_verify);
  char* init[] = {"media_dl", "--init-site", "https://x/series"};
  expect_mode(3, init, k_mdl_cli_mode_init_site);
  char* pack[] = {"media_dl", "--pack", "pages", "--format", "cbz"};
  expect_mode(5, pack, k_mdl_cli_mode_pack);
  char* artifact[] = {"media_dl", "https://x/book.INCOMPLETE.cbt.gz"};
  expect_mode(2, artifact, k_mdl_cli_mode_artifact);
  char* page[] = {"media_dl", "https://x/page"};
  expect_mode(2, page, k_mdl_cli_mode_page);
  char* help[] = {"media_dl", "--help"};
  expect_mode(2, help, k_mdl_cli_mode_help);
  char* short_help[] = {"media_dl", "-h"};
  expect_mode(2, short_help, k_mdl_cli_mode_help);
  char* version[] = {"media_dl", "--version"};
  expect_mode(2, version, k_mdl_cli_mode_version);
  TEST_END("CLI all primary modes");
}

/** @test Network/download controls are accepted only when they have an effect. */
static void test_allowlists(void)
{
  TEST_BEGIN("CLI option allowlists");
  char* series_all[] = {"media_dl",
                        "--config",
                        "s",
                        "--series",
                        "https://x/s",
                        "--out",
                        "d",
                        "--chapters",
                        "2",
                        "--from",
                        "1",
                        "--format",
                        "cbz",
                        "--separate",
                        "--update",
                        "--allow-incomplete",
                        "--progress",
                        "--refetch",
                        "--seed",
                        "2",
                        "--timeout",
                        "50",
                        "--contact",
                        "me",
                        "--max-bytes",
                        "99",
                        "--cookie-file",
                        "cookies",
                        "--polite",
                        "--ignore-robots",
                        "--allow-private",
                        "--cross-host",
                        "--proxy",
                        "http://127.0.0.1:1"};
  expect_mode((int)(sizeof(series_all) / sizeof(series_all[0])), series_all, k_mdl_cli_mode_series);
  char* picked[] = {"media_dl",
                    "--config",
                    "s",
                    "--search",
                    "x",
                    "--pick",
                    "1",
                    "--format",
                    "epub",
                    "--chapters",
                    "1"};
  expect_mode((int)(sizeof(picked) / sizeof(picked[0])), picked, k_mdl_cli_mode_search);
  char* unpicked_download_opt[] =
    {"media_dl", "--config", "s", "--search", "x", "--format", "epub"};
  expect_invalid(7, unpicked_download_opt);
  char* list_timeout[] = {"media_dl", "--list", "--timeout", "1"};
  expect_invalid(4, list_timeout);
  char* artifact_attr[] = {"media_dl", "https://x/book.cbz", "--attr", "src"};
  expect_invalid(4, artifact_attr);
  TEST_END("CLI option allowlists");
}

/** @test Ambiguous, duplicated, missing, and inconsistent forms are rejected. */
static void test_usage_errors(void)
{
  TEST_BEGIN("CLI usage errors");
  char* no_mode[] = {"media_dl", "--out", "x"};
  expect_invalid(3, no_mode);
  char* mixed[] = {"media_dl", "--list", "--browse", "--config", "s"};
  expect_invalid(5, mixed);
  char* duplicate[] = {"media_dl", "--list", "--list"};
  expect_invalid(3, duplicate);
  char* missing_value[] = {"media_dl", "--config", "--series", "https://x/s"};
  expect_invalid(4, missing_value);
  char* pack_no_format[] = {"media_dl", "--pack", "pages"};
  expect_invalid(3, pack_no_format);
  char* proxy_no_escape[] = {"media_dl", "https://x/p", "--proxy", "http://p"};
  expect_invalid(4, proxy_no_escape);
  char* two_proxies[] =
    {"media_dl", "https://x/p", "--proxy", "http://p", "--socks5", "socks5://p", "--allow-private"};
  expect_invalid(7, two_proxies);
  char* bad_attr[] = {"media_dl", "https://x/p", "--attr", "href"};
  expect_invalid(4, bad_attr);
  char* help_plus[] = {"media_dl", "--help", "--version"};
  expect_invalid(3, help_plus);
  char* insecure_artifact[] = {"media_dl", "http://x/book.epub"};
  expect_invalid(2, insecure_artifact);
  TEST_END("CLI usage errors");
}

/** @test Numeric zero/range contracts are strict after grammar validation. */
static void test_numeric_contracts(void)
{
  TEST_BEGIN("CLI numeric contracts");
  mdl_nums_t nums;
  mdl_args_t zero_timeout = {.timeout = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&zero_timeout, &nums));
  mdl_args_t zero_chapters = {.chapters = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&zero_chapters, &nums));
  mdl_args_t zero_bytes = {.max_bytes = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&zero_bytes, &nums));
  mdl_args_t zero_pick = {.pick = "0"};
  TEST_ASSERT(!mdl_cli_parse_nums(&zero_pick, &nums));
  mdl_args_t valid = {.timeout = "1", .chapters = "1", .max_bytes = "1", .pick = "1"};
  TEST_ASSERT(mdl_cli_parse_nums(&valid, &nums));
  TEST_ASSERT_EQ((uint32_t)1, nums.timeout);
  TEST_ASSERT_EQ((size_t)1, nums.chapters);
  TEST_ASSERT_EQ((size_t)1, nums.pick);
  TEST_END("CLI numeric contracts");
}

int32_t main(void)
{
  test_all_modes();
  test_allowlists();
  test_usage_errors();
  test_numeric_contracts();
  return 0;
}
