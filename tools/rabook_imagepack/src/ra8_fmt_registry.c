/**
 * @file ra8_fmt_registry.c
 * @brief The format registry: one descriptor row per first-party container.
 *
 * @details
 * The single source of truth the CLI drives dispatch and its usage text from.
 * Adding a format is one row here plus its verb implementations -- no CLI
 * change, which is the Open/Closed property the ::ra8_fmt_desc_t seam buys.
 *
 * ## Scope
 *
 * These are the first-party **content** container formats. Deliberately
 * excluded are the firmware and security artifacts (Root-of-Trust image
 * trailers, NPU/vela blobs, device-identity records): they are not content, are
 * not produced from a user file, and dumping them from a general-purpose media
 * tool would invite treating signed artifacts as convertible data.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <string.h>

#include "ra8_fmt.h"
#include "ra8_fmt_internal.h"

/** @brief Every format the tool knows, in CLI listing order. */
static const ra8_fmt_desc_t s_formats[] = {
  {
    .name    = "jof",
    .ext     = ".jof",
    .summary = "band-tile atlas (JOF): display-native, O(1) random access per tile",
    .sniff   = ra8_fmt_jof_sniff,
    .convert = ra8_fmt_jof_convert,
    .inspect = ra8_fmt_jof_inspect,
    .verify  = ra8_fmt_jof_verify,
  },
  {
    .name    = "rabook",
    .ext     = ".rabook",
    .summary = "chunked book container (RBKC): compiled book, one unit = one book",
    .sniff   = ra8_fmt_rabook_sniff,
    .convert = nullptr,
    .inspect = ra8_fmt_rabook_inspect,
    .verify  = nullptr,
  },
};

const ra8_fmt_desc_t* ra8_fmt_registry(size_t* out_count)
{
  if (out_count != nullptr) {
    *out_count = sizeof(s_formats) / sizeof(s_formats[0]);
  }
  return s_formats;
}

const ra8_fmt_desc_t* ra8_fmt_find(const char* name)
{
  if (name == nullptr) {
    return nullptr;
  }
  size_t                n    = 0U;
  const ra8_fmt_desc_t* regs = ra8_fmt_registry(&n);
  for (size_t i = 0U; i < n; ++i) { /* bounded by the registry size */
    if (strcmp(regs[i].name, name) == 0) {
      return &regs[i];
    }
  }
  return nullptr;
}

const ra8_fmt_desc_t* ra8_fmt_sniff(const ra8_fmt_blob_t* src)
{
  if ((src == nullptr) || (src->bytes == nullptr)) {
    return nullptr;
  }
  size_t                n    = 0U;
  const ra8_fmt_desc_t* regs = ra8_fmt_registry(&n);
  for (size_t i = 0U; i < n; ++i) { /* bounded by the registry size */
    if ((regs[i].sniff != nullptr) && regs[i].sniff(src)) {
      return &regs[i];
    }
  }
  return nullptr;
}
