/**
 * @file ra8_xml_writer.c
 * @brief Implementation of the bounded XML emitter.
 * @ingroup grp_fmt
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Pure string construction over caller-owned storage: no allocation,
 *          no recursion, no global state. Every append is length-checked
 *          before a byte is written, so a refused call leaves the document
 *          exactly as the last accepted call left it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_xml_writer.h"

/**
 * @enum xml_writer_literal_t
 * @brief Byte counts the builder reasons about while appending.
 */
typedef enum : uint8_t {
  k_xml_writer_nul   = 1U, /**< The terminator every append reserves. */
  k_xml_writer_delim = 1U, /**< One `<`, `>`, `/`, `=` or quote byte. */
} xml_writer_literal_t;

/** @brief The one declaration every document in the tree carries. */
#define XML_WRITER_DECLARATION "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"

/** @brief XML entity for a metacharacter, or NULL when `c` needs no escape. */
RA8_INTERNAL static const char* internal_entity(char c)
{
  switch (c) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '"':
      return "&quot;";
    case '\'':
      return "&apos;";
    default:
      return nullptr;
  }
}

/** @brief Whether `c` may open an XML Name. */
RA8_INTERNAL static bool internal_name_start(char c)
{
  const bool upper = (c >= 'A') && (c <= 'Z');
  const bool lower = (c >= 'a') && (c <= 'z');
  return upper || lower || (c == '_') || (c == ':');
}

/** @brief Whether `c` may continue an XML Name. */
RA8_INTERNAL static bool internal_name_char(char c)
{
  const bool digit = (c >= '0') && (c <= '9');
  return internal_name_start(c) || digit || (c == '.') || (c == '-');
}

/** @brief Whether `name` is a legal, non-empty XML Name. */
RA8_INTERNAL static bool internal_name_ok(const char* name)
{
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
  if (!internal_name_start(name[0])) {
    return false;
  }
  for (size_t i = 1U; name[i] != '\0'; ++i) {
    if (!internal_name_char(name[i])) {
      return false;
    }
  }
  return true;
}

/** @brief Record the first refusal and hand it back unchanged. */
RA8_INTERNAL static ra8_err_t internal_fail(ra8_xml_writer_t* w, ra8_err_t err)
{
  if (w->status == k_ra8_ok) {
    w->status = err;
  }
  return err;
}

/** @brief Append `n` bytes of `s`, or refuse without touching the document. */
RA8_INTERNAL static ra8_err_t
internal_put(ra8_xml_writer_t* w, const char* s, size_t n)
{
  if ((w->len + n + k_xml_writer_nul) > w->cap) {
    return internal_fail(w, k_ra8_err_no_mem);
  }
  memcpy(w->out + w->len, s, n);
  w->len += n;
  w->out[w->len] = '\0';
  return k_ra8_ok;
}

/** @brief Append a NUL-terminated literal. */
RA8_INTERNAL static ra8_err_t internal_puts(ra8_xml_writer_t* w, const char* s)
{
  return internal_put(w, s, strlen(s));
}

/** @brief Append `s` with the five predefined entities substituted. */
RA8_INTERNAL static ra8_err_t
internal_put_escaped(ra8_xml_writer_t* w, const char* s)
{
  for (size_t i = 0U; s[i] != '\0'; ++i) {
    const char*     ent = internal_entity(s[i]);
    const ra8_err_t err =
        (ent != nullptr) ? internal_puts(w, ent) : internal_put(w, &s[i], 1U);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/** @brief Close a pending start tag with `>` so content may follow. */
RA8_INTERNAL static ra8_err_t internal_close_tag(ra8_xml_writer_t* w)
{
  if (!w->tag_open) {
    return k_ra8_ok;
  }
  const ra8_err_t err = internal_puts(w, ">");
  if (err == k_ra8_ok) {
    w->tag_open = false;
  }
  return err;
}

/** @brief Whether `w` is armed and has not yet refused anything. */
RA8_INTERNAL static bool internal_usable(const ra8_xml_writer_t* w)
{
  return w->ready && (w->status == k_ra8_ok);
}

ra8_err_t ra8_xml_escape(const char* src, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  out[0] = '\0';
  if (src == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  size_t n = 0U;
  for (size_t i = 0U; src[i] != '\0'; ++i) {
    const char*  ent  = internal_entity(src[i]);
    const size_t need = (ent != nullptr) ? strlen(ent) : 1U;
    if ((n + need + k_xml_writer_nul) > cap) {
      out[0] = '\0';
      return k_ra8_err_no_mem;
    }
    if (ent != nullptr) {
      memcpy(out + n, ent, need);
    } else {
      out[n] = src[i];
    }
    n += need;
  }
  out[n] = '\0';
  return k_ra8_ok;
}

ra8_err_t ra8_xml_writer_init(ra8_xml_writer_t*       w,
                              char*                   out,
                              size_t                  cap,
                              ra8_xml_writer_frame_t* frames,
                              uint32_t                frame_cap)
{
  if ((w == nullptr) || (out == nullptr) || (frames == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((cap == 0U) || (frame_cap < k_ra8_xml_min_frame_cap)) {
    return k_ra8_err_invalid_arg;
  }
  w->out       = out;
  w->frames    = frames;
  w->cap       = cap;
  w->len       = 0U;
  w->frame_cap = frame_cap;
  w->depth     = 0U;
  w->status    = k_ra8_ok;
  w->tag_open  = false;
  w->ready     = true;
  out[0]       = '\0';
  return k_ra8_ok;
}

ra8_err_t ra8_xml_writer_declaration(ra8_xml_writer_t* w)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_usable(w)) {
    return w->ready ? w->status : k_ra8_err_invalid_state;
  }
  if ((w->depth != 0U) || (w->len != 0U)) {
    return internal_fail(w, k_ra8_err_invalid_state);
  }
  return internal_puts(w, XML_WRITER_DECLARATION);
}

ra8_err_t ra8_xml_writer_start_element(ra8_xml_writer_t* w, const char* name)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_usable(w)) {
    return w->ready ? w->status : k_ra8_err_invalid_state;
  }
  if (!internal_name_ok(name)) {
    return internal_fail(w, k_ra8_err_invalid_arg);
  }
  const size_t nlen = strlen(name);
  if (nlen >= (size_t)k_ra8_xml_name_cap) {
    return internal_fail(w, k_ra8_err_no_mem);
  }
  if (w->depth >= w->frame_cap) {
    return internal_fail(w, k_ra8_err_no_mem);
  }
  ra8_err_t err = internal_close_tag(w);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_puts(w, "<");
  if (err == k_ra8_ok) {
    err = internal_put(w, name, nlen);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  memcpy(w->frames[w->depth].name, name, nlen + k_xml_writer_nul);
  w->depth += 1U;
  w->tag_open = true;
  return k_ra8_ok;
}

ra8_err_t
ra8_xml_writer_attr(ra8_xml_writer_t* w, const char* name, const char* value)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_usable(w)) {
    return w->ready ? w->status : k_ra8_err_invalid_state;
  }
  if ((value == nullptr) || !internal_name_ok(name)) {
    return internal_fail(w, k_ra8_err_invalid_arg);
  }
  if (!w->tag_open) {
    return internal_fail(w, k_ra8_err_invalid_state);
  }
  ra8_err_t err = internal_puts(w, " ");
  if (err == k_ra8_ok) {
    err = internal_puts(w, name);
  }
  if (err == k_ra8_ok) {
    err = internal_puts(w, "=\"");
  }
  if (err == k_ra8_ok) {
    err = internal_put_escaped(w, value);
  }
  if (err == k_ra8_ok) {
    err = internal_puts(w, "\"");
  }
  return err;
}

ra8_err_t ra8_xml_writer_text(ra8_xml_writer_t* w, const char* text)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_usable(w)) {
    return w->ready ? w->status : k_ra8_err_invalid_state;
  }
  if (text == nullptr) {
    return internal_fail(w, k_ra8_err_invalid_arg);
  }
  if (w->depth == 0U) {
    return internal_fail(w, k_ra8_err_invalid_state);
  }
  const ra8_err_t err = internal_close_tag(w);
  return (err != k_ra8_ok) ? err : internal_put_escaped(w, text);
}

ra8_err_t ra8_xml_writer_end_element(ra8_xml_writer_t* w)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_usable(w)) {
    return w->ready ? w->status : k_ra8_err_invalid_state;
  }
  if (w->depth == 0U) {
    return internal_fail(w, k_ra8_err_invalid_state);
  }
  if (w->tag_open) {
    const ra8_err_t empty = internal_puts(w, "/>");
    if (empty != k_ra8_ok) {
      return empty;
    }
    w->tag_open = false;
    w->depth -= 1U;
    return k_ra8_ok;
  }
  ra8_err_t err = internal_puts(w, "</");
  if (err == k_ra8_ok) {
    err = internal_puts(w, w->frames[w->depth - 1U].name);
  }
  if (err == k_ra8_ok) {
    err = internal_puts(w, ">");
  }
  if (err != k_ra8_ok) {
    return err;
  }
  w->depth -= 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_xml_writer_finish(ra8_xml_writer_t* w, size_t* out_len)
{
  if (w == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  if (!w->ready) {
    return k_ra8_err_invalid_state;
  }
  if (w->status != k_ra8_ok) {
    return w->status;
  }
  if ((w->depth != 0U) || w->tag_open) {
    return internal_fail(w, k_ra8_err_invalid_state);
  }
  if (out_len != nullptr) {
    *out_len = w->len;
  }
  return k_ra8_ok;
}
