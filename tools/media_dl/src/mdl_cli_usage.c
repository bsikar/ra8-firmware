/**
 * @file mdl_cli_usage.c
 * @brief Stable command-line help rendering for media_dl.
 * @details Owns only process-edge usage text so the parser translation unit
 *          stays below the repository size cap without sharing parser state.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_cli.h"
#include "mdl_cli_internal.h"
#include "ra8_attributes.h"

/**
 * @brief Write series, discovery, and library help sections.
 * @param[in,out] diagnostic Bound destination for the usage text.
 * @param[in] a0 NUL-terminated program name displayed in usage examples.
 * @return Canonical stream status.
 * @retval k_ra8_ok The section was accepted completely.
 * @retval other The stream rejected one fragment.
 * @pre @p diagnostic and @p a0 are non-NULL and @p a0 is NUL-terminated.
 * @post Success describes series, search, browse, and library modes.
 * @post Failure stops at the first rejected fragment.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Writes fixed help fragments in order through the injected stream.
 *          The first sink error stops later output and is returned unchanged.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static ra8_err_t internal_cli_usage_discovery(ra8_io_stream_t* diagnostic,
                                                           const char*      a0)
{
  const char* const parts[] = {
    "  series:\n"
    "    ",
    a0,
    " --config SITE.conf --series URL [--chapters N] [--from CHAP]\n"
    "       [--update] [--out DIR] [--cache-dir DIR] [--format FMT] [--separate] [--seed S]\n"
    "       [--timeout MS]\n"
    "       Formats: cbz|cbt|cbt.gz|epub|jof\n"
    "       Default: N chapters combine into ONE <slug>-<lo>-<hi>.<ext>.\n"
    "       --separate keeps one archive per chapter.\n"
    "       --from CHAP starts at chapter NUMBERED CHAP (not an index).\n"
    "       --update fetches only incomplete chapters.\n\n"
    "  search:\n"
    "    ",
    a0,
    " --config SITE.conf --search TERM [--pick N ...opts]\n"
    "       Lists title + series URL per hit.\n"
    "       --pick N downloads hit N directly using --series options.\n\n"
    "  browse:\n"
    "    ",
    a0,
    " --config SITE.conf --browse [--pick N ...opts]\n"
    "       Lists site's latest updates (requires browse_url in conf).\n\n"
    "  library:\n"
    "    ",
    a0,
    " --list | --update-all --config SITE.conf | --remove URL|SLUG [--out DIR]\n\n",
  };
  return priv_mdl_cli_put_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
}

/**
 * @brief Write verify, init, pack, artifact, and direct-page help sections.
 * @param[in,out] diagnostic Bound destination for the usage text.
 * @param[in] a0 NUL-terminated program name displayed in examples.
 * @return Canonical stream status.
 * @retval k_ra8_ok Every section was accepted completely.
 * @retval other The stream rejected one fragment.
 * @pre @p diagnostic and @p a0 are non-NULL.
 * @pre @p a0 is NUL-terminated.
 * @post Success describes every non-discovery primary mode exactly once.
 * @post Failure stops at the first rejected fragment.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Writes fixed help fragments in order through the injected stream.
 *          The first sink error stops later output and is returned unchanged.
 */
RA8_INTERNAL static ra8_err_t internal_cli_usage_actions(ra8_io_stream_t* diagnostic,
                                                         const char*      a0)
{
  const char* const parts[] = {
    "  verify:\n"
    "    ",
    a0,
    " --verify [DIR]\n"
    "       Verify existing downloaded archives/files.\n\n"
    "  init-site:\n"
    "    ",
    a0,
    " --init-site URL\n"
    "       Generate starter .conf site descriptor template.\n\n"
    "  pack:\n"
    "    ",
    a0,
    " --pack DIR --format FMT\n"
    "       Package an existing folder of page images (no network).\n\n"
    "  direct artifact:\n"
    "    ",
    a0,
    " https://HOST/PATH/BOOK.cbz [--out DIR] [network options]\n"
    "       Downloads to staging and publishes only after structural\n"
    "       verification. Verified formats: cbz|cbt|cbt.gz|epub|jof.\n\n"
    "  page:\n"
    "    ",
    a0,
    " URL [--out DIR] [--max N] [--attr data-src|src] [--seed S]\n"
    "       [--timeout MS]\n\n",
  };
  return priv_mdl_cli_put_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
}

/**
 * @brief Write the identity, politeness, and network CLI options.
 * @param[in,out] diagnostic Bound destination for the usage text.
 * @return Canonical stream status.
 * @retval k_ra8_ok The complete section was accepted.
 * @retval other The stream rejected the section.
 * @pre @p diagnostic is non-NULL and bound.
 * @pre Invocation is serialized with other writers to the same stream.
 * @post Every supported network-policy option is described exactly once.
 * @post No parser state or caller-owned storage is modified.
 * @note Synchronous process-edge output; no ownership is transferred.
 * @since 0.1.0

 * @details Writes fixed help fragments in order through the injected stream.
 *          The first sink error stops later output and is returned unchanged.
 */
RA8_INTERNAL static ra8_err_t internal_cli_usage_network_options(ra8_io_stream_t* diagnostic)
{
  return ra8_io_stream_puts(
    diagnostic,
    "  identity / politeness / network options:\n"
    "    --contact <email|url>  Identify yourself in the User-Agent\n"
    "    --polite               Raise delays; per-host concurrency 1\n"
    "    --progress             Terminal progress bar during downloads\n"
    "    --cache-dir <DIR>      Per-host cache root (default: OUT/.mdl_cache)\n"
    "    --refetch              Force cache revalidation\n"
    "    --proxy <URL>          HTTP/HTTPS proxy (requires --allow-private)\n"
    "    --socks5 <URL>         SOCKS5 proxy (requires --allow-private)\n"
    "    --cookie-file <FILE>   Cookie file path for libcurl\n"
    "    --ca-file <FILE>       Custom PEM CA bundle (verification stays on)\n"
    "    --max-bytes N          Per-response size cap (default 64 MiB)\n"
    "    --ignore-robots        Do NOT honour robots.txt (logged loudly)\n"
    "    --allow-private        Permit loopback/private/link-local peers\n"
    "    --cross-host           Permit redirects to a different host\n"
    "    --allow-incomplete     Package run with failed pages; archive\n"
    "                           is named .INCOMPLETE so it is visibly partial\n");
}

ra8_err_t mdl_cli_usage(ra8_io_stream_t* diagnostic, const char* a0)
{
  if ((diagnostic == nullptr) || (a0 == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const char* const heading[] = {"usage:\n  ", a0, " --help | --version\n\n"};
  ra8_err_t err = priv_mdl_cli_put_parts(diagnostic, heading, sizeof(heading) / sizeof(heading[0]));
  if (err == k_ra8_ok) {
    err = internal_cli_usage_discovery(diagnostic, a0);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_usage_actions(diagnostic, a0);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_usage_network_options(diagnostic);
  }
  return err;
}
