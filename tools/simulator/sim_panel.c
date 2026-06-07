/**
 * @file sim_panel.c
 * @brief TOML (flat key = value) loader for simulator panel descriptors
 *
 * @details
 * Dependency-free, bounded parser: reads at most one key = value per line,
 * ignores blank lines and '#' comments, trims surrounding whitespace, and
 * strips quotes from string values. Uses strncmp / strtol (no unbounded
 * string ops, no dynamic allocation).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum : uint16_t {
  k_sim_line_max = 256U, /**< Max config line length incl NUL. */
} sim_parse_limits_t;

const char* sim_pixfmt_str(sim_pixfmt_t fmt)
{
  switch (fmt) {
    case k_sim_pixfmt_rgb565:
      return "rgb565";
    case k_sim_pixfmt_gray4:
      return "gray4";
    case k_sim_pixfmt_unknown:
    default:
      return "unknown";
  }
}

/**
 * @brief Advance past leading spaces/tabs.
 *
 * @param[in] s NUL-terminated string.
 * @return Pointer to the first non-blank character.
 */
static char* sim_skip_ws(char* s)
{
  while ((*s == ' ') || (*s == '\t')) {
    s++;
  }
  return s;
}

/**
 * @brief Trim trailing spaces, tabs, CR, and LF in place.
 *
 * @param[in,out] s NUL-terminated string.
 */
static void sim_rstrip(char* s)
{
  size_t n = strlen(s);
  while (n > 0U) {
    const char c = s[n - 1U];
    if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n')) {
      break;
    }
    s[n - 1U] = '\0';
    n--;
  }
}

/**
 * @brief Copy a string value (quotes stripped) into a bounded buffer.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  dst_len Destination capacity (incl NUL).
 * @param[in]  val     Source value (may be quoted).
 */
static void sim_copy_str(char* dst, size_t dst_len, const char* val)
{
  const char* start = val;
  size_t      len   = strlen(val);
  if ((len >= 2U) && (start[0] == '"') && (start[len - 1U] == '"')) {
    start += 1;
    len -= 2U;
  }
  if (len >= dst_len) {
    len = dst_len - 1U;
  }
  (void)memcpy(dst, start, len);
  dst[len] = '\0';
}

/**
 * @brief Apply one parsed key/value pair to the descriptor.
 *
 * @param[in,out] out Descriptor being filled.
 * @param[in]     key Trimmed key.
 * @param[in]     val Trimmed value.
 */
static void sim_apply(sim_panel_t* out, const char* key, const char* val)
{
  if (strncmp(key, "name", sizeof("name")) == 0) {
    sim_copy_str(out->name, sizeof(out->name), val);
  } else if (strncmp(key, "width", sizeof("width")) == 0) {
    out->width_px = (uint16_t)strtol(val, nullptr, 10);
  } else if (strncmp(key, "height", sizeof("height")) == 0) {
    out->height_px = (uint16_t)strtol(val, nullptr, 10);
  } else if (strncmp(key, "ppi", sizeof("ppi")) == 0) {
    out->ppi = (uint16_t)strtol(val, nullptr, 10);
  } else if (strncmp(key, "format", sizeof("format")) == 0) {
    char fmt[16];
    sim_copy_str(fmt, sizeof(fmt), val);
    if (strncmp(fmt, "rgb565", sizeof("rgb565")) == 0) {
      out->format = k_sim_pixfmt_rgb565;
    } else if (strncmp(fmt, "gray4", sizeof("gray4")) == 0) {
      out->format = k_sim_pixfmt_gray4;
    } else {
      out->format = k_sim_pixfmt_unknown;
    }
  }
}

ra_err_t sim_panel_load(const char* path, sim_panel_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra_err_null_ptr;
  }
  (void)memset(out, 0, sizeof(*out));

  FILE* f = fopen(path, "r"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    (void)fprintf(stderr, "sim: cannot open panel config: %s\n", path);
    return k_ra_err_invalid_arg;
  }

  char line[k_sim_line_max];
  while (fgets(line, (int)sizeof(line), f) != nullptr) {
    char* p = sim_skip_ws(line);
    if ((*p == '#') || (*p == '\0') || (*p == '\n') || (*p == '\r')) {
      continue;
    }
    char* eq = strchr(p, '=');
    if (eq == nullptr) {
      continue;
    }
    *eq       = '\0';
    char* key = p;
    char* val = sim_skip_ws(eq + 1);
    sim_rstrip(key);
    sim_rstrip(val);
    sim_apply(out, key, val);
  }
  (void)fclose(f);

  if ((out->width_px == 0U) || (out->width_px > (uint16_t)k_sim_panel_max_w) ||
      (out->height_px == 0U) || (out->height_px > (uint16_t)k_sim_panel_max_h)) {
    (void)fprintf(stderr, "sim: panel %s: width/height missing or out of range\n", path);
    return k_ra_err_invalid_arg;
  }
  if (out->format == k_sim_pixfmt_unknown) {
    (void)fprintf(stderr, "sim: panel %s: missing/unknown format\n", path);
    return k_ra_err_invalid_arg;
  }
  if (out->name[0] == '\0') {
    sim_copy_str(out->name, sizeof(out->name), "(unnamed)");
  }
  return k_ra_ok;
}
