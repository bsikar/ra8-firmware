/**
 * @file ra_reflow_css.h
 * @brief Minimal content-CSS cascade for the ra_reflow ereader engine (#111).
 *
 * @details
 * A small, hand-written, zero-allocation CSS subset that lets EPUB content
 * style itself. It sits inside `libs/ra_reflow` as a peer of the HTML
 * tokenizer: the tokenizer feeds `<style>` block text and inline `style="..."`
 * attribute values in, and asks this module for the computed style of each
 * element it opens.
 *
 * ## Supported subset (v1)
 *
 *   - **Selectors** (one *simple* selector per rule, comma-grouped):
 *     `*` (universal), `tag` (type), `.class`, `#id`. Compound (`p.note`),
 *     descendant (`div p`) and pseudo selectors are skipped (no match, no
 *     crash) -- a documented limitation, not an error.
 *   - **Properties** (the ones that fold onto ra_reflow's existing per-run
 *     style bits + per-block alignment, so no layout change is needed):
 *     `font-weight` (bold / normal), `font-style` (italic / normal),
 *     `text-decoration` (underline / none), `text-align`
 *     (left / right / center / justify).
 *   - **Cascade**: specificity rank id > class > type > universal; ties broken
 *     by source order (later wins); inline `style=""` beats every rule;
 *     inheritable properties (all four of the above) flow from parent to child.
 *
 * `color`, `font-size`, `display:none` and external/linked stylesheets are out
 * of v1 scope and tracked as follow-ups.
 *
 * The module is **pure** (no MMIO, no allocation): a parsed ::ra_css_sheet_t
 * and the cascade are trivially host-unit-testable with MC/DC.
 *
 * Tag / alignment / font-style values are carried as raw `uint8_t` (storage of
 * ::ra_reflow_html_tag_t, ::ra_reflow_align_t, ::ra_reflow_font_style_t) so this
 * header stays free of a circular include back into `ra_reflow.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_css_limits_t
 * @brief Static-allocation caps for one parsed stylesheet.
 *
 * @details Sized for the small author stylesheets EPUB chapters ship; anything
 * larger is silently truncated (extra rules / names dropped), never an error.
 */
typedef enum : uint16_t {
  k_ra_css_max_rules   = 128U,  /**< Max rules kept across all `<style>` blocks. */
  k_ra_css_name_pool   = 2048U, /**< Bytes of class / id selector-name storage. */
  k_ra_css_max_classes = 8U,    /**< Max classes matched per element.           */
  k_ra_css_name_max    = 64U,   /**< Max bytes of one class / id name.          */
} ra_css_limits_t;

/**
 * @enum ra_css_sel_kind_t
 * @brief The four simple-selector kinds v1 understands.
 */
typedef enum : uint8_t {
  k_ra_css_sel_universal = 0U, /**< `*` -- matches every element.       */
  k_ra_css_sel_type      = 1U, /**< `tag` -- matches by element kind.   */
  k_ra_css_sel_class     = 2U, /**< `.class` -- matches a class token.  */
  k_ra_css_sel_id        = 3U, /**< `#id` -- matches the element id.    */
} ra_css_sel_kind_t;

/**
 * @enum ra_css_set_t
 * @brief Which properties a declaration block actually set (presence bits).
 *
 * @details A property absent from this mask is "not declared" and so does not
 * participate in the cascade for that rule (letting a lower-priority rule or the
 * inherited value show through).
 */
typedef enum : uint8_t {
  k_ra_css_set_bold      = 1U << 0U, /**< `font-weight` was declared.     */
  k_ra_css_set_italic    = 1U << 1U, /**< `font-style` was declared.      */
  k_ra_css_set_underline = 1U << 2U, /**< `text-decoration` was declared. */
  k_ra_css_set_align     = 1U << 3U, /**< `text-align` was declared.      */
  k_ra_css_set_color     = 1U << 4U, /**< `color` was declared.           */
  k_ra_css_set_fontsize  = 1U << 5U, /**< `font-size` was declared.       */
} ra_css_set_t;

/**
 * @enum ra_css_font_unit_t
 * @brief How a declared `font-size` value is interpreted.
 *
 * @details `em` is normalised to a percentage at parse time (1em = 100%), so the
 * engine only ever stores absolute pixels or a percentage of the inherited size.
 */
typedef enum : uint8_t {
  k_ra_css_font_px  = 0U, /**< Value is an absolute pixel size.        */
  k_ra_css_font_pct = 1U, /**< Value is a percentage of inherited px.  */
} ra_css_font_unit_t;

/**
 * @struct ra_css_style_t
 * @brief A declared or computed style: a presence mask plus property values.
 *
 * @details `style` holds the bold / italic / underline VALUES as
 * ::ra_reflow_font_style_t bits (a set bit = "on"); `align` holds an
 * ::ra_reflow_align_t. A field is only meaningful when its ::ra_css_set_t bit is
 * present in `set`.
 */
typedef struct {
  uint8_t  set;       /**< ::ra_css_set_t bitmask: which properties are present.  */
  uint8_t  style;     /**< ::ra_reflow_font_style_t bit VALUES (bold/italic/ul).  */
  uint8_t  align;     /**< ::ra_reflow_align_t (valid iff `set & align`).         */
  uint8_t  font_unit; /**< ::ra_css_font_unit_t (valid iff `set & fontsize`).     */
  uint32_t color;     /**< 0xRRGGBB text colour (valid iff `set & color`).        */
  uint16_t font_val;  /**< font-size number: px or % (valid iff `set & fontsize`).*/
  uint16_t pad;       /**< Padding.                                               */
} ra_css_style_t;

/**
 * @struct ra_css_rule_t
 * @brief One `selector { ... }` rule: a simple selector plus its declarations.
 */
typedef struct {
  uint8_t        sel_kind; /**< ::ra_css_sel_kind_t.                          */
  uint8_t        sel_tag;  /**< ::ra_reflow_html_tag_t for a type selector.   */
  uint16_t       name_off; /**< class/id name slice offset into `names`.      */
  uint16_t       name_len; /**< class/id name slice length, bytes.            */
  uint16_t       order;    /**< Source order (lower = earlier; ties to later).*/
  ra_css_style_t decl;     /**< Declared properties.                          */
} ra_css_rule_t;

/**
 * @struct ra_css_sheet_t
 * @brief A parsed stylesheet: a fixed rule array + a selector-name byte pool.
 *
 * @details Embedded in the reflow engine handle. Reset per chapter with
 * ::ra_css_sheet_reset, then grown by ::ra_css_parse for each `<style>` block.
 */
typedef struct {
  ra_css_rule_t rules[k_ra_css_max_rules]; /**< Parsed rules.            */
  uint16_t      rule_count;                /**< Rules in use.            */
  uint16_t      next_order;                /**< Running source-order ctr.*/
  uint16_t      names_used;                /**< Bytes used in `names`.   */
  uint16_t      pad;                       /**< Padding.                 */
  uint8_t       names[k_ra_css_name_pool]; /**< class/id name bytes.     */
} ra_css_sheet_t;

/**
 * @struct ra_css_element_t
 * @brief The identity of one element, used to match selectors.
 *
 * @details `id` / `class_str` point at caller-owned bytes (e.g. slices of the
 * reflow text pool); they need not be NUL-terminated. A NULL pointer or zero
 * length means the attribute is absent.
 */
typedef struct {
  uint8_t     tag;       /**< ::ra_reflow_html_tag_t of the element.        */
  const char* id;        /**< `id` attribute bytes (NULL = none).           */
  uint16_t    id_len;    /**< Length of @ref id, bytes.                     */
  const char* class_str; /**< `class` attribute bytes (NULL = none).        */
  uint16_t    class_len; /**< Length of @ref class_str, bytes.              */
} ra_css_element_t;

/**
 * @brief Reset a stylesheet to empty (no rules, no names, order 0).
 *
 * @param[out] sheet Stylesheet to clear.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Cleared.
 * @retval k_ra_err_null_ptr @p sheet is NULL.
 *
 * @pre @p sheet is non-NULL.
 * @pre None.
 * @post `sheet->rule_count == 0` and `sheet->names_used == 0`.
 * @post `sheet->next_order == 0`.
 *
 * @note Pure aside from writing @p sheet; not thread-safe relative to it.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_css_sheet_reset(ra_css_sheet_t* sheet);

/**
 * @brief Parse one `<style>` block's CSS text, appending rules to @p sheet.
 *
 * @details Handles `/<star> comments <star>/`, comma-grouped simple selectors,
 * and the v1 property set. Unsupported selectors (compound / descendant /
 * pseudo) and unknown properties are skipped, not errored. Rules beyond
 * ::k_ra_css_max_rules (or names beyond the pool) are dropped silently.
 *
 * @param[in,out] sheet Stylesheet to grow.
 * @param[in]     css   CSS source bytes (a `<style>` block body).
 * @param[in]     len   Length of @p css, bytes.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Parsed (possibly with truncation).
 * @retval k_ra_err_null_ptr @p sheet or @p css is NULL.
 *
 * @pre @p sheet, @p css non-NULL.
 * @pre None.
 * @post New rules (if any room) are appended with ascending `order`.
 * @post `sheet->rule_count` never exceeds ::k_ra_css_max_rules.
 *
 * @note Pure aside from writing @p sheet.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_css_parse(ra_css_sheet_t* sheet, const char* css, uint32_t len);

/**
 * @brief Parse an inline `style="..."` declaration body into one style.
 *
 * @details Same property grammar as ::ra_css_parse but with no selector and no
 * braces -- just `prop: value; prop: value`. The result carries the inline
 * declaration; the caller applies it at the highest specificity.
 *
 * @param[in]  decls Declaration bytes (the `style` attribute value).
 * @param[in]  len   Length of @p decls, bytes.
 * @param[out] out   Receives the parsed declaration (`set == 0` if empty).
 *
 * @return ra_err_t
 * @retval k_ra_ok           Parsed (possibly empty).
 * @retval k_ra_err_null_ptr @p decls or @p out is NULL.
 *
 * @pre @p decls, @p out non-NULL.
 * @pre None.
 * @post `*out` holds only the properties present in @p decls.
 * @post Unknown properties leave their `set` bit clear.
 *
 * @note Pure aside from writing @p out.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_css_parse_inline(const char* decls, uint32_t len, ra_css_style_t* out);

/**
 * @brief Test whether rule @p rule's selector matches element @p el.
 *
 * @param[in] rule  Rule whose selector is tested.
 * @param[in] el    Element identity (tag + id + class list).
 * @param[in] sheet Sheet owning the rule's name pool.
 *
 * @return true iff the simple selector matches @p el.
 * @retval false A required pointer is NULL, or the selector does not match.
 *
 * @pre @p rule, @p el, @p sheet non-NULL (NULL -> false).
 * @pre None.
 * @post No state modified.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra_css_rule_matches(const ra_css_rule_t*    rule,
                                       const ra_css_element_t* el,
                                       const ra_css_sheet_t*   sheet);

/**
 * @brief Compute an element's cascaded style.
 *
 * @details Folds, in increasing priority: @p inherited (inheritable properties
 * only), every matching rule in @p sheet by (specificity, source order), then
 * @p inline_decl (always wins). Each property resolves independently to its
 * highest-priority setter.
 *
 * @param[in] sheet       Parsed stylesheet (may be empty).
 * @param[in] el          Element identity.
 * @param[in] inherited   Parent's computed style (inheritable props flow in).
 * @param[in] inline_decl Inline `style=""` declaration (`set == 0` if none).
 *
 * @return The computed ::ra_css_style_t for @p el.
 *
 * @pre @p sheet, @p el non-NULL (NULL @p sheet/@p el -> returns @p inherited).
 * @pre None.
 * @post Every property bit in the result was set by the winning source.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_css_style_t ra_css_cascade(const ra_css_sheet_t*   sheet,
                                            const ra_css_element_t* el,
                                            ra_css_style_t          inherited,
                                            ra_css_style_t          inline_decl);

#ifdef __cplusplus
}
#endif
