/**
 * @file ra8_reflow_tokenize.c
 * @brief No-heap streaming XHTML tokenizer for ra8_reflow (replaces the
 *        tinyxml2 DOM shim).
 *
 * @details
 * Scans the XHTML byte buffer in a single forward pass and populates the
 * engine's fixed token / text pools (`engine->tokens[]`,
 * `engine->text_pool[]`) directly -- no DOM, no heap. This is the on-target
 * parse path: the firmware traps `_sbrk`, and tinyxml2's `MemPoolT` grows
 * via `new`, so the DOM walk could not run on the device.
 *
 * Output is byte-for-byte equivalent to the former tinyxml2 walk for the
 * v1 conformance subset: a pre-order traversal that emits block-start /
 * block-end tokens for block elements, single tokens for the void
 * elements (br/hr/img), no token for inline-style elements (their style
 * bit is OR'd into descendant text), and pass-through for unknown tags.
 * Text runs are entity-decoded and whitespace-collapsed per text node
 * (HTML rules: runs of ASCII whitespace collapse to one space; a node's
 * leading whitespace is dropped). Inline style (bold/italic/underline)
 * propagates down a bounded explicit stack -- no recursion (NASA P10
 * Rule 1) and bounded loops (Rule 2).
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
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_tokenize_internal.h"

/* ===========================================================================
 * Mutable tokenizer context (markup-engine private; lexical + attribute
 * primitives live in the companion TUs and never touch this struct).
 * ===========================================================================
 */

/**
 * @struct tok_ctx_t
 * @brief Mutable tokenizer state threaded through the scan helpers.
 */
typedef struct {
  ra8_reflow_t* engine;      /**< Target engine pools.                */
  uint8_t       style;       /**< Current inline-style bits.          */
  uint8_t       active_link; /**< 1-based link id (0 = none).         */
  uint32_t      color;       /**< Current CSS colour / inherit.       */
  uint16_t      css_font_px; /**< Inherited CSS font px (0=none).     */
  uint16_t      family_off;  /**< Inherited font-family slice off.    */
  uint16_t      family_len;  /**< Inherited font-family len (0=none). */
  uint8_t       face_slot;   /**< Resolved embedded face (0=default). */
  /** Open-element identities (tag+id+class) for the cascade + descendant match. */
  ra8_css_element_t stack_el[k_priv_max_depth];
  uint8_t           stack_style[k_priv_max_depth];   /**< Style to restore on pop.            */
  uint8_t           stack_link[k_priv_max_depth];    /**< Link id to restore on pop.          */
  uint32_t          stack_color[k_priv_max_depth];   /**< Colour to restore on pop.           */
  uint16_t          stack_font[k_priv_max_depth];    /**< CSS font px to restore on pop.      */
  uint16_t          stack_fam_off[k_priv_max_depth]; /**< font-family off to restore.         */
  uint16_t          stack_fam_len[k_priv_max_depth]; /**< font-family len to restore.         */
  uint8_t           stack_face[k_priv_max_depth];    /**< Face slot to restore on pop.        */
  uint32_t          sp;                              /**< Stack depth.                        */
  uint32_t          suppress_sp;                     /**< display:none subtree depth (0=off). */
  bool              saw_element;                     /**< At least one element seen.          */
} tok_ctx_t;

/* ===========================================================================
 * Internal flow helpers (context-aware)
 * ===========================================================================
 */

/**
 * @brief Test whether the tokenizer is inside a `display:none` subtree.
 *
 * @details Returns true when `ctx->suppress_sp` is nonzero, meaning the scan
 * entered an element whose cascaded style declared `display:none` (#140). All
 * token and text-pool emits are suppressed until the matching close tag pops
 * the stack back above the recorded depth.
 *
 * @param[in] ctx Tokenizer context to query.
 * @return true while inside a `display:none` subtree; false otherwise.
 * @retval true  Emission is suppressed for the current position.
 * @retval false Emission proceeds normally.
 * @pre `ctx` is non-null.
 * @pre `ctx->suppress_sp` is a valid depth value (0 or a previous `sp`).
 * @post No state is modified (pure read of `ctx`).
 * @post `ctx->suppress_sp` is unchanged.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_suppressed(const tok_ctx_t* ctx)
{
  return ctx->suppress_sp != 0U;
}

/* ===========================================================================
 * Markup handlers (advance *pi)
 * ===========================================================================
 */

/**
 * @brief Handle a CDATA section, emitting its inner text verbatim.
 *
 * @details Inner bytes are whitespace-collapsed but NOT entity-decoded
 * (matching XML CDATA semantics). Emits a text token when inside an element
 * and the collapsed run is non-empty.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at "<![CDATA["; advanced past "]]>".
 * @param[in]     len Total buffer length.
 * @return k_ra8_ok, or k_ra8_err_no_mem on pool overflow.
 * @retval k_ra8_ok Section consumed.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi]` begins "<![CDATA[".
 * @post `*pi` advances past the section (or to `len`).
 * @post Token / text pools may grow by one text run.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_handle_cdata(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  const size_t inner = *pi + strlen("<![CDATA[");
  size_t       close = inner;
  while (((close + 3U) <= len) && (memcmp(&buf[close], "]]>", 3U) != 0)) {
    ++close;
  }
  if ((ctx->sp > 0U) && !priv_suppressed(ctx)) {
    uint32_t off     = ctx->engine->text_pool_used;
    bool     last_ws = true;
    for (size_t k = inner; k < close; ++k) {
      if (!ra8_reflow_tok_feed(ctx->engine, (char)buf[k], &last_ws)) {
        return k_ra8_err_no_mem;
      }
    }
    const uint32_t tlen = ctx->engine->text_pool_used - off;
    if ((tlen > 0U) && !ra8_reflow_tok_emit(ctx->engine,
                                            k_ra8_reflow_tok_text,
                                            k_ra8_reflow_tag_unknown,
                                            ctx->style,
                                            off,
                                            tlen)) {
      return k_ra8_err_no_mem;
    }
    if (tlen > 0U) {
      ctx->engine->tokens[ctx->engine->token_count - 1U].reserved16 = (uint16_t)ctx->face_slot;
    }
  }
  *pi = ((close + 3U) <= len) ? (close + 3U) : len;
  return k_ra8_ok;
}

/**
 * @brief Handle a closing tag, popping the stack and emitting block-end.
 *
 * @details Restores the style saved at the matching open and, if that
 * element was a block, emits its block-end token.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at "</"; advanced past '>'.
 * @param[in]     len Total buffer length.
 * @return k_ra8_ok, k_ra8_err_no_mem, or k_ra8_err_validation_failed.
 * @retval k_ra8_err_validation_failed Stray end tag (empty stack).
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi..*pi+1]` are "</".
 * @post `*pi` advances past the tag (or to `len`).
 * @post The element stack shrinks by one on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_handle_end(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  size_t i = *pi + 2U; /* past "</" */
  while ((i < len) && (buf[i] != '>')) {
    ++i;
  }
  *pi = (i < len) ? (i + 1U) : len;
  if (ctx->sp == 0U) {
    return k_ra8_err_validation_failed;
  }
  ctx->sp--;
  const ra8_reflow_html_tag_t tag = (ra8_reflow_html_tag_t)ctx->stack_el[ctx->sp].tag;
  ctx->style                      = ctx->stack_style[ctx->sp];
  ctx->active_link                = ctx->stack_link[ctx->sp];
  ctx->color                      = ctx->stack_color[ctx->sp];
  ctx->css_font_px                = ctx->stack_font[ctx->sp];
  ctx->family_off                 = ctx->stack_fam_off[ctx->sp];
  ctx->family_len                 = ctx->stack_fam_len[ctx->sp];
  ctx->face_slot                  = ctx->stack_face[ctx->sp];
  if (ra8_reflow_tok_is_block(tag) && !priv_suppressed(ctx) &&
      !ra8_reflow_tok_emit(ctx->engine, k_ra8_reflow_tok_block_end, tag, ctx->style, 0U, 0U)) {
    return k_ra8_err_no_mem;
  }
  /* Exited the display:none subtree once we pop back above its depth. */
  if ((ctx->suppress_sp != 0U) && (ctx->sp < ctx->suppress_sp)) {
    ctx->suppress_sp = 0U;
  }
  return k_ra8_ok;
}

/**
 * @brief Parse a start tag's name and self-close flag, advancing past '>'.
 *
 * @details Skips attributes quote-aware so a '>' inside an attribute value
 * does not end the tag.
 *
 * @param[in]     buf       Source buffer.
 * @param[in,out] pi        Cursor at '<'; advanced past '>'.
 * @param[in]     len       Total buffer length.
 * @param[out]    selfclose Set true for a "/>"-terminated tag.
 * @return The classified tag.
 * @retval k_ra8_reflow_tag_unknown Unrecognised element name.
 * @pre `buf`, `pi`, `selfclose` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the tag (or to `len`).
 * @post `*selfclose` reflects the "/>" terminator.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_reflow_html_tag_t
priv_parse_start(const uint8_t* buf, size_t* pi, size_t len, bool* selfclose)
{
  size_t       i      = *pi + 1U; /* past '<' */
  const size_t nstart = i;
  while ((i < len) && (buf[i] != '>') && (buf[i] != '/') &&
         !ra8_reflow_tok_is_xml_whitespace((char)buf[i])) {
    ++i;
  }
  const ra8_reflow_html_tag_t tag = ra8_reflow_tok_classify((const char*)&buf[nstart], i - nstart);
  *selfclose                      = false;
  while ((i < len) && (buf[i] != '>')) {
    const char c = (char)buf[i];
    if ((c == '"') || (c == '\'')) {
      ++i;
      while ((i < len) && (buf[i] != (uint8_t)c)) {
        ++i;
      }
    } else if ((c == '/') && ((i + 1U) < len) && (buf[i + 1U] == '>')) {
      *selfclose = true;
    }
    ++i;
  }
  *pi = (i < len) ? (i + 1U) : len;
  return tag;
}

/**
 * @brief Capture a just-opened tag's `id` (block) or `href` (`<a>`).
 *
 * @details For a block tag, captures `id="..."` and emits the block-start token
 * carrying the id slice (so layout can record an anchor position). For an `<a>`,
 * captures `href="..."`, interns it, and sets the active link id on @p ctx.
 *
 * @param[in,out] ctx  Tokenizer context (active link + engine pools).
 * @param[in]     tag  Raw tag span starting at '<'.
 * @param[in]     span Length of @p tag, bytes.
 * @param[in]     kind Classified tag.
 * @param[in]     block True iff @p kind is a block element.
 * @param[in]     comp Cascaded style (supplies the block alignment).
 * @param[in]     css_font_px Block CSS font px to stamp (0 = UA default).
 * @return k_ra8_ok, or k_ra8_err_no_mem on a token-pool overflow.
 * @retval k_ra8_err_no_mem Block-start token pool full.
 * @pre `ctx` and `tag` are non-null.
 * @pre `ctx->sp > 0` (the tag was already pushed).
 * @post For a block tag a block-start token is emitted (id slice or 0,0).
 * @post For `<a>` `ctx->active_link` reflects the interned href (0 if none).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_open_attrs(tok_ctx_t*             ctx,
                                 const uint8_t*         tag,
                                 size_t                 span,
                                 ra8_reflow_html_tag_t  kind,
                                 bool                   block,
                                 const ra8_css_style_t* comp,
                                 uint16_t               css_font_px)
{
  if (priv_suppressed(ctx)) {
    return k_ra8_ok; /* inside a display:none subtree -> emit nothing */
  }
  if (block) {
    uint32_t id_off = 0U;
    uint32_t id_len = 0U;
    ra8_reflow_tok_capture_attr(ctx->engine, tag, span, "id", sizeof("id") - 1U, &id_off, &id_len);
    if (!ra8_reflow_tok_emit(ctx->engine,
                             k_ra8_reflow_tok_block_start,
                             kind,
                             ctx->style,
                             id_off,
                             id_len)) {
      return k_ra8_err_no_mem;
    }
    /* Stash the block's cascaded alignment + CSS font px on the block-start token
     * (left + 0 = unstyled defaults, so unstyled blocks lay out byte-identically). */
    const uint8_t       align = ((comp->set & (uint8_t)k_ra8_css_set_align) != 0U)
                                  ? comp->align
                                  : (uint8_t)k_ra8_reflow_align_left;
    ra8_reflow_token_t* bst   = &ctx->engine->tokens[ctx->engine->token_count - 1U];
    bst->reserved             = align;
    bst->css_font_px          = css_font_px;
    return k_ra8_ok;
  }
  if (kind == k_ra8_reflow_tag_a) {
    uint32_t href_off = 0U;
    uint32_t href_len = 0U;
    ra8_reflow_tok_capture_attr(ctx->engine,
                                tag,
                                span,
                                "href",
                                sizeof("href") - 1U,
                                &href_off,
                                &href_len);
    ctx->active_link = ra8_reflow_tok_intern_link(ctx->engine, href_off, href_len);
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve a `<link rel="stylesheet" href>` via the bound CSS loader.
 *
 * @details Best-effort. When a CSS loader is bound and the tag is a stylesheet
 * link carrying an href, fetches the bytes and parses them into the chapter
 * sheet at this document position (so a later inline `<style>` / `style=`
 * overrides them). Any missing attribute / loader failure is silently ignored
 * -- the chapter renders with whatever rules are present.
 *
 * @param[in,out] ctx    Tokenizer context (carries the engine + its CSS sheet).
 * @param[in]     buf    Source buffer.
 * @param[in]     tag_lt Index of the tag's '<'.
 * @param[in]     end    One past the tag's '>'.
 * @return Nothing.
 * @pre `ctx`, `buf` non-null; `end >= tag_lt`.
 * @pre `ctx->engine` is non-null and its CSS sheet is initialised.
 * @post On a resolved stylesheet, its rules are appended to `ctx->engine->css`.
 * @post When no loader is bound or attributes are absent, the sheet is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_handle_link(tok_ctx_t* ctx, const uint8_t* buf, size_t tag_lt, size_t end)
{
  if (ctx->engine->css_loader == nullptr) {
    return;
  }
  const uint8_t* tagbuf   = &buf[tag_lt];
  const size_t   span     = (end > tag_lt) ? (end - tag_lt) : 0U;
  size_t         rel_off  = 0U;
  size_t         rel_len  = 0U;
  size_t         href_off = 0U;
  size_t         href_len = 0U;
  if (!ra8_reflow_tok_find_attr(tagbuf, span, "rel", sizeof("rel") - 1U, &rel_off, &rel_len) ||
      !ra8_reflow_tok_find_attr(tagbuf, span, "href", sizeof("href") - 1U, &href_off, &href_len) ||
      !ra8_reflow_tok_rel_is_stylesheet(&tagbuf[rel_off], rel_len)) {
    return;
  }
  const uint8_t* css_bytes = nullptr;
  size_t         css_len   = 0U;
  if ((ctx->engine->css_loader(ctx->engine->css_loader_ctx,
                               (const char*)&tagbuf[href_off],
                               (uint32_t)href_len,
                               &css_bytes,
                               &css_len) == k_ra8_ok) &&
      (css_bytes != nullptr) && (css_len > 0U)) {
    (void)ra8_css_parse(&ctx->engine->css, (const char*)css_bytes, (uint32_t)css_len);
  }
}

/**
 * @brief Emit the single token for a void tag (`<br>` / `<hr>` / `<img>`),
 *        or resolve a `<link>` stylesheet.
 *
 * @details `<br>` -> break, `<hr>` -> rule, `<img>` -> image (capturing its
 * `src`), `<link rel=stylesheet>` -> external CSS load (no token). Returns
 * whether @p tag was a void tag so the caller can fall through to ordinary
 * open-tag handling otherwise.
 *
 * @param[in,out] ctx     Tokenizer context.
 * @param[in]     buf     Source buffer.
 * @param[in]     tag_lt  Index of the tag's '<'.
 * @param[in]     end     One past the tag's '>'.
 * @param[in]     tag     Classified tag.
 * @param[out]    out_err Receives the emit result when handled.
 * @return true iff @p tag was a void tag (then @p *out_err is set).
 * @retval true  Void tag handled; check @p *out_err.
 * @retval false Not a void tag; caller continues.
 * @pre `ctx`, `buf`, `out_err` are non-null.
 * @pre `end >= tag_lt`.
 * @post On true a single token was emitted (or the pool overflowed).
 * @post On false neither the token pool nor `*out_err` are modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_handle_void(tok_ctx_t*            ctx,
                             const uint8_t*        buf,
                             size_t                tag_lt,
                             size_t                end,
                             ra8_reflow_html_tag_t tag,
                             ra8_err_t*            out_err)
{
  if (tag == k_ra8_reflow_tag_link) {
    priv_handle_link(ctx, buf, tag_lt, end); /* external stylesheet (no token) */
    *out_err = k_ra8_ok;
    return true;
  }
  const bool is_void =
    (tag == k_ra8_reflow_tag_br) || (tag == k_ra8_reflow_tag_hr) || (tag == k_ra8_reflow_tag_img);
  if (is_void && priv_suppressed(ctx)) {
    *out_err = k_ra8_ok; /* void tag inside a display:none subtree -> drop */
    return true;
  }
  if (tag == k_ra8_reflow_tag_br) {
    *out_err = ra8_reflow_tok_emit(ctx->engine, k_ra8_reflow_tok_break, tag, ctx->style, 0U, 0U)
                 ? k_ra8_ok
                 : k_ra8_err_no_mem;
    return true;
  }
  if (tag == k_ra8_reflow_tag_hr) {
    *out_err = ra8_reflow_tok_emit(ctx->engine, k_ra8_reflow_tok_rule, tag, ctx->style, 0U, 0U)
                 ? k_ra8_ok
                 : k_ra8_err_no_mem;
    return true;
  }
  if (tag == k_ra8_reflow_tag_img) {
    uint32_t     src_off = 0U;
    uint32_t     src_len = 0U;
    const size_t span    = (end > tag_lt) ? (end - tag_lt) : 0U;
    ra8_reflow_tok_capture_attr(ctx->engine,
                                &buf[tag_lt],
                                span,
                                "src",
                                sizeof("src") - 1U,
                                &src_off,
                                &src_len);
    *out_err =
      ra8_reflow_tok_emit(ctx->engine, k_ra8_reflow_tok_image, tag, ctx->style, src_off, src_len)
        ? k_ra8_ok
        : k_ra8_err_no_mem;
    return true;
  }
  return false;
}

/**
 * @brief Resolve an element's effective CSS font px from the cascade + inherited.
 *
 * @details Returns the inherited size (0 = none -> UA default) when no font-size
 * is declared, else resolves the declared value: an absolute `px`, or a `%`/`em`
 * of the parent size (the inherited px, or the body size when no ancestor set
 * one). The result clamps to [k_ra8_reflow_min_font_px, k_ra8_reflow_max_font_px].
 *
 * @param[in] ctx  Tokenizer context (carries the inherited font px + body size).
 * @param[in] comp The element's cascaded style.
 * @return The element's effective CSS font px, or 0 for "use the UA default".
 * @retval 0 No font-size declared for this element and no ancestor has set one.
 * @pre `ctx` and `comp` are non-null.
 * @pre `ctx->engine->font_px` is the body font size.
 * @post No state mutated.
 * @post Return value is clamped to [k_ra8_reflow_min_font_px, k_ra8_reflow_max_font_px] when non-zero.
 * @note Pure aside from reading @p ctx.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t priv_css_font_px(const tok_ctx_t* ctx, const ra8_css_style_t* comp)
{
  if ((comp->set & (uint8_t)k_ra8_css_set_fontsize) == 0U) {
    return ctx->css_font_px; /* inherit (0 = none -> UA default) */
  }
  const uint16_t parent = (ctx->css_font_px != 0U) ? ctx->css_font_px : ctx->engine->font_px;
  uint32_t       px =
    ((ra8_css_font_unit_t)comp->font_unit == k_ra8_css_font_pct)
      ? (((uint32_t)parent * (uint32_t)comp->font_val) / (uint32_t)k_ra8_reflow_pct_full)
      : (uint32_t)comp->font_val;
  if (px < (uint32_t)k_ra8_reflow_min_font_px) {
    px = (uint32_t)k_ra8_reflow_min_font_px;
  }
  if (px > (uint32_t)k_ra8_reflow_max_font_px) {
    px = (uint32_t)k_ra8_reflow_max_font_px;
  }
  return (uint16_t)px;
}

/**
 * @brief Run the CSS cascade for a just-pushed element and apply the result.
 *
 * @details Builds the element's CSS identity and inherited run style, runs the
 * author `<style>` rules + inline `style` attribute through `ra8_css_cascade_ctx()`
 * (#111 / #140), emits the block-start via `priv_open_attrs()` carrying the
 * cascaded alignment + font size, and updates `ctx->style`, `ctx->color`,
 * `ctx->css_font_px`, `ctx->family_off`/`len`, and `ctx->face_slot` for
 * descendant content. Unstyled content lays out byte-identically to the
 * pre-CSS engine. Factored out of `priv_handle_start()` to stay within the
 * NASA Rule 4 function-length budget.
 *
 * @param[in,out] ctx    Tokenizer context (element already pushed onto stack).
 * @param[in]     tagbuf Raw tag span starting at '<'.
 * @param[in]     span   Length of @p tagbuf, bytes.
 * @param[in]     tag    Classified tag.
 * @param[in]     block  True iff @p tag is a block element.
 * @return k_ra8_ok, or k_ra8_err_no_mem on a block-start token-pool overflow.
 * @retval k_ra8_ok        Cascade applied; context updated.
 * @retval k_ra8_err_no_mem Token pool full when emitting the block-start token.
 * @pre `ctx` and `tagbuf` are non-null.
 * @pre `ctx->sp > 0` (the element has already been pushed by the caller).
 * @post `ctx->style` / `ctx->color` / `ctx->css_font_px` reflect the cascaded style.
 * @post A block element emitted its block-start token with cascaded alignment.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_open_styled(tok_ctx_t*            ctx,
                                  const uint8_t*        tagbuf,
                                  size_t                span,
                                  ra8_reflow_html_tag_t tag,
                                  bool                  block)
{
  const uint8_t           base = (uint8_t)(ctx->style | ra8_reflow_tok_style_for(tag));
  const ra8_css_element_t el   = ctx->stack_el[ctx->sp - 1U]; /* pushed by the caller */
  ra8_css_style_t         inh  = {.set = (uint8_t)k_priv_style_mask, .style = base};
  if (ctx->color != (uint32_t)k_ra8_reflow_color_inherit) {
    inh.set   = (uint8_t)(inh.set | (uint8_t)k_ra8_css_set_color);
    inh.color = ctx->color;
  }
  /* font-family inherits (#109): seed the cascade with the parent's resolved
   * family so a child without its own `font-family` keeps the ancestor's face. */
  if (ctx->family_len != 0U) {
    inh.set        = (uint8_t)(inh.set | (uint8_t)k_ra8_css_set_family);
    inh.family_off = ctx->family_off;
    inh.family_len = ctx->family_len;
  }
  const ra8_css_style_t inl = ra8_reflow_tok_css_inline(tagbuf, span);
  /* Ancestors = the open-element stack below this one (stack_el[0 .. sp-2]). */
  const uint8_t         n_anc = (uint8_t)(ctx->sp - 1U);
  const ra8_css_style_t comp =
    ra8_css_cascade_ctx(&ctx->engine->css, &el, inh, inl, ctx->stack_el, n_anc);
  const uint16_t fpx = priv_css_font_px(ctx, &comp);
  /* display:none (#140): begin suppressing this element + its subtree at the
   * current depth; priv_open_attrs and the emit sites then drop every token
   * until the matching close pops back above this depth. */
  const bool hidden = ((comp.set & (uint8_t)k_ra8_css_set_display) != 0U) && (comp.display != 0U);
  if (hidden && (ctx->suppress_sp == 0U)) {
    ctx->suppress_sp = ctx->sp;
  }
  const ra8_err_t aerr = priv_open_attrs(ctx, tagbuf, span, tag, block, &comp, fpx);
  if (aerr != k_ra8_ok) {
    return aerr;
  }
  ctx->style       = (uint8_t)(comp.style & (uint8_t)k_priv_style_mask);
  ctx->color       = ((comp.set & (uint8_t)k_ra8_css_set_color) != 0U)
                       ? comp.color
                       : (uint32_t)k_ra8_reflow_color_inherit;
  ctx->css_font_px = fpx;
  /* Resolve the embedded face for this element from its cascaded family +
   * emphasis (#109). Children inherit the family slice; text runs stamp the
   * resolved slot. Both are 0 / unchanged for content without `@font-face`. */
  if ((comp.set & (uint8_t)k_ra8_css_set_family) != 0U) {
    ctx->family_off = comp.family_off;
    ctx->family_len = comp.family_len;
  }
  ctx->face_slot = ra8_reflow_tok_resolve_face_slot(ctx->engine, &comp);
  return k_ra8_ok;
}

/**
 * @brief Handle an opening, void, or self-closing start tag.
 *
 * @details Parses the tag name and self-close flag, then dispatches: void tags
 * (br / hr / img) emit a single token and return; self-closing block tags emit
 * an empty block-start / block-end pair; ordinary tags push the element onto
 * the style stack (after bounds-checking depth) and call `priv_open_styled()`
 * to cascade + apply CSS and emit any block-start token.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at '<'; advanced past '>'.
 * @param[in]     len Total buffer length.
 * @return k_ra8_ok, k_ra8_err_no_mem, or k_ra8_err_validation_failed.
 * @retval k_ra8_ok        Tag processed successfully.
 * @retval k_ra8_err_no_mem Token pool or nesting-depth bound exceeded.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi] == '<'` and the tag is not an end-tag, comment, or declaration.
 * @post `*pi` advances past the tag (or to `len` on truncation).
 * @post The element stack and token pool grow as required by the tag kind.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_handle_start(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  const size_t                tag_lt    = *pi; /* index of '<' before parse */
  bool                        selfclose = false;
  const ra8_reflow_html_tag_t tag       = priv_parse_start(buf, pi, len, &selfclose);
  ctx->saw_element                      = true;

  ra8_err_t verr = k_ra8_ok;
  if (priv_handle_void(ctx, buf, tag_lt, *pi, tag, &verr)) {
    return verr;
  }

  const bool block = ra8_reflow_tok_is_block(tag);
  if (selfclose) {
    if (block && !priv_suppressed(ctx)) {
      if (!ra8_reflow_tok_emit(ctx->engine,
                               k_ra8_reflow_tok_block_start,
                               tag,
                               ctx->style,
                               0U,
                               0U)) {
        return k_ra8_err_no_mem;
      }
      if (!ra8_reflow_tok_emit(ctx->engine, k_ra8_reflow_tok_block_end, tag, ctx->style, 0U, 0U)) {
        return k_ra8_err_no_mem;
      }
    }
    return k_ra8_ok;
  }

  if (ctx->sp >= (uint32_t)k_priv_max_depth) {
    return k_ra8_err_no_mem;
  }
  const size_t span = (*pi > tag_lt) ? (*pi - tag_lt) : 0U;
  /* Record the element's full identity (tag + id + class, aliasing the chapter
   * buffer) so the cascade reads it back and descendant selectors can match it
   * as an ancestor of a later element. */
  ctx->stack_el[ctx->sp]      = ra8_reflow_tok_css_element(tag, &buf[tag_lt], span);
  ctx->stack_style[ctx->sp]   = ctx->style;
  ctx->stack_link[ctx->sp]    = ctx->active_link;
  ctx->stack_color[ctx->sp]   = ctx->color;
  ctx->stack_font[ctx->sp]    = ctx->css_font_px;
  ctx->stack_fam_off[ctx->sp] = ctx->family_off;
  ctx->stack_fam_len[ctx->sp] = ctx->family_len;
  ctx->stack_face[ctx->sp]    = ctx->face_slot;
  ctx->sp++;
  return priv_open_styled(ctx, &buf[tag_lt], span, tag, block);
}

/**
 * @brief True iff `<name` starts at `buf[i]` and is followed by a tag delimiter.
 *
 * @details Distinguishes `<style ...>` / `<script>` from look-alikes such as a
 * hypothetical `<styled>` by requiring `>`, `/`, whitespace, or end-of-buffer
 * after the name.
 *
 * @param[in] buf  Source buffer.
 * @param[in] i    Offset of the leading '<'.
 * @param[in] len  Total buffer length.
 * @param[in] name Literal element open including '<' (e.g. "<style").
 * @return true iff the element name matches with a following delimiter.
 * @retval false No match, or a longer name continues past @p name.
 * @pre `buf` and `name` are non-null.
 * @pre `i <= len` (caller guarantees the cursor is within bounds).
 * @post No state is modified (pure).
 * @post Return value depends solely on the inputs.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_tag_is(const uint8_t* buf, size_t i, size_t len, const char* name)
{
  if (!ra8_reflow_tok_starts_with(buf, i, len, name)) {
    return false;
  }
  const size_t n = strlen(name);
  if ((i + n) >= len) {
    return true;
  }
  const char c = (char)buf[i + n];
  return (c == '>') || (c == '/') || ra8_reflow_tok_is_xml_whitespace(c);
}

/**
 * @brief Consume a raw-text element (`<style>` / `<script>`) without emitting.
 *
 * @details Per HTML raw-text rules the content runs verbatim (no nested
 * elements) to the matching lowercase close tag. `<style>` content is parsed
 * into the engine stylesheet (#111); `<script>` content is discarded. A missing
 * close tag consumes to end-of-buffer. Lowercase close literals match valid
 * XHTML (XML is case-sensitive and requires lowercase element names).
 *
 * @param[in,out] ctx       Tokenizer context (engine stylesheet).
 * @param[in]     buf       Source buffer.
 * @param[in,out] pi        Cursor at '<'; advanced past the close tag.
 * @param[in]     len       Total buffer length.
 * @param[in]     close_lit Lowercase close tag literal (e.g. "</style>").
 * @param[in]     is_style  True to parse the content as CSS, false to discard.
 * @pre `ctx`, `buf`, `pi`, `close_lit` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the element (or to `len`).
 * @post For `<style>` any parsed rules are appended to `ctx->engine->css`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_handle_raw_text(tok_ctx_t*     ctx,
                                 const uint8_t* buf,
                                 size_t*        pi,
                                 size_t         len,
                                 const char*    close_lit,
                                 bool           is_style)
{
  ctx->saw_element      = true;
  const size_t open_end = ra8_reflow_tok_skip_past(buf, *pi, len, ">");
  const size_t close_at = ra8_reflow_tok_find_lit(buf, open_end, len, close_lit);
  if (is_style && (close_at > open_end)) {
    (void)ra8_css_parse(&ctx->engine->css,
                        (const char*)&buf[open_end],
                        (uint32_t)(close_at - open_end));
  }
  *pi = (close_at < len) ? (close_at + strlen(close_lit)) : len;
}

/**
 * @brief Dispatch the markup construct beginning at `buf[*pi] == '<'`.
 *
 * @details Routes to the comment / CDATA / processing-instruction /
 * declaration skip, or to the end-tag / start-tag handlers.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at '<'; advanced past the construct.
 * @param[in]     len Total buffer length.
 * @return Result of the selected handler (k_ra8_ok on a skip).
 * @retval k_ra8_ok Comment / PI / declaration consumed.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the construct.
 * @post Token / text pools may grow per the selected handler.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_handle_lt(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  if (ra8_reflow_tok_starts_with(buf, *pi, len, "<!--")) {
    *pi = ra8_reflow_tok_skip_past(buf, *pi + 4U, len, "-->");
    return k_ra8_ok;
  }
  if (ra8_reflow_tok_starts_with(buf, *pi, len, "<![CDATA[")) {
    return priv_handle_cdata(ctx, buf, pi, len);
  }
  if (ra8_reflow_tok_starts_with(buf, *pi, len, "<?")) {
    *pi = ra8_reflow_tok_skip_past(buf, *pi + 2U, len, "?>");
    return k_ra8_ok;
  }
  if (ra8_reflow_tok_starts_with(buf, *pi, len, "<!")) {
    *pi = ra8_reflow_tok_skip_past(buf, *pi + 2U, len, ">");
    return k_ra8_ok;
  }
  if (priv_tag_is(buf, *pi, len, "<style")) {
    priv_handle_raw_text(ctx, buf, pi, len, "</style>", true);
    return k_ra8_ok;
  }
  if (priv_tag_is(buf, *pi, len, "<script")) {
    priv_handle_raw_text(ctx, buf, pi, len, "</script>", false);
    return k_ra8_ok;
  }
  if (((*pi + 1U) < len) && (buf[*pi + 1U] == '/')) {
    return priv_handle_end(ctx, buf, pi, len);
  }
  return priv_handle_start(ctx, buf, pi, len);
}

/**
 * @brief Stash + emit a character-data run, tagging it with the active link id.
 *
 * @details Text outside any open element is dropped. Otherwise the run is
 * entity-decoded + whitespace-collapsed into the text pool and emitted as a text
 * token whose `reserved` byte carries the active `<a>` link id (0 = none).
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in]     run Run start offset (inclusive).
 * @param[in]     end Run end offset (exclusive).
 * @return true on success, false on a pool overflow.
 * @retval false Text or token pool full.
 * @pre `ctx` and `buf` are non-null; `run <= end`.
 * @pre `ctx->engine->token_count` is consistent.
 * @post On a non-empty stored run a text token is appended + link-tagged.
 * @post On `ctx->sp == 0` or inside `display:none` the pools are unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_emit_text_run(tok_ctx_t* ctx, const uint8_t* buf, size_t run, size_t end)
{
  if ((ctx->sp == 0U) || priv_suppressed(ctx)) {
    return true; /* outside any element, or inside display:none -> drop */
  }
  uint32_t off = 0U;
  uint32_t tln = 0U;
  if (!ra8_reflow_tok_stash_run(ctx->engine, buf, run, end, &off, &tln)) {
    return false;
  }
  if (tln == 0U) {
    return true;
  }
  if (!ra8_reflow_tok_emit(ctx->engine,
                           k_ra8_reflow_tok_text,
                           k_ra8_reflow_tag_unknown,
                           ctx->style,
                           off,
                           tln)) {
    return false;
  }
  ctx->engine->tokens[ctx->engine->token_count - 1U].reserved   = ctx->active_link;
  ctx->engine->tokens[ctx->engine->token_count - 1U].color      = ctx->color;
  ctx->engine->tokens[ctx->engine->token_count - 1U].reserved16 = (uint16_t)ctx->face_slot;
  return true;
}

/* ===========================================================================
 * Entry point (called by ra8_reflow_parse.c)
 * ===========================================================================
 */

ra8_err_t priv_reflow_xml_walk(ra8_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if ((engine == nullptr) || (xhtml_buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  (void)ra8_css_sheet_reset(&engine->css); /* fresh CSS rules per chapter (#111) */

  /* The tokenizer scratch (per-open-element cascade stacks) is ~2 kB, which
   * would push this frame past the project stack-usage budget. Hold it in
   * module-static storage instead of on the stack and reset it on entry.
   * Safe: priv_reflow_xml_walk has a single, non-recursive, single-threaded
   * caller (priv_reflow_parse), so the shared instance never overlaps. */
  static tok_ctx_t s_walk_ctx;
  s_walk_ctx     = (tok_ctx_t){};
  tok_ctx_t* ctx = &s_walk_ctx;
  ctx->engine    = engine;
  ctx->style     = (uint8_t)k_ra8_reflow_style_normal;
  ctx->color     = (uint32_t)k_ra8_reflow_color_inherit;

  size_t i = 0U;
  while (i < xhtml_len) {
    if (xhtml_buf[i] == '<') {
      const ra8_err_t err = priv_handle_lt(ctx, xhtml_buf, &i, xhtml_len);
      if (err != k_ra8_ok) {
        return err;
      }
      continue;
    }
    const size_t run = i;
    while ((i < xhtml_len) && (xhtml_buf[i] != '<')) {
      ++i;
    }
    if (!priv_emit_text_run(ctx, xhtml_buf, run, i)) {
      return k_ra8_err_no_mem;
    }
  }

  if (!ctx->saw_element) {
    return k_ra8_err_validation_failed;
  }
  if (ctx->sp != 0U) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}
