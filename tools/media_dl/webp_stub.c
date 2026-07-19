/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file webp_stub.c
 * @brief JPEG-only JOF export: stub the WebP transcode arm out of the firmware
 *        tile-atlas producer so the CLI links without vendoring libwebp.
 *
 * @details
 * `ra8_tileatlas_produce()` references `ra8_ta_priv_webp_transcode()` in its
 * WebP-magic branch. media_dl only ever transcodes downloaded JPEG pages and
 * passes `webp_work == NULL`, which fail-closed rejects any WebP source before
 * this arm runs -- so the symbol is needed only to satisfy the linker, never
 * called. Defining it here (instead of compiling ra8_tileatlas_produce_webp.c)
 * keeps libwebp + the ra8_webp facade out of the host tool.
 *
 * Kept out of src/ so it is outside this tool's clang-tidy scope (it is
 * firmware-SOUP-adjacent glue, compiled quietly like the reused libs).
 */
#include "ra8_tileatlas_internal.h"

RA8_PRIV ra8_err_t ra8_ta_priv_webp_transcode(ra8_ta_prod_state_t* st, ra8_ta_prefix_pull_t* pfx)
{
  (void)st;
  (void)pfx;
  return k_ra8_err_not_supported;
}
