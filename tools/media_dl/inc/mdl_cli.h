/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_cli.h
 * @brief Command-line parsing for the media_dl CLI.
 *
 * @details
 * Splits argv handling out of `main.c`: the raw option strings land in
 * ::mdl_args_t, and the cross-cutting security/politeness knobs are folded into
 * ::mdl_run_opts_t for the run entry points. Numeric fields stay as strings so
 * `main` owns their conversion and defaulting.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_net.h"

/**
 * @struct mdl_run_opts_t
 * @brief Cross-cutting options threaded into every run mode.
 * @details Bundles the network policy and identity/politeness knobs so the run
 *          entry points take one options pointer rather than a long, swappable
 *          scalar parameter list.
 * @invariant `policy.max_response_bytes` is non-zero for a bounded fetch.
 * @see mdl_cli_run_opts()
 * @since 0.1.0
 */
typedef struct {
  mdl_net_policy_t policy;           /**< Backend security policy.             */
  const char*      contact;          /**< --contact override, or NULL.         */
  bool             honor_robots;     /**< False when --ignore-robots is set.   */
  bool             polite;           /**< True when --polite is set.           */
  bool             allow_incomplete; /**< True when --allow-incomplete is set. */
} mdl_run_opts_t;

/**
 * @struct mdl_args_t
 * @brief Parsed command-line options in string form (converted by main).
 * @details Every value field is a borrowed pointer into `argv`; the boolean
 *          fields record bare flags.
 * @invariant `bad` is set when an unrecognised argument was seen.
 * @see mdl_cli_parse()
 * @since 0.1.0
 */
typedef struct {
  const char* cfg;              /**< --config path.                                            */
  const char* series;           /**< --series URL.                                             */
  const char* page_url;         /**< positional page URL (page mode).                          */
  const char* out;              /**< --out dir.                                                */
  const char* attr;             /**< --attr.                                                   */
  const char* chapters;         /**< --chapters.                                               */
  const char* from;             /**< --from: first chapter NUMBER to fetch (not an index).     */
  const char* max;              /**< --max.                                                    */
  const char* seed;             /**< --seed.                                                   */
  const char* timeout;          /**< --timeout.                                                */
  const char* format;           /**< --format (cbz/cbt/cbr/cbt.xz/cbt.gz/epub/jof/rabook).     */
  const char* pack;             /**< --pack DIR: package an existing folder, no network.       */
  const char* contact;          /**< --contact: operator identity for the User-Agent.          */
  const char* max_bytes;        /**< --max-bytes: per-response size cap.                       */
  const char* remove_series;    /**< --remove: series URL/slug to drop from the library.       */
  const char* search;           /**< --search TERM: find series by title, no known URL.        */
  const char* pick;             /**< --pick N: download the Nth discovery hit (1-based).       */
  bool        browse;           /**< --browse: list a site's latest-updates page.              */
  bool        separate;         /**< --separate: one archive per chapter (default: combine).   */
  bool        update;           /**< --update: fetch only chapters not already complete.       */
  bool        list;             /**< --list: list tracked series with coverage, then exit.     */
  bool        update_all;       /**< --update-all: incremental update of every tracked series. */
  bool        polite;           /**< --polite: raise per-host delays.                          */
  bool        ignore_robots;    /**< --ignore-robots: escape hatch (off by default).           */
  bool        allow_private;    /**< --allow-private: permit private/loopback peers.           */
  bool        cross_host;       /**< --cross-host: permit cross-host redirects.                */
  bool        allow_incomplete; /**< --allow-incomplete: package a run with failed pages.      */
  bool        bad;              /**< An unrecognised argument was seen.                        */
} mdl_args_t;

/**
 * @brief Print usage to stderr.
 * @param[in] a0 `argv[0]`, the program name shown in the usage lines.
 * @return Nothing.
 * @pre `a0` is a NUL-terminated string.
 * @pre stderr is open.
 * @post A usage block is written to stderr.
 * @post No state is modified.
 * @note Thread-safe: writes only to stderr.
 * @since 0.1.0
 */
void mdl_cli_usage(const char* a0);

/**
 * @brief Parse argv into `a`; numeric fields stay as strings for main.
 * @param[in]  argc Argument count.
 * @param[in]  argv Argument vector.
 * @param[out] a    Parsed options; `a->bad` is set on any unknown argument.
 * @return Nothing.
 * @pre `argv` holds `argc` entries; `a` is non-NULL.
 * @pre `a` was zero-initialised (defaults applied) before the call.
 * @post Every recognised option is recorded in `a`.
 * @post `a->bad` reflects whether an argument was unrecognised.
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
void mdl_cli_parse(int argc, char** argv, mdl_args_t* a);

/**
 * @brief Fold parsed args into the cross-cutting run options.
 * @param[in] a Parsed command-line options.
 * @return The assembled run options (network policy + identity/politeness).
 * @retval mdl_run_opts_t A value with the policy and knobs set from `a`.
 * @pre `a` is non-NULL.
 * @pre `a` was populated by ::mdl_cli_parse.
 * @post The returned policy has the size cap defaulted when `--max-bytes` was
 *       absent.
 * @note Thread-safe: depends only on its argument.
 * @since 0.1.0
 */
mdl_run_opts_t mdl_cli_run_opts(const mdl_args_t* a);

/**
 * @struct mdl_nums_t
 * @brief The validated numeric CLI scalars, parsed once before any run mode.
 * @details Parsing the string fields in one strict place means every run mode
 *          sees the same rejection of non-numeric input rather than a silent 0.
 * @invariant `chapters >= 1` after ::mdl_cli_parse_nums succeeds.
 * @see mdl_cli_parse_nums()
 * @since 0.1.0
 */
typedef struct {
  uint64_t seed;         /**< --seed (default 1).                         */
  uint32_t timeout;      /**< --timeout ms (default ::k_req_timeout_def). */
  size_t   chapters;     /**< --chapters window (clamped to >= 1).        */
  uint32_t max_imgs;     /**< --max page images (0 = all).                */
  bool     from_present; /**< Whether --from was supplied.                */
  long     from_num;     /**< --from chapter number.                      */
  size_t   pick;         /**< --pick discovery hit (1-based; 0 = list).   */
} mdl_nums_t;

/**
 * @brief Strictly parse and validate every numeric CLI field.
 *
 * @details
 * Converts the string-form numeric options (`--timeout`, `--chapters`, `--max`,
 * `--seed`, `--from`, and `--max-bytes` for presence-validation) into typed
 * scalars, rejecting any non-numeric or trailing-garbage value with a usage
 * message on stderr rather than silently substituting 0. `--chapters` of 0 is
 * clamped up to 1.
 *
 * @param[in]  a Parsed command-line options (never NULL).
 * @param[out] n Receives the validated scalars (never NULL).
 *
 * @return Whether every present numeric field parsed cleanly.
 * @retval true  All fields valid; @p n is fully populated.
 * @retval false A field was non-numeric or out of range; a usage error was
 *               printed and @p n is partially written.
 *
 * @pre @p a and @p n are non-NULL; @p a was populated by ::mdl_cli_parse.
 * @pre The caller aborts with a usage exit code when this returns false.
 * @post On true, `n->chapters >= 1` and every scalar reflects the args.
 * @post On false, a diagnostic naming the bad option was written to stderr.
 *
 * @note Not thread-safe: writes caller storage and stderr.
 * @since 0.1.0
 */
bool mdl_cli_parse_nums(const mdl_args_t* a, mdl_nums_t* n);
