/**
 * @file test_media_dl_cli.c
 * @brief Standalone command grammar and mode-matrix qualification.
 * @details Exercises the strict primary-mode grammar, option allowlists, and
 *          numeric contracts without linking network or exporter backends.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "mdl_cli.h"
#include "ra8_attributes.h"
#include "ra8_io_stream_ram.h"
#include "test_media_dl_cli_internal.h"
#include "unity_minimal.h"

/** @brief Bytes reserved for one expected CLI diagnostic. */
typedef enum : uint16_t {
  k_cli_test_diag_bytes = 512, /**< Larger than every single rejection line. */
} mdl_cli_test_bound_t;

/**
 * @brief Validate parsed arguments through a fresh bounded diagnostic stream.
 * @param[in] args Parsed argument record.
 * @param[out] mode Receives the selected mode on success.
 * @return Canonical CLI validation status.
 * @pre @p args and @p mode are non-NULL.
 * @pre @p args was populated by ::mdl_cli_parse.
 * @post The diagnostic bytes and stream state do not escape the call.
 * @post @p mode follows the public validation contract.
 * @note Test helper with fixed caller-owned stack storage.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 */
RA8_INTERNAL static ra8_err_t internal_cli_validate(const mdl_args_t* args, mdl_cli_mode_t* mode)
{
  uint8_t                   bytes[k_cli_test_diag_bytes];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  const ra8_err_t           init   = ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes));
  return (init == k_ra8_ok) ? mdl_cli_validate(args, &stream, mode) : init;
}

/**
 * @brief Parse numeric options through a fresh bounded diagnostic stream.
 * @param[in] args Parsed argument record or equivalent fixture.
 * @param[out] nums Receives numeric values only on success.
 * @return Canonical numeric CLI status.
 * @pre @p args and @p nums are non-NULL.
 * @pre Every non-null argument string is NUL-terminated.
 * @post The diagnostic bytes and stream state do not escape the call.
 * @post Failure leaves @p nums unchanged.
 * @note Test helper with fixed caller-owned stack storage.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @retval k_ra8_ok The bounded helper operation completed.
 * @retval other The documented validation, storage, or sink error occurred.
 */
RA8_INTERNAL static ra8_err_t internal_cli_parse_nums(const mdl_args_t* args, mdl_nums_t* nums)
{
  uint8_t                   bytes[k_cli_test_diag_bytes];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  const ra8_err_t           init   = ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes));
  return (init == k_ra8_ok) ? mdl_cli_parse_nums(args, &stream, nums) : init;
}

/**
 * @brief Parse one vector and require its expected validated mode.
 * @details Runs the public parser and validator, then checks both the enum and
 *          printable mode-name contracts for an expected-valid invocation.
 * @param[in] argc     Argument count.
 * @param[in] argv     Mutable argument-vector view used by the parser.
 * @param[in] expected Expected validated mode.
 * @pre @p argv names at least @p argc readable, NUL-terminated arguments.
 * @pre @p expected is a valid non-invalid CLI mode.
 * @post The parsed invocation is asserted valid with mode @p expected.
 * @post The validated mode is asserted to have a non-`invalid` public name.
 * @note Host-only assertion helper; terminates the test process on failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_mode(int argc, char** argv, mdl_cli_mode_t expected)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  mdl_cli_parse(argc, argv, &args);
  TEST_ASSERT_EQ(k_ra8_ok, internal_cli_validate(&args, &mode));
  TEST_ASSERT_EQ((uint8_t)expected, (uint8_t)mode);
  TEST_ASSERT(strcmp(mdl_cli_mode_name(mode), "invalid") != 0);
}

/**
 * @brief Parse one vector and require a usage rejection.
 * @details Runs the public parser and validator and verifies that validation
 *          resets its output mode to the invalid sentinel.
 * @param[in] argc Argument count.
 * @param[in] argv Mutable argument-vector view used by the parser.
 * @pre @p argv names at least @p argc readable, NUL-terminated arguments.
 * @pre Standard error is available for the expected validation diagnostic.
 * @post The parsed invocation is asserted invalid.
 * @post The output mode is asserted equal to ::k_mdl_cli_mode_invalid.
 * @note Host-only assertion helper; expected diagnostics are not suppressed.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_invalid(int argc, char** argv)
{
  mdl_args_t     args = {};
  mdl_cli_mode_t mode = k_mdl_cli_mode_page;
  mdl_cli_parse(argc, argv, &args);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_validate(&args, &mode));
  TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_invalid, (uint8_t)mode);
}

/**
 * @test test_all_modes
 * @brief Verify every primary command has an accepted minimal spelling.
 * @details Exercises series, discovery, library, verification, initialization,
 *          packing, direct artifact, page, help, and version mode selection.
 * @pre The public parser and mode validator are linked into the host test.
 * @pre All fixture arguments are immutable NUL-terminated strings.
 * @post Every minimal vector validates to its named mode.
 * @post Both long and short help spellings validate as help mode.
 * @note Host-only parser test with no filesystem or network activity.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_all_modes(void)
{
  TEST_BEGIN("CLI all primary modes");
  char* series[] = {"media_dl", "--config", "site.conf", "--series", "https://x/book"};
  internal_expect_mode(5, series, k_mdl_cli_mode_series);
  char* search[] = {"media_dl", "--config", "site.conf", "--search", "title"};
  internal_expect_mode(5, search, k_mdl_cli_mode_search);
  char* browse[] = {"media_dl", "--config", "site.conf", "--browse"};
  internal_expect_mode(4, browse, k_mdl_cli_mode_browse);
  char* list[] = {"media_dl", "--list"};
  internal_expect_mode(2, list, k_mdl_cli_mode_list);
  char* update_all[] = {"media_dl", "--update-all", "--config", "site.conf"};
  internal_expect_mode(4, update_all, k_mdl_cli_mode_update_all);
  char* remove[] = {"media_dl", "--remove", "slug"};
  internal_expect_mode(3, remove, k_mdl_cli_mode_remove);
  char* verify[] = {"media_dl", "--verify"};
  internal_expect_mode(2, verify, k_mdl_cli_mode_verify);
  char* init[] = {"media_dl", "--init-site", "https://x/series"};
  internal_expect_mode(3, init, k_mdl_cli_mode_init_site);
  char* pack[] = {"media_dl", "--pack", "pages", "--format", "cbz"};
  internal_expect_mode(5, pack, k_mdl_cli_mode_pack);
  char* artifact[] = {"media_dl", "https://x/book.INCOMPLETE.cbt.gz"};
  internal_expect_mode(2, artifact, k_mdl_cli_mode_artifact);
  char* page[] = {"media_dl", "https://x/page"};
  internal_expect_mode(2, page, k_mdl_cli_mode_page);
  char* help[] = {"media_dl", "--help"};
  internal_expect_mode(2, help, k_mdl_cli_mode_help);
  char* short_help[] = {"media_dl", "-h"};
  internal_expect_mode(2, short_help, k_mdl_cli_mode_help);
  char* version[] = {"media_dl", "--version"};
  internal_expect_mode(2, version, k_mdl_cli_mode_version);
  TEST_END("CLI all primary modes");
}

/**
 * @brief Require every series-mode option to coexist in one valid vector.
 * @pre The public parser and validator are linked.
 * @pre The option allowlist reflects every advertised series control.
 * @post The complete vector validates as series mode.
 * @post No fixture pointer escapes the helper.
 * @note Test helper with immutable command tokens.
 * @since 0.1.0
 * @details Exercises the allowlists series scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 */
RA8_INTERNAL static void internal_test_allowlists_series(void)
{
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
                        "--ca-file",
                        "fixture-ca.pem",
                        "--polite",
                        "--ignore-robots",
                        "--allow-private",
                        "--cross-host",
                        "--proxy",
                        "http://127.0.0.1:1"};
  internal_expect_mode((int)(sizeof(series_all) / sizeof(series_all[0])),
                       series_all,
                       k_mdl_cli_mode_series);
}

/**
 * @test test_allowlists
 * @brief Verify controls are accepted only in modes where they take effect.
 * @details Covers a fully configured series download, picked and unpicked
 *          discovery, offline rejection, and direct-artifact restrictions.
 * @pre The public parser and mode validator are linked into the host test.
 * @pre All valid network-mode fixtures provide required primary arguments.
 * @post Applicable series and picked-discovery controls are accepted.
 * @post Inapplicable offline, unpicked, and artifact controls are rejected.
 * @note Host-only parser test with no filesystem or network activity.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_allowlists(void)
{
  TEST_BEGIN("CLI option allowlists");
  internal_test_allowlists_series();
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
  internal_expect_mode((int)(sizeof(picked) / sizeof(picked[0])), picked, k_mdl_cli_mode_search);
  char* unpicked_download_opt[] =
    {"media_dl", "--config", "s", "--search", "x", "--format", "epub"};
  internal_expect_invalid(7, unpicked_download_opt);
  char* list_timeout[] = {"media_dl", "--list", "--timeout", "1"};
  internal_expect_invalid(4, list_timeout);
  char* artifact_attr[] = {"media_dl", "https://x/book.cbz", "--attr", "src"};
  internal_expect_invalid(4, artifact_attr);
  TEST_END("CLI option allowlists");
}

/**
 * @test test_usage_errors
 * @brief Reject ambiguous, duplicated, missing, and inconsistent forms.
 * @details Exercises absent and conflicting modes, missing values, incomplete
 *          pack requests, proxy conjunctions, help conflicts, and insecure
 * artifacts.
 * @pre The public parser and mode validator are linked into the host test.
 * @pre Standard error is available for expected validation diagnostics.
 * @post Every malformed fixture is rejected.
 * @post Every rejected fixture leaves the validated mode invalid.
 * @note Host-only parser test with expected diagnostics on standard error.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_usage_errors(void)
{
  TEST_BEGIN("CLI usage errors");
  char* no_mode[] = {"media_dl", "--out", "x"};
  internal_expect_invalid(3, no_mode);
  char* mixed[] = {"media_dl", "--list", "--browse", "--config", "s"};
  internal_expect_invalid(5, mixed);
  char* duplicate[] = {"media_dl", "--list", "--list"};
  internal_expect_invalid(3, duplicate);
  char* missing_value[] = {"media_dl", "--config", "--series", "https://x/s"};
  internal_expect_invalid(4, missing_value);
  char* from_missing_before_flag[] =
    {"media_dl", "--config", "s", "--series", "https://x/s", "--from", "--separate"};
  internal_expect_invalid(7, from_missing_before_flag);
  char* pack_no_format[] = {"media_dl", "--pack", "pages"};
  internal_expect_invalid(3, pack_no_format);
  char* proxy_no_escape[] = {"media_dl", "https://x/p", "--proxy", "http://p"};
  internal_expect_invalid(4, proxy_no_escape);
  char* two_proxies[] =
    {"media_dl", "https://x/p", "--proxy", "http://p", "--socks5", "socks5://p", "--allow-private"};
  internal_expect_invalid(7, two_proxies);
  char* bad_attr[] = {"media_dl", "https://x/p", "--attr", "href"};
  internal_expect_invalid(4, bad_attr);
  char* help_plus[] = {"media_dl", "--help", "--version"};
  internal_expect_invalid(3, help_plus);
  char* ca_offline[] = {"media_dl", "--list", "--ca-file", "fixture-ca.pem"};
  internal_expect_invalid(4, ca_offline);
  char* insecure_artifact[] = {"media_dl", "http://x/book.epub"};
  internal_expect_invalid(2, insecure_artifact);
  TEST_END("CLI usage errors");
}

/**
 * @test test_numeric_contracts
 * @brief Verify strict numeric zero, range, and grammar contracts.
 * @details Rejects zero-only positive fields and non-finite or tailed chapter
 *          numbers while accepting and checking a complete valid tuple and a
 *          signed `--from` value through the actual argv parser.
 * @pre The public numeric parser is linked into the host test.
 * @pre Numeric fixtures are immutable NUL-terminated strings.
 * @post Invalid zero and floating-point forms are rejected.
 * @post Valid integral and fractional values are parsed without loss.
 * @note Host-only parser test with no filesystem or network activity.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_numeric_contracts(void)
{
  TEST_BEGIN("CLI numeric contracts");
  mdl_nums_t nums;
  mdl_args_t zero_timeout = {.timeout = "0"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&zero_timeout, &nums));
  mdl_args_t zero_chapters = {.chapters = "0"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&zero_chapters, &nums));
  mdl_args_t zero_bytes = {.max_bytes = "0"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&zero_bytes, &nums));
  mdl_args_t zero_pick = {.pick = "0"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&zero_pick, &nums));
  mdl_args_t valid = {.timeout   = "1",
                      .chapters  = "1",
                      .from      = "108.5",
                      .max_bytes = "1",
                      .pick      = "1"};
  TEST_ASSERT_EQ(k_ra8_ok, internal_cli_parse_nums(&valid, &nums));
  TEST_ASSERT_EQ((uint32_t)1, nums.timeout);
  TEST_ASSERT_EQ((size_t)1, nums.chapters);
  TEST_ASSERT_EQ((size_t)1, nums.pick);
  TEST_ASSERT(nums.from_present);
  TEST_ASSERT(nums.from_num == 108.5);
  char* signed_argv[] = {"media_dl", "--config", "s", "--series", "https://x/s", "--from", "-1.5"};
  mdl_args_t signed_args = {};
  mdl_cli_parse((int)(sizeof(signed_argv) / sizeof(signed_argv[0])), signed_argv, &signed_args);
  mdl_cli_mode_t signed_mode = k_mdl_cli_mode_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, internal_cli_validate(&signed_args, &signed_mode));
  TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_series, (uint8_t)signed_mode);
  TEST_ASSERT_EQ(k_ra8_ok, internal_cli_parse_nums(&signed_args, &nums));
  TEST_ASSERT(nums.from_present);
  TEST_ASSERT(nums.from_num == -1.5);
  mdl_args_t nan_from = {.from = "nan"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&nan_from, &nums));
  mdl_args_t tailed_from = {.from = "108.5x"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_cli_parse_nums(&tailed_from, &nums));
  TEST_END("CLI numeric contracts");
}

/**
 * @test test_diagnostic_failures_propagate
 * @brief Require CLI APIs to preserve injected sink failures and outputs.
 * @details Uses deliberately undersized RAM streams so validation and help
 *          cannot hide partial diagnostic writes behind ordinary usage errors.
 * @pre The public CLI and RAM-stream backends are linked.
 * @pre Fixture outputs are initialized with nonzero sentinels.
 * @post Usage and invalid-option APIs return the stream capacity failure.
 * @post Numeric and mode outputs remain unchanged/invalid as documented.
 * @note No host descriptor or C-runtime stream is used.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_diagnostic_failures_propagate(void)
{
  TEST_BEGIN("CLI diagnostic failures propagate");
  uint8_t                   bytes[4];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes)));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_cli_usage(&stream, "media_dl"));

  state  = (ra8_io_stream_ram_state_t){};
  stream = (ra8_io_stream_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes)));
  mdl_args_t     invalid = {};
  mdl_cli_mode_t mode    = k_mdl_cli_mode_page;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_cli_validate(&invalid, &stream, &mode));
  TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_invalid, (uint8_t)mode);

  state  = (ra8_io_stream_ram_state_t){};
  stream = (ra8_io_stream_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes)));
  const mdl_nums_t sentinel = {.seed         = UINT64_MAX,
                               .timeout      = UINT32_MAX,
                               .chapters     = SIZE_MAX,
                               .max_imgs     = UINT32_MAX,
                               .from_present = true,
                               .from_num     = 9.5,
                               .pick         = SIZE_MAX};
  mdl_nums_t       nums     = sentinel;
  mdl_args_t       bad_num  = {.timeout = "0"};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, mdl_cli_parse_nums(&bad_num, &stream, &nums));
  TEST_ASSERT(memcmp(&nums, &sentinel, sizeof(nums)) == 0);
  TEST_END("CLI diagnostic failures propagate");
}

int main(void)
{
  priv_test_mdl_cli_matrix_run();
  internal_test_all_modes();
  internal_test_allowlists();
  internal_test_usage_errors();
  internal_test_numeric_contracts();
  internal_test_diagnostic_failures_propagate();
  return 0;
}
