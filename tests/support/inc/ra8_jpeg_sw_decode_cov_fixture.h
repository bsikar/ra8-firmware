/**
 * @file ra8_jpeg_sw_decode_cov_fixture.h
 * @brief Fixed JPEG marker fragments and bounded assembly helper for decode coverage tests.
 *
 * @details
 * Owns the immutable hand-built SOF, DHT, and SOS fragments shared by
 * test_ra8_jpeg_sw_decode_cov.c. The header is intentionally private to that
 * one test translation unit; every object has internal linkage and no storage
 * or ownership escapes the executable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_buf_size_t
 * @brief Three scratch sizes around one MCU row, one of them not a power of two so an allocator rounding up is visible.
 */
typedef enum : uint8_t {
  k_jpeg_buf_large = 128, /**< The largest, over one MCU row.                      */
  k_jpeg_buf_small = 64,  /**< Smallest of three scratch sizes, under one MCU row. */
  k_jpeg_buf_mid =
    80, /**< A size that is not a power of two, so an allocator rounding up is visible. */
} jpeg_buf_size_t;

/**
 * @enum ra8_jpeg_dec_cov_const_t
 * @brief Buffer sizes shared across the coverage fixtures.
 */
typedef enum : uint16_t {
  k_cov_out_big   = 768U, /**< >= 8x8x3 and 5x5x3; passes the size guard. */
  k_cov_out_small = 10U,  /**< < 8x8x3; trips the size guard.             */
} ra8_jpeg_dec_cov_const_t;

/* Shared destination for successful/edge decodes and for the scan
 * error legs that must first clear the output-size guard. */
static uint8_t s_out[(uint32_t)k_cov_out_big];

/* ------------------------------------------------------------------ */
/* Reusable JPEG segment fragments (composed into full streams below) */
/* ------------------------------------------------------------------ */

/** @brief Start-of-image marker. */
static const uint8_t s_soi[] = {0xFFU, 0xD8U};

/** @brief End-of-image marker. */
static const uint8_t s_eoi[] = {0xFFU, 0xD9U};

/** @brief Baseline SOF0: 5x5, 1 grayscale component (edge-crop case). */
static const uint8_t s_sof0_5x5_gray[] = {
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x05U,
  0x00U,
  0x05U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
};

/** @brief Baseline SOF0: 8x8, 1 grayscale component (single full MCU). */
static const uint8_t s_sof0_8x8_gray[] = {
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x08U,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
};

/** @brief Baseline SOF0: 8x8, 3-component 4:4:4 (H1V1 x3). */
static const uint8_t s_sof0_444[] = {
  0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x08U, 0x00U, 0x08U, 0x03U,
  0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U,
};

/** @brief DHT: DC table 0 with a single 1-bit code -> symbol 0. */
static const uint8_t s_dht_dc[] = {
  0xFFU, 0xC4U, 0x00U, 0x14U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

/** @brief DHT: AC table 0 with a single 1-bit code -> symbol 0x00 (EOB). */
static const uint8_t s_dht_ac[] = {
  0xFFU, 0xC4U, 0x00U, 0x14U, 0x10U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

/** @brief DHT: AC table 0, single 1-bit code -> symbol 0x0F (size 15). */
static const uint8_t s_dht_ac_f0[] = {
  0xFFU, 0xC4U, 0x00U, 0x14U, 0x10U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0FU,
};

/** @brief DHT: AC table 0, two 1-bit codes -> 0xF0 (ZRL) and 0xF1. */
static const uint8_t s_dht_ac_zrl[] = {
  0xFFU, 0xC4U, 0x00U, 0x15U, 0x10U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xF0U, 0xF1U,
};

/** @brief DHT: DC table 0, single 4-bit code "0000" -> symbol 0x08 (t=8). */
static const uint8_t s_dht_dc_t8[] = {
  0xFFU, 0xC4U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U,
};

/** @brief SOS: 1 component, DC/AC selector 0 (grayscale). */
static const uint8_t s_sos_gray[] = {
  0xFFU,
  0xDAU,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x00U,
  0x00U,
  0x3FU,
  0x00U,
};

/** @brief SOS: 3 comp -- luma table 0, both chroma select table 1 (undef). */
static const uint8_t s_sos_444_cb_fail[] = {
  0xFFU,
  0xDAU,
  0x00U,
  0x0CU,
  0x03U,
  0x01U,
  0x00U,
  0x02U,
  0x11U,
  0x03U,
  0x11U,
  0x00U,
  0x3FU,
  0x00U,
};

/** @brief SOS: 3 comp -- luma+Cb table 0, Cr selects table 1 (undef). */
static const uint8_t s_sos_444_cr_fail[] = {
  0xFFU,
  0xDAU,
  0x00U,
  0x0CU,
  0x03U,
  0x01U,
  0x00U,
  0x02U,
  0x00U,
  0x03U,
  0x11U,
  0x00U,
  0x3FU,
  0x00U,
};

/**
 * @brief Append `n` bytes of `seg` into `dst` at offset `*off`.
 *
 * @details
 * Streaming byte-splicer used by the fixtures to compose a JPEG
 * stream from the shared marker fragments above without repeating
 * the 16-byte BITS lists in every test.  Copies `seg[0..n)` to
 * `dst[*off..*off+n)` and advances `*off`.  Pure host helper; no
 * hardware state is touched.
 *
 * @param[out]    dst Destination stream buffer (>= `*off + n` bytes).
 * @param[in]     cap Capacity of @p dst in bytes; the append is asserted to fit.
 * @param[in,out] off Running write cursor; advanced by `n` on return.
 * @param[in]     seg Source fragment (non-NULL, `n` readable bytes).
 * @param[in]     n   Fragment length in bytes.
 *
 *
 * @pre `dst`, `off` and `seg` are non-NULL.
 * @pre `*off + n` does not exceed the capacity of `dst`.
 * @post `dst[*off_old .. *off_old + n)` equals `seg[0 .. n)`.
 * @post `*off` increased by exactly `n`.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_cov_append(uint8_t* dst, uint32_t cap, uint32_t* off, const uint8_t* seg, uint32_t n)
{
  TEST_ASSERT_NOT_NULL(dst);
  TEST_ASSERT_NOT_NULL(seg);
  TEST_ASSERT((*off + n) <= cap);
  memcpy(&dst[*off], seg, n);
  *off += n;
}
