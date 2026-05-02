/**
 * @file ra_reflow_parse.c
 * @brief HTML-subset parser entry point for ra_reflow.
 *
 * @details
 * The actual DOM walk lives in `ra_reflow_xml_shim.cpp` because it
 * uses tinyxml2 (C++ API). This C TU is the public side of that
 * boundary: `ra_reflow_layout_chapter()` dispatches into
 * `ra_reflow_parse_xhtml()`, which validates arguments and forwards
 * to the shim's `priv_reflow_xml_walk()`.
 *
 * Keeping the C-side small lets the rest of the library stay in plain
 * C and lets clang-tidy's `readability-function-size` rule act on a
 * single small function.
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
 * Cross-TU shim entry point. The C++ implementation lives in
 * ra_reflow_xml_shim.cpp; this declaration keeps it visible to the
 * C-side driver below without dragging the C++ header into this TU.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Walk the XHTML DOM and populate `engine->tokens[]`.
 *
 * @details
 * Defined in `ra_reflow_xml_shim.cpp`. Splitting parse out of the C
 * driver keeps tinyxml2's C++ surface confined to one TU.
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

/**
 * @brief Implementation of ra_reflow_parse_xhtml (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] engine See implementation.
 * @param[in] xhtml_buf See implementation.
 * @param[in] xhtml_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_reflow_parse_xhtml(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if (engine == NULL || xhtml_buf == NULL) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  /* Reset the token + text pools before each parse. The layout pool
   * is cleared separately by ra_reflow_run_layout(). */
  engine->token_count    = 0U;
  engine->text_pool_used = 0U;

  return priv_reflow_xml_walk(engine, xhtml_buf, xhtml_len);
}
