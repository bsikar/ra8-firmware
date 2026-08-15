/**
 * @file mdl_cli.c
 * @brief Implementation of the media_dl command-line parser.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_cli.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief CLI numeric-parse constants. */
typedef enum : uint8_t {
  k_cli_dec_base = 10, /**< strtoull() radix for --max-bytes. */
} mdl_cli_parse_t;

/** @brief Default per-request time budget when --timeout is absent. */
typedef enum : uint32_t {
  k_req_timeout_def = 25000U, /**< 25 s per-request budget, ms. */
} mdl_cli_timeout_t;

/** @brief Default per-response size cap: bound a hostile/broken stream. */
typedef enum : uint64_t {
  k_max_response_bytes_def = 64ULL * 1024ULL * 1024ULL, /**< 64 MiB per response. */
} mdl_cli_cap_t;

void mdl_cli_usage(const char* a0)
{
  (void)fprintf(stderr,
                "usage:\n"
                "  %s --help | --version\n\n"
                "  series:\n"
                "    %s --config SITE.conf --series URL [--chapters N] [--from CHAP]\n"
                "       [--update] [--out DIR] [--format FMT] [--separate] [--seed S]\n"
                "       [--timeout MS]\n"
                "       Formats: cbz|cbt|cbt.gz|epub|jof\n"
                "       Default: N chapters combine into ONE <slug>-<lo>-<hi>.<ext>.\n"
                "       --separate keeps one archive per chapter.\n"
                "       --from CHAP starts at chapter NUMBERED CHAP (not an index).\n"
                "       --update fetches only incomplete chapters.\n\n"

                "  search:\n"
                "    %s --config SITE.conf --search TERM [--pick N ...opts]\n"
                "       Lists title + series URL per hit.\n"
                "       --pick N downloads hit N directly using --series options.\n\n"

                "  browse:\n"
                "    %s --config SITE.conf --browse [--pick N ...opts]\n"
                "       Lists site's latest updates (requires browse_url in conf).\n\n"

                "  library:\n"
                "    %s --list | --update-all --config SITE.conf | --remove URL|SLUG "
                "[--out DIR]\n\n"

                "  verify:\n"
                "    %s --verify [DIR]\n"
                "       Verify existing downloaded archives/files.\n\n"

                "  init-site:\n"
                "    %s --init-site URL\n"
                "       Generate starter .conf site descriptor template.\n\n"

                "  pack:\n"
                "    %s --pack DIR --format FMT\n"
                "       Package an existing folder of page images (no network).\n\n"

                "  direct artifact:\n"
                "    %s https://HOST/PATH/BOOK.cbz [--out DIR] [network options]\n"
                "       Downloads to staging and publishes only after structural\n"
                "       verification. Verified formats: cbz|cbt|cbt.gz|epub|jof.\n\n"

                "  page:\n"
                "    %s URL [--out DIR] [--max N] [--attr data-src|src] [--seed S]\n"
                "       [--timeout MS]\n\n"

                "  identity / politeness / network options:\n"
                "    --contact <email|url>  Identify yourself in the User-Agent\n"
                "    --polite               Raise delays; per-host concurrency 1\n"
                "    --progress             Terminal progress bar during downloads\n"
                "    --refetch              Bypass the local page cache\n"
                "    --proxy <URL>          HTTP/HTTPS proxy (requires --allow-private)\n"
                "    --socks5 <URL>         SOCKS5 proxy (requires --allow-private)\n"
                "    --cookie-file <FILE>   Cookie file path for libcurl\n"
                "    --ca-file <FILE>       Custom PEM CA bundle (verification stays "
                "on)\n"
                "    --max-bytes N          Per-response size cap (default 64 MiB)\n"
                "    --ignore-robots        Do NOT honour robots.txt (logged loudly)\n"
                "    --allow-private        Permit loopback/private/link-local "
                "peers\n"
                "    --cross-host           Permit redirects to a different host\n"
                "    --allow-incomplete     Package run with failed pages; archive\n"
                "                           is named .INCOMPLETE so it is visibly "
                "partial\n",
                a0,
                a0,
                a0,
                a0,
                a0,
                a0,
                a0,
                a0,
                a0,
                a0);
}

/**
 * @brief Decide whether one token is a value for the matched option.
 * @details Ordinary values cannot begin with `-`; `--from` additionally
 *          accepts a leading minus followed by a digit or decimal point.
 * @param[in] flag Matched option spelling.
 * @param[in] value Candidate following token, or NULL.
 * @return Whether @p value belongs to @p flag.
 * @retval true The token is an ordinary or signed chapter value.
 * @retval false The token is missing or begins another option.
 * @pre @p flag is non-NULL and NUL-terminated.
 * @post No state is modified.
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_option_value(const char* flag, const char* value)
{
  if (value == nullptr) {
    return false;
  }
  if (value[0] != '-') {
    return true;
  }
  if (strcmp(flag, "--from") != 0) {
    return false;
  }
  if ((value[1] >= '0') && (value[1] <= '9')) {
    return true;
  }
  return value[1] == '.';
}

/**
 * @brief Consume one value-bearing option at the current argument.
 * @details Matches @p flag exactly, records its following value, and marks
 *          duplicate or missing values through @p bad. The signed numeric
 *          grammar documented for `--from` is accepted without treating a
 *          different option as its value.
 * @param[in]     argv Argument vector.
 * @param[in]     argc Argument count.
 * @param[in,out] i    Current argument index.
 * @param[in]     flag Option spelling to match.
 * @param[in,out] dst  Destination for the borrowed value pointer.
 * @param[in,out] bad  Accumulated parse-error flag.
 * @return Whether the current argument matched @p flag.
 * @retval true  The option matched, whether or not its value was valid.
 * @retval false The current argument did not match and outputs are unchanged.
 * @pre All pointer arguments are non-NULL.
 * @pre `0 <= *i < argc` and `argv[*i]` is readable.
 * @post On a valid match, @p i advances once and @p dst receives the value.
 * @post A duplicate or missing value sets @p bad true.
 * @note Not thread-safe: mutates caller-owned parse state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
take_opt(char** argv, int argc, int* i, const char* flag, const char** dst, bool* bad)
{
  if ((argv[*i] == nullptr) || (strcmp(argv[*i], flag) != 0)) {
    return false;
  }
  if (((*i + 1) < argc) && internal_is_option_value(flag, argv[*i + 1])) {
    *i += 1;
    if (*dst != nullptr) {
      *bad = true;
    }
    *dst = argv[*i];
  } else {
    *bad = true;
  }
  return true;
}

/**
 * @brief Consume one bare Boolean option.
 * @details Matches @p arg against @p flag and treats a repeated flag as a parse
 *          error while leaving the option selected.
 * @param[in]     arg  Current argument, or NULL.
 * @param[in]     flag Option spelling to match.
 * @param[in,out] dst  Boolean field selected by the option.
 * @param[in,out] bad  Accumulated parse-error flag.
 * @return Whether @p arg matched @p flag.
 * @retval true  The flag matched and @p dst is true.
 * @retval false The flag did not match and outputs are unchanged.
 * @pre @p flag, @p dst, and @p bad are non-NULL.
 * @pre A non-NULL @p arg is NUL-terminated.
 * @post On a first match, @p dst is true without setting @p bad.
 * @post On a repeated match, both @p dst and @p bad are true.
 * @note Not thread-safe: mutates caller-owned parse state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool take_flag(const char* arg, const char* flag, bool* dst, bool* bad)
{
  if ((arg != nullptr) && (strcmp(arg, flag) == 0)) {
    if (*dst) {
      *bad = true;
    }
    *dst = true;
    return true;
  }
  return false;
}

/** @brief Consume any recognised boolean flag at `arg`. */
RA8_INTERNAL static bool parse_bool_flags(const char* arg, mdl_args_t* a)
{
  return take_flag(arg, "--help", &a->help, &a->bad) || take_flag(arg, "-h", &a->help, &a->bad) ||
         take_flag(arg, "--version", &a->version, &a->bad) ||
         take_flag(arg, "--separate", &a->separate, &a->bad) ||
         take_flag(arg, "--update", &a->update, &a->bad) ||
         take_flag(arg, "--list", &a->list, &a->bad) ||
         take_flag(arg, "--update-all", &a->update_all, &a->bad) ||
         take_flag(arg, "--browse", &a->browse, &a->bad) ||
         take_flag(arg, "--polite", &a->polite, &a->bad) ||
         take_flag(arg, "--ignore-robots", &a->ignore_robots, &a->bad) ||
         take_flag(arg, "--allow-private", &a->allow_private, &a->bad) ||
         take_flag(arg, "--cross-host", &a->cross_host, &a->bad) ||
         take_flag(arg, "--allow-incomplete", &a->allow_incomplete, &a->bad) ||
         take_flag(arg, "--progress", &a->progress, &a->bad) ||
         take_flag(arg, "--refetch", &a->refetch, &a->bad);
}

void mdl_cli_parse(int argc, char** argv, mdl_args_t* a)
{
  /* Table-driven long options: each entry binds a flag to the field it fills.
   */
  const struct {
    const char*  flag; /**< Long-option spelling, including the leading "--". */
    const char** dst;  /**< Field in @p a that receives the option's value.   */
  } opts[] = {
    {"--config", &a->cfg},
    {"--series", &a->series},
    {"--out", &a->out},
    {"--attr", &a->attr},
    {"--chapters", &a->chapters},
    {"--from", &a->from},
    {"--max", &a->max},
    {"--seed", &a->seed},
    {"--timeout", &a->timeout},
    {"--format", &a->format},
    {"--pack", &a->pack},
    {"--contact", &a->contact},
    {"--max-bytes", &a->max_bytes},
    {"--remove", &a->remove_series},
    {"--search", &a->search},
    {"--pick", &a->pick},
    {"--proxy", &a->proxy},
    {"--socks5", &a->socks5},
    {"--cookie-file", &a->cookie_file},
    {"--ca-file", &a->ca_file},
    {"--init-site", &a->init_site_url},
  };
  for (int i = 1; i < argc; ++i) {
    if (parse_bool_flags(argv[i], a)) {
      continue;
    }
    if ((argv[i] != nullptr) && (strcmp(argv[i], "--verify") == 0)) {
      if (a->verify) {
        a->bad = true;
      }
      a->verify = true;
      if ((i + 1 < argc) && (argv[i + 1] != nullptr) && (argv[i + 1][0] != '-')) {
        i += 1;
        a->verify_dir = argv[i];
      }
      continue;
    }
    bool matched = false;
    for (size_t k = 0U; (k < (sizeof(opts) / sizeof(opts[0]))) && !matched; ++k) {
      matched = take_opt(argv, argc, &i, opts[k].flag, opts[k].dst, &a->bad);
    }
    if (matched) {
      continue;
    }
    if ((argv[i] != nullptr) && (argv[i][0] != '-')) {
      if (a->page_url != nullptr) {
        a->bad = true;
      }
      a->page_url = argv[i];
      continue;
    }
    a->bad = true;
  }
}

/** @brief Bit positions for every CLI spelling, used by mode allowlists. */
typedef enum : uint64_t {
  k_arg_cfg              = 1ULL << 0U,  /**< `--config`.           */
  k_arg_series           = 1ULL << 1U,  /**< `--series`.           */
  k_arg_page             = 1ULL << 2U,  /**< Positional URL.       */
  k_arg_out              = 1ULL << 3U,  /**< `--out`.              */
  k_arg_attr             = 1ULL << 4U,  /**< `--attr`.             */
  k_arg_chapters         = 1ULL << 5U,  /**< `--chapters`.         */
  k_arg_from             = 1ULL << 6U,  /**< `--from`.             */
  k_arg_max              = 1ULL << 7U,  /**< `--max`.              */
  k_arg_seed             = 1ULL << 8U,  /**< `--seed`.             */
  k_arg_timeout          = 1ULL << 9U,  /**< `--timeout`.          */
  k_arg_format           = 1ULL << 10U, /**< `--format`.           */
  k_arg_pack             = 1ULL << 11U, /**< `--pack`.             */
  k_arg_contact          = 1ULL << 12U, /**< `--contact`.          */
  k_arg_max_bytes        = 1ULL << 13U, /**< `--max-bytes`.        */
  k_arg_remove           = 1ULL << 14U, /**< `--remove`.           */
  k_arg_search           = 1ULL << 15U, /**< `--search`.           */
  k_arg_pick             = 1ULL << 16U, /**< `--pick`.             */
  k_arg_proxy            = 1ULL << 17U, /**< `--proxy`.            */
  k_arg_socks5           = 1ULL << 18U, /**< `--socks5`.           */
  k_arg_cookie           = 1ULL << 19U, /**< `--cookie-file`.      */
  k_arg_verify_dir       = 1ULL << 20U, /**< Optional verify dir.  */
  k_arg_init             = 1ULL << 21U, /**< `--init-site`.        */
  k_arg_browse           = 1ULL << 22U, /**< `--browse`.           */
  k_arg_separate         = 1ULL << 23U, /**< `--separate`.         */
  k_arg_update           = 1ULL << 24U, /**< `--update`.           */
  k_arg_list             = 1ULL << 25U, /**< `--list`.             */
  k_arg_update_all       = 1ULL << 26U, /**< `--update-all`.       */
  k_arg_polite           = 1ULL << 27U, /**< `--polite`.           */
  k_arg_ignore_robots    = 1ULL << 28U, /**< `--ignore-robots`.    */
  k_arg_allow_private    = 1ULL << 29U, /**< `--allow-private`.    */
  k_arg_cross_host       = 1ULL << 30U, /**< `--cross-host`.       */
  k_arg_allow_incomplete = 1ULL << 31U, /**< `--allow-incomplete`. */
  k_arg_progress         = 1ULL << 32U, /**< `--progress`.         */
  k_arg_refetch          = 1ULL << 33U, /**< `--refetch`.          */
  k_arg_verify           = 1ULL << 34U, /**< `--verify`.           */
  k_arg_help             = 1ULL << 35U, /**< `--help`.             */
  k_arg_version          = 1ULL << 36U, /**< `--version`.          */
  k_arg_ca_file          = 1ULL << 37U, /**< `--ca-file`.          */
} mdl_cli_arg_bit_t;

/**
 * @brief Convert pointer presence to an option-mask bit.
 * @details Returns the supplied single-bit value only for a non-NULL pointer.
 * @param[in] value Borrowed pointer-valued option.
 * @param[in] bit   Presence bit assigned to that option.
 * @return @p bit when present, otherwise zero.
 * @retval 0 @p value is NULL.
 * @pre @p bit is zero or one valid option bit.
 * @pre @p value is borrowed and never dereferenced.
 * @post No caller or global state is modified.
 * @post The result contains no bit other than @p bit.
 * @note Thread-safe: pure value conversion.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t value_bit(const void* value, uint64_t bit)
{
  return (value != nullptr) ? bit : 0U;
}

/**
 * @brief Convert a Boolean option to an option-mask bit.
 * @details Returns the supplied single-bit value only when @p value is true.
 * @param[in] value Boolean option value.
 * @param[in] bit   Presence bit assigned to that option.
 * @return @p bit when selected, otherwise zero.
 * @retval 0 @p value is false.
 * @pre @p value is a canonical C Boolean.
 * @pre @p bit is zero or one valid option bit.
 * @post No caller or global state is modified.
 * @post The result contains no bit other than @p bit.
 * @note Thread-safe: pure value conversion.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t flag_bit(bool value, uint64_t bit)
{
  return value ? bit : 0U;
}

/**
 * @brief Convert populated CLI fields into one presence mask.
 * @details Maps every pointer and Boolean option to its named allowlist bit.
 * @param[in] a Parsed CLI arguments.
 * @return Bitwise union of all present options.
 * @retval 0 No tracked option is populated.
 * @pre @p a is non-NULL.
 * @pre Pointer fields in @p a are NULL or borrowed NUL-terminated arguments.
 * @post @p a is unchanged.
 * @post Every set result bit corresponds to one populated field in @p a.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t args_mask(const mdl_args_t* a)
{
  return value_bit(a->cfg, k_arg_cfg) | value_bit(a->series, k_arg_series) |
         value_bit(a->page_url, k_arg_page) | value_bit(a->out, k_arg_out) |
         value_bit(a->attr, k_arg_attr) | value_bit(a->chapters, k_arg_chapters) |
         value_bit(a->from, k_arg_from) | value_bit(a->max, k_arg_max) |
         value_bit(a->seed, k_arg_seed) | value_bit(a->timeout, k_arg_timeout) |
         value_bit(a->format, k_arg_format) | value_bit(a->pack, k_arg_pack) |
         value_bit(a->contact, k_arg_contact) | value_bit(a->max_bytes, k_arg_max_bytes) |
         value_bit(a->remove_series, k_arg_remove) | value_bit(a->search, k_arg_search) |
         value_bit(a->pick, k_arg_pick) | value_bit(a->proxy, k_arg_proxy) |
         value_bit(a->socks5, k_arg_socks5) | value_bit(a->cookie_file, k_arg_cookie) |
         value_bit(a->ca_file, k_arg_ca_file) | value_bit(a->verify_dir, k_arg_verify_dir) |
         value_bit(a->init_site_url, k_arg_init) | flag_bit(a->browse, k_arg_browse) |
         flag_bit(a->separate, k_arg_separate) | flag_bit(a->update, k_arg_update) |
         flag_bit(a->list, k_arg_list) | flag_bit(a->update_all, k_arg_update_all) |
         flag_bit(a->polite, k_arg_polite) | flag_bit(a->ignore_robots, k_arg_ignore_robots) |
         flag_bit(a->allow_private, k_arg_allow_private) |
         flag_bit(a->cross_host, k_arg_cross_host) |
         flag_bit(a->allow_incomplete, k_arg_allow_incomplete) |
         flag_bit(a->progress, k_arg_progress) | flag_bit(a->refetch, k_arg_refetch) |
         flag_bit(a->verify, k_arg_verify) | flag_bit(a->help, k_arg_help) |
         flag_bit(a->version, k_arg_version);
}

const char* mdl_cli_mode_name(mdl_cli_mode_t mode)
{
  static const char* const names[] = {"invalid",
                                      "series",
                                      "search",
                                      "browse",
                                      "list",
                                      "update-all",
                                      "remove",
                                      "verify",
                                      "init-site",
                                      "pack",
                                      "artifact",
                                      "page",
                                      "help",
                                      "version"};
  return ((unsigned)mode < (sizeof(names) / sizeof(names[0]))) ? names[mode] : names[0];
}

/**
 * @brief Emit one CLI validation diagnostic and return false.
 * @details Centralises the failure convention used by mode validation.
 * @param[in] message Human-readable error text.
 * @return Always false for direct propagation from validation branches.
 * @retval false The diagnostic was emitted.
 * @pre @p message is non-NULL.
 * @pre @p message is NUL-terminated.
 * @post One newline-terminated diagnostic is attempted on standard error.
 * @post No parsed argument state is modified.
 * @note Not thread-safe with concurrent standard-error writers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool invalid(const char* message)
{
  (void)fprintf(stderr, "media_dl: %s\n", message);
  return false;
}

/**
 * @brief Record one selected primary CLI mode.
 * @details Updates the candidate and selection count only when @p condition is
 *          true, enabling explicit conjunction-based mode resolution.
 * @param[in]     condition Whether the candidate mode was selected.
 * @param[in]     candidate Mode associated with the condition.
 * @param[in,out] mode      Last selected mode.
 * @param[in,out] count     Number of selected primary modes.
 * @pre @p mode and @p count are non-NULL.
 * @pre @p count can be incremented without `size_t` overflow.
 * @post On true, @p mode equals @p candidate and @p count grows by one.
 * @post On false, both outputs are unchanged.
 * @note Not thread-safe: mutates caller-owned resolution state.
 * @since 0.1.0
 */
RA8_INTERNAL static void
record_mode(bool condition, mdl_cli_mode_t candidate, mdl_cli_mode_t* mode, size_t* count)
{
  if (condition) {
    *mode = candidate;
    *count += 1U;
  }
}

/**
 * @brief Test an ASCII suffix without case sensitivity.
 * @details Compares only the candidate suffix bytes and performs no filesystem
 *          access or locale-dependent conversion.
 * @param[in] text   NUL-terminated candidate string.
 * @param[in] suffix NUL-terminated suffix.
 * @return Whether @p text ends with @p suffix under ASCII folding.
 * @retval true  Every suffix byte matches.
 * @retval false The suffix is longer or at least one byte differs.
 * @pre @p text and @p suffix are non-NULL.
 * @pre Both inputs are NUL-terminated.
 * @post Both inputs are unchanged.
 * @post No filesystem or locale state is accessed.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool cli_ends_ci(const char* text, const char* suffix)
{
  const size_t tl = strlen(text);
  const size_t sl = strlen(suffix);
  if (sl > tl) {
    return false;
  }
  for (size_t i = 0U; i < sl; ++i) {
    char a = text[tl - sl + i];
    char b = suffix[i];
    if ((a >= 'A') && (a <= 'Z')) {
      a = (char)(a + ('a' - 'A'));
    }
    if ((b >= 'A') && (b <= 'Z')) {
      b = (char)(b + ('a' - 'A'));
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Recognise an artifact suffix in a URL path.
 * @details Removes query/fragment text, bounds the path locally, and checks the
 *          configured archive/book suffix allowlist case-insensitively.
 * @param[in] url Candidate URL, or NULL.
 * @return Whether the URL path names a known artifact suffix.
 * @retval true  One allowlisted suffix matched.
 * @retval false The URL is NULL, empty/overlong, or has no known suffix.
 * @pre A non-NULL @p url is NUL-terminated.
 * @pre @p url remains valid for the call duration.
 * @post @p url is unchanged.
 * @post No filesystem or network state is accessed.
 * @note Thread-safe: uses only automatic and immutable storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool cli_artifact_url(const char* url)
{
  if (url == nullptr) {
    return false;
  }
  const size_t path_len = strcspn(url, "?#");
  char         path[1024];
  if ((path_len == 0U) || (path_len >= sizeof(path))) {
    return false;
  }
  memcpy(path, url, path_len);
  path[path_len] = '\0';
  static const char* const suffixes[] =
    {".cbt.gz", ".cbt.xz", ".rabook", ".epub", ".cbz", ".cbr", ".cbt", ".jof"};
  for (size_t i = 0U; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    if (cli_ends_ci(path, suffixes[i])) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Resolve primary-mode fields and count their selections.
 * @details Classifies positional URLs as artifacts or pages and records every
 *          other mutually-exclusive primary-mode option.
 * @param[in]     a     Parsed CLI arguments.
 * @param[in,out] count Selection counter, normally initialised to zero.
 * @return The last selected mode, or ::k_mdl_cli_mode_invalid.
 * @retval k_mdl_cli_mode_invalid No primary mode was selected.
 * @pre @p a and @p count are non-NULL.
 * @pre @p count can grow by the number of primary modes without overflow.
 * @post @p count grows once per selected primary mode.
 * @post @p a is unchanged.
 * @note Thread-safe: mutates only @p count.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_cli_mode_t resolve_mode(const mdl_args_t* a, size_t* count)
{
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  record_mode(a->series != nullptr, k_mdl_cli_mode_series, &mode, count);
  record_mode(a->search != nullptr, k_mdl_cli_mode_search, &mode, count);
  record_mode(a->browse, k_mdl_cli_mode_browse, &mode, count);
  record_mode(a->list, k_mdl_cli_mode_list, &mode, count);
  record_mode(a->update_all, k_mdl_cli_mode_update_all, &mode, count);
  record_mode(a->remove_series != nullptr, k_mdl_cli_mode_remove, &mode, count);
  record_mode(a->verify, k_mdl_cli_mode_verify, &mode, count);
  record_mode(a->init_site_url != nullptr, k_mdl_cli_mode_init_site, &mode, count);
  record_mode(a->pack != nullptr, k_mdl_cli_mode_pack, &mode, count);
  const bool artifact = cli_artifact_url(a->page_url);
  record_mode((a->page_url != nullptr) && artifact, k_mdl_cli_mode_artifact, &mode, count);
  record_mode((a->page_url != nullptr) && !artifact, k_mdl_cli_mode_page, &mode, count);
  record_mode(a->help, k_mdl_cli_mode_help, &mode, count);
  record_mode(a->version, k_mdl_cli_mode_version, &mode, count);
  return mode;
}

RA8_INTERNAL static const char* option_name(uint64_t bit)
{
  static const struct {
    uint64_t    bit;  /**< Presence bit to identify. */
    const char* name; /**< User-facing option name.  */
  } names[] = {{k_arg_cfg, "--config"},
               {k_arg_series, "--series"},
               {k_arg_page, "URL"},
               {k_arg_out, "--out"},
               {k_arg_attr, "--attr"},
               {k_arg_chapters, "--chapters"},
               {k_arg_from, "--from"},
               {k_arg_max, "--max"},
               {k_arg_seed, "--seed"},
               {k_arg_timeout, "--timeout"},
               {k_arg_format, "--format"},
               {k_arg_pack, "--pack"},
               {k_arg_contact, "--contact"},
               {k_arg_max_bytes, "--max-bytes"},
               {k_arg_remove, "--remove"},
               {k_arg_search, "--search"},
               {k_arg_pick, "--pick"},
               {k_arg_proxy, "--proxy"},
               {k_arg_socks5, "--socks5"},
               {k_arg_cookie, "--cookie-file"},
               {k_arg_ca_file, "--ca-file"},
               {k_arg_verify_dir, "DIR"},
               {k_arg_init, "--init-site"},
               {k_arg_browse, "--browse"},
               {k_arg_separate, "--separate"},
               {k_arg_update, "--update"},
               {k_arg_list, "--list"},
               {k_arg_update_all, "--update-all"},
               {k_arg_polite, "--polite"},
               {k_arg_ignore_robots, "--ignore-robots"},
               {k_arg_allow_private, "--allow-private"},
               {k_arg_cross_host, "--cross-host"},
               {k_arg_allow_incomplete, "--allow-incomplete"},
               {k_arg_progress, "--progress"},
               {k_arg_refetch, "--refetch"},
               {k_arg_verify, "--verify"},
               {k_arg_help, "--help"},
               {k_arg_version, "--version"}};
  for (size_t i = 0U; i < (sizeof(names) / sizeof(names[0])); ++i) {
    if ((bit & names[i].bit) != 0U) {
      return names[i].name;
    }
  }
  return "option";
}

bool mdl_cli_validate(const mdl_args_t* a, mdl_cli_mode_t* mode)
{
  if ((a == nullptr) || (mode == nullptr)) {
    return false;
  }
  *mode = k_mdl_cli_mode_invalid;
  if (a->bad) {
    return invalid("unknown, duplicate, or missing-value option");
  }
  size_t         primary_count = 0U;
  mdl_cli_mode_t selected      = resolve_mode(a, &primary_count);
  if (primary_count == 0U) {
    return invalid("exactly one command mode is required");
  }
  if (primary_count != 1U) {
    return invalid("command modes are mutually exclusive");
  }

  const uint64_t net = k_arg_seed | k_arg_timeout | k_arg_contact | k_arg_max_bytes | k_arg_proxy |
                       k_arg_socks5 | k_arg_cookie | k_arg_ca_file | k_arg_polite |
                       k_arg_ignore_robots | k_arg_allow_private | k_arg_cross_host;
  const uint64_t download = k_arg_out | k_arg_chapters | k_arg_from | k_arg_format |
                            k_arg_separate | k_arg_update | k_arg_allow_incomplete |
                            k_arg_progress | k_arg_refetch;
  uint64_t       allowed  = 0U;
  switch (selected) {
    case k_mdl_cli_mode_series:
      allowed = k_arg_series | k_arg_cfg | net | download;
      break;
    case k_mdl_cli_mode_search:
      allowed =
        k_arg_search | k_arg_cfg | k_arg_pick | net | ((a->pick != nullptr) ? download : 0U);
      break;
    case k_mdl_cli_mode_browse:
      allowed =
        k_arg_browse | k_arg_cfg | k_arg_pick | net | ((a->pick != nullptr) ? download : 0U);
      break;
    case k_mdl_cli_mode_list:
      allowed = k_arg_list | k_arg_out;
      break;
    case k_mdl_cli_mode_update_all:
      allowed = k_arg_update_all | k_arg_cfg | k_arg_out | k_arg_format | k_arg_separate |
                k_arg_allow_incomplete | k_arg_progress | k_arg_refetch | net;
      break;
    case k_mdl_cli_mode_remove:
      allowed = k_arg_remove | k_arg_out;
      break;
    case k_mdl_cli_mode_verify:
      allowed = k_arg_verify | k_arg_verify_dir | k_arg_out;
      break;
    case k_mdl_cli_mode_init_site:
      allowed = k_arg_init;
      break;
    case k_mdl_cli_mode_pack:
      allowed = k_arg_pack | k_arg_format;
      break;
    case k_mdl_cli_mode_artifact:
      allowed = k_arg_page | k_arg_out | net;
      break;
    case k_mdl_cli_mode_page:
      allowed = k_arg_page | k_arg_out | k_arg_attr | k_arg_max | net;
      break;
    case k_mdl_cli_mode_help:
      allowed = k_arg_help;
      break;
    case k_mdl_cli_mode_version:
      allowed = k_arg_version;
      break;
    default:
      return invalid("invalid command mode");
  }
  const uint64_t disallowed = args_mask(a) & ~allowed;
  if (disallowed != 0U) {
    (void)fprintf(stderr,
                  "media_dl: %s is not valid in %s mode\n",
                  option_name(disallowed),
                  mdl_cli_mode_name(selected));
    return false;
  }
  if (((selected == k_mdl_cli_mode_series) || (selected == k_mdl_cli_mode_search) ||
       (selected == k_mdl_cli_mode_browse) || (selected == k_mdl_cli_mode_update_all)) &&
      ((a->cfg == nullptr) || (a->cfg[0] == '\0'))) {
    return invalid("this mode requires --config SITE.conf");
  }
  if ((selected == k_mdl_cli_mode_search) && (a->search[0] == '\0')) {
    return invalid("--search requires a non-empty term");
  }
  if ((selected == k_mdl_cli_mode_pack) && (a->format == nullptr)) {
    return invalid("--pack requires an explicit --format");
  }
  if ((selected == k_mdl_cli_mode_verify) && (a->verify_dir != nullptr) && (a->out != nullptr)) {
    return invalid("--verify DIR and --out DIR are alternate directory "
                   "spellings; use one");
  }
  if ((selected == k_mdl_cli_mode_artifact) &&
      (strncmp(a->page_url, "https://", strlen("https://")) != 0)) {
    return invalid("direct artifact downloads require an https:// URL");
  }
  if ((a->proxy != nullptr) && (a->socks5 != nullptr)) {
    return invalid("--proxy and --socks5 are mutually exclusive");
  }
  if (((a->proxy != nullptr) || (a->socks5 != nullptr)) && !a->allow_private) {
    return invalid("--proxy/--socks5 requires --allow-private");
  }
  if ((a->attr != nullptr) && (strcmp(a->attr, "data-src") != 0) && (strcmp(a->attr, "src") != 0)) {
    return invalid("--attr expects data-src or src");
  }
  *mode = selected;
  return true;
}

mdl_run_opts_t mdl_cli_run_opts(const mdl_args_t* a)
{
  const uint64_t max_bytes = (a->max_bytes == nullptr)
                               ? (uint64_t)k_max_response_bytes_def
                               : strtoull(a->max_bytes, nullptr, k_cli_dec_base);
  return (mdl_run_opts_t){
    .policy           = {.allow_private_hosts       = a->allow_private,
                         .allow_cross_host_redirect = a->cross_host,
                         .max_response_bytes        = max_bytes,
                         .proxy                     = a->proxy,
                         .socks5                    = a->socks5,
                         .cookie_file               = a->cookie_file,
                         .ca_file                   = a->ca_file},
    .contact          = a->contact,
    .honor_robots     = !a->ignore_robots,
    .polite           = a->polite,
    .allow_incomplete = a->allow_incomplete,
    .progress         = a->progress,
    .refetch          = a->refetch,
  };
}

/** @brief Strict decimal unsigned parse: false unless all of `s` is digits. */
RA8_INTERNAL static bool parse_ul(const char* s, unsigned long* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] < '0') {
    return false; /* rejects "", sign, whitespace, and other leading non-digits
                   */
  }
  if (s[0] > '9') {
    return false;
  }
  errno                   = 0;
  char*               end = nullptr;
  const unsigned long v   = strtoul(s, &end, k_cli_dec_base);
  if (errno != 0) {
    return false;
  }
  if (*end != '\0') {
    return false; /* trailing garbage ("12abc") is not a number */
  }
  *out = v;
  return true;
}

/** @brief Strict decimal 64-bit unsigned parse (as ::parse_ul, wider type). */
RA8_INTERNAL static bool parse_ull(const char* s, uint64_t* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] < '0') {
    return false;
  }
  if (s[0] > '9') {
    return false;
  }
  errno                        = 0;
  char*                    end = nullptr;
  const unsigned long long v   = strtoull(s, &end, k_cli_dec_base);
  if (errno != 0) {
    return false;
  }
  if (*end != '\0') {
    return false;
  }
  *out = (uint64_t)v;
  return true;
}

/**
 * @brief Parse one complete finite decimal chapter number for `--from`.
 *
 * @details Uses the C locale numeric grammar exposed by `strtod`, then rejects
 * overflow, a missing conversion, trailing bytes, NaN, and infinity. A leading
 * sign and a fractional part are accepted so chapter 0 and chapter 108.5 remain
 * distinguishable from a missing value.
 *
 * @param[in]  s   NUL-terminated option value.
 * @param[out] out Receives the finite chapter number.
 *
 * @return Whether the complete value was accepted.
 * @retval true  @p out received one finite number.
 * @retval false An argument or numeric spelling was invalid.
 *
 * @pre @p s and @p out are non-NULL for success.
 * @pre The caller does not consume @p out after false.
 * @post On true, `isfinite(*out)` is true.
 * @post No global state other than the temporary `errno` value is retained.
 *
 * @note Not thread-safe with code that concurrently depends on `errno`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_chapter_value(const char* s, double* out)
{
  if ((s == nullptr) || (out == nullptr) || (s[0] == '\0')) {
    return false;
  }
  errno            = 0;
  char*        end = nullptr;
  const double v   = strtod(s, &end);
  if ((errno != 0) || (end == s) || (*end != '\0') || !isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

/** @brief Optional unsigned option: default when absent, usage error on
 * garbage. */
RA8_INTERNAL static bool
opt_ul(const char* name, const char* s, unsigned long dflt, unsigned long* out)
{
  if (s == nullptr) {
    *out = dflt;
    return true;
  }
  if (parse_ul(s, out)) {
    return true;
  }
  (void)fprintf(stderr, "media_dl: --%s expects a non-negative integer, got '%s'\n", name, s);
  return false;
}

/** @brief Optional 64-bit unsigned option: default when absent, error on
 * garbage. */
RA8_INTERNAL static bool opt_ull(const char* name, const char* s, uint64_t dflt, uint64_t* out)
{
  if (s == nullptr) {
    *out = dflt;
    return true;
  }
  if (parse_ull(s, out)) {
    return true;
  }
  (void)fprintf(stderr, "media_dl: --%s expects a non-negative integer, got '%s'\n", name, s);
  return false;
}

bool mdl_cli_parse_nums(const mdl_args_t* a, mdl_nums_t* n)
{
  unsigned long timeout_ul  = 0UL;
  unsigned long chapters_ul = 0UL;
  unsigned long max_ul      = 0UL;
  if (!opt_ul("timeout", a->timeout, (unsigned long)k_req_timeout_def, &timeout_ul)) {
    return false;
  }
  if (!opt_ul("chapters", a->chapters, 1UL, &chapters_ul)) {
    return false;
  }
  if (!opt_ul("max", a->max, 0UL, &max_ul)) {
    return false;
  }
  if (!opt_ull("seed", a->seed, 1U, &n->seed)) {
    return false;
  }
  uint64_t max_bytes = 0U;
  if (!opt_ull("max-bytes", a->max_bytes, 0U, &max_bytes)) {
    return false; /* validate presence-garbage; the default is applied in
                     run_opts */
  }
  n->from_present = (a->from != nullptr);
  n->from_num     = 0.0;
  if (a->from != nullptr) {
    if (!parse_chapter_value(a->from, &n->from_num)) {
      (void)fprintf(stderr,
                    "media_dl: --from expects a finite chapter number, got '%s'\n",
                    a->from);
      return false;
    }
  }
  unsigned long pick_ul = 0UL;
  if (!opt_ul("pick", a->pick, 0UL, &pick_ul)) {
    return false;
  }
  if ((timeout_ul == 0UL) || (timeout_ul > (unsigned long)UINT32_MAX)) {
    (void)fprintf(stderr, "media_dl: --timeout must be in 1..%u\n", UINT32_MAX);
    return false;
  }
  if ((chapters_ul == 0UL) || ((uintmax_t)chapters_ul > (uintmax_t)SIZE_MAX)) {
    (void)fprintf(stderr, "media_dl: --chapters must be in 1..%zu\n", SIZE_MAX);
    return false;
  }
  if (max_ul > (unsigned long)UINT32_MAX) {
    (void)fprintf(stderr, "media_dl: --max must not exceed %u\n", UINT32_MAX);
    return false;
  }
  if ((a->max_bytes != nullptr) && (max_bytes == 0U)) {
    return invalid("--max-bytes must be greater than zero");
  }
  if ((a->pick != nullptr) && ((pick_ul == 0UL) || ((uintmax_t)pick_ul > (uintmax_t)SIZE_MAX))) {
    (void)fprintf(stderr, "media_dl: --pick must be in 1..%zu\n", SIZE_MAX);
    return false;
  }
  n->timeout  = (uint32_t)timeout_ul;
  n->chapters = (size_t)chapters_ul;
  n->max_imgs = (uint32_t)max_ul;
  n->pick     = (size_t)pick_ul;
  return true;
}
