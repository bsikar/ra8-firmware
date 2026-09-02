/**
 * @file reflow_parse.c
 * @brief HTML-subset parser entry point for reflow.
 *
 * @details
 * The actual scan lives in `reflow_tokenize.c` (a no-heap streaming
 * tokenizer). This C TU is the public entry point:
 * `reflow_layout_chapter()` dispatches into `reflow_parse_xhtml()`,
 * which validates arguments, resets the pools, and forwards to
 * `priv_reflow_xml_walk()`.
 *
 * Keeping this entry point small lets clang-tidy's
 * `readability-function-size` rule act on a single small function.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "reflow.h"
#include "reflow_tokenize_internal.h"

/* ---------------------------------------------------------------------------
 * Public helpers (declared in reflow.h). The cross-TU tokenizer entry
 * point `priv_reflow_xml_walk()` is declared in
 * reflow_tokenize_internal.h and defined in reflow_tokenize.c.
 * ---------------------------------------------------------------------------
 */

ra8_err_t reflow_parse_xhtml(reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if ((engine == nullptr) || (xhtml_buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (xhtml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  /* Reset the token + text pools and the link-target table before each parse.
   * The layout pools (glyphs/pages/images/link-rects/anchors) are cleared
   * separately by reflow_run_layout(). */
  engine->token_count       = 0U;
  engine->text_pool_used    = 0U;
  engine->link_target_count = 0U;

  return priv_reflow_xml_walk(engine, xhtml_buf, xhtml_len);
}
