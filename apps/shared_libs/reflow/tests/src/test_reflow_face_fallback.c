/**
 * @file test_reflow_face_fallback.c
 * @brief Coverage-based fallback-face vectors for reflow_render.c (#687).
 *
 * @details
 * Drives ::priv_reflow_render_pick_face, the resolver the render pass runs
 * after CSS has chosen a run's face, against a table-backed coverage probe so
 * no font file is needed and every branch is reachable:
 *
 *  - the run's own face wins whenever it covers the code point, and the probe
 *    is asked exactly once;
 *  - the engine's bound default face (index 0) is the first fallback;
 *  - a registered face is taken in index order when neither covers it;
 *  - a code point no face covers returns the run's own face, so the
 *    missing-glyph box is drawn at the surrounding text's metrics;
 *  - a blank code point is never hunted for (the probe is not asked at all);
 *  - the guard arms: no probe, a single face, and an out-of-range index.
 *
 * The probe records what it was asked, so "asked once" and "never asked
 * twice about the run's own face" are asserted rather than assumed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_test_output.h"
#include "reflow_internal.h"
#include "unity_minimal.h"

/**
 * @enum face_fb_consts_t
 * @brief Fixture sizes and code points for the fallback-face vectors.
 */
typedef enum : int32_t {
  k_ff_faces        = 4,      /**< Faces in the fixture set (0 = default). */
  k_ff_face_default = 0,      /**< The engine's bound default face.        */
  k_ff_face_body    = 1,      /**< A registered @font-face body face.      */
  k_ff_face_greek   = 2,      /**< A registered face carrying Greek.       */
  k_ff_face_last    = 3,      /**< The last registered face.               */
  k_ff_cp_letter_a  = 0x41,   /**< LATIN CAPITAL LETTER A: every face.     */
  k_ff_cp_eacute    = 0xE9,   /**< e-acute: the default face only.         */
  k_ff_cp_alpha     = 0x391,  /**< GREEK CAPITAL ALPHA: face 2 only.       */
  k_ff_cp_omega     = 0x3A9,  /**< GREEK CAPITAL OMEGA: faces 2 and 3.     */
  k_ff_cp_cjk       = 0x4E00, /**< CJK IDEOGRAPH-4E00: no face at all.     */
  k_ff_cp_space     = 0x20,   /**< SPACE: blank, never hunted for.         */
  k_ff_cp_zwsp      = 0x200B, /**< ZERO WIDTH SPACE: blank.                */
} face_fb_consts_t;

/**
 * @struct face_fb_probe_t
 * @brief Table-backed coverage probe plus a per-face ask counter.
 */
typedef struct {
  uint8_t asked[k_ff_faces]; /**< Times each face was probed. */
  uint8_t asks;              /**< Total probe calls.          */
} face_fb_probe_t;

static face_fb_probe_t s_probe;

/**
 * @brief Answer coverage for the fixture face set and record the ask.
 *
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ctx      Probe record (::face_fb_probe_t).
 * @param[in] face_idx Face being probed.
 * @param[in] cp       Code point being probed.
 * @return Boolean coverage answer for the fixture.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre @p ctx addresses the fixture probe record.
 * @pre @p face_idx is within the fixture face set.
 * @post The ask is recorded in the probe record.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_probe(const void* ctx, uint8_t face_idx, int32_t cp)
{
  face_fb_probe_t* rec = (face_fb_probe_t*)ctx;
  if ((rec != nullptr) && (face_idx < (uint8_t)k_ff_faces)) {
    rec->asked[face_idx]++;
    rec->asks++;
  }
  switch (cp) {
    case (int32_t)k_ff_cp_letter_a:
      return true; /* Latin is everywhere. */
    case (int32_t)k_ff_cp_eacute:
      return face_idx == (uint8_t)k_ff_face_default;
    case (int32_t)k_ff_cp_alpha:
      return face_idx == (uint8_t)k_ff_face_greek;
    case (int32_t)k_ff_cp_omega:
      return (face_idx == (uint8_t)k_ff_face_greek) || (face_idx == (uint8_t)k_ff_face_last);
    default:
      return false; /* Including the CJK ideograph: nothing carries it. */
  }
}

/**
 * @brief Reset the probe record before a vector.
 *
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post The probe record reads zero asks.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_probe_reset(void)
{
  for (uint8_t k = 0U; k < (uint8_t)k_ff_faces; ++k) {
    s_probe.asked[k] = 0U;
  }
  s_probe.asks = 0U;
}

/**
 * @test internal_test_primary_wins
 *
 * @brief A face that covers the code point keeps the run, asked once.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_primary_wins(void)
{
  TEST_BEGIN("reflow face fallback: the run's own face wins");
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_body,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_letter_a));
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asks);
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asked[k_ff_face_body]);

  /* The default face is a legitimate primary and takes the same short path. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_default,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_default,
                                              (int32_t)k_ff_cp_eacute));
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asks);
  TEST_END("reflow face fallback: the run's own face wins");
}

/**
 * @test internal_test_fallback_order
 *
 * @brief Default face first, then the registered faces in index order.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fallback_order(void)
{
  TEST_BEGIN("reflow face fallback: default face, then registered order");
  /* The body face lacks e-acute; the default face carries it. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_default,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_eacute));
  TEST_ASSERT_EQ(2U, (uint32_t)s_probe.asks);
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asked[k_ff_face_body]);
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asked[k_ff_face_default]);

  /* Neither the run's face nor the default carries Greek: face 2 does. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_greek,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_alpha));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asked[k_ff_face_last]);

  /* Two faces carry omega; the lower index is taken. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_greek,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_omega));

  /* The run's own face is asked exactly once even when the scan walks past
   * its index: the k != primary arm of the scan decision. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_last,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_greek,
                                              (int32_t)k_ff_cp_omega));
  TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asked[k_ff_face_greek]);
  TEST_END("reflow face fallback: default face, then registered order");
}

/**
 * @test internal_test_no_face_covers
 *
 * @brief An uncovered code point stays on the run's face for the box.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_no_face_covers(void)
{
  TEST_BEGIN("reflow face fallback: nothing covers it");
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_body,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_cjk));
  /* Every face was asked exactly once, and the run kept its own face so the
   * missing-glyph box is drawn at the surrounding text's metrics. */
  TEST_ASSERT_EQ((uint32_t)k_ff_faces, (uint32_t)s_probe.asks);
  for (uint8_t k = 0U; k < (uint8_t)k_ff_faces; ++k) {
    TEST_ASSERT_EQ(1U, (uint32_t)s_probe.asked[k]);
  }
  TEST_END("reflow face fallback: nothing covers it");
}

/**
 * @test internal_test_guard_arms
 *
 * @brief Blank code points, a single face, no probe, and a bogus index.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_guard_arms(void)
{
  TEST_BEGIN("reflow face fallback: guard arms");
  /* A blank code point must stay blank in its own face: never hunted. */
  internal_probe_reset();
  TEST_ASSERT_EQ((uint8_t)k_ff_face_body,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_space));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asks);
  TEST_ASSERT_EQ((uint8_t)k_ff_face_body,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_zwsp));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asks);

  /* A book with no embedded faces pays no coverage probe at all. */
  TEST_ASSERT_EQ((uint8_t)k_ff_face_default,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              1U,
                                              (uint8_t)k_ff_face_default,
                                              (int32_t)k_ff_cp_cjk));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asks);
  TEST_ASSERT_EQ((uint8_t)k_ff_face_default,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              0U,
                                              (uint8_t)k_ff_face_default,
                                              (int32_t)k_ff_cp_cjk));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asks);

  /* No probe: fallback is disabled and the run keeps its face. */
  TEST_ASSERT_EQ((uint8_t)k_ff_face_body,
                 priv_reflow_render_pick_face(nullptr,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_face_body,
                                              (int32_t)k_ff_cp_cjk));

  /* An index past the face set is returned untouched rather than probed. */
  TEST_ASSERT_EQ((uint8_t)k_ff_faces,
                 priv_reflow_render_pick_face(internal_probe,
                                              &s_probe,
                                              (uint8_t)k_ff_faces,
                                              (uint8_t)k_ff_faces,
                                              (int32_t)k_ff_cp_letter_a));
  TEST_ASSERT_EQ(0U, (uint32_t)s_probe.asks);
  TEST_END("reflow face fallback: guard arms");
}

int main(void)
{
  internal_test_primary_wins();
  internal_test_fallback_order();
  internal_test_no_face_covers();
  internal_test_guard_arms();
  TEST_ASSERT_EQ(
    k_ra8_test_output_ok,
    internal_test_output_fd_text(STDERR_FILENO, "[OK ] test_reflow_face_fallback.c\n"));
  return 0;
}
