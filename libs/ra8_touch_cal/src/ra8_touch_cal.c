/**
 * @file ra8_touch_cal.c
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

#include "ra8_touch_cal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/* ===========================================================================
 * Internal constants
 * ===========================================================================
 */

/** @brief Row-major flat indices of a 3x3 matrix (a[r*3+c]). */
typedef enum : uint8_t {
  k_m3_00  = 0U, /**< M3 00.     */
  k_m3_01  = 1U, /**< M3 01.     */
  k_m3_02  = 2U, /**< M3 02.     */
  k_m3_10  = 3U, /**< M3 10.     */
  k_m3_11  = 4U, /**< M3 11.     */
  k_m3_12  = 5U, /**< M3 12.     */
  k_m3_20  = 6U, /**< M3 20.     */
  k_m3_21  = 7U, /**< M3 21.     */
  k_m3_22  = 8U, /**< M3 22.     */
  k_m3_len = 9U, /**< M3 length. */
} mat3_idx_t;

/** @brief Affine calibration coefficient indices (a..f). */
typedef enum : uint8_t {
  k_coeff_a = 0U, /**< Coeff a. */
  k_coeff_b = 1U, /**< Coeff b. */
  k_coeff_c = 2U, /**< Coeff c. */
  k_coeff_d = 3U, /**< Coeff d. */
  k_coeff_e = 4U, /**< Coeff e. */
  k_coeff_f = 5U, /**< Coeff f. */
} affine_coeff_idx_t;

/**
 * @enum internal_const_t
 * @brief Module-private numeric constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_internal_centre_div    = 2U,          /**< Halve to get panel centre.      */
  k_internal_byte_mask     = 0xFFU,       /**< 8-bit mask for byte extracts.   */
  k_internal_byte_shift_8  = 8U,          /**< Shift to byte 1.                */
  k_internal_byte_shift_16 = 16U,         /**< Shift to byte 2.                */
  k_internal_byte_shift_24 = 24U,         /**< Shift to byte 3.                */
  k_internal_crc32_init    = 0xFFFFFFFFU, /**< IEEE 802.3 CRC seed.            */
  k_internal_crc32_poly    = 0xEDB88320U, /**< IEEE 802.3 reversed polynomial. */
  k_internal_bits_per_byte = 8U,          /**< Bits in one byte.               */
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

/** @brief Round-to-nearest bias for float->int conversion. */
static const float s_round_bias = 0.5F;

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
RA8_INTERNAL
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * Bit-banged so the function works without depending on ra8_crc.
 *
 * @param[in] data  Input bytes.
 * @param[in] len   Number of bytes.
 * @return Final CRC32, post-XOR.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
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
RA8_INTERNAL
static void internal_solve3(const float a[k_m3_len], const float b[3], float x[3], bool* ok)
{
  const float det = (a[k_m3_00] * ((a[k_m3_11] * a[k_m3_22]) - (a[k_m3_12] * a[k_m3_21]))) -
                    (a[k_m3_01] * ((a[k_m3_10] * a[k_m3_22]) - (a[k_m3_12] * a[k_m3_20]))) +
                    (a[k_m3_02] * ((a[k_m3_10] * a[k_m3_21]) - (a[k_m3_11] * a[k_m3_20])));

  const float abs_det = (det < 0.0F) ? -det : det;
  if (abs_det < s_min_det) {
    *ok = false;
    return;
  }

  const float dx = (b[0] * ((a[k_m3_11] * a[k_m3_22]) - (a[k_m3_12] * a[k_m3_21]))) -
                   (a[k_m3_01] * ((b[1] * a[k_m3_22]) - (a[k_m3_12] * b[2]))) +
                   (a[k_m3_02] * ((b[1] * a[k_m3_21]) - (a[k_m3_11] * b[2])));

  const float dy = (a[k_m3_00] * ((b[1] * a[k_m3_22]) - (a[k_m3_12] * b[2]))) -
                   (b[0] * ((a[k_m3_10] * a[k_m3_22]) - (a[k_m3_12] * a[k_m3_20]))) +
                   (a[k_m3_02] * ((a[k_m3_10] * b[2]) - (b[1] * a[k_m3_20])));

  const float dz = (a[k_m3_00] * ((a[k_m3_11] * b[2]) - (b[1] * a[k_m3_21]))) -
                   (a[k_m3_01] * ((a[k_m3_10] * b[2]) - (b[1] * a[k_m3_20]))) +
                   (b[0] * ((a[k_m3_10] * a[k_m3_21]) - (a[k_m3_11] * a[k_m3_20])));

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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
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

/**
 * @struct internal_lsq_sums_t
 * @brief Accumulated least-squares sums driving the calibration solve.
 *
 * @details
 * Holds the 11 running sums needed to assemble the 3x3 normal-equation
 * matrix and both right-hand sides for the weighted-least-squares affine
 * fit (Fang & Chang 2007). Bundling them keeps the
 * ::ra8_touch_cal_compute body compact while preserving bit-exact math.
 */
typedef struct {
  float Sx;  /**< Sum of raw x.            */
  float Sy;  /**< Sum of raw y.            */
  float Sxx; /**< Sum of raw x^2.          */
  float Syy; /**< Sum of raw y^2.          */
  float Sxy; /**< Sum of raw x*y.          */
  float Su;  /**< Sum of screen x.         */
  float Sv;  /**< Sum of screen y.         */
  float Sxu; /**< Sum of raw x * screen x. */
  float Syu; /**< Sum of raw y * screen x. */
  float Sxv; /**< Sum of raw x * screen y. */
  float Syv; /**< Sum of raw y * screen y. */
} internal_lsq_sums_t;

/**
 * @brief Accumulate the 11 least-squares sums over ``n`` sample pairs.
 *
 * @details
 * Pure-data helper extracted from ::ra8_touch_cal_compute to keep the
 * top-level function within the NASA P10 Rule 4 cap. The arithmetic is
 * bit-identical to the inlined original.
 *
 * @param[in]  raw    Raw touch samples (length ``n``).
 * @param[in]  screen Screen targets (length ``n``).
 * @param[in]  n      Sample count.
 * @param[out] s      Accumulated sums (zero-initialized by the helper).
 *
 * @pre All pointers are non-NULL and ``n`` >= 1.
 * @pre Caller has already validated argument ranges.
 * @post ``s`` holds the 11 sums over ``[0, n)``.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_accumulate_sums(const ra8_touch_cal_point_t* raw,
                                     const ra8_touch_cal_point_t* screen,
                                     uint8_t                      n,
                                     internal_lsq_sums_t*         s)
{
  s->Sx  = 0.0F;
  s->Sy  = 0.0F;
  s->Sxx = 0.0F;
  s->Syy = 0.0F;
  s->Sxy = 0.0F;
  s->Su  = 0.0F;
  s->Sv  = 0.0F;
  s->Sxu = 0.0F;
  s->Syu = 0.0F;
  s->Sxv = 0.0F;
  s->Syv = 0.0F;
  for (uint8_t i = 0U; i < n; i++) {
    const float xi = (float)raw[i].x;
    const float yi = (float)raw[i].y;
    const float ui = (float)screen[i].x;
    const float vi = (float)screen[i].y;
    s->Sx += xi;
    s->Sy += yi;
    s->Sxx += xi * xi;
    s->Syy += yi * yi;
    s->Sxy += xi * yi;
    s->Su += ui;
    s->Sv += vi;
    s->Sxu += xi * ui;
    s->Syu += yi * ui;
    s->Sxv += xi * vi;
    s->Syv += yi * vi;
  }
}

ra8_err_t ra8_touch_cal_compute(const ra8_touch_cal_point_t* raw,
                                const ra8_touch_cal_point_t* screen,
                                uint8_t                      n,
                                ra8_touch_cal_matrix_t*      out_mtx)
{
  if ((raw == nullptr) || (screen == nullptr) || (out_mtx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((n < (uint8_t)k_ra8_touch_cal_min_targets) || (n > (uint8_t)k_ra8_touch_cal_max_targets)) {
    return k_ra8_err_invalid_arg;
  }

  internal_lsq_sums_t s;
  internal_accumulate_sums(raw, screen, n, &s);

  const float fn          = (float)n;
  const float norm_mat[9] = {
    s.Sxx,
    s.Sxy,
    s.Sx,
    s.Sxy,
    s.Syy,
    s.Sy,
    s.Sx,
    s.Sy,
    fn,
  };
  const float rhs_u[3] = {s.Sxu, s.Syu, s.Su};
  const float rhs_v[3] = {s.Sxv, s.Syv, s.Sv};

  float sol_u[3] = {0.0F, 0.0F, 0.0F};
  float sol_v[3] = {0.0F, 0.0F, 0.0F};
  bool  ok_u     = false;
  bool  ok_v     = false;
  internal_solve3(norm_mat, rhs_u, sol_u, &ok_u);
  internal_solve3(norm_mat, rhs_v, sol_v, &ok_v);
  // mcdc-deactivated: ra8_touch_cal_compute internal_solve3 success gate; A is the same 3x3 calibration matrix for both Bu and Bv solves, so internal_solve3 either succeeds for both right-hand sides (det(A) != 0) or fails for both (det(A) == 0) -- ok_u and ok_v are co-determined by the matrix conditioning.
  if (!ok_u || !ok_v) {
    return k_ra8_err_invalid_arg;
  }

  out_mtx->a = sol_u[0];
  out_mtx->b = sol_u[1];
  out_mtx->c = sol_u[2];
  out_mtx->d = sol_v[0];
  out_mtx->e = sol_v[1];
  out_mtx->f = sol_v[2];
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API -- run (drives the on-screen sequence)
 * ===========================================================================
 */

ra8_err_t ra8_touch_cal_run(const ra8_touch_cal_run_cfg_t* cfg, ra8_touch_cal_matrix_t* out_matrix)
{
  if ((cfg == nullptr) || (out_matrix == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->draw_target == nullptr) || (cfg->read_raw == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->screen_width == 0U) || (cfg->screen_height == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t margin_total = (uint32_t)cfg->inset_px * 2U;
  if ((margin_total >= (uint32_t)cfg->screen_width) ||
      (margin_total >= (uint32_t)cfg->screen_height)) {
    return k_ra8_err_invalid_arg;
  }

  const int32_t w   = (int32_t)cfg->screen_width;
  const int32_t h   = (int32_t)cfg->screen_height;
  const int32_t ins = (int32_t)cfg->inset_px;

  ra8_touch_cal_point_t targets[k_ra8_touch_cal_n_targets] = {
    {ins, ins},                                                               /* top-left     */
    {w - 1 - ins, ins},                                                       /* top-right    */
    {w - 1 - ins, h - 1 - ins},                                               /* bottom-right */
    {ins, h - 1 - ins},                                                       /* bottom-left  */
    {w / (int32_t)k_internal_centre_div, h / (int32_t)k_internal_centre_div}, /* centre       */
  };

  ra8_touch_cal_point_t samples[k_ra8_touch_cal_n_targets] = {
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
  };

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_touch_cal_n_targets; i++) {
    const ra8_err_t er_draw = cfg->draw_target(cfg->draw_ctx, targets[i]);
    if (er_draw != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    const ra8_err_t er_read = cfg->read_raw(cfg->read_ctx, &samples[i]);
    if (er_read != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
  }

  return ra8_touch_cal_compute(samples, targets, (uint8_t)k_ra8_touch_cal_n_targets, out_matrix);
}

/* ===========================================================================
 * Public API -- apply
 * ===========================================================================
 */

ra8_err_t ra8_touch_cal_apply(ra8_touch_cal_point_t         raw,
                              const ra8_touch_cal_matrix_t* matrix,
                              uint16_t                      screen_width,
                              uint16_t                      screen_height,
                              ra8_touch_cal_point_t*        out_screen)
{
  if ((matrix == nullptr) || (out_screen == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((screen_width == 0U) || (screen_height == 0U)) {
    return k_ra8_err_invalid_arg;
  }

  const float xf = (float)raw.x;
  const float yf = (float)raw.y;
  const float u  = (matrix->a * xf) + (matrix->b * yf) + matrix->c;
  const float v  = (matrix->d * xf) + (matrix->e * yf) + matrix->f;

  /* Round-to-nearest before clipping. */
  const int32_t ui = (u >= 0.0F) ? (int32_t)(u + s_round_bias) : (int32_t)(u - s_round_bias);
  const int32_t vi = (v >= 0.0F) ? (int32_t)(v + s_round_bias) : (int32_t)(v - s_round_bias);

  out_screen->x = internal_clip32(ui, 0, (int32_t)screen_width - 1);
  out_screen->y = internal_clip32(vi, 0, (int32_t)screen_height - 1);
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API -- save / load
 * ===========================================================================
 */

ra8_err_t ra8_touch_cal_save(const ra8_touch_cal_matrix_t* matrix, uint8_t* dst, size_t dst_size)
{
  if ((matrix == nullptr) || (dst == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (dst_size < (size_t)k_ra8_touch_cal_blob_size) {
    return k_ra8_err_invalid_size;
  }

  dst[(size_t)k_ra8_touch_cal_off_magic + 0U]    = (uint8_t)k_ra8_touch_cal_magic_b0;
  dst[(size_t)k_ra8_touch_cal_off_magic + 1U]    = (uint8_t)k_ra8_touch_cal_magic_b1;
  dst[(size_t)k_ra8_touch_cal_off_magic + 2U]    = (uint8_t)k_ra8_touch_cal_magic_b2;
  dst[(size_t)k_ra8_touch_cal_off_magic + 3U]    = (uint8_t)k_ra8_touch_cal_magic_b3;
  dst[(size_t)k_ra8_touch_cal_off_version]       = (uint8_t)k_ra8_touch_cal_storage_version;
  dst[(size_t)k_ra8_touch_cal_off_reserved + 0U] = 0U;
  dst[(size_t)k_ra8_touch_cal_off_reserved + 1U] = 0U;
  dst[(size_t)k_ra8_touch_cal_off_reserved + 2U] = 0U;

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
    internal_pack_le32(&dst[(size_t)k_ra8_touch_cal_off_coeffs + ((size_t)i * sizeof(uint32_t))],
                       bits);
  }

  const uint32_t crc = internal_crc32(dst, (size_t)k_ra8_touch_cal_off_crc32);
  internal_pack_le32(&dst[(size_t)k_ra8_touch_cal_off_crc32], crc);
  return k_ra8_ok;
}

ra8_err_t
ra8_touch_cal_load(const uint8_t* src, size_t src_size, ra8_touch_cal_matrix_t* out_matrix)
{
  if ((src == nullptr) || (out_matrix == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (src_size < (size_t)k_ra8_touch_cal_blob_size) {
    return k_ra8_err_invalid_size;
  }

  if ((src[(size_t)k_ra8_touch_cal_off_magic + 0U] != (uint8_t)k_ra8_touch_cal_magic_b0) ||
      (src[(size_t)k_ra8_touch_cal_off_magic + 1U] != (uint8_t)k_ra8_touch_cal_magic_b1) ||
      (src[(size_t)k_ra8_touch_cal_off_magic + 2U] != (uint8_t)k_ra8_touch_cal_magic_b2) ||
      (src[(size_t)k_ra8_touch_cal_off_magic + 3U] != (uint8_t)k_ra8_touch_cal_magic_b3)) {
    return k_ra8_err_invalid_arg;
  }
  if (src[(size_t)k_ra8_touch_cal_off_version] != (uint8_t)k_ra8_touch_cal_storage_version) {
    return k_ra8_err_invalid_arg;
  }
  if ((src[(size_t)k_ra8_touch_cal_off_reserved + 0U] != 0U) ||
      (src[(size_t)k_ra8_touch_cal_off_reserved + 1U] != 0U) ||
      (src[(size_t)k_ra8_touch_cal_off_reserved + 2U] != 0U)) {
    return k_ra8_err_invalid_arg;
  }

  const uint32_t want_crc = internal_unpack_le32(&src[(size_t)k_ra8_touch_cal_off_crc32]);
  const uint32_t have_crc = internal_crc32(src, (size_t)k_ra8_touch_cal_off_crc32);
  if (want_crc != have_crc) {
    return k_ra8_err_crc_mismatch;
  }

  float coeffs[6] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  for (uint8_t i = 0U; i < 6U; i++) {
    const uint32_t bits = internal_unpack_le32(
      &src[(size_t)k_ra8_touch_cal_off_coeffs + ((size_t)i * sizeof(uint32_t))]);
    coeffs[i] = internal_u32_to_float(bits);
  }
  out_matrix->a = coeffs[k_coeff_a];
  out_matrix->b = coeffs[k_coeff_b];
  out_matrix->c = coeffs[k_coeff_c];
  out_matrix->d = coeffs[k_coeff_d];
  out_matrix->e = coeffs[k_coeff_e];
  out_matrix->f = coeffs[k_coeff_f];
  return k_ra8_ok;
}
