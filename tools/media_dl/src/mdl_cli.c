/**
 * @file mdl_cli.c
 * @brief Implementation of the media_dl command-line parser.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_cli.h"

#include <errno.h>
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
  (void)fprintf(
      stderr,
      "usage:\n"
      "  series:\n"
      "    %s --config SITE.conf --series URL [--chapters N] [--from CHAP]\n"
      "       [--update] [--out DIR] [--format FMT] [--separate] [--seed S]\n"
      "       [--timeout MS]\n"
      "       Formats: cbz|cbt|cbr|cbt.xz|cbt.gz|epub|jof|rabook\n"
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

      "  page:\n"
      "    %s URL [--out DIR] [--max N] [--attr data-src|src] [--seed S]\n"
      "       [--timeout MS]\n\n"

      "  identity / politeness / network options:\n"
      "    --contact <email|url>  Identify yourself in the User-Agent\n"
      "    --polite               Raise delays; per-host concurrency 1\n"
      "    --progress             Terminal progress bar during downloads\n"
      "    --proxy <URL>          HTTP/HTTPS proxy URL for libcurl\n"
      "    --socks5 <URL>         SOCKS5 proxy URL for libcurl\n"
      "    --cookie-file <FILE>   Cookie file path for libcurl\n"
      "    --max-bytes N          Per-response size cap (default 64 MiB)\n"
      "    --ignore-robots        Do NOT honour robots.txt (logged loudly)\n"
      "    --allow-private        Permit loopback/private/link-local "
      "peers\n"
      "    --cross-host           Permit redirects to a different host\n"
      "    --allow-incomplete     Package run with failed pages; archive\n"
      "                           is named .INCOMPLETE so it is visibly "
      "partial\n",
      a0, a0, a0, a0, a0, a0, a0, a0);
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
         take_flag(arg, "--browse", &a->browse) || take_flag(arg, "--polite", &a->polite) ||
         take_flag(arg, "--ignore-robots", &a->ignore_robots) ||
         take_flag(arg, "--allow-private", &a->allow_private) ||
         take_flag(arg, "--cross-host", &a->cross_host) ||
         take_flag(arg, "--allow-incomplete", &a->allow_incomplete) ||
         take_flag(arg, "--progress", &a->progress);
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
    {"--search", &a->search},
    {"--pick", &a->pick},
    {"--proxy", &a->proxy},
    {"--socks5", &a->socks5},
    {"--cookie-file", &a->cookie_file},
    {"--init-site", &a->init_site_url},
  };
  for (int i = 1; i < argc; ++i) {
    if (parse_bool_flags(argv[i], a)) {
      continue;
    }
    if ((argv[i] != nullptr) && (strcmp(argv[i], "--verify") == 0)) {
      a->verify = true;
      if ((i + 1 < argc) && (argv[i + 1] != nullptr) && (argv[i + 1][0] != '-')) {
        i += 1;
        a->verify_dir = argv[i];
      }
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
    .policy           = {.allow_private_hosts       = a->allow_private,
                         .allow_cross_host_redirect = a->cross_host,
                         .max_response_bytes        = max_bytes,
                         .proxy                     = a->proxy,
                         .socks5                    = a->socks5,
                         .cookie_file               = a->cookie_file},
    .contact          = a->contact,
    .honor_robots     = !a->ignore_robots,
    .polite           = a->polite,
    .allow_incomplete = a->allow_incomplete,
    .progress         = a->progress,
  };
}

/** @brief Strict decimal unsigned parse: false unless all of `s` is digits. */
RA8_INTERNAL static bool parse_ul(const char* s, unsigned long* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] < '0') {
    return false; /* rejects "", sign, whitespace, and other leading non-digits */
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

/** @brief Strict decimal signed parse for `--from` (a leading sign is allowed). */
RA8_INTERNAL static bool parse_l(const char* s, long* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] == '\0') {
    return false;
  }
  errno          = 0;
  char*      end = nullptr;
  const long v   = strtol(s, &end, k_cli_dec_base);
  if (errno != 0) {
    return false;
  }
  if (end == s) {
    return false;
  }
  if (*end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

/** @brief Optional unsigned option: default when absent, usage error on garbage. */
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

/** @brief Optional 64-bit unsigned option: default when absent, error on garbage. */
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
    return false; /* validate presence-garbage; the default is applied in run_opts */
  }
  n->from_present = (a->from != nullptr);
  n->from_num     = 0L;
  if (a->from != nullptr) {
    if (!parse_l(a->from, &n->from_num)) {
      (void)fprintf(stderr, "media_dl: --from expects an integer, got '%s'\n", a->from);
      return false;
    }
  }
  unsigned long pick_ul = 0UL;
  if (!opt_ul("pick", a->pick, 0UL, &pick_ul)) {
    return false;
  }
  n->timeout  = (uint32_t)timeout_ul;
  n->chapters = (chapters_ul == 0UL) ? 1U : (size_t)chapters_ul;
  n->max_imgs = (uint32_t)max_ul;
  n->pick     = (size_t)pick_ul;
  return true;
}
