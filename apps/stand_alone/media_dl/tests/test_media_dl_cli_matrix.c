/**
 * @file test_media_dl_cli_matrix.c
 * @brief Exhaustive public mode-by-option CLI matrix qualification.
 * @details Exercises every independent mode/option cell plus repetition, pick,
 *          numeric-bound, and stable-help conjunctions through public APIs.
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

/** @brief Fixed capacities used by the exhaustive CLI matrix. */
typedef enum : uint16_t {
  k_matrix_argv_cap  = 16,   /**< Maximum argv entries in one matrix vector. */
  k_matrix_help_cap  = 8192, /**< Captured usage-text buffer bytes.          */
  k_matrix_token_cap = 64,   /**< Maximum help option spelling bytes.        */
  k_matrix_diag_cap  = 512,  /**< Maximum one-line validation diagnostic.    */
} cli_matrix_limit_t;

/** @brief Independent test-table columns, one per public command mode. */
typedef enum : uint8_t {
  k_matrix_series = 0, /**< Series mode column.       */
  k_matrix_search,     /**< Search mode column.       */
  k_matrix_browse,     /**< Browse mode column.       */
  k_matrix_list,       /**< List mode column.         */
  k_matrix_update_all, /**< Update-all mode column.   */
  k_matrix_remove,     /**< Remove mode column.       */
  k_matrix_verify,     /**< Verify mode column.       */
  k_matrix_init,       /**< Init-site mode column.    */
  k_matrix_pack,       /**< Pack mode column.         */
  k_matrix_artifact,   /**< Direct-artifact column.   */
  k_matrix_page,       /**< Page mode column.         */
  k_matrix_help,       /**< Help mode column.         */
  k_matrix_version,    /**< Version mode column.      */
  k_matrix_mode_count, /**< Number of matrix columns. */
} cli_matrix_mode_t;

/** @brief Independent option-row identities used by test construction. */
typedef enum : uint8_t {
  k_opt_config = 0,      /**< `--config`.            */
  k_opt_series,          /**< `--series`.            */
  k_opt_page_url,        /**< Positional page URL.   */
  k_opt_artifact_url,    /**< Positional artifact.   */
  k_opt_out,             /**< `--out`.               */
  k_opt_cache_dir,       /**< `--cache-dir`.         */
  k_opt_attr,            /**< `--attr`.              */
  k_opt_chapters,        /**< `--chapters`.          */
  k_opt_from,            /**< `--from`.              */
  k_opt_max,             /**< `--max`.               */
  k_opt_seed,            /**< `--seed`.              */
  k_opt_timeout,         /**< `--timeout`.           */
  k_opt_format,          /**< `--format`.            */
  k_opt_pack,            /**< `--pack`.              */
  k_opt_contact,         /**< `--contact`.           */
  k_opt_max_bytes,       /**< `--max-bytes`.         */
  k_opt_remove,          /**< `--remove`.            */
  k_opt_search,          /**< `--search`.            */
  k_opt_pick,            /**< `--pick`.              */
  k_opt_proxy,           /**< `--proxy`.             */
  k_opt_socks5,          /**< `--socks5`.            */
  k_opt_cookie,          /**< `--cookie-file`.       */
  k_opt_ca_file,         /**< `--ca-file`.           */
  k_opt_verify_dir,      /**< `--verify DIR`.        */
  k_opt_init,            /**< `--init-site`.         */
  k_opt_browse,          /**< `--browse`.            */
  k_opt_separate,        /**< `--separate`.          */
  k_opt_update,          /**< `--update`.            */
  k_opt_list,            /**< `--list`.              */
  k_opt_update_all,      /**< `--update-all`.        */
  k_opt_polite,          /**< `--polite`.            */
  k_opt_ignore_robots,   /**< `--ignore-robots`.     */
  k_opt_allow_private,   /**< `--allow-private`.     */
  k_opt_cross_host,      /**< `--cross-host`.        */
  k_opt_incomplete,      /**< `--allow-incomplete`.  */
  k_opt_progress,        /**< `--progress`.          */
  k_opt_refetch,         /**< `--refetch`.           */
  k_opt_verify,          /**< `--verify`.            */
  k_opt_help,            /**< `--help`.              */
  k_opt_short_help,      /**< `-h`.                  */
  k_opt_version,         /**< `--version`.           */
  k_matrix_option_count, /**< Number of option rows. */
} cli_matrix_option_t;

/** @brief One independently specified command-mode fixture. */
typedef struct {
  const char*    name;     /**< Diagnostic mode name.                */
  mdl_cli_mode_t expected; /**< Public mode expected on success.     */
  const char*    argv[5];  /**< Minimal valid argv, including argv0. */
  size_t         argc;     /**< Number of populated argv entries.    */
} cli_mode_case_t;

/** @brief One public option and its explicit per-mode applicability row. */
typedef struct {
  const char*       name;                         /**< Diagnostic row name.                     */
  const char*       token[2];                     /**< One flag/value pair or positional token. */
  size_t            count;                        /**< Number of populated tokens.              */
  cli_matrix_mode_t repeat_at;                    /**< Valid mode used by repetition coverage.  */
  bool              allowed[k_matrix_mode_count]; /**< Independent truth row.                   */
} cli_option_case_t;

/** @brief Minimal valid invocation for each matrix column. */
static const cli_mode_case_t s_matrix_modes[k_matrix_mode_count] = {
  [k_matrix_series]     = {"series",
                           k_mdl_cli_mode_series,
                           {"media_dl", "--config", "site.conf", "--series", "https://x/book"},
                           5U},
  [k_matrix_search]     = {"search",
                           k_mdl_cli_mode_search,
                           {"media_dl", "--config", "site.conf", "--search", "title"},
                           5U},
  [k_matrix_browse]     = {"browse",
                           k_mdl_cli_mode_browse,
                           {"media_dl", "--config", "site.conf", "--browse"},
                           4U},
  [k_matrix_list]       = {"list", k_mdl_cli_mode_list, {"media_dl", "--list"}, 2U},
  [k_matrix_update_all] = {"update-all",
                           k_mdl_cli_mode_update_all,
                           {"media_dl", "--update-all", "--config", "site.conf"},
                           4U},
  [k_matrix_remove]     = {"remove", k_mdl_cli_mode_remove, {"media_dl", "--remove", "slug"}, 3U},
  [k_matrix_verify]     = {"verify", k_mdl_cli_mode_verify, {"media_dl", "--verify"}, 2U},
  [k_matrix_init]       = {"init-site",
                           k_mdl_cli_mode_init_site,
                           {"media_dl", "--init-site", "https://x/book"},
                           3U},
  [k_matrix_pack]       = {"pack",
                           k_mdl_cli_mode_pack,
                           {"media_dl", "--pack", "pages", "--format", "cbz"},
                           5U},
  [k_matrix_artifact]   = {"artifact",
                           k_mdl_cli_mode_artifact,
                           {"media_dl", "https://x/book.cbz"},
                           2U},
  [k_matrix_page]       = {"page", k_mdl_cli_mode_page, {"media_dl", "https://x/page"}, 2U},
  [k_matrix_help]       = {"help", k_mdl_cli_mode_help, {"media_dl", "--help"}, 2U},
  [k_matrix_version]    = {"version", k_mdl_cli_mode_version, {"media_dl", "--version"}, 2U},
};

/** @brief Independent option-by-mode truth table (never references production
 * masks). */
static const cli_option_case_t s_matrix_options[k_matrix_option_count] = {
  [k_opt_config] = {"--config",
                    {"--config", "other.conf"},
                    2U,
                    k_matrix_series,
                    {[k_matrix_series]     = true,
                     [k_matrix_search]     = true,
                     [k_matrix_browse]     = true,
                     [k_matrix_update_all] = true}},
  [k_opt_series] =
    {"--series", {"--series", "https://x/other"}, 2U, k_matrix_series, {[k_matrix_series] = true}},
  [k_opt_page_url] =
    {"page URL", {"https://x/other-page"}, 1U, k_matrix_page, {[k_matrix_page] = true}},
  [k_opt_artifact_url] =
    {"artifact URL", {"https://x/other.cbz"}, 1U, k_matrix_artifact, {[k_matrix_artifact] = true}},
  [k_opt_out]       = {"--out",
                       {"--out", "out"},
                       2U,
                       k_matrix_series,
                       {[k_matrix_series]     = true,
                        [k_matrix_search]     = true,
                        [k_matrix_browse]     = true,
                        [k_matrix_list]       = true,
                        [k_matrix_update_all] = true,
                        [k_matrix_remove]     = true,
                        [k_matrix_verify]     = true,
                        [k_matrix_artifact]   = true,
                        [k_matrix_page]       = true}},
  [k_opt_cache_dir] = {"--cache-dir",
                       {"--cache-dir", "cache"},
                       2U,
                       k_matrix_series,
                       {[k_matrix_series]     = true,
                        [k_matrix_search]     = true,
                        [k_matrix_browse]     = true,
                        [k_matrix_update_all] = true}},
  [k_opt_attr]      = {"--attr", {"--attr", "src"}, 2U, k_matrix_page, {[k_matrix_page] = true}},
  [k_opt_chapters]  = {"--chapters",
                       {"--chapters", "2"},
                       2U,
                       k_matrix_series,
                       {[k_matrix_series] = true,
                        [k_matrix_search] = true,
                        [k_matrix_browse] = true}},
  [k_opt_from] = {"--from",
                  {"--from", "1.5"},
                  2U,
                  k_matrix_series,
                  {[k_matrix_series] = true, [k_matrix_search] = true, [k_matrix_browse] = true}},
  [k_opt_max]  = {"--max", {"--max", "2"}, 2U, k_matrix_page, {[k_matrix_page] = true}},
  [k_opt_seed] = {"--seed",
                  {"--seed", "2"},
                  2U,
                  k_matrix_series,
                  {[k_matrix_series]     = true,
                   [k_matrix_search]     = true,
                   [k_matrix_browse]     = true,
                   [k_matrix_update_all] = true,
                   [k_matrix_artifact]   = true,
                   [k_matrix_page]       = true}},
  [k_opt_timeout] = {"--timeout",
                     {"--timeout", "10"},
                     2U,
                     k_matrix_series,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_artifact]   = true,
                      [k_matrix_page]       = true}},
  [k_opt_format]  = {"--format",
                     {"--format", "cbz"},
                     2U,
                     k_matrix_pack,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_pack]       = true}},
  [k_opt_pack] = {"--pack", {"--pack", "other-pages"}, 2U, k_matrix_pack, {[k_matrix_pack] = true}},
  [k_opt_contact]   = {"--contact",
                       {"--contact", "operator@example.test"},
                       2U,
                       k_matrix_series,
                       {[k_matrix_series]     = true,
                        [k_matrix_search]     = true,
                        [k_matrix_browse]     = true,
                        [k_matrix_update_all] = true,
                        [k_matrix_artifact]   = true,
                        [k_matrix_page]       = true}},
  [k_opt_max_bytes] = {"--max-bytes",
                       {"--max-bytes", "1024"},
                       2U,
                       k_matrix_series,
                       {[k_matrix_series]     = true,
                        [k_matrix_search]     = true,
                        [k_matrix_browse]     = true,
                        [k_matrix_update_all] = true,
                        [k_matrix_artifact]   = true,
                        [k_matrix_page]       = true}},
  [k_opt_remove] =
    {"--remove", {"--remove", "other"}, 2U, k_matrix_remove, {[k_matrix_remove] = true}},
  [k_opt_search] =
    {"--search", {"--search", "other"}, 2U, k_matrix_search, {[k_matrix_search] = true}},
  [k_opt_pick]    = {"--pick",
                     {"--pick", "1"},
                     2U,
                     k_matrix_search,
                     {[k_matrix_search] = true, [k_matrix_browse] = true}},
  [k_opt_proxy]   = {"--proxy",
                     {"--proxy", "http://127.0.0.1:1"},
                     2U,
                     k_matrix_series,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_artifact]   = true,
                      [k_matrix_page]       = true}},
  [k_opt_socks5]  = {"--socks5",
                     {"--socks5", "socks5://127.0.0.1:1"},
                     2U,
                     k_matrix_series,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_artifact]   = true,
                      [k_matrix_page]       = true}},
  [k_opt_cookie]  = {"--cookie-file",
                     {"--cookie-file", "cookies.txt"},
                     2U,
                     k_matrix_series,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_artifact]   = true,
                      [k_matrix_page]       = true}},
  [k_opt_ca_file] = {"--ca-file",
                     {"--ca-file", "ca.pem"},
                     2U,
                     k_matrix_series,
                     {[k_matrix_series]     = true,
                      [k_matrix_search]     = true,
                      [k_matrix_browse]     = true,
                      [k_matrix_update_all] = true,
                      [k_matrix_artifact]   = true,
                      [k_matrix_page]       = true}},
  [k_opt_verify_dir] =
    {"--verify DIR", {"--verify", "verify-dir"}, 2U, k_matrix_verify, {[k_matrix_verify] = true}},
  [k_opt_init]     = {"--init-site",
                      {"--init-site", "https://x/other"},
                      2U,
                      k_matrix_init,
                      {[k_matrix_init] = true}},
  [k_opt_browse]   = {"--browse", {"--browse"}, 1U, k_matrix_browse, {[k_matrix_browse] = true}},
  [k_opt_separate] = {"--separate",
                      {"--separate"},
                      1U,
                      k_matrix_series,
                      {[k_matrix_series]     = true,
                       [k_matrix_search]     = true,
                       [k_matrix_browse]     = true,
                       [k_matrix_update_all] = true}},
  [k_opt_update] = {"--update",
                    {"--update"},
                    1U,
                    k_matrix_series,
                    {[k_matrix_series] = true, [k_matrix_search] = true, [k_matrix_browse] = true}},
  [k_opt_list]   = {"--list", {"--list"}, 1U, k_matrix_list, {[k_matrix_list] = true}},
  [k_opt_update_all] =
    {"--update-all", {"--update-all"}, 1U, k_matrix_update_all, {[k_matrix_update_all] = true}},
  [k_opt_polite]        = {"--polite",
                           {"--polite"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true,
                            [k_matrix_artifact]   = true,
                            [k_matrix_page]       = true}},
  [k_opt_ignore_robots] = {"--ignore-robots",
                           {"--ignore-robots"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true,
                            [k_matrix_artifact]   = true,
                            [k_matrix_page]       = true}},
  [k_opt_allow_private] = {"--allow-private",
                           {"--allow-private"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true,
                            [k_matrix_artifact]   = true,
                            [k_matrix_page]       = true}},
  [k_opt_cross_host]    = {"--cross-host",
                           {"--cross-host"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true,
                            [k_matrix_artifact]   = true,
                            [k_matrix_page]       = true}},
  [k_opt_incomplete]    = {"--allow-incomplete",
                           {"--allow-incomplete"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true}},
  [k_opt_progress]      = {"--progress",
                           {"--progress"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true}},
  [k_opt_refetch]       = {"--refetch",
                           {"--refetch"},
                           1U,
                           k_matrix_series,
                           {[k_matrix_series]     = true,
                            [k_matrix_search]     = true,
                            [k_matrix_browse]     = true,
                            [k_matrix_update_all] = true}},
  [k_opt_verify]     = {"--verify", {"--verify"}, 1U, k_matrix_verify, {[k_matrix_verify] = true}},
  [k_opt_help]       = {"--help", {"--help"}, 1U, k_matrix_help, {[k_matrix_help] = true}},
  [k_opt_short_help] = {"-h", {"-h"}, 1U, k_matrix_help, {[k_matrix_help] = true}},
  [k_opt_version] = {"--version", {"--version"}, 1U, k_matrix_version, {[k_matrix_version] = true}},
};

/**
 * @brief Append one immutable token to a bounded mutable argv vector.
 * @details The parser does not modify token bytes, so the test safely exposes
 *          table literals through its historical mutable argv signature.
 * @param[in,out] argv Argument vector being assembled.
 * @param[in,out] argc Current populated count.
 * @param[in]     token NUL-terminated token to append.
 * @pre @p argv, @p argc, and @p token are non-NULL.
 * @pre `*argc < k_matrix_argv_cap`.
 * @post @p token occupies the prior end of @p argv.
 * @post @p argc grows by exactly one.
 * @note Not thread-safe: mutates caller-owned vector storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_matrix_push(char* argv[], size_t* argc, const char* token)
{
  TEST_ASSERT(*argc < (size_t)k_matrix_argv_cap);
  argv[*argc] = (char*)token;
  *argc += 1U;
}

/**
 * @brief Determine whether a mode fixture already carries an option row.
 * @details Enumerates primary/required fixture tokens independently of parser
 *          fields or production option masks.
 * @param[in] mode Matrix mode column.
 * @param[in] opt  Matrix option row.
 * @return Whether the minimal/special fixture already contains @p opt.
 * @retval true  Appending the option would be a repetition.
 * @retval false The option still needs to be appended for this vector.
 * @pre @p mode is below ::k_matrix_mode_count.
 * @pre @p opt is below ::k_matrix_option_count.
 * @post No table or caller state is modified.
 * @post The result depends only on the independent test fixtures.
 * @note Thread-safe: reads immutable test data only.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_matrix_base_contains(cli_matrix_mode_t   mode,
                                                       cli_matrix_option_t opt)
{
  if ((opt == k_opt_config) && ((mode == k_matrix_series) || (mode == k_matrix_search) ||
                                (mode == k_matrix_browse) || (mode == k_matrix_update_all))) {
    return true;
  }
  if ((opt == k_opt_format) && (mode == k_matrix_pack)) {
    return true;
  }
  if (((opt == k_opt_series) && (mode == k_matrix_series)) ||
      ((opt == k_opt_search) && (mode == k_matrix_search)) ||
      ((opt == k_opt_browse) && (mode == k_matrix_browse)) ||
      ((opt == k_opt_list) && (mode == k_matrix_list)) ||
      ((opt == k_opt_update_all) && (mode == k_matrix_update_all)) ||
      ((opt == k_opt_remove) && (mode == k_matrix_remove)) ||
      ((opt == k_opt_init) && (mode == k_matrix_init)) ||
      ((opt == k_opt_pack) && (mode == k_matrix_pack)) ||
      ((opt == k_opt_artifact_url) && (mode == k_matrix_artifact)) ||
      ((opt == k_opt_page_url) && (mode == k_matrix_page)) ||
      ((opt == k_opt_version) && (mode == k_matrix_version))) {
    return true;
  }
  return ((mode == k_matrix_verify) && ((opt == k_opt_verify) || (opt == k_opt_verify_dir))) ||
         ((mode == k_matrix_help) && ((opt == k_opt_help) || (opt == k_opt_short_help)));
}

/**
 * @brief Identify download options conditional on a discovery pick.
 * @details Lists the options that search/browse may use only with `--pick`.
 * @param[in] opt Matrix option row.
 * @return Whether @p opt requires a pick in discovery modes.
 * @retval true  A search/browse vector must also add `--pick 1`.
 * @retval false No discovery-pick prerequisite applies.
 * @pre @p opt is below ::k_matrix_option_count.
 * @pre The independent applicability table is fully initialised.
 * @post No test data is modified.
 * @post The result is independent of production masks.
 * @note Thread-safe: pure row classification.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_matrix_needs_pick(cli_matrix_option_t opt)
{
  return (opt == k_opt_out) || (opt == k_opt_cache_dir) || (opt == k_opt_chapters) ||
         (opt == k_opt_from) || (opt == k_opt_format) || (opt == k_opt_separate) ||
         (opt == k_opt_update) || (opt == k_opt_incomplete) || (opt == k_opt_progress) ||
         (opt == k_opt_refetch);
}

/**
 * @brief Assemble one mode's minimal base invocation.
 * @details Copies the independent fixture and substitutes the short-help or
 *          verify-directory spelling when that row itself is under test.
 * @param[in]  mode Matrix mode column.
 * @param[in]  opt  Option row under test.
 * @param[out] argv Bounded argv destination.
 * @param[out] argc Populated argument count.
 * @pre All output pointers are non-NULL.
 * @pre @p mode and @p opt are within their table bounds.
 * @post @p argv describes one valid base invocation for @p mode.
 * @post `1 <= *argc <= k_matrix_argv_cap`.
 * @note Not thread-safe: writes caller-owned vector storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_matrix_build_base(cli_matrix_mode_t   mode,
                                                    cli_matrix_option_t opt,
                                                    char*               argv[],
                                                    size_t*             argc)
{
  *argc                       = 0U;
  const cli_mode_case_t* base = &s_matrix_modes[mode];
  for (size_t i = 0U; i < base->argc; ++i) {
    internal_matrix_push(argv, argc, base->argv[i]);
  }
  if ((mode == k_matrix_help) && (opt == k_opt_short_help)) {
    argv[1] = (char*)"-h";
  }
  if ((mode == k_matrix_verify) &&
      ((opt == k_opt_verify_dir) || (opt == k_opt_page_url) || (opt == k_opt_artifact_url))) {
    internal_matrix_push(argv, argc, "verify-dir");
  }
}

/**
 * @brief Append prerequisite options for one expected-valid matrix cell.
 * @details Adds discovery pick or private-network opt-in only where the public
 *          CLI contract makes the tested option effective.
 * @param[in]     mode    Matrix mode column.
 * @param[in]     opt     Matrix option row.
 * @param[in,out] argv    Argument vector being assembled.
 * @param[in,out] argc    Current populated argument count.
 * @pre @p argv and @p argc are non-NULL.
 * @pre The matrix cell for @p mode/@p opt is expected valid.
 * @post Required conjunction tokens are appended exactly once.
 * @post No tested option token itself is appended.
 * @note Not thread-safe: mutates caller-owned vector storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_matrix_add_prerequisites(cli_matrix_mode_t   mode,
                                                           cli_matrix_option_t opt,
                                                           char*               argv[],
                                                           size_t*             argc)
{
  if (((mode == k_matrix_search) || (mode == k_matrix_browse)) && internal_matrix_needs_pick(opt)) {
    internal_matrix_push(argv, argc, "--pick");
    internal_matrix_push(argv, argc, "1");
  }
  if ((opt == k_opt_proxy) || (opt == k_opt_socks5)) {
    internal_matrix_push(argv, argc, "--allow-private");
  }
}

/**
 * @brief Append one option row's concrete argv tokens.
 * @details Copies one positional token, bare flag, or flag/value pair.
 * @param[in]     opt  Matrix option row.
 * @param[in,out] argv Argument vector being assembled.
 * @param[in,out] argc Current populated argument count.
 * @pre @p argv and @p argc are non-NULL.
 * @pre @p opt is below ::k_matrix_option_count.
 * @post Exactly `s_matrix_options[opt].count` tokens are appended.
 * @post Token ordering matches the public CLI spelling.
 * @note Not thread-safe: mutates caller-owned vector storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_matrix_add_option(cli_matrix_option_t opt, char* argv[], size_t* argc)
{
  const cli_option_case_t* option = &s_matrix_options[opt];
  for (size_t i = 0U; i < option->count; ++i) {
    internal_matrix_push(argv, argc, option->token[i]);
  }
}

/**
 * @brief Parse and validate one assembled matrix vector.
 * @details Runs the public parser and validator without inspecting internals.
 * @param[in]  argv Argument vector.
 * @param[in]  argc Populated argument count.
 * @param[out] mode Validated mode result.
 * @return Whether the public CLI accepted the vector.
 * @retval true  Validation succeeded and @p mode names one command.
 * @retval false Validation failed and @p mode is invalid.
 * @pre @p argv and @p mode are non-NULL.
 * @pre `1 <= argc <= k_matrix_argv_cap` and every token is NUL-terminated.
 * @post @p mode reflects ::mdl_cli_validate exactly.
 * @post Token bytes are unchanged.
 * @note Thread-safe across distinct stack-local diagnostic streams.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_matrix_validate(char* argv[], size_t argc, mdl_cli_mode_t* mode)
{
  mdl_args_t args = {};
  mdl_cli_parse((int)argc, argv, &args);
  uint8_t                   bytes[k_matrix_diag_cap];
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  if (ra8_io_stream_ram_init(&stream, &state, bytes, sizeof(bytes)) != k_ra8_ok) {
    return false;
  }
  const ra8_err_t err = mdl_cli_validate(&args, &stream, mode);
  return err == k_ra8_ok;
}

/**
 * @brief Check whether a long help spelling has an option-table row.
 * @details Searches concrete row tokens rather than production parser tables.
 * @param[in] spelling NUL-terminated long option such as `--timeout`.
 * @return Whether any independent matrix row owns that exact spelling.
 * @retval true  A concrete option token matched.
 * @retval false No option row covers the help spelling.
 * @pre @p spelling is non-NULL and NUL-terminated.
 * @pre @p spelling begins with two hyphens.
 * @post The option matrix is unchanged.
 * @post No production parser state is accessed.
 * @note Thread-safe: reads immutable test data only.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_matrix_has_spelling(const char* spelling)
{
  for (size_t i = 0U; i < (size_t)k_matrix_option_count; ++i) {
    const cli_option_case_t* option = &s_matrix_options[i];
    for (size_t token = 0U; token < option->count; ++token) {
      if (strcmp(option->token[token], spelling) == 0) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Capture the public usage block through its injected stream contract.
 * @details Binds a RAM stream directly over the caller buffer, preserving one
 *          byte for the terminating NUL used by the lexer.
 * @param[out] out Captured NUL-terminated usage text.
 * @param[in]  cap Destination capacity including NUL.
 * @return Whether binding and complete bounded capture succeeded.
 * @retval true  @p out contains the complete usage block.
 * @retval false Stream binding, writing, or text capacity failed.
 * @pre @p out is non-NULL.
 * @pre `1 < cap <= UINT32_MAX`.
 * @post @p out is NUL-terminated.
 * @post No process descriptor or C-runtime stream state is touched.
 * @note Thread-safe across distinct caller buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_capture_usage(char* out, size_t cap)
{
  if ((out == nullptr) || (cap < 2U) || (cap > (size_t)UINT32_MAX)) {
    return false;
  }
  out[0]                           = '\0';
  ra8_io_stream_ram_state_t state  = {};
  ra8_io_stream_t           stream = {};
  if (ra8_io_stream_ram_init(&stream, &state, (uint8_t*)out, (uint32_t)(cap - 1U)) != k_ra8_ok) {
    return false;
  }
  if (mdl_cli_usage(&stream, "media_dl") != k_ra8_ok) {
    return false;
  }
  uint32_t used = 0U;
  if (ra8_io_stream_ram_used(&state, &used) != k_ra8_ok) {
    return false;
  }
  out[used] = '\0';
  return true;
}

/**
 * @test test_exhaustive_mode_option_matrix
 * @brief Qualify every public option against every public mode.
 * @details Uses an independent Boolean truth table, exercising primary-mode
 *          conjunctions, direct artifacts, `--ca-file`, and every network-only
 *          knob without referencing production masks.
 * @pre The static matrix has one row per ::cli_matrix_option_t value.
 * @pre Every mode fixture is independently valid.
 * @post Every allowed cell validates to its expected public mode.
 * @post Every rejected cell leaves the public mode invalid.
 * @note Host-only parser test with no filesystem or network activity.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_exhaustive_mode_option_matrix(void)
{
  TEST_BEGIN("CLI exhaustive mode x option matrix");
  for (size_t mode_index = 0U; mode_index < (size_t)k_matrix_mode_count; ++mode_index) {
    const cli_matrix_mode_t mode = (cli_matrix_mode_t)mode_index;
    for (size_t opt_index = 0U; opt_index < (size_t)k_matrix_option_count; ++opt_index) {
      const cli_matrix_option_t opt = (cli_matrix_option_t)opt_index;
      char*                     argv[k_matrix_argv_cap];
      size_t                    argc = 0U;
      internal_matrix_build_base(mode, opt, argv, &argc);
      const bool expected = s_matrix_options[opt].allowed[mode];
      if (expected) {
        internal_matrix_add_prerequisites(mode, opt, argv, &argc);
      }
      if (!internal_matrix_base_contains(mode, opt)) {
        internal_matrix_add_option(opt, argv, &argc);
      }
      mdl_cli_mode_t actual_mode = k_mdl_cli_mode_invalid;
      const bool     accepted    = internal_matrix_validate(argv, argc, &actual_mode);
      TEST_ASSERT(accepted == expected);
      TEST_ASSERT(!accepted || (actual_mode == s_matrix_modes[mode].expected));
      TEST_ASSERT(accepted || (actual_mode == k_mdl_cli_mode_invalid));
    }
  }
  TEST_END("CLI exhaustive mode x option matrix");
}

/**
 * @test test_every_public_option_repeated
 * @brief Reject a repeated occurrence of every public option row.
 * @details Reuses each row's explicitly chosen valid mode, adds required
 *          conjunction tokens, and supplies a second complete occurrence.
 * @pre Every option row names one mode where it is independently allowed.
 * @pre Matrix argv capacity accommodates the largest repeated vector.
 * @post Every repeated flag, value option, positional URL, and verify-dir form
 * fails.
 * @post Every failed vector leaves its mode invalid.
 * @note Parser test with expected diagnostics captured in fixed RAM.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_every_public_option_repeated(void)
{
  TEST_BEGIN("CLI every option repetition rejected");
  for (size_t opt_index = 0U; opt_index < (size_t)k_matrix_option_count; ++opt_index) {
    const cli_matrix_option_t opt  = (cli_matrix_option_t)opt_index;
    const cli_matrix_mode_t   mode = s_matrix_options[opt].repeat_at;
    char*                     argv[k_matrix_argv_cap];
    size_t                    argc = 0U;
    internal_matrix_build_base(mode, opt, argv, &argc);
    internal_matrix_add_prerequisites(mode, opt, argv, &argc);
    if (!internal_matrix_base_contains(mode, opt)) {
      internal_matrix_add_option(opt, argv, &argc);
    }
    internal_matrix_add_option(opt, argv, &argc);
    mdl_cli_mode_t actual_mode = k_mdl_cli_mode_invalid;
    TEST_ASSERT(!internal_matrix_validate(argv, argc, &actual_mode));
    TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_invalid, (uint8_t)actual_mode);
  }
  TEST_END("CLI every option repetition rejected");
}

/**
 * @test test_discovery_download_options_require_pick
 * @brief Reject every conditional download option without a discovery pick.
 * @details Complements each allowed pick conjunction in the main matrix with
 *          the corresponding unpicked search and browse invocation.
 * @pre Search and browse fixtures omit `--pick`.
 * @pre ::internal_matrix_needs_pick enumerates every conditional download row.
 * @post Every unpicked conditional row is rejected in both discovery modes.
 * @post Every rejection leaves the mode invalid.
 * @note Parser test with expected diagnostics captured in fixed RAM.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_discovery_download_options_require_pick(void)
{
  TEST_BEGIN("CLI discovery download options require pick");
  const cli_matrix_mode_t modes[] = {k_matrix_search, k_matrix_browse};
  for (size_t mode_index = 0U; mode_index < (sizeof(modes) / sizeof(modes[0])); ++mode_index) {
    for (size_t opt_index = 0U; opt_index < (size_t)k_matrix_option_count; ++opt_index) {
      const cli_matrix_option_t opt = (cli_matrix_option_t)opt_index;
      if (!internal_matrix_needs_pick(opt)) {
        continue;
      }
      char*  argv[k_matrix_argv_cap];
      size_t argc = 0U;
      internal_matrix_build_base(modes[mode_index], opt, argv, &argc);
      internal_matrix_add_option(opt, argv, &argc);
      mdl_cli_mode_t actual_mode = k_mdl_cli_mode_invalid;
      TEST_ASSERT(!internal_matrix_validate(argv, argc, &actual_mode));
      TEST_ASSERT_EQ((uint8_t)k_mdl_cli_mode_invalid, (uint8_t)actual_mode);
    }
  }
  TEST_END("CLI discovery download options require pick");
}

/**
 * @test test_help_long_options_have_matrix_rows
 * @brief Require every long option printed by help to exist in the matrix.
 * @details Captures the real usage text, lexes each `--name` token, and checks
 *          it against concrete independent option-row spellings.
 * @pre A bounded RAM stream can hold the complete usage text.
 * @pre Help option spellings use lower-case letters, digits, and hyphens.
 * @post Every long option occurrence in help has a matrix row.
 * @post No host descriptor or C-runtime stream state changes.
 * @note Host-only coverage guard; performs no network activity.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_help_long_options_have_matrix_rows(void)
{
  TEST_BEGIN("CLI help options covered by matrix");
  char help[k_matrix_help_cap];
  TEST_ASSERT(internal_capture_usage(help, sizeof(help)));
  const char* at = help;
  while ((at = strstr(at, "--")) != nullptr) {
    const char* end = at + 2;
    while (((*end >= 'a') && (*end <= 'z')) || ((*end >= '0') && (*end <= '9')) || (*end == '-')) {
      ++end;
    }
    const size_t len = (size_t)(end - at);
    TEST_ASSERT(len < (size_t)k_matrix_token_cap);
    char token[k_matrix_token_cap];
    memcpy(token, at, len);
    token[len] = '\0';
    TEST_ASSERT(internal_matrix_has_spelling(token));
    at = end;
  }
  TEST_END("CLI help options covered by matrix");
}

/**
 * @test test_matrix_tables_are_populated
 * @brief Require every designated matrix row to carry a real fixture.
 * @details Both tables are built with designated initializers indexed by their
 *          own enum, so a mode or option added to the enum without a matching
 *          row stays zero-filled and is then exercised as an empty fixture that
 *          proves nothing. Reading each row's diagnostic name detects exactly
 *          that omission.
 * @pre ::s_matrix_modes has one row per ::cli_matrix_mode_t value.
 * @pre ::s_matrix_options has one row per ::cli_matrix_option_t value.
 * @post Every mode row carries a nonempty name and a populated argv.
 * @post Every option row carries a nonempty name and at least one token.
 * @note Host-only table audit; performs no parsing, filesystem, or network I/O.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_matrix_tables_are_populated(void)
{
  TEST_BEGIN("CLI matrix tables fully populated");
  for (size_t mode = 0U; mode < (size_t)k_matrix_mode_count; ++mode) {
    TEST_ASSERT_NOT_NULL(s_matrix_modes[mode].name);
    TEST_ASSERT(s_matrix_modes[mode].name[0] != '\0');
    TEST_ASSERT(s_matrix_modes[mode].argc > 0U);
  }
  for (size_t opt = 0U; opt < (size_t)k_matrix_option_count; ++opt) {
    TEST_ASSERT_NOT_NULL(s_matrix_options[opt].name);
    TEST_ASSERT(s_matrix_options[opt].name[0] != '\0');
    TEST_ASSERT(s_matrix_options[opt].count > 0U);
  }
  TEST_END("CLI matrix tables fully populated");
}

/* see header for full description */
RA8_PRIV void priv_test_mdl_cli_matrix_run(void)
{
  internal_test_matrix_tables_are_populated();
  internal_test_exhaustive_mode_option_matrix();
  internal_test_every_public_option_repeated();
  internal_test_discovery_download_options_require_pick();
  internal_test_help_long_options_have_matrix_rows();
}
