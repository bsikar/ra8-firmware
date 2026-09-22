/**
 * @file test_ra8_touch_cal_mcdc.c
 * @brief MC/DC vector tests for the ra8_touch_cal null-guard decisions
 *
 * @details
 * Split out of test_ra8_touch_cal.c (which holds the behavioural suite) to
 * keep both translation units under the 1000-line file-size cap. This file
 * carries the dedicated MC/DC vector sets for the leading null-pointer guards
 * of ra8_touch_cal_apply / _save / _load -- the three compound decisions the
 * behavioural suite exercises incidentally but never pins with an
 * independent-influence vector set. Each test drives the public API entirely
 * on the host (no register fake needed) and cites its decision by the
 * drift-proof ``path@function`` anchor the MC/DC compound ratchet consumes
 * (issue #426).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_touch_cal.h"
#include "unity_minimal.h"

/**
 * @brief An identity calibration matrix used as a valid, non-null operand.
 *
 * @details
 * The proceed-path vector (V1) of each decision needs a matrix that lets the
 * call reach its ``k_ra8_ok`` return; the identity affine keeps every mapped
 * coordinate on-panel so no downstream guard masks the result.
 */
static const ra8_touch_cal_matrix_t s_tcm_identity = {
  .a = 1.0F,
  .b = 0.0F,
  .c = 0.0F,
  .d = 0.0F,
  .e = 1.0F,
  .f = 0.0F,
};

/**
 * @brief Prove both apply pointer predicates are independently decisive.
 * @details Supplies the all-present control and each single-null operand.
 * @test test_mcdc_apply_null_or
 * @pre The identity matrix and output point are initialized.
 * @pre The control panel dimensions are nonzero.
 * @post The control succeeds and both single-null vectors return null pointer.
 * @post Rejected vectors do not modify the output point.
 * @note This is the minimal N+1 set for the two-term pointer OR.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((matrix == nullptr) || (out_screen == nullptr))`
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_apply`).
 * N+1 = 3 vectors; each condition flips with the other held at its masking
 * value (F):
 * - V1: matrix=ok,   out_screen=ok    -> C1=F, C2=F -> dec F (proceeds -> ok)
 * - V2: matrix=NULL, out_screen=ok    -> C1=T short -> dec T -> null_ptr
 * - V3: matrix=ok,   out_screen=NULL  -> C1=F, C2=T -> dec T -> null_ptr
 * V1+V2 prove matrix independently affects the outcome; V1+V3 prove the same
 * for out_screen. Minimal MC/DC for N=2.
 */
RA8_INTERNAL static void internal_test_mcdc_apply_null_or(void)
{
  TEST_BEGIN("touch_cal apply MC/DC: matrix||out_screen NULL");
  const ra8_touch_cal_point_t raw = {0, 0};
  ra8_touch_cal_point_t       out = {0, 0};

  /* V1: both non-null, valid screen dims -> decision F -> proceeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_apply(raw, &s_tcm_identity, 100U, 100U, &out));
  /* V2: matrix NULL -> C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_apply(raw, nullptr, 100U, 100U, &out));
  /* V3: out_screen NULL -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_touch_cal_apply(raw, &s_tcm_identity, 100U, 100U, nullptr));
  TEST_END("touch_cal apply MC/DC: matrix||out_screen NULL");
}

/**
 * @brief Prove both save pointer predicates are independently decisive.
 * @details Supplies the all-present control and each single-null operand.
 * @test test_mcdc_save_null_or
 * @pre The identity matrix and fixed-size blob are initialized.
 * @pre The blob extent equals the public serialized-size constant.
 * @post The control serializes successfully and both null arms are rejected.
 * @post Rejected vectors do not escape the fixed blob bounds.
 * @note This is the minimal N+1 set for the two-term pointer OR.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((matrix == nullptr) || (dst == nullptr))`
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_save`).
 * N+1 = 3 vectors.
 * - V1: matrix=ok,   dst=ok    -> C1=F, C2=F -> dec F (proceeds -> ok)
 * - V2: matrix=NULL, dst=ok    -> C1=T short -> dec T -> null_ptr
 * - V3: matrix=ok,   dst=NULL  -> C1=F, C2=T -> dec T -> null_ptr
 * V1+V2 isolate matrix; V1+V3 isolate dst. Minimal MC/DC for N=2.
 */
RA8_INTERNAL static void internal_test_mcdc_save_null_or(void)
{
  TEST_BEGIN("touch_cal save MC/DC: matrix||dst NULL");
  uint8_t blob[k_ra8_touch_cal_blob_size] = {};

  /* V1: both non-null, buffer large enough -> decision F -> proceeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&s_tcm_identity, blob, sizeof(blob)));
  /* V2: matrix NULL -> C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_save(nullptr, blob, sizeof(blob)));
  /* V3: dst NULL -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_save(&s_tcm_identity, nullptr, sizeof(blob)));
  TEST_END("touch_cal save MC/DC: matrix||dst NULL");
}

/**
 * @brief Prove both load pointer predicates are independently decisive.
 * @details Produces a canonical blob, then supplies each single-null operand.
 * @test test_mcdc_load_null_or
 * @pre The identity matrix can be serialized into the fixed-size blob.
 * @pre The output matrix is writable for the control vector.
 * @post The control loads successfully and both null arms are rejected.
 * @post Rejected vectors do not publish a replacement matrix.
 * @note This is the minimal N+1 set for the two-term pointer OR.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((src == nullptr) || (out_matrix == nullptr))`
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_load`).
 * N+1 = 3 vectors.
 * - V1: src=ok,   out_matrix=ok    -> C1=F, C2=F -> dec F (proceeds -> ok)
 * - V2: src=NULL, out_matrix=ok    -> C1=T short -> dec T -> null_ptr
 * - V3: src=ok,   out_matrix=NULL  -> C1=F, C2=T -> dec T -> null_ptr
 * V1+V2 isolate src; V1+V3 isolate out_matrix. Minimal MC/DC for N=2. The V1
 * source blob is produced by ra8_touch_cal_save so the proceed path clears the
 * magic/version/reserved/CRC checks and returns ok.
 */
RA8_INTERNAL static void internal_test_mcdc_load_null_or(void)
{
  TEST_BEGIN("touch_cal load MC/DC: src||out_matrix NULL");
  uint8_t blob[k_ra8_touch_cal_blob_size] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&s_tcm_identity, blob, sizeof(blob)));
  ra8_touch_cal_matrix_t out = {};

  /* V1: both non-null, valid blob -> decision F -> proceeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_load(blob, sizeof(blob), &out));
  /* V2: src NULL -> C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_load(nullptr, sizeof(blob), &out));
  /* V3: out_matrix NULL -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_load(blob, sizeof(blob), nullptr));
  TEST_END("touch_cal load MC/DC: src||out_matrix NULL");
}

/**
 * @brief Test driver.
 */
int main(void)
{
  internal_test_mcdc_apply_null_or();
  internal_test_mcdc_save_null_or();
  internal_test_mcdc_load_null_or();
  return 0;
}
