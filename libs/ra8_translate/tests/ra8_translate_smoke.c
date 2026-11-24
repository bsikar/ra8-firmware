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

int ra8_translate_header_contract_smoke(const ra8_translate_sidecar_header_t* hdr)
{
  return (hdr != nullptr) ? 1 : 0;
}
