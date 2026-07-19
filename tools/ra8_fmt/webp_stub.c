/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file webp_stub.c
 * @brief Stub the WebP transcode arm out of the firmware tile-atlas producer so
 *        `ra8_fmt` links without vendoring libwebp into a host debugging tool.
 *
 * @details
 * `ra8_tileatlas_produce()` references `ra8_ta_priv_webp_transcode()` from its
 * WebP-magic branch. `ra8_fmt` passes `webp_work == NULL`, which fail-closes on
 * any WebP source before this arm can run, so the symbol is needed only to
 * satisfy the linker and is never called. Defining it here (rather than
 * compiling `ra8_tileatlas_produce_webp.c`) keeps libwebp and the ra8_webp
 * facade out of the tool, exactly as `tools/media_dl/webp_stub.c` does.
 *
 * Consequence: `ra8_fmt` converts and verifies JPEG and PNG sources. A WebP
 * source is rejected with `k_ra8_err_not_supported` rather than mis-decoded.
 *
 * Kept out of src/ so it sits outside this tool's clang-tidy scope, matching
 * the media_dl precedent (it is firmware-SOUP-adjacent link glue).
 */
#include "ra8_tileatlas_internal.h"

RA8_PRIV ra8_err_t ra8_ta_priv_webp_transcode(ra8_ta_prod_state_t* st, ra8_ta_prefix_pull_t* pfx)
{
  (void)st;
  (void)pfx;
  return k_ra8_err_not_supported;
}
