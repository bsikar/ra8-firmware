/**
 * @file ra8_reflow_parse.c
 * @brief HTML-subset parser entry point for ra8_reflow.
 *
 * @details
 * The actual scan lives in `ra8_reflow_tokenize.c` (a no-heap streaming
 * tokenizer). This C TU is the public entry point:
 * `ra8_reflow_layout_chapter()` dispatches into `ra8_reflow_parse_xhtml()`,
 * which validates arguments, resets the pools, and forwards to
 * `priv_reflow_xml_walk()`.
 *
 * Keeping this entry point small lets clang-tidy's
 * `readability-function-size` rule act on a single small function.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_reflow.h"

/* ---------------------------------------------------------------------------
 * Cross-TU tokenizer entry point. The implementation lives in
 * ra8_reflow_tokenize.c; this declaration keeps it visible to the driver
 * below.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Tokenize the XHTML buffer and populate `engine->tokens[]`.
 *
 * @details
 * Defined in `ra8_reflow_tokenize.c`. Single forward pass, no heap, no DOM.
 *
 * @param[in,out] engine    Engine whose token / text pools to populate.
 * @param[in]     xhtml_buf XHTML source bytes.
 * @param[in]     xhtml_len Length of `xhtml_buf`, bytes.
 *
 * @return ra8_err_t -- see `ra8_reflow_parse_xhtml()`.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
ra8_err_t priv_reflow_xml_walk(ra8_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

/* ---------------------------------------------------------------------------
 * Public helpers (declared in ra8_reflow.h)
 * ---------------------------------------------------------------------------
 */

ra8_err_t ra8_reflow_parse_xhtml(ra8_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if (engine == nullptr || xhtml_buf == nullptr) {
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
   * separately by ra8_reflow_run_layout(). */
  engine->token_count       = 0U;
  engine->text_pool_used    = 0U;
  engine->link_target_count = 0U;

  return priv_reflow_xml_walk(engine, xhtml_buf, xhtml_len);
}
