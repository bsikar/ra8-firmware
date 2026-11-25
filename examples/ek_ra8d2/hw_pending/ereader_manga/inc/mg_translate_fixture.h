/**
 * @file inc/mg_translate_fixture.h
 * @brief Golden chapter sidecar + queued/translating fixture for ereader_manga.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_reader.h"

/**
 * @enum mg_trn_fix_const_t
 * @brief Golden translation showcase constants for ereader_manga.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_trn_fix_title_id      = 101U, /**< Demo manga title id.          */
  k_mg_trn_fix_chapter_count = 4U,   /**< Ready, queued, active, stale. */
  k_mg_trn_fix_b0_conf_bps   = 9650U,
  k_mg_trn_fix_b1_conf_bps   = 8840U,
} mg_trn_fix_const_t;

/**
 * @brief Build the golden `TRN1` sidecars and live queued/translating scheduler
 *        state in SDRAM arenas and attach the translation controller to @p r.
 * @param[in,out] r Bound manga reader.
 * @return True if fixture initialization and title bind succeeded.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_fixture_attach(mg_reader_t* r);

/**
 * @brief Return the attached translation controller managed by the fixture.
 * @return Pointer to static controller storage, or NULL before attach.
 * @since 0.1.0
 */
[[nodiscard]] const mg_translate_ctx_t* mg_translate_fixture_ctx(void);

#ifdef __cplusplus
}
#endif
