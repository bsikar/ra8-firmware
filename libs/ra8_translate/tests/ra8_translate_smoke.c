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
static_assert(sizeof(ra8_translate_det_line_t) == 16U, "smoke det line size");
static_assert(sizeof(ra8_translate_det_bubble_t) == 20U, "smoke det bubble size");
static_assert(sizeof(ra8_translate_det_cfg_t) == 32U, "smoke det cfg size");
static_assert(sizeof(ra8_translate_job_chapter_t) == 68U, "smoke job chapter size");
static_assert(sizeof(ra8_translate_job_policy_t) == 32U, "smoke job policy size");
static_assert(sizeof(ra8_translate_job_env_t) == 4U, "smoke job env size");
static_assert(sizeof(ra8_translate_job_ocr_bubble_t) == 24U, "smoke job ocr bubble size");
static_assert(sizeof(ra8_translate_job_span_t) == 8U, "smoke job span size");
static_assert(sizeof(ra8_translate_job_status_t) == 32U, "smoke job status size");
static_assert(sizeof(ra8_translate_job_hooks_t) == sizeof(void*) * 8U, "smoke job hooks size");

int ra8_translate_header_contract_smoke(const ra8_translate_sidecar_header_t* hdr)
{
  size_t work_bytes = 0U;
  (void)ra8_translate_job_work_bytes(&work_bytes);
  return (hdr != nullptr) ? (int)(work_bytes != 0U) : 0;
}
