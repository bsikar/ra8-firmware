/**
 * @file ra8_xml_writer.h
 * @brief Bounded XML emitter: the write half the tree has never owned.
 * @ingroup grp_fmt
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * The tree can read XML and cannot describe how to write it. `xml_reader_*`
 * parses, `xml_decode` unescapes, `xml_attr_next` walks attributes, and every
 * producer of a document builds it out of `snprintf` fragments plus its own
 * escaper. A downloader application consequently owns the only XML escaper in
 * the tree, and the well-formedness of every document the product emits rests
 * on format strings that no parser ever checks.
 *
 * This is the other half of that seam: a builder that owns escaping, owns tag
 * balance, and refuses rather than truncates. The caller supplies both the
 * output buffer and the element stack, so the writer allocates nothing and its
 * worst-case footprint is visible at the call site.
 *
 * Three properties are what make it worth using instead of `snprintf`:
 *
 * 1. Text and attribute values are escaped by the writer, so an untrusted
 *    title or filename cannot close a tag it was interpolated into.
 * 2. Element names are validated and the stack is checked, so an unbalanced
 *    or misspelled document is refused at the call that makes it wrong rather
 *    than discovered by whoever parses the file later.
 * 3. Failure is sticky. The first refusal is recorded and every later call is
 *    a no-op, so a caller writes the whole document and tests once at
 *    ::ra8_xml_writer_finish instead of checking every fragment.
 *
 * @code
 * char                    buf[512];
 * ra8_xml_writer_frame_t  frames[8];
 * ra8_xml_writer_t        w;
 *
 * (void)ra8_xml_writer_init(&w, buf, sizeof(buf), frames, 8U);
 * (void)ra8_xml_writer_declaration(&w);
 * (void)ra8_xml_writer_start_element(&w, "ComicInfo");
 * (void)ra8_xml_writer_attr(&w, "xmlns:xsi", k_xsi_ns);
 * (void)ra8_xml_writer_start_element(&w, "Title");
 * (void)ra8_xml_writer_text(&w, untrusted_title);
 * (void)ra8_xml_writer_end_element(&w);
 * (void)ra8_xml_writer_end_element(&w);
 *
 * size_t len = 0U;
 * if (ra8_xml_writer_finish(&w, &len) != k_ra8_ok) {
 *   return k_ra8_err_invalid_state;  // nothing partial is ever published
 * }
 * @endcode
 *
 * @note The writer emits no whitespace of its own. A document meant to be
 *       read by a person gets its indentation from the caller through
 *       ::ra8_xml_writer_text, which is deliberate: the writer never inserts
 *       bytes into element content it was not given.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_xml_writer_limits_t
 * @brief Fixed sizes a caller of the writer sizes its own storage against.
 *
 * @details
 * ::k_ra8_xml_name_cap bounds one element name, which is copied into the
 * caller's frame array so the writer can close a tag without holding a
 * pointer into storage the caller may have reused. The XML Name production
 * itself is unbounded; every name the tree emits is a fixed literal well
 * inside this, and a longer one is refused rather than truncated.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_xml_name_cap      = 64U, /**< Element name bytes, NUL included.  */
  k_ra8_xml_entity_cap    = 6U,  /**< Longest entity: `&quot;` plus NUL. */
  k_ra8_xml_min_frame_cap = 1U,  /**< Shallowest usable element stack.   */
} ra8_xml_writer_limits_t;

/**
 * @struct ra8_xml_writer_frame_t
 * @brief One open element, stored in the caller's stack array.
 *
 * @details
 * The writer copies the name rather than borrowing the caller's pointer, so a
 * document can be built from names assembled in a scratch buffer that is
 * overwritten between calls.
 *
 * @since 0.1.0
 */
typedef struct {
  char name[k_ra8_xml_name_cap]; /**< Element name, NUL-terminated. */
} ra8_xml_writer_frame_t;

/**
 * @struct ra8_xml_writer_t
 * @brief Builder state over a caller-owned buffer and element stack.
 *
 * @details
 * Treat every field as private to the implementation. The struct is published
 * so it can live on a caller's stack or inside a workspace; nothing outside
 * the writer reads or writes it.
 *
 * @invariant `len <= cap` at every point a public entry point returns.
 * @invariant `depth <= frame_cap`.
 * @invariant Once `status` is not ::k_ra8_ok it never changes again.
 *
 * @since 0.1.0
 */
typedef struct {
  char*                   out;       /**< Caller's document buffer.      */
  ra8_xml_writer_frame_t* frames;    /**< Caller's element stack.        */
  size_t                  cap;       /**< Bytes available in `out`.      */
  size_t                  len;       /**< Bytes written so far.          */
  uint32_t                frame_cap; /**< Entries available in `frames`. */
  uint32_t                depth;     /**< Elements currently open.       */
  ra8_err_t               status;    /**< First refusal, sticky.         */
  bool                    tag_open;  /**< Start tag still taking attrs.  */
  bool                    ready;     /**< init() ran and accepted.       */
} ra8_xml_writer_t;

/**
 * @brief Escape the five XML predefined entities into a bounded buffer.
 *
 * @details
 * Replaces `&`, `<`, `>`, `"` and `'` with their entity forms and copies
 * everything else verbatim. This is the escaper the writer itself uses; it is
 * public because a caller assembling a fragment outside the builder needs the
 * same policy rather than a second copy of it.
 *
 * Escaping all five everywhere, rather than the three that element content
 * strictly requires, keeps one policy for text and attribute values both. The
 * cost is a longer document; the benefit is that a string cannot become
 * unsafe by being moved from one position to the other.
 *
 * @param[in]  src Source string (NUL-terminated), or NULL.
 * @param[out] out Destination for the escaped, NUL-terminated result.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether the fully escaped string fit.
 * @retval k_ra8_ok             `out` holds the complete escaped form.
 * @retval k_ra8_err_invalid_arg `src` or `out` was NULL, or `cap` was 0.
 * @retval k_ra8_err_no_mem     The escaped result did not fit in `cap`.
 *
 * @post On any refusal with `cap > 0`, `out[0]` is `'\0'`: no partial and
 *       possibly malformed fragment is ever left for a caller to use.
 * @post `src` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_escape(const char* src, char* out, size_t cap);

/**
 * @brief Bind a writer to a caller-owned buffer and element stack.
 *
 * @details
 * Clears the buffer to an empty document and arms the builder. Calling init
 * again on a used writer restarts it, which is how a caller emits a second
 * document into the same storage without carrying the first one's failure.
 *
 * @param[out] w         Writer to initialise.
 * @param[out] out       Document buffer.
 * @param[in]  cap       Capacity of `out` in bytes; must be at least 1.
 * @param[out] frames    Element stack storage.
 * @param[in]  frame_cap Entries in `frames`; the deepest nesting allowed.
 *
 * @return Whether the writer is usable.
 * @retval k_ra8_ok              Ready; `out` holds an empty document.
 * @retval k_ra8_err_invalid_arg A pointer was NULL or a capacity was 0.
 *
 * @post On success `out[0]` is `'\0'` and no element is open.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_init(ra8_xml_writer_t*       w,
                                            char*                   out,
                                            size_t                  cap,
                                            ra8_xml_writer_frame_t* frames,
                                            uint32_t                frame_cap);

/**
 * @brief Emit the XML declaration.
 *
 * @details
 * Writes `<?xml version="1.0" encoding="UTF-8"?>`, the single declaration
 * every document in the tree carries. It is a distinct call rather than part
 * of init so that a fragment, which must not carry one, is the default.
 *
 * @param[in,out] w Writer.
 *
 * @return Whether the declaration was written.
 * @retval k_ra8_ok               Written.
 * @retval k_ra8_err_invalid_arg  `w` was NULL.
 * @retval k_ra8_err_invalid_state Writer unarmed, already failed, or an
 *                                element is already open.
 * @retval k_ra8_err_no_mem       The declaration did not fit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_declaration(ra8_xml_writer_t* w);

/**
 * @brief Open an element, leaving its start tag able to take attributes.
 *
 * @details
 * The name is validated against the XML Name production (a letter, `_` or `:`
 * first, then letters, digits, `.`, `-`, `_` or `:`) and copied into the
 * caller's frame array. A previously open start tag is closed with `>` first,
 * so nesting needs no call from the caller to mark the transition.
 *
 * @param[in,out] w    Writer.
 * @param[in]     name Element name, NUL-terminated.
 *
 * @return Whether the element was opened.
 * @retval k_ra8_ok                Open; attributes may follow.
 * @retval k_ra8_err_invalid_arg   `w` or `name` was NULL, or the name is not
 *                                 a legal XML Name.
 * @retval k_ra8_err_invalid_state Writer unarmed or already failed.
 * @retval k_ra8_err_no_mem        Stack full, name too long, or no room.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_start_element(ra8_xml_writer_t* w,
                                                     const char*       name);

/**
 * @brief Add an escaped attribute to the element whose start tag is open.
 *
 * @details
 * Legal only between ::ra8_xml_writer_start_element and the first text or
 * child element. That ordering is a property of XML rather than of this
 * writer, and refusing it here is what turns a malformed document into a
 * refused call.
 *
 * @param[in,out] w     Writer.
 * @param[in]     name  Attribute name, NUL-terminated.
 * @param[in]     value Attribute value; escaped before it is written.
 *
 * @return Whether the attribute was written.
 * @retval k_ra8_ok                Written.
 * @retval k_ra8_err_invalid_arg   A pointer was NULL or `name` is not legal.
 * @retval k_ra8_err_invalid_state No start tag is open.
 * @retval k_ra8_err_no_mem        The escaped attribute did not fit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xml_writer_attr(ra8_xml_writer_t* w, const char* name, const char* value);

/**
 * @brief Append escaped character data to the open element.
 *
 * @details
 * Closes a pending start tag with `>` first. The text is escaped, so a value
 * carrying `<` or `&` becomes content rather than markup.
 *
 * @param[in,out] w    Writer.
 * @param[in]     text Character data, NUL-terminated.
 *
 * @return Whether the text was written.
 * @retval k_ra8_ok                Written.
 * @retval k_ra8_err_invalid_arg   `w` or `text` was NULL.
 * @retval k_ra8_err_invalid_state No element is open, or the writer failed.
 * @retval k_ra8_err_no_mem        The escaped text did not fit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_text(ra8_xml_writer_t* w,
                                            const char*       text);

/**
 * @brief Close the innermost open element.
 *
 * @details
 * An element that took no text and no children is emitted in the empty form
 * `<name/>` rather than `<name></name>`, which is what the documents in the
 * tree already write by hand for spine items and manifest entries.
 *
 * @param[in,out] w Writer.
 *
 * @return Whether the element was closed.
 * @retval k_ra8_ok                Closed.
 * @retval k_ra8_err_invalid_arg   `w` was NULL.
 * @retval k_ra8_err_invalid_state No element is open, or the writer failed.
 * @retval k_ra8_err_no_mem        The end tag did not fit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_end_element(ra8_xml_writer_t* w);

/**
 * @brief Seal the document and report its length.
 *
 * @details
 * Succeeds only when every element opened was closed and no earlier call was
 * refused. This is the one test a caller has to make: everything before it
 * may be discarded with `(void)`, because the first refusal is what `finish`
 * reports.
 *
 * @param[in,out] w       Writer.
 * @param[out]    out_len Document length in bytes, NUL excluded; optional.
 *
 * @return Whether a complete, well-formed document was produced.
 * @retval k_ra8_ok                Complete; the buffer may be published.
 * @retval k_ra8_err_invalid_arg   `w` was NULL.
 * @retval k_ra8_err_invalid_state Writer unarmed, or elements are still open.
 * @retval other                   The first refusal any earlier call made.
 *
 * @post On any refusal `*out_len` is 0 and the buffer must not be published.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_writer_finish(ra8_xml_writer_t* w,
                                              size_t*           out_len);

#ifdef __cplusplus
}
#endif
