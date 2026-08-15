/**
 * @file mdl_cli.c
 * @brief Implementation of the media_dl command-line parser.
 * @details Parses bounded option state and emits validation diagnostics through
 *          the injected CLI stream without performing application work.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_cli.h"

#include <stdlib.h>
#include <string.h>

#include "mdl_cli_internal.h"
#include "ra8_attributes.h"

/** @brief Radix used when materializing the validated response-size option. */
typedef enum : uint8_t {
  k_cli_dec_base = 10, /**< Decimal conversion radix. */
} mdl_cli_run_parse_t;

/** @brief Default per-response size cap: bound a hostile/broken stream. */
typedef enum : uint64_t {
  k_max_response_bytes_def = 64ULL * 1024ULL * 1024ULL, /**< 64 MiB per response. */
} mdl_cli_cap_t;

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
 * @pre @p value, when non-NULL, points to a NUL-terminated argument token.
 * @post No state is modified.
 * @post The result depends only on @p value and @p flag.
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
internal_take_opt(char** argv, int argc, int* i, const char* flag, const char** dst, bool* bad)
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
RA8_INTERNAL static bool internal_take_flag(const char* arg, const char* flag, bool* dst, bool* bad)
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

/** @brief Consume any recognised boolean flag at `arg`.
 * @details Matches recognized flag spellings and updates their bounded fields.
 *          Unrecognized input leaves the argument record unchanged.
 * @param[in] arg Current command-line argument.
 * @param[in,out] a Argument record updated for a recognized flag.
 * @return True when @p arg names a recognized boolean flag.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_parse_bool_flags(const char* arg, mdl_args_t* a)
{
  return internal_take_flag(arg, "--help", &a->help, &a->bad) ||
         internal_take_flag(arg, "-h", &a->help, &a->bad) ||
         internal_take_flag(arg, "--version", &a->version, &a->bad) ||
         internal_take_flag(arg, "--separate", &a->separate, &a->bad) ||
         internal_take_flag(arg, "--update", &a->update, &a->bad) ||
         internal_take_flag(arg, "--list", &a->list, &a->bad) ||
         internal_take_flag(arg, "--update-all", &a->update_all, &a->bad) ||
         internal_take_flag(arg, "--browse", &a->browse, &a->bad) ||
         internal_take_flag(arg, "--polite", &a->polite, &a->bad) ||
         internal_take_flag(arg, "--ignore-robots", &a->ignore_robots, &a->bad) ||
         internal_take_flag(arg, "--allow-private", &a->allow_private, &a->bad) ||
         internal_take_flag(arg, "--cross-host", &a->cross_host, &a->bad) ||
         internal_take_flag(arg, "--allow-incomplete", &a->allow_incomplete, &a->bad) ||
         internal_take_flag(arg, "--progress", &a->progress, &a->bad) ||
         internal_take_flag(arg, "--refetch", &a->refetch, &a->bad);
}

/**
 * @brief Consume the optional `--verify DIR` argument pair.
 * @details Recognizes only `--verify`, requires one following token, records the
 *          directory, and advances the shared argument index exactly once.
 * @param[in] argc Number of readable entries in @p argv.
 * @param[in] argv Borrowed command token vector.
 * @param[in,out] i Index of the token being considered.
 * @param[in,out] a Parsed argument state receiving the directory or bad flag.
 * @return Whether the current token was `--verify` and was consumed.
 * @retval true The option was recognized; missing value is recorded in @p a.
 * @retval false The current token was not `--verify` and state is unchanged.
 * @pre @p argv, @p i, and @p a are non-NULL.
 * @pre `0 <= *i < argc` and @p argv has @p argc readable entries.
 * @post A true result advances @p i to the value token when it exists.
 * @post No command-token pointer ownership is transferred.
 * @note Parsing is bounded by @p argc and performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_verify_opt(int argc, char** argv, int* i, mdl_args_t* a)
{
  if ((argv[*i] == nullptr) || (strcmp(argv[*i], "--verify") != 0)) {
    return false;
  }
  if (a->verify) {
    a->bad = true;
  }
  a->verify = true;
  if (((*i + 1) < argc) && (argv[*i + 1] != nullptr) && (argv[*i + 1][0] != '-')) {
    *i += 1;
    a->verify_dir = argv[*i];
  }
  return true;
}

/**
 * @brief Consume one recognized option that requires a value.
 * @details Searches the fixed option table, records the following token through
 *          its destination field, and marks truncated option pairs invalid.
 * @param[in] argc Number of readable entries in @p argv.
 * @param[in] argv Borrowed command token vector.
 * @param[in,out] i Index of the token being considered.
 * @param[in,out] a Parsed argument state receiving the borrowed value pointer.
 * @return Whether the current token names a value-bearing option.
 * @retval true The option was recognized, including a recorded missing value.
 * @retval false The token is not in the fixed value-option table.
 * @pre @p argv, @p i, and @p a are non-NULL.
 * @pre `0 <= *i < argc` and @p argv has @p argc readable entries.
 * @post A recognized option advances @p i only when a value token exists.
 * @post No command-token pointer ownership is transferred.
 * @note Table size and argument traversal are compile-time bounded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_value_opt(int argc, char** argv, int* i, mdl_args_t* a)
{
  const struct {
    const char*  flag; /**< Exact option spelling.                     */
    const char** dst;  /**< Parsed-argument field receiving its value. */
  } opts[] = {
    {"--config", &a->cfg},
    {"--series", &a->series},
    {"--out", &a->out},
    {"--cache-dir", &a->cache_dir},
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
  for (size_t k = 0U; k < (sizeof(opts) / sizeof(opts[0])); ++k) {
    if (internal_take_opt(argv, argc, i, opts[k].flag, opts[k].dst, &a->bad)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Record a positional URL or reject an unexpected token.
 * @details Stores the first non-option token as the page URL; any additional,
 *          NULL, or option-shaped token sets the parser's bad-input flag.
 * @param[in] arg Borrowed command token being classified.
 * @param[in,out] a Parsed argument state to update.
 * @pre @p a is non-NULL.
 * @pre @p arg, when non-NULL, is NUL-terminated.
 * @post At most one positional URL is stored.
 * @post Rejected input sets @p a `bad` without taking token ownership.
 * @note The pointer stored in @p a remains owned by the command vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_parse_positional_or_bad(char* arg, mdl_args_t* a)
{
  if ((arg != nullptr) && (arg[0] != '-')) {
    if (a->page_url != nullptr) {
      a->bad = true;
    }
    a->page_url = arg;
    return;
  }
  a->bad = true;
}

void mdl_cli_parse(int argc, char** argv, mdl_args_t* a)
{
  for (int i = 1; i < argc; ++i) {
    if (internal_parse_bool_flags(argv[i], a) || internal_parse_verify_opt(argc, argv, &i, a) ||
        internal_parse_value_opt(argc, argv, &i, a)) {
      continue;
    }
    internal_parse_positional_or_bad(argv[i], a);
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
  k_arg_cache_dir        = 1ULL << 38U, /**< `--cache-dir`.        */
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
RA8_INTERNAL static uint64_t internal_value_bit(const void* value, uint64_t bit)
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
RA8_INTERNAL static uint64_t internal_flag_bit(bool value, uint64_t bit)
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
RA8_INTERNAL static uint64_t internal_args_mask(const mdl_args_t* a)
{
  return internal_value_bit(a->cfg, k_arg_cfg) | internal_value_bit(a->series, k_arg_series) |
         internal_value_bit(a->page_url, k_arg_page) | internal_value_bit(a->out, k_arg_out) |
         internal_value_bit(a->cache_dir, k_arg_cache_dir) |
         internal_value_bit(a->attr, k_arg_attr) | internal_value_bit(a->chapters, k_arg_chapters) |
         internal_value_bit(a->from, k_arg_from) | internal_value_bit(a->max, k_arg_max) |
         internal_value_bit(a->seed, k_arg_seed) | internal_value_bit(a->timeout, k_arg_timeout) |
         internal_value_bit(a->format, k_arg_format) | internal_value_bit(a->pack, k_arg_pack) |
         internal_value_bit(a->contact, k_arg_contact) |
         internal_value_bit(a->max_bytes, k_arg_max_bytes) |
         internal_value_bit(a->remove_series, k_arg_remove) |
         internal_value_bit(a->search, k_arg_search) | internal_value_bit(a->pick, k_arg_pick) |
         internal_value_bit(a->proxy, k_arg_proxy) | internal_value_bit(a->socks5, k_arg_socks5) |
         internal_value_bit(a->cookie_file, k_arg_cookie) |
         internal_value_bit(a->ca_file, k_arg_ca_file) |
         internal_value_bit(a->verify_dir, k_arg_verify_dir) |
         internal_value_bit(a->init_site_url, k_arg_init) |
         internal_flag_bit(a->browse, k_arg_browse) |
         internal_flag_bit(a->separate, k_arg_separate) |
         internal_flag_bit(a->update, k_arg_update) | internal_flag_bit(a->list, k_arg_list) |
         internal_flag_bit(a->update_all, k_arg_update_all) |
         internal_flag_bit(a->polite, k_arg_polite) |
         internal_flag_bit(a->ignore_robots, k_arg_ignore_robots) |
         internal_flag_bit(a->allow_private, k_arg_allow_private) |
         internal_flag_bit(a->cross_host, k_arg_cross_host) |
         internal_flag_bit(a->allow_incomplete, k_arg_allow_incomplete) |
         internal_flag_bit(a->progress, k_arg_progress) |
         internal_flag_bit(a->refetch, k_arg_refetch) | internal_flag_bit(a->verify, k_arg_verify) |
         internal_flag_bit(a->help, k_arg_help) | internal_flag_bit(a->version, k_arg_version);
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
 * @brief Emit one CLI validation diagnostic and return canonical rejection.
 * @details Centralises the failure convention while preserving stream errors.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @param[in] message Human-readable error text.
 * @return Invalid-argument after a complete write, or the stream failure.
 * @retval k_ra8_err_invalid_arg The complete diagnostic was accepted.
 * @retval other The stream rejected a fragment.
 * @pre @p diagnostic and @p message are non-NULL.
 * @pre @p message is NUL-terminated.
 * @post One newline-terminated diagnostic is attempted on @p diagnostic.
 * @post No parsed argument state is modified.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cli_invalid(ra8_io_stream_t* diagnostic, const char* message)
{
  const char* const parts[] = {"media_dl: ", message, "\n"};
  return priv_mdl_cli_reject_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
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
internal_record_mode(bool condition, mdl_cli_mode_t candidate, mdl_cli_mode_t* mode, size_t* count)
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
RA8_INTERNAL static bool internal_cli_ends_ci(const char* text, const char* suffix)
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
RA8_INTERNAL static bool internal_cli_artifact_url(const char* url)
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
    if (internal_cli_ends_ci(path, suffixes[i])) {
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
RA8_INTERNAL static mdl_cli_mode_t internal_resolve_mode(const mdl_args_t* a, size_t* count)
{
  mdl_cli_mode_t mode = k_mdl_cli_mode_invalid;
  internal_record_mode(a->series != nullptr, k_mdl_cli_mode_series, &mode, count);
  internal_record_mode(a->search != nullptr, k_mdl_cli_mode_search, &mode, count);
  internal_record_mode(a->browse, k_mdl_cli_mode_browse, &mode, count);
  internal_record_mode(a->list, k_mdl_cli_mode_list, &mode, count);
  internal_record_mode(a->update_all, k_mdl_cli_mode_update_all, &mode, count);
  internal_record_mode(a->remove_series != nullptr, k_mdl_cli_mode_remove, &mode, count);
  internal_record_mode(a->verify, k_mdl_cli_mode_verify, &mode, count);
  internal_record_mode(a->init_site_url != nullptr, k_mdl_cli_mode_init_site, &mode, count);
  internal_record_mode(a->pack != nullptr, k_mdl_cli_mode_pack, &mode, count);
  const bool artifact = internal_cli_artifact_url(a->page_url);
  internal_record_mode((a->page_url != nullptr) && artifact, k_mdl_cli_mode_artifact, &mode, count);
  internal_record_mode((a->page_url != nullptr) && !artifact, k_mdl_cli_mode_page, &mode, count);
  internal_record_mode(a->help, k_mdl_cli_mode_help, &mode, count);
  internal_record_mode(a->version, k_mdl_cli_mode_version, &mode, count);
  return mode;
}

RA8_INTERNAL static const char* internal_option_name(uint64_t bit)
{
  static const struct {
    uint64_t    bit;  /**< Presence bit to identify. */
    const char* name; /**< User-facing option name.  */
  } names[] = {{k_arg_cfg, "--config"},
               {k_arg_series, "--series"},
               {k_arg_page, "URL"},
               {k_arg_out, "--out"},
               {k_arg_cache_dir, "--cache-dir"},
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

/**
 * @brief Compute the option mask allowed by one selected mode.
 * @details Resolves mode-specific variants such as series configuration versus
 *          direct URL use and returns the exact compatible option-bit mask.
 * @param[in] a Parsed argument state used to resolve mode variants.
 * @param[in] selected Primary mode selected by exclusivity validation.
 * @param[out] allowed Receives the exact option-bit mask for @p selected.
 * @return Whether @p selected has a valid option-mask definition.
 * @retval true @p allowed was initialized with the selected mode's mask.
 * @retval false The mode or its required variant was invalid.
 * @pre @p a and @p allowed are non-NULL.
 * @pre @p selected is a single ::mdl_cli_mode_t value.
 * @post Success initializes @p allowed exactly once.
 * @post Failure does not grant any unvalidated option.
 * @note Pure mask construction; no argument pointer ownership is transferred.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_cli_allowed_args(const mdl_args_t* a, mdl_cli_mode_t selected, uint64_t* allowed)
{
  const uint64_t net = k_arg_seed | k_arg_timeout | k_arg_contact | k_arg_max_bytes | k_arg_proxy |
                       k_arg_socks5 | k_arg_cookie | k_arg_ca_file | k_arg_polite |
                       k_arg_ignore_robots | k_arg_allow_private | k_arg_cross_host;
  const uint64_t download = k_arg_out | k_arg_cache_dir | k_arg_chapters | k_arg_from |
                            k_arg_format | k_arg_separate | k_arg_update | k_arg_allow_incomplete |
                            k_arg_progress | k_arg_refetch;
  switch (selected) {
    case k_mdl_cli_mode_series:
      *allowed = k_arg_series | k_arg_cfg | net | download;
      return true;
    case k_mdl_cli_mode_search:
      *allowed =
        k_arg_search | k_arg_cfg | k_arg_pick | net | ((a->pick != nullptr) ? download : 0U);
      return true;
    case k_mdl_cli_mode_browse:
      *allowed =
        k_arg_browse | k_arg_cfg | k_arg_pick | net | ((a->pick != nullptr) ? download : 0U);
      return true;
    case k_mdl_cli_mode_list:
      *allowed = k_arg_list | k_arg_out;
      return true;
    case k_mdl_cli_mode_update_all:
      *allowed = k_arg_update_all | k_arg_cfg | k_arg_out | k_arg_cache_dir | k_arg_format |
                 k_arg_separate | k_arg_allow_incomplete | k_arg_progress | k_arg_refetch | net;
      return true;
    case k_mdl_cli_mode_remove:
      *allowed = k_arg_remove | k_arg_out;
      return true;
    case k_mdl_cli_mode_verify:
      *allowed = k_arg_verify | k_arg_verify_dir | k_arg_out;
      return true;
    case k_mdl_cli_mode_init_site:
      *allowed = k_arg_init;
      return true;
    case k_mdl_cli_mode_pack:
      *allowed = k_arg_pack | k_arg_format;
      return true;
    case k_mdl_cli_mode_artifact:
      *allowed = k_arg_page | k_arg_out | net;
      return true;
    case k_mdl_cli_mode_page:
      *allowed = k_arg_page | k_arg_out | k_arg_attr | k_arg_max | net;
      return true;
    case k_mdl_cli_mode_help:
      *allowed = k_arg_help;
      return true;
    case k_mdl_cli_mode_version:
      *allowed = k_arg_version;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Reject option bits that are incompatible with the selected mode.
 * @details Obtains the allowed mask, identifies the first disallowed supplied
 *          bit deterministically, and emits its stable option name.
 * @param[in] a Fully parsed argument state.
 * @param[in,out] diagnostic Bound rejection stream.
 * @param[in] selected Primary mode selected by exclusivity validation.
 * @return Canonical validation or stream status.
 * @retval k_ra8_ok The supplied option mask is allowed.
 * @retval k_ra8_err_invalid_arg One incompatible option was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre @p a and @p diagnostic are non-NULL.
 * @pre @p selected is a single ::mdl_cli_mode_t value.
 * @post No parsed argument field is modified.
 * @post Failure reports at most the lowest-numbered invalid option.
 * @note Validation is bounded by the fixed-width option mask.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_allowed_args(const mdl_args_t* a,
                                                             ra8_io_stream_t*  diagnostic,
                                                             mdl_cli_mode_t    selected)
{
  uint64_t allowed = 0U;
  if (!internal_cli_allowed_args(a, selected, &allowed)) {
    return internal_cli_invalid(diagnostic, "invalid command mode");
  }
  const uint64_t disallowed = internal_args_mask(a) & ~allowed;
  if (disallowed == 0U) {
    return k_ra8_ok;
  }
  const char* const parts[] = {"media_dl: ",
                               internal_option_name(disallowed),
                               " is not valid in ",
                               mdl_cli_mode_name(selected),
                               " mode\n"};
  return priv_mdl_cli_reject_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
}

/**
 * @brief Validate required and mutually exclusive fields for one mode.
 * @details Enforces the page, series, discovery, update-all, and verify operand
 *          shapes after primary-mode exclusivity has been established.
 * @param[in] a Fully parsed argument state.
 * @param[in,out] diagnostic Bound rejection stream.
 * @param[in] selected Primary mode being validated.
 * @return Canonical validation or stream status.
 * @retval k_ra8_ok Every required operand shape is valid.
 * @retval k_ra8_err_invalid_arg One invalid field conjunction was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre @p a and @p diagnostic are non-NULL.
 * @pre @p selected is a single ::mdl_cli_mode_t value.
 * @post No parsed argument field or borrowed token is modified.
 * @post The result reflects all mode-field conjunctions, not option spelling.
 * @note Compatible-option validation is performed separately.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_mode_fields(const mdl_args_t* a,
                                                            ra8_io_stream_t*  diagnostic,
                                                            mdl_cli_mode_t    selected)
{
  const bool config_mode =
    (selected == k_mdl_cli_mode_series) || (selected == k_mdl_cli_mode_search) ||
    (selected == k_mdl_cli_mode_browse) || (selected == k_mdl_cli_mode_update_all);
  if (config_mode && ((a->cfg == nullptr) || (a->cfg[0] == '\0'))) {
    return internal_cli_invalid(diagnostic, "this mode requires --config SITE.conf");
  }
  if ((selected == k_mdl_cli_mode_search) && (a->search[0] == '\0')) {
    return internal_cli_invalid(diagnostic, "--search requires a non-empty term");
  }
  if ((selected == k_mdl_cli_mode_pack) && (a->format == nullptr)) {
    return internal_cli_invalid(diagnostic, "--pack requires an explicit --format");
  }
  if ((selected == k_mdl_cli_mode_verify) && (a->verify_dir != nullptr) && (a->out != nullptr)) {
    return internal_cli_invalid(
      diagnostic,
      "--verify DIR and --out DIR are alternate directory spellings; use one");
  }
  if ((selected == k_mdl_cli_mode_artifact) &&
      (strncmp(a->page_url, "https://", strlen("https://")) != 0)) {
    return internal_cli_invalid(diagnostic, "direct artifact downloads require an https:// URL");
  }
  return k_ra8_ok;
}

/**
 * @brief Validate mutually exclusive and required network-policy arguments.
 * @details Rejects simultaneous proxy transports, partial mTLS credentials,
 *          insecure URLs without opt-in, and malformed contact identities.
 * @param[in] a Fully parsed argument state.
 * @param[in,out] diagnostic Bound rejection stream.
 * @return Canonical validation or stream status.
 * @retval k_ra8_ok Every supplied network-policy field is compatible.
 * @retval k_ra8_err_invalid_arg One unsafe conjunction was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre @p a and @p diagnostic are non-NULL.
 * @pre Every non-NULL string field in @p a is NUL-terminated.
 * @post No parsed argument field or borrowed token is modified.
 * @post Failure is reported before a network interface is initialized.
 * @note Performs syntax and conjunction checks only; no network I/O occurs.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_network_args(const mdl_args_t* a,
                                                             ra8_io_stream_t*  diagnostic)
{
  if ((a->proxy != nullptr) && (a->socks5 != nullptr)) {
    return internal_cli_invalid(diagnostic, "--proxy and --socks5 are mutually exclusive");
  }
  if (((a->proxy != nullptr) || (a->socks5 != nullptr)) && !a->allow_private) {
    return internal_cli_invalid(diagnostic, "--proxy/--socks5 requires --allow-private");
  }
  if ((a->attr != nullptr) && (strcmp(a->attr, "data-src") != 0) && (strcmp(a->attr, "src") != 0)) {
    return internal_cli_invalid(diagnostic, "--attr expects data-src or src");
  }
  return k_ra8_ok;
}

ra8_err_t mdl_cli_validate(const mdl_args_t* a, ra8_io_stream_t* diagnostic, mdl_cli_mode_t* mode)
{
  if ((a == nullptr) || (diagnostic == nullptr) || (mode == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *mode = k_mdl_cli_mode_invalid;
  if (a->bad) {
    return internal_cli_invalid(diagnostic, "unknown, duplicate, or missing-value option");
  }
  size_t               primary_count = 0U;
  const mdl_cli_mode_t selected      = internal_resolve_mode(a, &primary_count);
  if (primary_count == 0U) {
    return internal_cli_invalid(diagnostic, "exactly one command mode is required");
  }
  if (primary_count != 1U) {
    return internal_cli_invalid(diagnostic, "command modes are mutually exclusive");
  }
  ra8_err_t err = internal_validate_allowed_args(a, diagnostic, selected);
  if (err == k_ra8_ok) {
    err = internal_validate_mode_fields(a, diagnostic, selected);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_network_args(a, diagnostic);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  *mode = selected;
  return k_ra8_ok;
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
                         .socks5                    = a->socks5},
    .contact          = a->contact,
    .honor_robots     = !a->ignore_robots,
    .polite           = a->polite,
    .allow_incomplete = a->allow_incomplete,
    .progress         = a->progress,
    .refetch          = a->refetch,
  };
}
