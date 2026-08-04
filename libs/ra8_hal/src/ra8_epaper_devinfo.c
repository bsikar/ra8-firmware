/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_epaper_devinfo.c
 * @brief IT8951 ``GET_DEV_INFO`` response decoder
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The controller answers ``GET_DEV_INFO`` (0x0302) with a fixed 20-word
 * block: panel geometry, the two halves of its image-buffer base address,
 * and two 16-character version strings packed two ASCII chars per word.
 * Decoding that block is pure -- it touches no bus, no GPIO and no driver
 * state -- so it lives here rather than in ``ra8_epaper.c``, alongside the
 * same split that produced ``ra8_epaper_geom.c``.
 *
 * Separating the decode from the read is what makes it testable. The
 * reported LUT version is the input to ``ra8_epaper_waveform_cfg_for_lut``,
 * and therefore decides which waveform mode number A2 refreshes use; a
 * mis-decoded version string silently selects the wrong mode on a panel
 * nobody has attached yet. As a pure function over a word array, every
 * field of the layout can be pinned by a host test with no controller
 * present.
 *
 * Version bytes outside printable ASCII are dropped to NUL: the strings
 * are logged, and a controller that has not finished loading its waveform
 * answers early reads with garbage that must not reach a log as control
 * characters.
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_epaper.h"
#include "ra8_err.h"

/**
 * @var s_tag
 * @brief Logging tag used by the null-argument guards in this TU.
 */
static const char* const s_tag = "EPAPER";

/**
 * @enum ra8_epaper_devinfo_const_t
 * @brief Word-unpacking constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_epaper_di_byte_mask    = 0xFFU, /**< Low-byte extraction mask. */
  k_ra8_epaper_di_byte_shift   = 8U,    /**< Bits per byte.            */
  k_ra8_epaper_di_word_shift   = 16U,   /**< Bits per 16-bit word.     */
  k_ra8_epaper_di_ver_per_word = 2U,    /**< Version chars per word.   */
  k_ra8_epaper_di_ascii_min    = 0x20U, /**< Lowest printable ASCII.   */
  k_ra8_epaper_di_ascii_max    = 0x7FU, /**< One past printable ASCII. */
} ra8_epaper_devinfo_const_t;

/**
 * @enum ra8_epaper_devinfo_idx_t
 * @brief Word offsets inside the 20-word ``GET_DEV_INFO`` response.
 *
 * @details
 * The layout is fixed by the controller firmware, so each word is placed
 * by index rather than by a parser state machine.
 */
typedef enum : uint8_t {
  k_ra8_epaper_di_idx_width  = 0U,  /**< Panel width.                    */
  k_ra8_epaper_di_idx_height = 1U,  /**< Panel height.                   */
  k_ra8_epaper_di_idx_buf_lo = 2U,  /**< Image-buffer base, low half.    */
  k_ra8_epaper_di_idx_buf_hi = 3U,  /**< Image-buffer base, high half.   */
  k_ra8_epaper_di_idx_fw     = 4U,  /**< First firmware-version word.    */
  k_ra8_epaper_di_idx_lut    = 12U, /**< First LUT-version word.         */
  k_ra8_epaper_di_idx_end    = 20U, /**< One past the last decoded word. */
} ra8_epaper_devinfo_idx_t;

/**
 * @brief Unpack one 16-bit version word into two ASCII chars.
 *
 * @details
 * Version strings arrive packed two characters per word, high byte first.
 * Any byte outside printable ASCII is dropped to NUL so a garbled read
 * cannot inject control characters into a string the app may log.
 *
 * @param[in]  word Source word.
 * @param[out] dst  Destination for two chars; non-NULL.
 *
 * @pre  ``dst`` has room for two chars.
 * @pre  ``word`` is one version word of a GET_DEV_INFO response.
 * @post ``dst[0]`` and ``dst[1]`` are printable ASCII or NUL.
 * @post No driver state is mutated.
 *
 * @note Not thread-safe; called only from the decode loop below.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_epaper_unpack_ver_word(uint16_t word, char* dst)
{
  const uint8_t hi =
    (uint8_t)((word >> (uint16_t)k_ra8_epaper_di_byte_shift) & (uint16_t)k_ra8_epaper_di_byte_mask);
  const uint8_t lo = (uint8_t)(word & (uint16_t)k_ra8_epaper_di_byte_mask);
  const bool    hi_p =
    (hi >= (uint8_t)k_ra8_epaper_di_ascii_min) && (hi < (uint8_t)k_ra8_epaper_di_ascii_max);
  const bool lo_p =
    (lo >= (uint8_t)k_ra8_epaper_di_ascii_min) && (lo < (uint8_t)k_ra8_epaper_di_ascii_max);
  /* Cast the whole conditional, not each arm: the usual arithmetic
   * conversions give the ternary type `int`, so casting only the value arm
   * still leaves an implicit int -> char narrowing at the assignment. Both
   * arms are bounded by the printable-ASCII test above (0x20 .. 0x7E), so
   * every value is representable in a signed char. */
  dst[0] = (char)(hi_p ? hi : 0U);
  dst[1] = (char)(lo_p ? lo : 0U);
}

/**
 * @brief Fold one ``GET_DEV_INFO`` word into the decoded info block.
 *
 * @details
 * Geometry first, then the two halves of the image-buffer base address,
 * then the firmware and LUT version strings two ASCII chars per word.
 *
 * @param[in]     idx  Word index within the 20-word response.
 * @param[in]     word The word to fold in.
 * @param[in,out] out  Info block being assembled; non-NULL.
 *
 * @pre  ``out`` points at a block zeroed before the first word.
 * @pre  ``idx`` is less than ::k_ra8_epaper_di_idx_end.
 * @post Exactly the one field selected by ``idx`` is updated.
 * @post No bus transaction is issued.
 *
 * @note Not thread-safe; called only from the decode loop below.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_ra8_epaper_decode_dev_word(uint32_t idx, uint16_t word, ra8_epaper_dev_info_t* out)
{
  if (idx == (uint32_t)k_ra8_epaper_di_idx_width) {
    out->panel_width = word;
  } else if (idx == (uint32_t)k_ra8_epaper_di_idx_height) {
    out->panel_height = word;
  } else if (idx == (uint32_t)k_ra8_epaper_di_idx_buf_lo) {
    out->image_buf_base |= (uint32_t)word;
  } else if (idx == (uint32_t)k_ra8_epaper_di_idx_buf_hi) {
    out->image_buf_base |= ((uint32_t)word << (uint32_t)k_ra8_epaper_di_word_shift);
  } else if (idx < (uint32_t)k_ra8_epaper_di_idx_lut) {
    const uint32_t off =
      (idx - (uint32_t)k_ra8_epaper_di_idx_fw) * (uint32_t)k_ra8_epaper_di_ver_per_word;
    internal_ra8_epaper_unpack_ver_word(word, &out->fw_version[off]);
  } else {
    const uint32_t off =
      (idx - (uint32_t)k_ra8_epaper_di_idx_lut) * (uint32_t)k_ra8_epaper_di_ver_per_word;
    internal_ra8_epaper_unpack_ver_word(word, &out->lut_version[off]);
  }
}

[[nodiscard]] ra8_err_t
ra8_epaper_decode_dev_info(const uint16_t* words, size_t count, ra8_epaper_dev_info_t* out_info)
{
  RA8_CHECK_NULL_PTR(words, s_tag, "decode_dev_info: words null");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "decode_dev_info: out null");
  if (count < (size_t)k_ra8_epaper_di_idx_end) {
    ra8_log_error(s_tag, "decode_dev_info: short response");
    return k_ra8_err_invalid_arg;
  }

  *out_info = (ra8_epaper_dev_info_t){};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_epaper_di_idx_end; i++) {
    internal_ra8_epaper_decode_dev_word(i, words[i], out_info);
  }
  /* The version fields are one char longer than the packed data, so the
   * terminator lands past the last decoded char and can never be
   * overwritten by the loop above. */
  out_info->fw_version[k_ra8_epaper_ver_chars]  = '\0';
  out_info->lut_version[k_ra8_epaper_ver_chars] = '\0';
  return k_ra8_ok;
}
