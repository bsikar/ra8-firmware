/**
 * @file ra8_imgdec_sniff.c
 * @brief The shared container sniff behind the image-decoder seam (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * One signature test for the whole tree. The four in-tree copies it replaces
 * disagreed about how much they covered, not about the bytes: the reflow blit
 * path tests RIFF/WEBP only, the JOF producer tests JPEG and PNG and
 * RIFF/WEBP, the JOF exporter tests RIFF/WEBP again, and the media_dl URL
 * namer tests all of those plus GIF and BMP. Every one of them reads fixed
 * bytes at fixed offsets, so the union is a single function with no policy in
 * it beyond "which signatures does this tree recognise".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/* =============================================================================
 * Signature vocabulary
 * =============================================================================
 */

/**
 * @enum ra8_imgdec_sig_len_t
 * @brief How many leading bytes each signature needs (no magic numbers).
 *
 * @invariant Every value is at most ::k_ra8_imgdec_sniff_bytes, which is what
 *            makes that constant an honest promise about the read window.
 */
typedef enum : uint8_t {
  k_sig_len_jpeg = 3U,  /**< FF D8 FF: SOI plus the first marker prefix. */
  k_sig_len_png  = 8U,  /**< The eight-byte PNG signature.               */
  k_sig_len_webp = 12U, /**< "RIFF", a size, then the "WEBP" form type.  */
  k_sig_len_gif  = 6U,  /**< "GIF87a" or "GIF89a".                       */
  k_sig_len_bmp  = 2U,  /**< "BM".                                       */
  k_sig_len_tag  = 3U,  /**< "GIF", and either version trailer.          */
  k_sig_len_four = 4U,  /**< One RIFF fourCC.                            */
} ra8_imgdec_sig_len_t;

/**
 * @enum ra8_imgdec_sig_off_t
 * @brief Offsets within a signature that are not simply byte 0 onwards.
 */
typedef enum : uint8_t {
  k_sig_off_webp_riff = 0U, /**< Offset of the "RIFF" fourCC.           */
  k_sig_off_webp_form = 8U, /**< Offset of the "WEBP" form type.        */
  k_sig_off_gif_ver   = 3U, /**< First byte of the "87a"/"89a" trailer. */
} ra8_imgdec_sig_off_t;

/** @brief The eight bytes every PNG file opens with. */
static const uint8_t k_png_signature[k_sig_len_png] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

/** @brief The three bytes a JFIF/EXIF JPEG opens with (SOI + marker prefix). */
static const uint8_t k_jpeg_signature[k_sig_len_jpeg] = {0xFFU, 0xD8U, 0xFFU};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Compare @p len bytes at @p offset against @p want, bounds first.
 *
 * @details Every signature test in this file goes through here so the length
 * check is written once. A buffer too short to carry the signature is not a
 * mismatch to be reported differently; it simply is not that format.
 */
RA8_INTERNAL static bool internal_bytes_match(const uint8_t* bytes,
                                              uint32_t       byte_count,
                                              uint32_t       offset,
                                              const uint8_t* want,
                                              uint32_t       len) {
  if (byte_count < (offset + len)) {
    return false;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    if (bytes[offset + i] != want[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Compare @p len bytes at @p offset against an ASCII tag.
 */
RA8_INTERNAL static bool internal_tag_match(const uint8_t* bytes,
                                            uint32_t       byte_count,
                                            uint32_t       offset,
                                            const char*    tag,
                                            uint32_t       len) {
  if (byte_count < (offset + len)) {
    return false;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    if (bytes[offset + i] != (uint8_t)tag[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True when the buffer opens with a RIFF container whose form is WEBP.
 *
 * @details Both fourCCs are required. A bare "RIFF" is some other RIFF payload
 * (WAVE, AVI), and routing one of those into a WebP decoder is exactly the
 * mis-route the four private copies each guarded against separately.
 */
RA8_INTERNAL static bool internal_is_webp(const uint8_t* bytes, uint32_t byte_count) {
  return internal_tag_match(bytes, byte_count, (uint32_t)k_sig_off_webp_riff, "RIFF",
                            (uint32_t)k_sig_len_four) &&
         internal_tag_match(bytes, byte_count, (uint32_t)k_sig_off_webp_form, "WEBP",
                            (uint32_t)k_sig_len_four);
}

/**
 * @brief True when the buffer opens with "GIF87a" or "GIF89a".
 *
 * @details The three-byte "GIF" tag alone is not enough: the version pair is
 * what separates a GIF from a file that merely starts with those letters, and
 * the media_dl namer already required it.
 */
RA8_INTERNAL static bool internal_is_gif(const uint8_t* bytes, uint32_t byte_count) {
  if (!internal_tag_match(bytes, byte_count, 0U, "GIF", (uint32_t)k_sig_len_tag)) {
    return false;
  }
  const bool v87 = internal_tag_match(bytes, byte_count, (uint32_t)k_sig_off_gif_ver, "87a",
                                      (uint32_t)k_sig_len_tag);
  const bool v89 = internal_tag_match(bytes, byte_count, (uint32_t)k_sig_off_gif_ver, "89a",
                                      (uint32_t)k_sig_len_tag);
  return v87 || v89;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_imgdec_sniff(const uint8_t* bytes, uint32_t byte_count, ra8_imgdec_format_t* out) {
  if ((bytes == nullptr) || (out == nullptr)) {
    if (out != nullptr) {
      *out = k_ra8_imgdec_format_none;
    }
    return k_ra8_err_null_ptr;
  }
  *out = k_ra8_imgdec_format_none;

  if (byte_count == 0U) {
    return k_ra8_err_invalid_size;
  }

  ra8_imgdec_format_t found = k_ra8_imgdec_format_none;

  if (internal_bytes_match(bytes, byte_count, 0U, k_png_signature, (uint32_t)k_sig_len_png)) {
    found = k_ra8_imgdec_format_png;
  } else if (internal_bytes_match(bytes, byte_count, 0U, k_jpeg_signature,
                                  (uint32_t)k_sig_len_jpeg)) {
    found = k_ra8_imgdec_format_jpeg;
  } else if (internal_is_webp(bytes, byte_count)) {
    found = k_ra8_imgdec_format_webp;
  } else if (internal_is_gif(bytes, byte_count)) {
    found = k_ra8_imgdec_format_gif;
  } else if (internal_tag_match(bytes, byte_count, 0U, "BM", (uint32_t)k_sig_len_bmp)) {
    found = k_ra8_imgdec_format_bmp;
  } else {
    found = k_ra8_imgdec_format_none;
  }

  if (found == k_ra8_imgdec_format_none) {
    return k_ra8_err_not_found;
  }

  *out = found;
  return k_ra8_ok;
}
