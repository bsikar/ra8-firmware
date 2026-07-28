/**
 * @file ra8_touch_cal.h
 * @brief Resistive/capacitive touch-screen calibration utility
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * ``ra8_touch_cal`` computes and applies the 2-D affine transform that
 * maps raw touch-controller coordinates (the values reported by
 * ``ra8_touch_read`` straight from the GT911 / FT5x06 / Atmel maXTouch
 * silicon) onto on-screen pixel coordinates of the panel attached to
 * ``ra8_lcd``.
 *
 * The library is hardware-agnostic: it does not include any LCD or
 * touch-driver header. Instead, the caller supplies two function-pointer
 * shims (Dependency Inversion, NASA Power-of-10 deviation #9 documented
 * in CLAUDE.md):
 *
 *   - ``draw_target_fn`` -- paint a cross-hair at a screen coordinate.
 *   - ``read_raw_fn``    -- block until one stable raw touch sample is
 *                           captured, returning the controller-space
 *                           ``(x, y)`` pair.
 *
 * Algorithm:
 *
 *   The implementation follows the weighted-least-squares affine fit
 *   from Fang & Chang, "Calibration of Touch Screens for Use with
 *   Embedded Systems", which is itself a 2-D restatement of the
 *   Press / Teukolsky least-squares normal equations. Five target
 *   points (the four panel corners inset by an enum-defined margin
 *   plus the panel centre) are sufficient to produce a 3x3 normal-
 *   equation system that is solved per axis via Cramer's rule:
 *
 *       [ Sxx Sxy Sx ] [ a ]   [ Sxu ]
 *       [ Sxy Syy Sy ] [ b ] = [ Syu ]
 *       [ Sx  Sy  N  ] [ c ]   [ Su  ]
 *
 *   where ``(x, y)`` are raw touch samples, ``u`` is the screen X
 *   coordinate of the corresponding target, and ``N`` is the number of
 *   target points (here ``::k_ra8_touch_cal_n_targets`` == 5). The same
 *   3x3 system with ``v`` substituted for ``u`` solves the second row
 *   of the matrix.
 *
 *   Three points (``::k_ra8_touch_cal_n_targets`` reduced) would also
 *   work via a direct 3x3 inversion (Fang & Chang section 2), but the
 *   weighted 5-point fit averages out controller jitter and is the
 *   default. Three-point fitting can be exercised by passing exactly
 *   three sample pairs to ``ra8_touch_cal_compute()``.
 *
 * @par NASA Power-of-10 deviation
 *   The matrix coefficients are stored as IEEE-754 ``float`` and the
 *   normal-equation solve runs in float. The Cortex-M85 has a hardware
 *   FPU; using fixed-point Q15.16 here would eat two extra
 *   multiplications per pixel mapping for a worse numerical conditioning
 *   on the normal equations. The deviation is local to this module:
 *   the public ``ra8_touch_cal_apply`` API still accepts and returns
 *   integer pixel coordinates.
 *
 * Storage layout for ``ra8_touch_cal_save`` / ``ra8_touch_cal_load``:
 *
 * @verbatim
 *   offset  size  field
 *   ------  ----  ------------------------------------------------
 *   0       4     magic   = 'T','C','A','L' (ASCII)
 *   4       1     version = ::k_ra8_touch_cal_storage_version
 *   5       3     reserved (zeroed)
 *   8       24    coeffs  = a..f, six little-endian IEEE-754 floats
 *   32      4     crc32   = IEEE 802.3 polynomial over bytes 0..31
 *   ------  ----  ------------------------------------------------
 *   36 total bytes (::k_ra8_touch_cal_blob_size)
 * @endverbatim
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra8_touch_cal_limits_t
 * @brief Static-allocation caps and protocol constants.
 */
typedef enum : uint8_t {
  k_ra8_touch_cal_n_targets       = 5U,  /**< 4 corners + centre.            */
  k_ra8_touch_cal_min_targets     = 3U,  /**< Floor for a unique solve.      */
  k_ra8_touch_cal_max_targets     = 5U,  /**< Ceiling for the fit.           */
  k_ra8_touch_cal_blob_size       = 36U, /**< Bytes in ::ra8_touch_cal_save. */
  k_ra8_touch_cal_storage_version = 1U,  /**< On-disk format version.        */
} ra8_touch_cal_limits_t;

/**
 * @enum ra8_touch_cal_layout_t
 * @brief Byte-offsets inside the serialised storage blob.
 */
typedef enum : uint8_t {
  k_ra8_touch_cal_off_magic    = 0U,  /**< Offset of 'TCAL' magic.            */
  k_ra8_touch_cal_off_version  = 4U,  /**< Offset of version byte.            */
  k_ra8_touch_cal_off_reserved = 5U,  /**< Offset of 3 reserved zero bytes.   */
  k_ra8_touch_cal_off_coeffs   = 8U,  /**< Offset of the 6-float coeff array. */
  k_ra8_touch_cal_off_crc32    = 32U, /**< Offset of the CRC32 trailer.       */
} ra8_touch_cal_layout_t;

/**
 * @enum ra8_touch_cal_magic_t
 * @brief Bytes of the storage magic 'TCAL'.
 */
typedef enum : uint8_t {
  k_ra8_touch_cal_magic_b0 = 0x54U, /**< 'T' */
  k_ra8_touch_cal_magic_b1 = 0x43U, /**< 'C' */
  k_ra8_touch_cal_magic_b2 = 0x41U, /**< 'A' */
  k_ra8_touch_cal_magic_b3 = 0x4CU, /**< 'L' */
} ra8_touch_cal_magic_t;

/* ===========================================================================
 * Public types
 * ===========================================================================
 */

/**
 * @struct ra8_touch_cal_point_t
 * @brief One ``(x, y)`` integer coordinate pair.
 *
 * @details
 * Used both for screen-space target locations (passed to
 * ``draw_target_fn``) and for the raw controller-space samples returned
 * by ``read_raw_fn``. Components are ``int32_t`` so transforms that
 * temporarily produce negative intermediate values do not wrap. Final
 * mapped pixel coordinates are still clipped into the panel rectangle
 * by ``ra8_touch_cal_apply``.
 *
 * @invariant Components fit a 16-bit panel; the wider type is for
 *            arithmetic head-room only.
 */
typedef struct {
  int32_t x; /**< Horizontal coordinate. */
  int32_t y; /**< Vertical coordinate.   */
} ra8_touch_cal_point_t;

/**
 * @struct ra8_touch_cal_matrix_t
 * @brief 2-D affine transform ``screen = [a b; d e] * raw + [c; f]``.
 *
 * @details
 * The six floats are the row-major coefficients of the affine map. The
 * mapping is:
 *
 *   ``u = a*x + b*y + c``
 *   ``v = d*x + e*y + f``
 *
 * where ``(x, y)`` is a raw controller sample and ``(u, v)`` is the
 * resulting screen pixel.
 *
 * @invariant ``ra8_touch_cal_compute`` populates all six members or
 *            returns an error and leaves the struct untouched.
 *
 * @see ra8_touch_cal_compute
 * @see ra8_touch_cal_apply
 */
typedef struct {
  float a; /**< Row 0, column 0: raw-X gain on screen-X. */
  float b; /**< Row 0, column 1: raw-Y gain on screen-X. */
  float c; /**< Row 0 offset: screen-X bias.             */
  float d; /**< Row 1, column 0: raw-X gain on screen-Y. */
  float e; /**< Row 1, column 1: raw-Y gain on screen-Y. */
  float f; /**< Row 1 offset: screen-Y bias.             */
} ra8_touch_cal_matrix_t;

/**
 * @typedef ra8_touch_cal_draw_target_fn_t
 * @brief LCD shim: paint a cross-hair at the given pixel.
 *
 * @param[in] ctx     Caller-supplied context (forwarded verbatim).
 * @param[in] target  Centre of the cross-hair, in screen pixels.
 *
 * @return ``ra8_err_t`` -- shim implementation defined; ``k_ra8_ok`` on
 *         success.
 */
typedef ra8_err_t (*ra8_touch_cal_draw_target_fn_t)(void* ctx, ra8_touch_cal_point_t target);

/**
 * @typedef ra8_touch_cal_read_raw_fn_t
 * @brief Touch shim: block until one raw touch sample is captured.
 *
 * @param[in]  ctx       Caller-supplied context.
 * @param[out] out_raw   Destination for the raw controller coordinates.
 *
 * @return ``ra8_err_t`` -- shim implementation defined; ``k_ra8_ok`` on
 *         success.
 */
typedef ra8_err_t (*ra8_touch_cal_read_raw_fn_t)(void* ctx, ra8_touch_cal_point_t* out_raw);

/**
 * @struct ra8_touch_cal_run_cfg_t
 * @brief Configuration for ``ra8_touch_cal_run``.
 *
 * @details
 * The library does not include ra8_lcd.h or ra8_touch.h; the caller
 * provides function-pointer shims, satisfying SOLID-D (Dependency
 * Inversion) and keeping ``ra8_touch_cal`` host-testable without the
 * register fake.
 */
typedef struct {
  uint16_t                       screen_width;  /**< Panel width, pixels.  */
  uint16_t                       screen_height; /**< Panel height, pixels. */
  uint16_t                       inset_px;      /**< Margin from panel edge
                                                     to corner targets.      */
  ra8_touch_cal_draw_target_fn_t draw_target;   /**< Cross-hair painter.   */
  void*                          draw_ctx;      /**< Forwarded to painter. */
  ra8_touch_cal_read_raw_fn_t    read_raw;      /**< Raw sample reader.    */
  void*                          read_ctx;      /**< Forwarded to reader.  */
} ra8_touch_cal_run_cfg_t;

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

/**
 * @brief Compute an affine transform from N target/sample pairs.
 *
 * @details
 * Pure-math entry point. Solves the 3x3 normal-equation system per
 * axis using Cramer's rule. The function does not touch any hardware
 * and is the workhorse exercised by the unit tests.
 *
 * Algorithm (Fang & Chang, eqn. 4--7):
 *   1. Accumulate sums ``Sx``, ``Sy``, ``Sxx``, ``Syy``, ``Sxy``,
 *      ``Sxu``, ``Syu``, ``Su``, ``Sxv``, ``Syv``, ``Sv``.
 *   2. Compute ``det`` of the 3x3 system. Reject as
 *      ``k_ra8_err_invalid_arg`` if ``|det|`` is below
 *      ``::k_ra8_touch_cal_min_det`` (panel-collinear targets).
 *   3. Solve for ``(a, b, c)`` and ``(d, e, f)`` via Cramer's rule.
 *
 * @param[in]  raw      Array of ``n`` raw controller samples.
 * @param[in]  screen   Array of ``n`` corresponding screen targets.
 * @param[in]  n        Number of pairs (3..5).
 * @param[out] out_mtx  Destination matrix.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Matrix populated.
 * @retval k_ra8_err_null_ptr     Any pointer NULL.
 * @retval k_ra8_err_invalid_arg  ``n`` out of range or system is singular.
 *
 * @pre ``raw``, ``screen``, ``out_mtx`` non-NULL.
 * @pre ``n`` in [::k_ra8_touch_cal_min_targets,
 *      ::k_ra8_touch_cal_max_targets].
 * @post On success ``out_mtx`` holds the affine transform.
 * @post On error ``out_mtx`` is unmodified.
 *
 * @note Pure function -- no hardware access, safe from any context.
 *
 * @par Example:
 * @code
 * ra8_touch_cal_point_t raw[5] = { ... };
 * ra8_touch_cal_point_t scr[5] = { ... };
 * ra8_touch_cal_matrix_t m = {};
 * if (ra8_touch_cal_compute(raw, scr, 5, &m) == k_ra8_ok) { ... }
 * @endcode
 *
 * @see ra8_touch_cal_run
 * @see ra8_touch_cal_apply
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_cal_compute(const ra8_touch_cal_point_t* raw,
                                              const ra8_touch_cal_point_t* screen,
                                              uint8_t                      n,
                                              ra8_touch_cal_matrix_t*      out_mtx);

/**
 * @brief Drive the on-screen calibration sequence and produce a matrix.
 *
 * @details
 * For each of the five built-in targets (4 corners inset by
 * ``cfg->inset_px`` plus the panel centre), this function:
 *   1. Calls ``cfg->draw_target`` to paint the cross-hair.
 *   2. Calls ``cfg->read_raw`` to capture the user's touch.
 *   3. Stores the sample.
 *
 * Once all five samples are captured, it invokes
 * ``ra8_touch_cal_compute`` and copies the result into ``out_matrix``.
 *
 * @param[in]  cfg         Configuration (non-NULL).
 * @param[out] out_matrix  Destination matrix (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Calibration matrix produced.
 * @retval k_ra8_err_null_ptr     Any pointer or shim NULL.
 * @retval k_ra8_err_invalid_arg  Panel size or inset implausible, or
 *                               solve failed.
 * @retval k_ra8_err_hw_error     A draw or read shim returned non-OK.
 *
 * @pre ``cfg->draw_target``, ``cfg->read_raw`` non-NULL.
 * @pre ``cfg->inset_px * 2 < min(width, height)``.
 * @post On success ``*out_matrix`` is fully written.
 *
 * @note Blocks for the duration of all five touches. Caller must ensure
 *       any concurrent UI is paused.
 *
 * @see ra8_touch_cal_compute
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_cal_run(const ra8_touch_cal_run_cfg_t* cfg,
                                          ra8_touch_cal_matrix_t*        out_matrix);

/**
 * @brief Apply a calibration matrix to a single raw touch sample.
 *
 * @details
 * Computes ``(u, v) = M * (x, y) + t`` and clips to
 * ``[0, screen_width-1] x [0, screen_height-1]``. The clip is necessary
 * because least-squares fits can map slightly off-panel raw samples to
 * negative or beyond-panel pixel coordinates.
 *
 * @param[in]  raw            Raw controller sample.
 * @param[in]  matrix         Pre-computed affine transform.
 * @param[in]  screen_width   Panel width in pixels (>0).
 * @param[in]  screen_height  Panel height in pixels (>0).
 * @param[out] out_screen     Destination screen pixel.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Sample mapped.
 * @retval k_ra8_err_null_ptr     ``matrix`` or ``out_screen`` NULL.
 * @retval k_ra8_err_invalid_arg  Panel size is zero.
 *
 * @pre ``matrix``, ``out_screen`` non-NULL.
 * @post ``out_screen->x`` in [0, screen_width-1].
 * @post ``out_screen->y`` in [0, screen_height-1].
 *
 * @note Pure function. Constant-time, branch-free except for the clip.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_touch_cal_apply(ra8_touch_cal_point_t         raw,
                                            const ra8_touch_cal_matrix_t* matrix,
                                            uint16_t                      screen_width,
                                            uint16_t                      screen_height,
                                            ra8_touch_cal_point_t*        out_screen);

/**
 * @brief Serialise a calibration matrix to a fixed-size byte blob.
 *
 * @details
 * Writes ``::k_ra8_touch_cal_blob_size`` bytes laid out as documented in
 * the file header. The byte order of the IEEE-754 ``float`` words is
 * little-endian to match the Cortex-M85 native order. The blob is
 * intended for ``ra8_flash`` page storage but the function does not
 * touch flash itself -- the caller is responsible for the actual write.
 *
 * @param[in]  matrix      Matrix to serialise (non-NULL).
 * @param[out] dst         Destination buffer (non-NULL).
 * @param[in]  dst_size    Capacity of ``dst`` in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Blob written.
 * @retval k_ra8_err_null_ptr     Any pointer NULL.
 * @retval k_ra8_err_invalid_size ``dst_size`` <
 *                               ::k_ra8_touch_cal_blob_size.
 *
 * @pre ``dst_size`` >= ::k_ra8_touch_cal_blob_size.
 * @post On success the first ::k_ra8_touch_cal_blob_size bytes of
 *       ``dst`` form a valid blob.
 *
 * @note Round-trip stable with ::ra8_touch_cal_load on any platform that
 *       uses IEEE-754 binary32 floats (i.e. all Cortex-M85 builds and
 *       all glibc / musl hosts).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_touch_cal_save(const ra8_touch_cal_matrix_t* matrix, uint8_t* dst, size_t dst_size);

/**
 * @brief Deserialise a calibration matrix from a byte blob.
 *
 * @param[in]  src        Source buffer (non-NULL, >= blob size).
 * @param[in]  src_size   Bytes available in ``src``.
 * @param[out] out_matrix Destination matrix.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Blob accepted, matrix populated.
 * @retval k_ra8_err_null_ptr      Any pointer NULL.
 * @retval k_ra8_err_invalid_size  ``src_size`` is too small.
 * @retval k_ra8_err_invalid_arg   Magic, version, or reserved bytes wrong.
 * @retval k_ra8_err_crc_mismatch  CRC trailer does not match payload.
 *
 * @pre ``src``, ``out_matrix`` non-NULL.
 * @pre ``src_size`` >= ::k_ra8_touch_cal_blob_size.
 * @post On error ``out_matrix`` is unmodified.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_touch_cal_load(const uint8_t* src, size_t src_size, ra8_touch_cal_matrix_t* out_matrix);

#ifdef __cplusplus
}
#endif
