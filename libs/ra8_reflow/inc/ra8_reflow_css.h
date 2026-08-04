/**
 * @file ra8_reflow_css.h
 * @brief Minimal content-CSS cascade for the ra8_reflow ereader engine (#111).
 * @ingroup grp_ereader
 *
 * @details
 * A small, hand-written, zero-allocation CSS subset that lets EPUB content
 * style itself. It sits inside `libs/ra8_reflow` as a peer of the HTML
 * tokenizer: the tokenizer feeds `<style>` block text and inline `style="..."`
 * attribute values in, and asks this module for the computed style of each
 * element it opens.
 *
 * ## Supported subset (v1)
 *
 *   - **Selectors** (comma-grouped): `*` (universal), `tag` (type), `.class`,
 *     `#id`, compound (`p.note`), and descendant (`div p`, `.chapter p`,
 *     `.a .b`, up to ::k_ra8_css_max_anc ancestor parts). Multiple classes per
 *     compound (`.a.b`), child/sibling combinators and pseudo selectors are
 *     skipped (no match, no crash) -- a documented limitation, not an error.
 *   - **Properties** (the ones that fold onto ra8_reflow's existing per-run
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
 * The module is **pure** (no MMIO, no allocation): a parsed ::ra8_css_sheet_t
 * and the cascade are trivially host-unit-testable with MC/DC.
 *
 * Tag / alignment / font-style values are carried as raw `uint8_t` (storage of
 * ::ra8_reflow_html_tag_t, ::ra8_reflow_align_t, ::ra8_reflow_font_style_t) so this
 * header stays free of a circular include back into `ra8_reflow.h`.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_css_limits_t
 * @brief Static-allocation caps for one parsed stylesheet.
 *
 * @details Sized for the small author stylesheets EPUB chapters ship; anything
 * larger is silently truncated (extra rules / names dropped), never an error.
 */
typedef enum : uint16_t {
  k_ra8_css_max_rules   = 128U,  /**< Max rules kept across all `<style>` blocks. */
  k_ra8_css_name_pool   = 2048U, /**< Bytes of class / id / font name storage.    */
  k_ra8_css_max_classes = 8U,    /**< Max classes matched per element.            */
  k_ra8_css_name_max    = 64U,   /**< Max bytes of one class / id / font name.    */
  k_ra8_css_max_faces   = 16U,   /**< Max `@font-face` rules kept per sheet.      */
  k_ra8_css_max_anc     = 4U,    /**< Max descendant-ancestor parts per selector. */
} ra8_css_limits_t;

/**
 * @enum ra8_css_face_match_t
 * @brief Sentinel for ::ra8_css_match_face when no `@font-face` entry matches.
 */
typedef enum : int16_t {
  k_ra8_css_no_face = -1, /**< No `@font-face` matched the requested family. */
} ra8_css_face_match_t;

/**
 * @enum ra8_css_set_t
 * @brief Which properties a declaration block actually set (presence bits).
 *
 * @details A property absent from this mask is "not declared" and so does not
 * participate in the cascade for that rule (letting a lower-priority rule or the
 * inherited value show through).
 */
typedef enum : uint8_t {
  k_ra8_css_set_bold      = 1U << 0U, /**< `font-weight` was declared.     */
  k_ra8_css_set_italic    = 1U << 1U, /**< `font-style` was declared.      */
  k_ra8_css_set_underline = 1U << 2U, /**< `text-decoration` was declared. */
  k_ra8_css_set_align     = 1U << 3U, /**< `text-align` was declared.      */
  k_ra8_css_set_color     = 1U << 4U, /**< `color` was declared.           */
  k_ra8_css_set_fontsize  = 1U << 5U, /**< `font-size` was declared.       */
  k_ra8_css_set_display   = 1U << 6U, /**< `display` was declared.         */
  k_ra8_css_set_family    = 1U << 7U, /**< `font-family` was declared.     */
} ra8_css_set_t;

/**
 * @enum ra8_css_font_unit_t
 * @brief How a declared `font-size` value is interpreted.
 *
 * @details `em` is normalised to a percentage at parse time (1em = 100%), so the
 * engine only ever stores absolute pixels or a percentage of the inherited size.
 */
typedef enum : uint8_t {
  k_ra8_css_font_px  = 0U, /**< Value is an absolute pixel size.       */
  k_ra8_css_font_pct = 1U, /**< Value is a percentage of inherited px. */
} ra8_css_font_unit_t;

/**
 * @struct ra8_css_style_t
 * @brief A declared or computed style: a presence mask plus property values.
 *
 * @details `style` holds the bold / italic / underline VALUES as
 * ::ra8_reflow_font_style_t bits (a set bit = "on"); `align` holds an
 * ::ra8_reflow_align_t. A field is only meaningful when its ::ra8_css_set_t bit is
 * present in `set`.
 */
typedef struct {
  uint8_t  set;        /**< ::ra8_css_set_t bitmask: which properties are present.  */
  uint8_t  style;      /**< ::ra8_reflow_font_style_t bit VALUES (bold/italic/ul).  */
  uint8_t  align;      /**< ::ra8_reflow_align_t (valid iff `set & align`).         */
  uint8_t  font_unit;  /**< ::ra8_css_font_unit_t (valid iff `set & fontsize`).     */
  uint32_t color;      /**< 0xRRGGBB text colour (valid iff `set & color`).         */
  uint16_t font_val;   /**< font-size number: px or % (valid iff `set & fontsize`). */
  uint8_t  display;    /**< 1 = `display:none` (valid iff `set & display`).         */
  uint8_t  pad;        /**< Padding.                                                */
  uint16_t family_off; /**< font-family name slice offset (valid iff `set&family`). */
  uint16_t family_len; /**< font-family name slice length (0 = none).               */
} ra8_css_style_t;

/**
 * @struct ra8_css_anc_t
 * @brief One ancestor part of a descendant selector (e.g. the `.chapter` in
 *        `.chapter p`).
 *
 * @details Same compound shape as a rule's subject (one type + one class + one
 * id), but it constrains some ANCESTOR of the matched element rather than the
 * element itself. `tag == k_ra8_reflow_tag_unknown` / `class_len == 0` /
 * `id_len == 0` mean that part of the constraint is absent.
 */
typedef struct {
  uint8_t  tag;       /**< Type tag, or k_ra8_reflow_tag_unknown = no type. */
  uint8_t  pad8;      /**< Padding.                                         */
  uint16_t class_off; /**< Class name slice offset (class_len 0 = none).    */
  uint16_t class_len; /**< Class name slice length, bytes.                  */
  uint16_t id_off;    /**< Id name slice offset (id_len 0 = none).          */
  uint16_t id_len;    /**< Id name slice length, bytes.                     */
} ra8_css_anc_t;

/**
 * @struct ra8_css_rule_t
 * @brief One `selector { ... }` rule: a (possibly descendant) selector plus its
 *        declarations.
 *
 * @details The SUBJECT is a compound of up to one type + one class + one id
 * constraint (e.g. `p`, `.note`, `#x`, `p.note`, `p#x`); an element matches when
 * every present constraint matches. A constraint is "present" when its field is
 * non-default: `sel_tag != k_ra8_reflow_tag_unknown`, `class_len > 0`,
 * `id_len > 0`. No constraint at all (`*`) matches every element.
 *
 * A DESCENDANT selector (`div p`, `.chapter p`, `.a .b`, `#id p`) adds up to
 * ::k_ra8_css_max_anc ancestor compounds in @ref anc (selector order, outermost
 * first); the rule matches only when the subject matches AND each ancestor part
 * matches some element on the open-element ancestor stack, in order (the CSS
 * descendant combinator -- any depth between parts). Specificity sums every
 * part. Multiple classes per compound (`.a.b`), child/sibling combinators and
 * pseudo selectors are still not parsed (the rule is dropped).
 */
typedef struct {
  uint8_t         sel_tag;                /**< Type tag, or k_ra8_reflow_tag_unknown = no type. */
  uint8_t         anc_count;              /**< Ancestor parts in @ref anc (0 = simple).         */
  uint16_t        class_off;              /**< Class name slice offset (class_len 0 = none).    */
  uint16_t        class_len;              /**< Class name slice length, bytes.                  */
  uint16_t        id_off;                 /**< Id name slice offset (id_len 0 = none).          */
  uint16_t        id_len;                 /**< Id name slice length, bytes.                     */
  uint16_t        order;                  /**< Source order (lower = earlier; ties to later).   */
  ra8_css_anc_t   anc[k_ra8_css_max_anc]; /**< Descendant ancestor parts.                       */
  ra8_css_style_t decl;                   /**< Declared properties.                             */
} ra8_css_rule_t;

/**
 * @struct ra8_css_fontface_t
 * @brief One parsed `@font-face` rule: family + weight/style + `src` href.
 *
 * @details `family`/`src` are slices into the owning ::ra8_css_sheet_t `names`
 * pool (the `src` is the de-`url()`-wrapped manifest href, e.g.
 * `fonts/Body.ttf`). `weight_bold` / `style_italic` capture which face this is so
 * ::ra8_css_match_face can pick the right one for a run's emphasis. The
 * reflow-side loading of that href is #109's remainder, not this module.
 */
typedef struct {
  uint16_t family_off;   /**< `font-family` name slice offset.   */
  uint16_t family_len;   /**< `font-family` name slice length.   */
  uint16_t src_off;      /**< `src` href slice offset.           */
  uint16_t src_len;      /**< `src` href slice length.           */
  uint8_t  weight_bold;  /**< 1 = this face is the bold weight.  */
  uint8_t  style_italic; /**< 1 = this face is italic / oblique. */
  uint16_t pad;          /**< Padding.                           */
} ra8_css_fontface_t;

/**
 * @struct ra8_css_sheet_t
 * @brief A parsed stylesheet: rule + `@font-face` arrays and a name byte pool.
 *
 * @details Embedded in the reflow engine handle. Reset per chapter with
 * ::ra8_css_sheet_reset, then grown by ::ra8_css_parse for each `<style>` block.
 */
typedef struct {
  ra8_css_rule_t     rules[k_ra8_css_max_rules]; /**< Parsed rules.              */
  ra8_css_fontface_t faces[k_ra8_css_max_faces]; /**< Parsed `@font-face` rules. */
  uint16_t           rule_count;                 /**< Rules in use.              */
  uint16_t           face_count;                 /**< `@font-face` rules in use. */
  uint16_t           next_order;                 /**< Running source-order ctr.  */
  uint16_t           names_used;                 /**< Bytes used in `names`.     */
  uint8_t            names[k_ra8_css_name_pool]; /**< class/id/font name bytes.  */
} ra8_css_sheet_t;

/**
 * @struct ra8_css_element_t
 * @brief The identity of one element, used to match selectors.
 *
 * @details `id` / `class_str` point at caller-owned bytes (e.g. slices of the
 * reflow text pool); they need not be NUL-terminated. A NULL pointer or zero
 * length means the attribute is absent.
 */
typedef struct {
  uint8_t     tag;       /**< ::ra8_reflow_html_tag_t of the element. */
  const char* id;        /**< `id` attribute bytes (NULL = none).     */
  uint16_t    id_len;    /**< Length of @ref id, bytes.               */
  const char* class_str; /**< `class` attribute bytes (NULL = none).  */
  uint16_t    class_len; /**< Length of @ref class_str, bytes.        */
} ra8_css_element_t;

/**
 * @brief Reset a stylesheet to empty (no rules, no names, order 0).
 *
 * @param[out] sheet Stylesheet to clear.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Cleared.
 * @retval k_ra8_err_null_ptr @p sheet is NULL.
 *
 * @pre @p sheet is non-NULL.
 * @pre None.
 * @post `sheet->rule_count == 0` and `sheet->names_used == 0`.
 * @post `sheet->next_order == 0`.
 *
 * @note Pure aside from writing @p sheet; not thread-safe relative to it.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_css_sheet_reset(ra8_css_sheet_t* sheet);

/**
 * @brief Parse one `<style>` block's CSS text, appending rules to @p sheet.
 *
 * @details Handles `/<star> comments <star>/`, comma-grouped simple selectors,
 * and the v1 property set. Unsupported selectors (compound / descendant /
 * pseudo) and unknown properties are skipped, not errored. Rules beyond
 * ::k_ra8_css_max_rules (or names beyond the pool) are dropped silently.
 *
 * @param[in,out] sheet Stylesheet to grow.
 * @param[in]     css   CSS source bytes (a `<style>` block body).
 * @param[in]     len   Length of @p css, bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Parsed (possibly with truncation).
 * @retval k_ra8_err_null_ptr @p sheet or @p css is NULL.
 *
 * @pre @p sheet, @p css non-NULL.
 * @pre None.
 * @post New rules (if any room) are appended with ascending `order`.
 * @post `sheet->rule_count` never exceeds ::k_ra8_css_max_rules.
 *
 * @note Pure aside from writing @p sheet.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_css_parse(ra8_css_sheet_t* sheet, const char* css, uint32_t len);

/**
 * @brief Parse an inline `style="..."` declaration body into one style.
 *
 * @details Same property grammar as ::ra8_css_parse but with no selector and no
 * braces -- just `prop: value; prop: value`. The result carries the inline
 * declaration; the caller applies it at the highest specificity.
 *
 * @param[in]  decls Declaration bytes (the `style` attribute value).
 * @param[in]  len   Length of @p decls, bytes.
 * @param[out] out   Receives the parsed declaration (`set == 0` if empty).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Parsed (possibly empty).
 * @retval k_ra8_err_null_ptr @p decls or @p out is NULL.
 *
 * @pre @p decls, @p out non-NULL.
 * @pre None.
 * @post `*out` holds only the properties present in @p decls.
 * @post Unknown properties leave their `set` bit clear.
 *
 * @note Pure aside from writing @p out.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_css_parse_inline(const char* decls, uint32_t len, ra8_css_style_t* out);

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
[[nodiscard]] bool ra8_css_rule_matches(const ra8_css_rule_t*    rule,
                                        const ra8_css_element_t* el,
                                        const ra8_css_sheet_t*   sheet);

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
 * @return The computed ::ra8_css_style_t for @p el.
 *
 * @pre @p sheet, @p el non-NULL (NULL @p sheet/@p el -> returns @p inherited).
 * @pre None.
 * @post Every property bit in the result was set by the winning source.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_css_style_t ra8_css_cascade(const ra8_css_sheet_t*   sheet,
                                              const ra8_css_element_t* el,
                                              ra8_css_style_t          inherited,
                                              ra8_css_style_t          inline_decl);

/**
 * @brief Compute an element's cascaded style with descendant-selector context.
 *
 * @details Identical to ::ra8_css_cascade but also resolves descendant selectors
 * (`.chapter p`, `div p`): a rule with ancestor parts matches only when its
 * subject matches @p el AND each ancestor part matches some element in
 * @p ancestors (the open-element stack, outermost first), in order. ::ra8_css_cascade
 * is the @p n_anc == 0 case (descendant rules never match without ancestor
 * context). Each matched ancestor part also adds to the rule's specificity.
 *
 * @param[in] sheet       Parsed stylesheet (may be empty).
 * @param[in] el          Element identity (the selector subject).
 * @param[in] inherited   Parent's computed style (inheritable props flow in).
 * @param[in] inline_decl Inline `style=""` declaration (`set == 0` if none).
 * @param[in] ancestors   Open-element ancestors, outermost (root) first; may be
 *                        NULL iff @p n_anc is 0.
 * @param[in] n_anc       Number of entries in @p ancestors (0 = no context).
 *
 * @return The computed ::ra8_css_style_t for @p el.
 *
 * @pre @p sheet, @p el non-NULL (NULL -> returns @p inherited).
 * @pre @p ancestors non-NULL when @p n_anc > 0.
 * @post Every property bit in the result was set by the winning source.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_css_style_t ra8_css_cascade_ctx(const ra8_css_sheet_t*   sheet,
                                                  const ra8_css_element_t* el,
                                                  ra8_css_style_t          inherited,
                                                  ra8_css_style_t          inline_decl,
                                                  const ra8_css_element_t* ancestors,
                                                  uint8_t                  n_anc);

/**
 * @brief Pick the `@font-face` whose family + emphasis best fits a run.
 *
 * @details Scans @p sheet's parsed `@font-face` table for entries whose
 * `font-family` equals @p family (case-insensitive, exact). Among those it
 * prefers an exact `(weight, style)` match; failing that it falls back to the
 * family's regular (non-bold, non-italic) face; failing that it reports
 * ::k_ra8_css_no_face so the caller uses its default face. This is the
 * face-selection decision #109's remainder consumes (it then resolves the
 * winning entry's ::ra8_css_face_src href to a loaded typeface).
 *
 * @param[in] sheet       Parsed stylesheet (its `faces` table is scanned).
 * @param[in] family      Resolved `font-family` bytes (e.g. a cascade result's
 *                        name slice); need not be NUL-terminated.
 * @param[in] family_len  Length of @p family, bytes.
 * @param[in] want_bold   Whether the run is bold.
 * @param[in] want_italic Whether the run is italic.
 *
 * @return The matching `faces` index in `[0, face_count)`, or ::k_ra8_css_no_face.
 *
 * @pre @p sheet, @p family non-NULL and @p family_len > 0 (else no match).
 * @pre None.
 * @post No state modified.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] int16_t ra8_css_match_face(const ra8_css_sheet_t* sheet,
                                         const char*            family,
                                         uint16_t               family_len,
                                         bool                   want_bold,
                                         bool                   want_italic);

/**
 * @brief Read a parsed `@font-face` entry's `src` href bytes.
 *
 * @details Returns the de-`url()`-wrapped manifest href slice for face @p idx
 * (the value ::ra8_css_match_face's result indexes), so the caller can resolve it
 * against the EPUB manifest. The bytes live in @p sheet's name pool and are
 * valid for the sheet's lifetime; they are not NUL-terminated.
 *
 * @param[in]  sheet   Parsed stylesheet owning the face table.
 * @param[in]  idx     Face index in `[0, face_count)`.
 * @param[out] out_src Receives a pointer to the href bytes.
 * @param[out] out_len Receives the href length, bytes.
 *
 * @return true iff @p idx is valid and the href was returned.
 * @retval false A NULL pointer, or @p idx out of range.
 *
 * @pre @p sheet, @p out_src, @p out_len non-NULL.
 * @pre None.
 * @post On true, `*out_src`/`*out_len` describe the face's href; else unchanged.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_css_face_src(const ra8_css_sheet_t* sheet,
                                    uint16_t               idx,
                                    const char**           out_src,
                                    uint16_t*              out_len);

#ifdef __cplusplus
}
#endif
