/**
 * @file ra_reflow_parse.c
 * @brief HTML-subset parser entry point for ra_reflow.
 *
 * @details
 * The actual scan lives in `ra_reflow_tokenize.c` (a no-heap streaming
 * tokenizer). This C TU is the public entry point:
 * `ra_reflow_layout_chapter()` dispatches into `ra_reflow_parse_xhtml()`,
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

#include "ra_err.h"
#include "ra_reflow.h"

/* ---------------------------------------------------------------------------
 * Cross-TU tokenizer entry point. The implementation lives in
 * ra_reflow_tokenize.c; this declaration keeps it visible to the driver
 * below.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Tokenize the XHTML buffer and populate `engine->tokens[]`.
 *
 * @details
 * Defined in `ra_reflow_tokenize.c`. Single forward pass, no heap, no DOM.
 *
 * @param[in,out] engine    Engine whose token / text pools to populate.
 * @param[in]     xhtml_buf XHTML source bytes.
 * @param[in]     xhtml_len Length of `xhtml_buf`, bytes.
 *
 * @return ra_err_t -- see `ra_reflow_parse_xhtml()`.
 *
 * @since 0.1.0
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

/* ---------------------------------------------------------------------------
 * Public helpers (declared in ra_reflow.h)
 * ---------------------------------------------------------------------------
 */

ra_err_t ra_reflow_parse_xhtml(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if (engine == nullptr || xhtml_buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  /* Reset the token + text pools and the link-target table before each parse.
   * The layout pools (glyphs/pages/images/link-rects/anchors) are cleared
   * separately by ra_reflow_run_layout(). */
  engine->token_count       = 0U;
  engine->text_pool_used    = 0U;
  engine->link_target_count = 0U;

  return priv_reflow_xml_walk(engine, xhtml_buf, xhtml_len);
}
