/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_cli.c
 * @brief Implementation of the media_dl command-line parser.
 */
#include "mdl_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief CLI numeric-parse constants. */
typedef enum : uint8_t {
  k_cli_dec_base = 10, /**< strtoull() radix for --max-bytes. */
} mdl_cli_parse_t;

/** @brief Default per-response size cap: bound a hostile/broken stream. */
typedef enum : uint64_t {
  k_max_response_bytes_def = 64ULL * 1024ULL * 1024ULL, /**< 64 MiB per response. */
} mdl_cli_cap_t;

void mdl_cli_usage(const char* a0)
{
  (void)fprintf(stderr,
                "usage:\n"
                "  series: %s --config SITE.conf --series URL [--chapters N] "
                "[--from CHAP] [--update] [--out DIR] "
                "[--format cbz|cbt|cbr|cbt.xz|cbt.gz|epub|jof|rabook] "
                "[--separate] [--seed S] [--timeout MS]\n"
                "          N chapters combine into ONE <slug>-<lo>-<hi>.<ext> by "
                "default; --separate keeps one archive per chapter.\n"
                "          --from CHAP starts at the chapter NUMBERED CHAP (not a "
                "list index); --update fetches only chapters not already complete.\n"
                "  library (over --out): %s --list | --update-all --config SITE.conf "
                "| --remove URL|SLUG [--out DIR]\n"
                "  pack:   %s --pack DIR --format FMT   "
                "package an existing folder of page images (no network)\n"
                "  page:   %s URL [--out DIR] [--max N] [--attr data-src|src] "
                "[--seed S] [--timeout MS]\n"
                "  identity/politeness (series + page):\n"
                "    --contact <email|url>  identify yourself in the User-Agent\n"
                "    --polite               raise delays; per-host concurrency 1\n"
                "    --max-bytes N          per-response size cap (default 64 MiB)\n"
                "    --ignore-robots        do NOT honour robots.txt (logged loudly)\n"
                "    --allow-private        permit loopback/private/link-local peers\n"
                "    --cross-host           permit redirects to a different host\n",
                a0,
                a0,
                a0,
                a0);
}

/** @brief If argv[*i] == `flag`, store its value in *dst and advance `*i`. */
RA8_INTERNAL static bool take_opt(char** argv, int argc, int* i, const char* flag, const char** dst)
{
  if ((argv[*i] == nullptr) || (strcmp(argv[*i], flag) != 0)) {
    return false;
  }
  if ((*i + 1) < argc) {
    *i += 1;
    *dst = argv[*i];
  }
  return true;
}

/** @brief Match a bare boolean flag, setting `*dst` when it is `arg`. */
RA8_INTERNAL static bool take_flag(const char* arg, const char* flag, bool* dst)
{
  if ((arg != nullptr) && (strcmp(arg, flag) == 0)) {
    *dst = true;
    return true;
  }
  return false;
}

/** @brief Consume any recognised boolean flag at `arg`. */
RA8_INTERNAL static bool parse_bool_flags(const char* arg, mdl_args_t* a)
{
  return take_flag(arg, "--separate", &a->separate) || take_flag(arg, "--update", &a->update) ||
         take_flag(arg, "--list", &a->list) || take_flag(arg, "--update-all", &a->update_all) ||
         take_flag(arg, "--polite", &a->polite) ||
         take_flag(arg, "--ignore-robots", &a->ignore_robots) ||
         take_flag(arg, "--allow-private", &a->allow_private) ||
         take_flag(arg, "--cross-host", &a->cross_host);
}

void mdl_cli_parse(int argc, char** argv, mdl_args_t* a)
{
  /* Table-driven long options: each entry binds a flag to the field it fills. */
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
  };
  for (int i = 1; i < argc; ++i) {
    if (parse_bool_flags(argv[i], a)) {
      continue;
    }
    bool matched = false;
    for (size_t k = 0U; (k < (sizeof(opts) / sizeof(opts[0]))) && !matched; ++k) {
      matched = take_opt(argv, argc, &i, opts[k].flag, opts[k].dst);
    }
    if (matched) {
      continue;
    }
    if ((argv[i] != nullptr) && (argv[i][0] != '-')) {
      a->page_url = argv[i];
      continue;
    }
    a->bad = true;
  }
}

mdl_run_opts_t mdl_cli_run_opts(const mdl_args_t* a)
{
  const uint64_t max_bytes = (a->max_bytes == nullptr)
                               ? (uint64_t)k_max_response_bytes_def
                               : strtoull(a->max_bytes, nullptr, k_cli_dec_base);
  return (mdl_run_opts_t){
    .policy       = {.allow_private_hosts       = a->allow_private,
                     .allow_cross_host_redirect = a->cross_host,
                     .max_response_bytes        = max_bytes},
    .contact      = a->contact,
    .honor_robots = !a->ignore_robots,
    .polite       = a->polite,
  };
}
