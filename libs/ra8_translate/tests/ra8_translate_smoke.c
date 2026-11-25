/**
 * @file ra8_translate_smoke.c
 * @brief Freestanding C23 contract compile check for `inc/ra8_translate.h`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_translate.h"

static_assert(sizeof(ra8_translate_sidecar_header_t) == 76U, "smoke header size");
static_assert(sizeof(ra8_translate_sidecar_page_t) == 12U, "smoke page size");
static_assert(sizeof(ra8_translate_sidecar_bubble_t) == 44U, "smoke bubble size");
static_assert(sizeof(ra8_translate_vocab_header_t) == 36U, "smoke vocab header size");
static_assert(sizeof(ra8_translate_overlay_style_t) == 24U, "smoke overlay style size");
static_assert(sizeof(ra8_translate_overlay_bubble_spec_t) == 16U, "smoke overlay bubble spec size");
static_assert(sizeof(ra8_translate_overlay_fit_t) == 12U, "smoke overlay fit size");
static_assert(sizeof(ra8_translate_ctc_charset_header_t) == 32U, "smoke ctc header size");
static_assert(sizeof(ra8_translate_ctc_result_t) == 24U, "smoke ctc result size");

int ra8_translate_header_contract_smoke(const ra8_translate_sidecar_header_t* hdr)
{
  return (hdr != nullptr) ? 1 : 0;
}
