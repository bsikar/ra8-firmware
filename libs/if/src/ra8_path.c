/**
 * @file ra8_path.c
 * @brief Implementation of the untrusted-name policy.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Pure string policy over caller-owned storage: no filesystem call,
 *          no allocation, no symlink resolution, no global state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_path.h"

/**
 * @enum path_sanitize_size_t
 * @brief Fixed sizes used while classifying a reserved device base name.
 */
typedef enum : uint8_t {
  k_path_reserved_base_max = 8U, /**< Buffer for a reserved-name base.   */
  k_path_reserved_len      = 4U, /**< Length of a COMx / LPTx name.      */
  k_path_reserved_digit_at = 3U, /**< Index of the digit in COMx / LPTx. */
} path_sanitize_size_t;

/** @brief Name substituted when a candidate sanitises to nothing usable. */
static const char* const s_path_fallback = "item";

/** @brief Reserved device base names matched in full (case-folded). */
static const char* const s_path_reserved_exact[] = {"con", "prn", "aux", "nul"};

/** @brief Reserved device prefixes that take a 1-9 suffix (case-folded). */
static const char* const s_path_reserved_numbered[] = {"com", "lpt"};

/**
 * @brief Lower-case one ASCII byte, locale-independently.
 * @param[in] c Input byte.
 * @return The lower-case mapping of @p c, or @p c unchanged outside `A-Z`.
 * @retval 0 The input byte was NUL.
 * @retval other Lower-case mapping or the unchanged input byte.
 * @pre None; the function reads only its argument.
 * @post No caller storage is written.
 * @note Pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static char priv_path_lower_ascii(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Report whether a byte may appear verbatim in a sanitised segment.
 * @param[in] c Input byte.
 * @return Whether @p c is an ASCII letter, digit, dot, dash or underscore.
 * @retval true The byte is in the allowed set.
 * @retval false The byte must be replaced.
 * @pre None; the function reads only its argument.
 * @post No caller storage is written.
 * @note Pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool priv_path_allowed_char(char c)
{
  return ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
         (c == '.') || (c == '-') || (c == '_');
}

/**
 * @brief Report whether a name is empty, `.` or `..`.
 * @param[in] name NUL-terminated name.
 * @return Whether @p name carries no usable segment.
 * @retval true The name is empty, `.` or `..`.
 * @retval false The name names something.
 * @pre @p name is NUL-terminated.
 * @post No caller storage is written.
 * @note Pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool priv_path_dot_segment(const char* name)
{
  return (name[0] == '\0') || (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0);
}

/**
 * @brief Copy the case-folded base name (up to the first `.`) into a buffer.
 * @param[in]  name NUL-terminated name.
 * @param[out] base Destination for the folded base.
 * @param[in]  cap  Capacity of @p base including its NUL.
 * @pre @p base addresses at least @p cap writable bytes.
 * @post @p base is NUL-terminated.
 * @note Thread-safe: writes only caller-provided storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void priv_path_base_of(const char* name, char* base, size_t cap)
{
  size_t i = 0U;
  while ((name[i] != '\0') && (name[i] != '.') && ((i + 1U) < cap)) {
    base[i] = priv_path_lower_ascii(name[i]);
    ++i;
  }
  base[i] = '\0';
}

/**
 * @brief Report whether a name's base is a reserved device name.
 * @param[in] name NUL-terminated name.
 * @return Whether the base of @p name is reserved.
 * @retval true The base is `con`, `prn`, `aux`, `nul`, `comN` or `lptN`.
 * @retval false The base is free to use.
 * @pre @p name is NUL-terminated.
 * @post No caller storage is written.
 * @note Pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool priv_path_reserved_base(const char* name)
{
  char base[k_path_reserved_base_max];
  priv_path_base_of(name, base, sizeof(base));
  for (size_t i = 0U; i < (sizeof(s_path_reserved_exact) / sizeof(s_path_reserved_exact[0])); ++i) {
    if (strcmp(base, s_path_reserved_exact[i]) == 0) {
      return true;
    }
  }
  const bool numbered = (strlen(base) == (size_t)k_path_reserved_len) &&
                        (base[k_path_reserved_digit_at] >= '1') &&
                        (base[k_path_reserved_digit_at] <= '9');
  if (!numbered) {
    return false;
  }
  for (size_t i = 0U;
       i < (sizeof(s_path_reserved_numbered) / sizeof(s_path_reserved_numbered[0])); ++i) {
    if (strncmp(base, s_path_reserved_numbered[i], strlen(s_path_reserved_numbered[i])) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Copy a candidate into the output, replacing every disallowed byte.
 * @param[in]  raw     Untrusted candidate, or NULL.
 * @param[out] out     Destination buffer.
 * @param[in]  cap     Capacity of @p out including its NUL.
 * @param[out] out_len Receives the produced byte length.
 * @return Whether every input byte survived unchanged and untruncated.
 * @retval true No byte was replaced and nothing was dropped.
 * @retval false A byte was replaced, or the input did not fit.
 * @pre @p out addresses at least @p cap writable bytes and @p cap is non-zero.
 * @post @p out is NUL-terminated and `*out_len` is its length.
 * @note Thread-safe: writes only caller-provided storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
priv_path_copy_sanitised(const char* raw, char* out, size_t cap, size_t* out_len)
{
  bool   clean = true;
  size_t n     = 0U;
  if (raw != nullptr) {
    size_t i = 0U;
    while ((raw[i] != '\0') && ((n + 1U) < cap)) {
      if (priv_path_allowed_char(raw[i])) {
        out[n] = raw[i];
      } else {
        out[n] = '_';
        clean  = false;
      }
      ++n;
      ++i;
    }
    if (raw[i] != '\0') {
      clean = false; /* the candidate did not fit: it was truncated */
    }
  } else {
    clean = false;
  }
  out[n]   = '\0';
  *out_len = n;
  return clean;
}

/**
 * @brief Prepend an underscore in place, dropping the tail that no longer fits.
 * @param[in,out] out Buffer holding the name to prefix.
 * @param[in]     cap Capacity of @p out including its NUL.
 * @param[in]     len Current length of the name in @p out.
 * @pre @p cap is at least ::k_ra8_path_segment_cap_min.
 * @post @p out is NUL-terminated and begins with `_`.
 * @note Thread-safe: writes only caller-provided storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void priv_path_prepend_underscore(char* out, size_t cap, size_t len)
{
  size_t keep = len;
  if ((keep + 2U) > cap) {
    keep = cap - 2U;
  }
  memmove(out + 1, out, keep);
  out[0]        = '_';
  out[keep + 1] = '\0';
}

/**
 * @brief Report whether a segment embeds a directory separator.
 * @param[in] seg NUL-terminated segment.
 * @return Whether @p seg would span more than one directory level.
 * @retval true A `/` is present.
 * @retval false The segment names one level.
 * @pre @p seg is NUL-terminated.
 * @post No caller storage is written.
 * @note Pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool priv_path_has_separator(const char* seg)
{
  return strchr(seg, '/') != nullptr;
}

ra8_err_t ra8_path_sanitize_segment(const char* raw, char* out, size_t cap, bool* out_verbatim)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cap < (size_t)k_ra8_path_segment_cap_min) {
    return k_ra8_err_invalid_size;
  }

  size_t     len      = 0U;
  bool       verbatim = priv_path_copy_sanitised(raw, out, cap, &len);
  const bool dotted   = priv_path_dot_segment(out);

  if (dotted) {
    size_t n = 0U;
    while ((s_path_fallback[n] != '\0') && ((n + 1U) < cap)) {
      out[n] = s_path_fallback[n];
      ++n;
    }
    out[n]   = '\0';
    verbatim = false;
  } else if (priv_path_reserved_base(out)) {
    priv_path_prepend_underscore(out, cap, len);
    verbatim = false;
  } else {
    /* the sanitised copy already stands on its own */
  }

  if (out_verbatim != nullptr) {
    *out_verbatim = verbatim;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_path_join_under(const char* parent, const char* seg, char* out, size_t cap)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  out[0] = '\0';
  if ((parent == nullptr) || (seg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (priv_path_dot_segment(seg) || priv_path_has_separator(seg)) {
    return k_ra8_err_invalid_arg;
  }

  const size_t plen = strlen(parent);
  const size_t slen = strlen(seg);
  const size_t need = plen + 1U + slen + 1U; /* parent + '/' + seg + NUL */
  if (need > cap) {
    return k_ra8_err_no_mem; /* refuse a truncated, thus different, path */
  }

  memcpy(out, parent, plen);
  out[plen] = '/';
  memcpy(out + plen + 1U, seg, slen);
  out[plen + 1U + slen] = '\0';
  return k_ra8_ok;
}

ra8_err_t ra8_path_contained(const char* parent, const char* candidate, bool* out_contained)
{
  if ((parent == nullptr) || (candidate == nullptr) || (out_contained == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  size_t plen = strlen(parent);
  while ((plen > 0U) && (parent[plen - 1U] == '/')) {
    --plen;
  }
  if (plen == 0U) {
    return k_ra8_err_invalid_arg; /* an empty or all-slash parent contains nothing */
  }

  if (strncmp(candidate, parent, plen) != 0) {
    *out_contained = false;
    return k_ra8_ok;
  }
  const char sep = candidate[plen];
  *out_contained = (sep == '/') || (sep == '\0');
  return k_ra8_ok;
}
