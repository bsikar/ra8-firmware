/**
 * @file reflow_tokenize_test_util.h
 * @brief Shared engine fixture for the test_reflow_tokenize_*_mcdc.c siblings.
 *
 * @details
 * Header-only test fixture providing the shared reflow engine instance, the
 * tokenizer walk wrapper, the token-stream probes (text_has, count_kind,
 * first_text_color), and the external-stylesheet loader stubs shared by
 * test_reflow_tokenize_scan_mcdc.c, test_reflow_tokenize_tag_mcdc.c,
 * and test_reflow_tokenize_link_mcdc.c. Everything here has internal
 * linkage, so each including test executable owns a private engine.
 *
 * The walk is reached font-free: `priv_reflow_xml_walk` populates the token /
 * text pools without any glyph layout, so no font fixture is needed for the
 * markup-dispatch decisions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "reflow.h"
#include "reflow_css.h"
#include "reflow_tokenize_internal.h"
#include "unity_minimal.h"

/* Forward decl of the tokenizer entry point (defined in the production TU). */

/** @brief Shared engine instance (large -- keep off the stack). */
static reflow_t s_engine;

/** @brief Numeric / sizing constants used across the vectors (no magic numbers). */
typedef enum : uint32_t {
  k_cp_uppercase_a   = 65U,         /**< 'A', decimal "&#65;" expectation.        */
  k_cp_hex_4f        = 0x4FU,       /**< 'O', "&#x4f;" lowercase-hex expectation. */
  k_cp_hex_4f_upper  = 0x4FU,       /**< 'O', "&#X4F;" uppercase-hex expectation. */
  k_avail_three      = 3U,          /**< Length of the 3-byte "&#x" fragment.     */
  k_used_dec_a       = 5U,          /**< Bytes consumed for "&#65;".              */
  k_default_font_px  = 16U,         /**< Body font size for the face-slot walk.   */
  k_viewport_w       = 200U,        /**< Layout viewport width, px.               */
  k_viewport_h       = 400U,        /**< Layout viewport height, px.              */
  k_body_color       = 0xFFFFFFU,   /**< Body colour for the layout engine.       */
  k_link_color       = 0x3060FFU,   /**< Link colour for the layout engine.       */
  k_face_css_idx     = 0U,          /**< `@font-face` table index to register.    */
  k_count_one        = 1U,          /**< Expected count of exactly one token.     */
  k_no_text_sentinel = 0xDEADBEEFU, /**< first_text_color(): no text token.       */
} test_consts_t;

/**
 * @brief Run the tokenizer over a NUL-terminated XHTML string.
 *
 * @details Resets the engine's token / text pools then invokes the production
 * single-pass walk, exactly as `tests/src/test_reflow_tokenize.c` does, so the
 * crafted markup drives the real dispatch chain.
 *
 * @param[in] xhtml NUL-terminated XHTML source.
 * @return The walk result code.
 */
static inline ra8_err_t walk(const char* xhtml)
{
  s_engine.token_count    = 0U;
  s_engine.text_pool_used = 0U;
  return priv_reflow_xml_walk(&s_engine, (const uint8_t*)xhtml, strlen(xhtml));
}

/** @brief True iff some text token's pool slice contains @p needle. */
static inline bool text_has(const char* needle)
{
  const size_t nl = strlen(needle);
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind != (uint8_t)k_reflow_tok_text) {
      continue;
    }
    const char*    t  = (const char*)&s_engine.text_pool[s_engine.tokens[i].text_off];
    const uint32_t tl = s_engine.tokens[i].text_len;
    if (nl > (size_t)tl) {
      continue;
    }
    for (uint32_t j = 0U; ((size_t)j + nl) <= (size_t)tl; ++j) {
      if (strncmp(&t[j], needle, nl) == 0) {
        return true;
      }
    }
  }
  return false;
}

/** @brief Count tokens of @p kind in the last walk's stream. */
static inline uint32_t count_kind(reflow_token_kind_t kind)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)kind) {
      ++n;
    }
  }
  return n;
}
/**
 * @brief reflow_css_loader_fn stub that hands back ::k_ext_css for any href.
 *
 * @details Implements the engine's external-stylesheet loader seam for the
 * `<link>` tests: ignores the requested href and always returns the fixed
 * ::k_ext_css bytes with k_ra8_ok, so a bound loader's success path is driven.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href, bytes (unused).
 * @param[out] out_bytes Receives a pointer to ::k_ext_css.
 * @param[out] out_len   Receives the ::k_ext_css length, bytes.
 * @return Always k_ra8_ok.
 * @retval k_ra8_ok Stylesheet bytes returned.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes` aliases ::k_ext_css and `*out_len` is its length.
 * @note Test helper; not thread-safe.
 */
static inline ra8_err_t
css_stub(void* ctx, const char* href, uint32_t href_len, const uint8_t** out_bytes, size_t* out_len)
{
  static const char k_ext_css[] = ".lead{color:#c80000}";
  (void)ctx;
  (void)href;
  (void)href_len;
  *out_bytes = (const uint8_t*)k_ext_css;
  *out_len   = sizeof(k_ext_css) - 1U;
  return k_ra8_ok;
}

/**
 * @brief reflow_css_loader_fn stub that always fails (no bytes).
 *
 * @details Drives the loader-failure path of priv_handle_link: clears the
 * output pointers and returns k_ra8_err_not_found, so the css-parse step is
 * skipped and the chapter keeps its default colours.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href, bytes (unused).
 * @param[out] out_bytes Set to nullptr (no bytes).
 * @param[out] out_len   Set to 0 (no bytes).
 * @return Always k_ra8_err_not_found.
 * @retval k_ra8_err_not_found Stylesheet unavailable.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes == nullptr` and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static inline ra8_err_t css_fail_stub(void*           ctx,
                                      const char*     href,
                                      uint32_t        href_len,
                                      const uint8_t** out_bytes,
                                      size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out_bytes = nullptr;
  *out_len   = 0U;
  return k_ra8_err_not_found;
}

/**
 * @brief Colour of the first text token after a walk (sentinel if none).
 *
 * @details Scans ::s_engine's token stream from the most recent walk and
 * returns the `color` field of the first text token. Used by the `<link>` /
 * `<style>` tests to observe whether a stylesheet rule resolved onto a run.
 *
 * @return The first text token's 0xRRGGBB colour (or k_reflow_color_inherit),
 *         or k_no_text_sentinel when the stream holds no text token.
 * @retval k_no_text_sentinel No text token in the last walk.
 * @pre A walk has populated ::s_engine.
 * @post No state is modified (read-only).
 * @note Test helper; reads ::s_engine, not thread-safe.
 */
static inline uint32_t first_text_color(void)
{
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)k_reflow_tok_text) {
      return s_engine.tokens[i].color;
    }
  }
  return (uint32_t)k_no_text_sentinel;
}
