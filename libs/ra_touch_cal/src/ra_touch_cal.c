/**
 * @file ra_touch_cal.c
 * @brief Implementation of the touch-screen calibration utility
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The math here implements the weighted least-squares affine fit from
 * Fang & Chang, "Calibration of Touch Screens for Use with Embedded
 * Systems" (Analog Dialogue 41-08, 2007). For ``N`` raw/screen sample
 * pairs the screen-X coefficients ``(a, b, c)`` satisfy
 *
 *     [ Sxx Sxy Sx ] [ a ]   [ Sxu ]
 *     [ Sxy Syy Sy ] [ b ] = [ Syu ]
 *     [ Sx  Sy  N  ] [ c ]   [ Su  ]
 *
 * where the sums run over the ``N`` samples. The same 3x3 system with
 * the right-hand side replaced by ``(Sxv, Syv, Sv)`` solves for the
 * screen-Y coefficients ``(d, e, f)``. The solver is Cramer's rule on
 * the shared coefficient matrix, which keeps the implementation
 * branch-light and avoids dynamic allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_touch_cal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-isolate-declaration,readability-identifier-naming,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result) */

/* ===========================================================================
 * Internal constants
 * ===========================================================================
 */

/**
 * @enum internal_const_t
 * @brief Module-private numeric constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_internal_centre_div    = 2U,          /**< Halve to get panel centre.     */
  k_internal_byte_mask     = 0xFFU,       /**< 8-bit mask for byte extracts.  */
  k_internal_byte_shift_8  = 8U,          /**< Shift to byte 1.               */
  k_internal_byte_shift_16 = 16U,         /**< Shift to byte 2.               */
  k_internal_byte_shift_24 = 24U,         /**< Shift to byte 3.               */
  k_internal_crc32_init    = 0xFFFFFFFFU, /**< IEEE 802.3 CRC seed.           */
  k_internal_crc32_poly    = 0xEDB88320U, /**< IEEE 802.3 reversed polynomial.*/
  k_internal_bits_per_byte = 8U,          /**< Bits in one byte.              */
} internal_const_t;

/**
 * @var s_min_det
 * @brief Floor for ``|det|`` below which the normal equations are
 *        treated as singular.
 *
 * @details
 * 1e-3 is chosen empirically for a 1024x600 panel: real-world raw-sample
 * spreads produce ``|det|`` in the 1e8 .. 1e10 range, so 1e-3 only
 * trips on truly collinear inputs.
 *
 * @note File-scope static; not thread-shared because it is read-only.
 */
static const float s_min_det = 1.0e-3F;

/* ===========================================================================
 * Internal helpers
 * ===========================================================================
 */

/**
 * @brief Pack a 32-bit little-endian word into a byte buffer.
 *
 * @param[out] dst   Destination (4 bytes available).
 * @param[in]  word  Value to write.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_pack_le32(uint8_t* dst, uint32_t word)
{
  dst[0] = (uint8_t)(word & (uint32_t)k_internal_byte_mask);
  dst[1] = (uint8_t)((word >> (uint32_t)k_internal_byte_shift_8) & (uint32_t)k_internal_byte_mask);
  dst[2] = (uint8_t)((word >> (uint32_t)k_internal_byte_shift_16) & (uint32_t)k_internal_byte_mask);
  dst[3] = (uint8_t)((word >> (uint32_t)k_internal_byte_shift_24) & (uint32_t)k_internal_byte_mask);
}

/**
 * @brief Unpack a 32-bit little-endian word from a byte buffer.
 *
 * @param[in] src  Source (4 bytes available).
 * @return Unpacked word.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_unpack_le32(const uint8_t* src)
{
  uint32_t w = (uint32_t)src[0];
  w |= (uint32_t)src[1] << (uint32_t)k_internal_byte_shift_8;
  w |= (uint32_t)src[2] << (uint32_t)k_internal_byte_shift_16;
  w |= (uint32_t)src[3] << (uint32_t)k_internal_byte_shift_24;
  return w;
}

/**
 * @brief Bitwise reinterpret a ``float`` as a ``uint32_t``.
 *
 * @details Implemented via memcpy to avoid strict-aliasing UB.
 *
 * @param[in] f  Source float.
 * @return Bit pattern of ``f``.
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_float_to_u32(float f)
{
  uint32_t out = 0U;
  (void)memcpy(&out, &f, sizeof(out));
  return out;
}

/**
 * @brief Bitwise reinterpret a ``uint32_t`` as a ``float``.
 *
 * @param[in] w  Source bit pattern.
 * @return Float with the supplied encoding.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static float internal_u32_to_float(uint32_t w)
{
  float out = 0.0F;
  (void)memcpy(&out, &w, sizeof(out));
  return out;
}

/**
 * @brief Compute the IEEE 802.3 CRC32 of a byte range.
 *
 * @details
 * Bit-banged so the function works without depending on ra_crc.
 *
 * @param[in] data  Input bytes.
 * @param[in] len   Number of bytes.
 * @return Final CRC32, post-XOR.
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_internal_crc32_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_internal_bits_per_byte; b++) {
      const uint32_t mask = (uint32_t)0U - (crc & 1U);
      crc                 = (crc >> 1U) ^ ((uint32_t)k_internal_crc32_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_internal_crc32_init;
}

/**
 * @brief Solve a 3x3 system ``A * x = b`` via Cramer's rule.
 *
 * @details
 * Pure float math. Degenerate (collinear) systems are flagged via the
 * ``ok`` out-parameter rather than an error code so the call sites can
 * fold the check into their existing return path.
 *
 * @param[in]  a    Row-major coefficient matrix (length 9).
 * @param[in]  b    Right-hand side (length 3).
 * @param[out] x    Solution vector (length 3).
 * @param[out] ok   Set to true on success, false if ``|det| < s_min_det``.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_solve3(const float a[9], const float b[3], float x[3], bool* ok)
{
  const float det = (a[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
                    (a[1] * ((a[3] * a[8]) - (a[5] * a[6]))) +
                    (a[2] * ((a[3] * a[7]) - (a[4] * a[6])));

  const float abs_det = (det < 0.0F) ? -det : det;
  if (abs_det < s_min_det) {
    *ok = false;
    return;
  }

  const float dx = (b[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
                   (a[1] * ((b[1] * a[8]) - (a[5] * b[2]))) +
                   (a[2] * ((b[1] * a[7]) - (a[4] * b[2])));

  const float dy = (a[0] * ((b[1] * a[8]) - (a[5] * b[2]))) -
                   (b[0] * ((a[3] * a[8]) - (a[5] * a[6]))) +
                   (a[2] * ((a[3] * b[2]) - (b[1] * a[6])));

  const float dz = (a[0] * ((a[4] * b[2]) - (b[1] * a[7]))) -
                   (a[1] * ((a[3] * b[2]) - (b[1] * a[6]))) +
                   (b[0] * ((a[3] * a[7]) - (a[4] * a[6])));

  x[0] = dx / det;
  x[1] = dy / det;
  x[2] = dz / det;
  *ok  = true;
}

/**
 * @brief Clip a value to ``[lo, hi]``.
 *
 * @details See implementation.
 * @param[in] v See implementation.
 * @param[in] lo See implementation.
 * @param[in] hi See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static int32_t internal_clip32(int32_t v, int32_t lo, int32_t hi)
{
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

/* ===========================================================================
 * Public API -- compute
 * ===========================================================================
 */

/* Implementation of ra_touch_cal_compute (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_touch_cal_compute(const ra_touch_cal_point_t* raw,
                              const ra_touch_cal_point_t* screen,
                              uint8_t                     n,
                              ra_touch_cal_matrix_t*      out_mtx)
{
  if ((raw == nullptr) || (screen == nullptr) || (out_mtx == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((n < (uint8_t)k_ra_touch_cal_min_targets) || (n > (uint8_t)k_ra_touch_cal_max_targets)) {
    return k_ra_err_invalid_arg;
  }

  float Sx = 0.0F, Sy = 0.0F;
  float Sxx = 0.0F, Syy = 0.0F, Sxy = 0.0F;
  float Su = 0.0F, Sv = 0.0F;
  float Sxu = 0.0F, Syu = 0.0F;
  float Sxv = 0.0F, Syv = 0.0F;

  for (uint8_t i = 0U; i < n; i++) {
    const float xi = (float)raw[i].x;
    const float yi = (float)raw[i].y;
    const float ui = (float)screen[i].x;
    const float vi = (float)screen[i].y;
    Sx += xi;
    Sy += yi;
    Sxx += xi * xi;
    Syy += yi * yi;
    Sxy += xi * yi;
    Su += ui;
    Sv += vi;
    Sxu += xi * ui;
    Syu += yi * ui;
    Sxv += xi * vi;
    Syv += yi * vi;
  }

  const float fn   = (float)n;
  const float A[9] = {
    Sxx,
    Sxy,
    Sx,
    Sxy,
    Syy,
    Sy,
    Sx,
    Sy,
    fn,
  };

  float       sol_u[3] = {0.0F, 0.0F, 0.0F};
  float       sol_v[3] = {0.0F, 0.0F, 0.0F};
  const float Bu[3]    = {Sxu, Syu, Su};
  const float Bv[3]    = {Sxv, Syv, Sv};

  bool ok_u = false;
  bool ok_v = false;
  internal_solve3(A, Bu, sol_u, &ok_u);
  internal_solve3(A, Bv, sol_v, &ok_v);
  // mcdc-deactivated: TU-local helper internal_clip32 solver-success gate; A is the same 3x3 calibration matrix for both Bu and Bv solves, so internal_solve3 either succeeds for both right-hand sides (det(A) != 0) or fails for both (det(A) == 0) -- ok_u and ok_v are co-determined by the matrix conditioning.
  if (!ok_u || !ok_v) {
    return k_ra_err_invalid_arg;
  }

  out_mtx->a = sol_u[0];
  out_mtx->b = sol_u[1];
  out_mtx->c = sol_u[2];
  out_mtx->d = sol_v[0];
  out_mtx->e = sol_v[1];
  out_mtx->f = sol_v[2];
  return k_ra_ok;
}

/* ===========================================================================
 * Public API -- run (drives the on-screen sequence)
 * ===========================================================================
 */

/* Implementation of ra_touch_cal_run (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_touch_cal_run(const ra_touch_cal_run_cfg_t* cfg, ra_touch_cal_matrix_t* out_matrix)
{
  if ((cfg == nullptr) || (out_matrix == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((cfg->draw_target == nullptr) || (cfg->read_raw == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((cfg->screen_width == 0U) || (cfg->screen_height == 0U)) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t margin_total = (uint32_t)cfg->inset_px * 2U;
  if ((margin_total >= (uint32_t)cfg->screen_width) ||
      (margin_total >= (uint32_t)cfg->screen_height)) {
    return k_ra_err_invalid_arg;
  }

  const int32_t w   = (int32_t)cfg->screen_width;
  const int32_t h   = (int32_t)cfg->screen_height;
  const int32_t ins = (int32_t)cfg->inset_px;

  ra_touch_cal_point_t targets[k_ra_touch_cal_n_targets] = {
    {ins, ins},                                                               /* top-left     */
    {w - 1 - ins, ins},                                                       /* top-right    */
    {w - 1 - ins, h - 1 - ins},                                               /* bottom-right */
    {ins, h - 1 - ins},                                                       /* bottom-left  */
    {w / (int32_t)k_internal_centre_div, h / (int32_t)k_internal_centre_div}, /* centre */
  };

  ra_touch_cal_point_t samples[k_ra_touch_cal_n_targets] = {
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
  };

  for (uint8_t i = 0U; i < (uint8_t)k_ra_touch_cal_n_targets; i++) {
    const ra_err_t er_draw = cfg->draw_target(cfg->draw_ctx, targets[i]);
    if (er_draw != k_ra_ok) {
      return k_ra_err_hw_error;
    }
    const ra_err_t er_read = cfg->read_raw(cfg->read_ctx, &samples[i]);
    if (er_read != k_ra_ok) {
      return k_ra_err_hw_error;
    }
  }

  return ra_touch_cal_compute(samples, targets, (uint8_t)k_ra_touch_cal_n_targets, out_matrix);
}

/* ===========================================================================
 * Public API -- apply
 * ===========================================================================
 */

/* Implementation of ra_touch_cal_apply (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_touch_cal_apply(ra_touch_cal_point_t         raw,
                            const ra_touch_cal_matrix_t* matrix,
                            uint16_t                     screen_width,
                            uint16_t                     screen_height,
                            ra_touch_cal_point_t*        out_screen)
{
  if ((matrix == nullptr) || (out_screen == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((screen_width == 0U) || (screen_height == 0U)) {
    return k_ra_err_invalid_arg;
  }

  const float xf = (float)raw.x;
  const float yf = (float)raw.y;
  const float u  = (matrix->a * xf) + (matrix->b * yf) + matrix->c;
  const float v  = (matrix->d * xf) + (matrix->e * yf) + matrix->f;

  /* Round-to-nearest before clipping. */
  const int32_t ui = (u >= 0.0F) ? (int32_t)(u + 0.5F) : (int32_t)(u - 0.5F);
  const int32_t vi = (v >= 0.0F) ? (int32_t)(v + 0.5F) : (int32_t)(v - 0.5F);

  out_screen->x = internal_clip32(ui, 0, (int32_t)screen_width - 1);
  out_screen->y = internal_clip32(vi, 0, (int32_t)screen_height - 1);
  return k_ra_ok;
}

/* ===========================================================================
 * Public API -- save / load
 * ===========================================================================
 */

/* Implementation of ra_touch_cal_save (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_touch_cal_save(const ra_touch_cal_matrix_t* matrix, uint8_t* dst, size_t dst_size)
{
  if ((matrix == nullptr) || (dst == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (dst_size < (size_t)k_ra_touch_cal_blob_size) {
    return k_ra_err_invalid_size;
  }

  dst[(size_t)k_ra_touch_cal_off_magic + 0U]    = (uint8_t)k_ra_touch_cal_magic_b0;
  dst[(size_t)k_ra_touch_cal_off_magic + 1U]    = (uint8_t)k_ra_touch_cal_magic_b1;
  dst[(size_t)k_ra_touch_cal_off_magic + 2U]    = (uint8_t)k_ra_touch_cal_magic_b2;
  dst[(size_t)k_ra_touch_cal_off_magic + 3U]    = (uint8_t)k_ra_touch_cal_magic_b3;
  dst[(size_t)k_ra_touch_cal_off_version]       = (uint8_t)k_ra_touch_cal_storage_version;
  dst[(size_t)k_ra_touch_cal_off_reserved + 0U] = 0U;
  dst[(size_t)k_ra_touch_cal_off_reserved + 1U] = 0U;
  dst[(size_t)k_ra_touch_cal_off_reserved + 2U] = 0U;

  const float coeffs[6] = {
    matrix->a,
    matrix->b,
    matrix->c,
    matrix->d,
    matrix->e,
    matrix->f,
  };
  for (uint8_t i = 0U; i < 6U; i++) {
    const uint32_t bits = internal_float_to_u32(coeffs[i]);
    internal_pack_le32(&dst[(size_t)k_ra_touch_cal_off_coeffs + ((size_t)i * sizeof(uint32_t))],
                       bits);
  }

  const uint32_t crc = internal_crc32(dst, (size_t)k_ra_touch_cal_off_crc32);
  internal_pack_le32(&dst[(size_t)k_ra_touch_cal_off_crc32], crc);
  return k_ra_ok;
}

/* Implementation of ra_touch_cal_load (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_touch_cal_load(const uint8_t* src, size_t src_size, ra_touch_cal_matrix_t* out_matrix)
{
  if ((src == nullptr) || (out_matrix == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (src_size < (size_t)k_ra_touch_cal_blob_size) {
    return k_ra_err_invalid_size;
  }

  if ((src[(size_t)k_ra_touch_cal_off_magic + 0U] != (uint8_t)k_ra_touch_cal_magic_b0) ||
      (src[(size_t)k_ra_touch_cal_off_magic + 1U] != (uint8_t)k_ra_touch_cal_magic_b1) ||
      (src[(size_t)k_ra_touch_cal_off_magic + 2U] != (uint8_t)k_ra_touch_cal_magic_b2) ||
      (src[(size_t)k_ra_touch_cal_off_magic + 3U] != (uint8_t)k_ra_touch_cal_magic_b3)) {
    return k_ra_err_invalid_arg;
  }
  if (src[(size_t)k_ra_touch_cal_off_version] != (uint8_t)k_ra_touch_cal_storage_version) {
    return k_ra_err_invalid_arg;
  }
  if ((src[(size_t)k_ra_touch_cal_off_reserved + 0U] != 0U) ||
      (src[(size_t)k_ra_touch_cal_off_reserved + 1U] != 0U) ||
      (src[(size_t)k_ra_touch_cal_off_reserved + 2U] != 0U)) {
    return k_ra_err_invalid_arg;
  }

  const uint32_t want_crc = internal_unpack_le32(&src[(size_t)k_ra_touch_cal_off_crc32]);
  const uint32_t have_crc = internal_crc32(src, (size_t)k_ra_touch_cal_off_crc32);
  if (want_crc != have_crc) {
    return k_ra_err_crc_mismatch;
  }

  float coeffs[6] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  for (uint8_t i = 0U; i < 6U; i++) {
    const uint32_t bits = internal_unpack_le32(
      &src[(size_t)k_ra_touch_cal_off_coeffs + ((size_t)i * sizeof(uint32_t))]);
    coeffs[i] = internal_u32_to_float(bits);
  }
  out_matrix->a = coeffs[0];
  out_matrix->b = coeffs[1];
  out_matrix->c = coeffs[2];
  out_matrix->d = coeffs[3];
  out_matrix->e = coeffs[4];
  out_matrix->f = coeffs[5];
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-isolate-declaration,readability-identifier-naming,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result) */
