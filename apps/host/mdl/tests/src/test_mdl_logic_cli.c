/**
 * @file test_mdl_logic_cli.c
 * @brief Strict CLI validation tests split from the media logic suite.
 * @details Owns parsing, mode allowlist, conflict, and numeric-boundary cases
 *          while the primary translation unit retains parser and robots logic.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "mdl_cli.h"
#include "ra8_io_stream_ram.h"
#include "test_mdl_logic_cli_internal.h"
#include "unity_minimal.h"

/** @brief Diagnostic RAM extent for one CLI validation call. */
typedef enum : uint16_t {
  k_cli_diagnostic_bytes = 512U, /**< Injected diagnostic sink bytes. */
} mdl_logic_cli_limit_t;

/**
 * @brief Validate one CLI vector through an isolated bounded diagnostic sink.
 * @param[in] args Parsed command arguments.
 * @param[out] mode Receives the selected mode on success.
 * @return Canonical CLI validation status.
 * @pre @p args and @p mode are non-NULL.
 * @pre @p args was populated by ::mdl_cli_parse.
 * @post The diagnostic sink and its bytes do not escape the call.
 * @post @p mode follows the public validation contract.
 * @note Test helper with fixed stack storage and no host stream dependency.
 * @since 0.1.0
 * @details Exercises the cli validate scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 */
RA8_INTERNAL static ra8_err_t internal_test_cli_validate(const mdl_args_t* args,
                                                         mdl_cli_mode_t*   mode)
{
  uint8_t                   bytes[k_cli_diagnostic_bytes];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  const ra8_err_t           init   = ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes));
  return (init == k_ra8_ok) ? mdl_cli_validate(args, &stream, mode) : init;
}

/**
 * @brief Parse numeric CLI values through an isolated diagnostic sink.
 * @param[in] args Parsed command arguments.
 * @param[out] nums Receives values only on success.
 * @return Canonical numeric CLI status.
 * @pre @p args and @p nums are non-NULL.
 * @pre @p args was populated by ::mdl_cli_parse or an equivalent fixture.
 * @post The diagnostic sink and its bytes do not escape the call.
 * @post Failure preserves @p nums according to the public contract.
 * @note Test helper with fixed stack storage and no host stream dependency.
 * @since 0.1.0
 * @details Exercises the cli parse nums scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 */
RA8_INTERNAL static ra8_err_t internal_test_cli_parse_nums(const mdl_args_t* args, mdl_nums_t* nums)
{
  uint8_t                   bytes[k_cli_diagnostic_bytes];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  const ra8_err_t           init   = ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes));
  return (init == k_ra8_ok) ? mdl_cli_parse_nums(args, &stream, nums) : init;
}

/**
 * @test Test new CLI flags (--proxy, --socks5, --cookie-file, --progress, --verify, --init-site).
 * @brief Exercise the cli new flags regression scenario.
 * @details Executes the cli new flags scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_new_flags(void)
{
  TEST_BEGIN("cli new flags parsing");
  char* argv[] = {"mdl",
                  "--proxy",
                  "http://proxy.example.com:8080",
                  "--socks5",
                  "socks5://127.0.0.1:1080",
                  "--cookie-file",
                  "/tmp/cookies.txt",
                  "--cache-dir",
                  "/tmp/http-cache",
                  "--progress",
                  "--refetch",
                  "--init-site",
                  "https://example.com/manga/",
                  "--verify",
                  "/tmp/downloads"};
  int   argc   = sizeof(argv) / sizeof(argv[0]);

  mdl_args_t a = {};
  mdl_cli_parse(argc, argv, &a);

  TEST_ASSERT(!a.bad);
  TEST_ASSERT(a.proxy != nullptr && strcmp(a.proxy, "http://proxy.example.com:8080") == 0);
  TEST_ASSERT(a.socks5 != nullptr && strcmp(a.socks5, "socks5://127.0.0.1:1080") == 0);
  TEST_ASSERT(a.cookie_file != nullptr && strcmp(a.cookie_file, "/tmp/cookies.txt") == 0);
  TEST_ASSERT(a.cache_dir != nullptr && strcmp(a.cache_dir, "/tmp/http-cache") == 0);
  TEST_ASSERT(a.progress == true);
  TEST_ASSERT(a.refetch == true);
  TEST_ASSERT(a.init_site_url != nullptr &&
              strcmp(a.init_site_url, "https://example.com/manga/") == 0);
  TEST_ASSERT(a.verify == true);
  TEST_ASSERT(a.verify_dir != nullptr && strcmp(a.verify_dir, "/tmp/downloads") == 0);

  mdl_run_opts_t opts = mdl_cli_run_opts(&a);
  TEST_ASSERT(opts.progress == true);
  TEST_ASSERT(opts.refetch == true);
  TEST_ASSERT(opts.policy.proxy != nullptr &&
              strcmp(opts.policy.proxy, "http://proxy.example.com:8080") == 0);
  TEST_ASSERT(opts.policy.socks5 != nullptr &&
              strcmp(opts.policy.socks5, "socks5://127.0.0.1:1080") == 0);
  TEST_ASSERT(opts.policy.cookies.data == nullptr);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)opts.policy.cookies.length);
  TEST_ASSERT(opts.policy.ca_pem.data == nullptr);
  TEST_ASSERT_EQ((uint16_t)0, (uint16_t)opts.policy.ca_pem.length);
  TEST_END("cli new flags parsing");
}

/**
 * @test A value-taking option at argv end is a usage error.
 * @brief Exercise the cli missing value regression scenario.
 * @details Executes the cli missing value scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_missing_value(void)
{
  TEST_BEGIN("cli missing option value");
  char*      argv[] = {"mdl", "--config"};
  mdl_args_t args   = {};
  mdl_cli_parse(2, argv, &args);
  TEST_ASSERT(args.bad);
  TEST_ASSERT(args.cfg == nullptr);
  TEST_END("cli missing option value");
}

/**
 * @brief Parse one vector and assert its validation result/mode.
 * @details Executes the assert cli scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] argc Argc value for this operation.
 * @param[out] argv Argv value for this operation.
 * @param[in] valid Valid value for this operation.
 * @param[in] expected Expected value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_assert_cli(int argc, char** argv, bool valid, mdl_cli_mode_t expected)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  mdl_cli_parse(argc, argv, &args);
  const ra8_err_t err = internal_test_cli_validate(&args, &mode);
  TEST_ASSERT((err == k_ra8_ok) == valid);
  TEST_ASSERT(mode == expected);
}

/**
 * @test Every advertised command mode has an unambiguous option allowlist.
 * @brief Exercise the cli series mode regression scenario.
 * @details Executes the cli series mode scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_series_mode(void)
{
  char* series[] = {"mdl",
                    "--config",
                    "site.conf",
                    "--series",
                    "https://s/x/",
                    "--out",
                    "library",
                    "--cache-dir",
                    "cache",
                    "--chapters",
                    "2",
                    "--from",
                    "1",
                    "--seed",
                    "2",
                    "--timeout",
                    "100",
                    "--format",
                    "loose",
                    "--contact",
                    "ops",
                    "--max-bytes",
                    "4096",
                    "--proxy",
                    "http://p",
                    "--allow-private",
                    "--separate",
                    "--update",
                    "--polite",
                    "--ignore-robots",
                    "--cross-host",
                    "--allow-incomplete",
                    "--progress",
                    "--refetch",
                    "--cookie-file",
                    "cookies"};
  internal_assert_cli((int)(sizeof(series) / sizeof(series[0])),
                      series,
                      true,
                      k_mdl_cli_mode_series);
}

/**
 * @brief Exercise valid search and browse CLI modes.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * @details Exercises the cli discovery modes scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 */
RA8_INTERNAL static void internal_test_cli_discovery_modes(void)
{
  char* search[] = {"mdl", "--config", "site.conf", "--search", "alpha"};
  internal_assert_cli((int)(sizeof(search) / sizeof(search[0])),
                      search,
                      true,
                      k_mdl_cli_mode_search);
  char* search_pick[] = {"mdl",
                         "--config",
                         "site.conf",
                         "--search",
                         "alpha",
                         "--pick",
                         "1",
                         "--format",
                         "cbz",
                         "--out",
                         "library"};
  internal_assert_cli((int)(sizeof(search_pick) / sizeof(search_pick[0])),
                      search_pick,
                      true,
                      k_mdl_cli_mode_search);
  char* browse[] = {"mdl", "--config", "site.conf", "--browse"};
  internal_assert_cli((int)(sizeof(browse) / sizeof(browse[0])),
                      browse,
                      true,
                      k_mdl_cli_mode_browse);
}

/**
 * @brief Exercise the remaining valid CLI modes.
 * @details Covers list, update-all, remove, verify, init, pack, and page modes
 * after the discovery modes have been checked separately.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * @post Normal return means every scenario assertion passed.
 */
RA8_INTERNAL static void internal_test_cli_other_valid_modes(void)
{
  internal_test_cli_discovery_modes();
  char* list[] = {"mdl", "--list", "--out", "library"};
  internal_assert_cli((int)(sizeof(list) / sizeof(list[0])), list, true, k_mdl_cli_mode_list);
  char* update_all[] = {"mdl",
                        "--update-all",
                        "--config",
                        "site.conf",
                        "--out",
                        "library",
                        "--cache-dir",
                        "cache",
                        "--format",
                        "loose",
                        "--refetch"};
  internal_assert_cli((int)(sizeof(update_all) / sizeof(update_all[0])),
                      update_all,
                      true,
                      k_mdl_cli_mode_update_all);
  char* remove[] = {"mdl", "--remove", "alpha", "--out", "library"};
  internal_assert_cli((int)(sizeof(remove) / sizeof(remove[0])),
                      remove,
                      true,
                      k_mdl_cli_mode_remove);
  char* verify[] = {"mdl", "--verify", "library"};
  internal_assert_cli((int)(sizeof(verify) / sizeof(verify[0])),
                      verify,
                      true,
                      k_mdl_cli_mode_verify);
  char* init[] = {"mdl", "--init-site", "https://s.example/"};
  internal_assert_cli((int)(sizeof(init) / sizeof(init[0])), init, true, k_mdl_cli_mode_init_site);
  char* pack[] = {"mdl", "--pack", "pages", "--format", "cbz"};
  internal_assert_cli((int)(sizeof(pack) / sizeof(pack[0])), pack, true, k_mdl_cli_mode_pack);
  char* page[] = {"mdl",
                  "https://s/page",
                  "--out",
                  "pages",
                  "--attr",
                  "src",
                  "--max",
                  "2",
                  "--socks5",
                  "socks5://proxy",
                  "--allow-private"};
  internal_assert_cli((int)(sizeof(page) / sizeof(page[0])), page, true, k_mdl_cli_mode_page);
}

/**
 * @brief Exercise the cli invalid modes regression scenario.
 * @details Executes the cli invalid modes scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_invalid_modes(void)
{
  char* conflict[] = {"mdl", "--list", "--series", "https://s/x", "--config", "s"};
  internal_assert_cli((int)(sizeof(conflict) / sizeof(conflict[0])),
                      conflict,
                      false,
                      k_mdl_cli_mode_invalid);
  char* no_mode[] = {"mdl", "--config", "s"};
  internal_assert_cli((int)(sizeof(no_mode) / sizeof(no_mode[0])),
                      no_mode,
                      false,
                      k_mdl_cli_mode_invalid);
  char* no_cfg[] = {"mdl", "--search", "x"};
  internal_assert_cli((int)(sizeof(no_cfg) / sizeof(no_cfg[0])),
                      no_cfg,
                      false,
                      k_mdl_cli_mode_invalid);
  char* no_effect[] = {"mdl", "--search", "x", "--config", "s", "--format", "cbz"};
  internal_assert_cli((int)(sizeof(no_effect) / sizeof(no_effect[0])),
                      no_effect,
                      false,
                      k_mdl_cli_mode_invalid);
  char* pack_default[] = {"mdl", "--pack", "pages"};
  internal_assert_cli((int)(sizeof(pack_default) / sizeof(pack_default[0])),
                      pack_default,
                      false,
                      k_mdl_cli_mode_invalid);
  char* proxy_closed[] = {"mdl", "https://s/page", "--proxy", "http://p"};
  internal_assert_cli((int)(sizeof(proxy_closed) / sizeof(proxy_closed[0])),
                      proxy_closed,
                      false,
                      k_mdl_cli_mode_invalid);
  char* verify_dirs[] = {"mdl", "--verify", "a", "--out", "b"};
  internal_assert_cli((int)(sizeof(verify_dirs) / sizeof(verify_dirs[0])),
                      verify_dirs,
                      false,
                      k_mdl_cli_mode_invalid);
  char* duplicate[] = {"mdl", "--list", "--list"};
  internal_assert_cli((int)(sizeof(duplicate) / sizeof(duplicate[0])),
                      duplicate,
                      false,
                      k_mdl_cli_mode_invalid);
}

/**
 * @brief Exercise the cli mode matrix regression scenario.
 * @details Executes the cli mode matrix scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_mode_matrix(void)
{
  TEST_BEGIN("cli strict mode matrix");
  internal_test_cli_series_mode();
  internal_test_cli_other_valid_modes();
  internal_test_cli_invalid_modes();
  TEST_END("cli strict mode matrix");
}

/**
 * @test Numeric values that used to wrap or silently clamp are usage errors.
 * @brief Exercise the cli numeric bounds regression scenario.
 * @details Executes the cli numeric bounds scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cli_numeric_bounds(void)
{
  TEST_BEGIN("cli numeric bounds");
  mdl_nums_t nums = {};
  mdl_args_t args = {.timeout = "0"};
  TEST_ASSERT(internal_test_cli_parse_nums(&args, &nums) == k_ra8_err_invalid_arg);
  args = (mdl_args_t){.chapters = "0"};
  TEST_ASSERT(internal_test_cli_parse_nums(&args, &nums) == k_ra8_err_invalid_arg);
  args = (mdl_args_t){.max_bytes = "0"};
  TEST_ASSERT(internal_test_cli_parse_nums(&args, &nums) == k_ra8_err_invalid_arg);
  args = (mdl_args_t){.pick = "0"};
  TEST_ASSERT(internal_test_cli_parse_nums(&args, &nums) == k_ra8_err_invalid_arg);
  TEST_END("cli numeric bounds");
}

/**
 * @brief Run the media logic CLI validation test group.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_logic_cli_run(void)
{
  internal_test_cli_new_flags();
  internal_test_cli_missing_value();
  internal_test_cli_mode_matrix();
  internal_test_cli_numeric_bounds();
}
