/**
 * @file ra8_imgdec_name.c
 * @brief The one naming table behind ra8_imgdec_name/_identify (#748).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * The rows live in the same library as the signature table they have to agree
 * with, so a format added to ::ra8_imgdec_format_t cannot be sniffable and
 * unnameable at the same time: the static_assert below fails the build the
 * moment the format mask grows past what this table covers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec_name.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"

/* =============================================================================
 * The table
 * =============================================================================
 */

/**
 * @brief One row per defined ::ra8_imgdec_format_t bit.
 *
 * @details Order is the enum's own order, so a reader can check the two lists
 * against each other by eye. TGA is present and is the one row
 * ::ra8_imgdec_identify can never reach, because TGA has no signature.
 */
static const ra8_imgdec_name_t s_names[] = {
  {k_ra8_imgdec_format_jpeg, "jpg", "image/jpeg"},
  {k_ra8_imgdec_format_png, "png", "image/png"},
  {k_ra8_imgdec_format_webp, "webp", "image/webp"},
  {k_ra8_imgdec_format_gif, "gif", "image/gif"},
  {k_ra8_imgdec_format_bmp, "bmp", "image/bmp"},
  {k_ra8_imgdec_format_tga, "tga", "image/x-tga"},
};

/** @brief Rows in ::s_names. */
#define RA8_IMGDEC_NAME_ROWS ((uint32_t)(sizeof(s_names) / sizeof(s_names[0])))

/**
 * @brief Every defined format bit must have a row.
 *
 * @details The mask is contiguous from bit 0, so its population count is the
 * number of defined formats and comparing it with the row count is enough.
 * A new format bit widens the mask and breaks this build until its name is
 * written, which is the whole point of putting the two tables in one library.
 */
static_assert(((uint32_t)k_ra8_imgdec_format_mask + 1U) ==
                (1U << (uint32_t)(sizeof(s_names) / sizeof(s_names[0]))),
              "every defined ra8_imgdec_format_t bit needs exactly one name row");

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_imgdec_name(ra8_imgdec_format_t format, ra8_imgdec_name_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_imgdec_name_t){};

  for (uint32_t i = 0U; i < RA8_IMGDEC_NAME_ROWS; ++i) {
    if (s_names[i].format == format) {
      *out = s_names[i];
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

ra8_err_t ra8_imgdec_identify(const uint8_t* bytes, uint32_t byte_count, ra8_imgdec_name_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_imgdec_name_t){};

  ra8_imgdec_format_t format = k_ra8_imgdec_format_none;
  const ra8_err_t     err    = ra8_imgdec_sniff(bytes, byte_count, &format);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_imgdec_name(format, out);
}
