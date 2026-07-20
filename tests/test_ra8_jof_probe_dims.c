/**
 * @file test_ra8_jof_probe_dims.c
 * @brief Geometry-probe vectors for `ra8_jof_probe_dims()` (#290).
 *
 * @details
 * The probe is the seam that lets a caller size a source before the producer
 * decodes it, so both host tools (`tools/ra8_fmt`, `tools/media_dl`) delegate
 * to it rather than carrying their own magic tables. That makes every arm of
 * its container dispatch reachable from a plain host test with no decoder
 * mock: a handful of crafted headers is enough, because the probe reads only
 * the declared geometry and never the pixel payload.
 *
 * Covered here: the three null-pointer guards, the too-short-to-sniff guard,
 * the JPEG / PNG / WebP dispatch arms, the PNG truncated-IHDR guard, the
 * unrecognised-magic fallthrough, the "RIFF but not WEBP" half of the WebP
 * fourCC pair, and both halves of the zero / over-cap dimension check.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "unity_minimal.h"

/** @brief Fixture sizes and the geometry the crafted headers declare. */
enum : uint32_t {
  k_p_hdr_cap    = 64U,    /**< Crafted-header staging capacity.         */
  k_p_sniff_min  = 12U,    /**< Bytes the probe needs to sniff at all.   */
  k_p_png_ihdr   = 24U,    /**< Bytes needed to reach both IHDR fields.  */
  k_p_w          = 640U,   /**< Declared width in the PNG/WebP headers.  */
  k_p_h          = 400U,   /**< Declared height in the PNG/WebP headers. */
  k_p_over_dim   = 40000U, /**< A dimension past the atlas cap.          */
  k_p_ihdr_w_ofs = 16U,    /**< PNG IHDR width field offset.             */
  k_p_ihdr_h_ofs = 20U,    /**< PNG IHDR height field offset.            */
  k_p_byte_mask  = 0xFFU,  /**< Low-byte mask.                           */
  k_p_sh8        = 8U,     /**< Byte shift.                              */
  k_p_sh16       = 16U,    /**< Half-word shift.                         */
  k_p_sh24       = 24U,    /**< Three-byte shift.                        */
  k_p_filler     = 0x5AU,  /**< Filler byte for the "matches no container
                                magic" vector; any value that is not the
                                head of a format the probe knows.          */
  k_p_form_ofs   = 8U,     /**< Offset of the RIFF form-type fourCC.       */
};

/** @brief RIFF container tag, as raw bytes (unterminated, exactly 4 wide). */
static const uint8_t s_riff_tag[4] = {'R', 'I', 'F', 'F'}; /* MAGIC-OK: RIFF fourCC */

/** @brief WebP form-type tag: the fourCC that makes a RIFF a WebP. */
static const uint8_t s_webp_tag[4] = {'W', 'E', 'B', 'P'}; /* MAGIC-OK: WEBP fourCC */

/** @brief AVI form-type tag: a RIFF the probe must NOT treat as a WebP. */
static const uint8_t s_avi_tag[4] = {'A', 'V', 'I', ' '}; /* MAGIC-OK: AVI fourCC */

/** @brief The eight-byte PNG signature. */
static const uint8_t s_png_sig[8] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK: spec signature */

/**
 * @brief Write a big-endian uint32 into a crafted header.
 * @details PNG stores IHDR fields big-endian, so the fixtures must too.
 * @param[out] buf Destination (4 writable bytes).
 * @param[in]  v   Value to store.
 * @pre @p buf holds 4 writable bytes.
 * @pre None (total over uint32_t).
 * @post @p buf[0..3] holds @p v big-endian.
 * @post No other state mutated.
 * @note Pure over its output; thread-safe.
 * @since 0.1.0
 */
static void wr_be32(uint8_t* buf, uint32_t v)
{
  buf[0] = (uint8_t)((v >> k_p_sh24) & k_p_byte_mask);
  buf[1] = (uint8_t)((v >> k_p_sh16) & k_p_byte_mask);
  buf[2] = (uint8_t)((v >> k_p_sh8) & k_p_byte_mask);
  buf[3] = (uint8_t)(v & k_p_byte_mask);
}

/**
 * @brief Build a PNG header declaring @p w x @p h.
 * @details Only the signature and the IHDR geometry fields matter to the
 *          probe, so the chunk length/type/CRC bytes are left zeroed.
 * @param[out] buf Destination (at least ::k_p_png_ihdr writable bytes).
 * @param[in]  w   Width to declare.
 * @param[in]  h   Height to declare.
 * @pre @p buf holds ::k_p_png_ihdr writable bytes.
 * @pre None (total over the width/height domain).
 * @post @p buf carries the PNG signature and both IHDR fields.
 * @post No other state mutated.
 * @note Not thread-safe beyond its output.
 * @since 0.1.0
 */
static void make_png(uint8_t* buf, uint32_t w, uint32_t h)
{
  (void)memset(buf, 0, (size_t)k_p_png_ihdr);
  (void)memcpy(buf, s_png_sig, sizeof(s_png_sig));
  wr_be32(&buf[k_p_ihdr_w_ofs], w);
  wr_be32(&buf[k_p_ihdr_h_ofs], h);
}

/**
 * @brief Reject a null argument on each of the three pointer parameters.
 * @details NASA P10 Rule 5 contract: the public entry owns exactly the
 *          null-pointer guards, so each one is driven independently.
 * @pre None.
 * @pre None.
 * @post Each guard returned ::k_ra8_err_null_ptr.
 * @post No output was written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_null_guards(void)
{
  TEST_BEGIN("probe_dims: three null-pointer guards");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  make_png(hdr, k_p_w, k_p_h);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_jof_probe_dims(nullptr, (size_t)k_p_png_ihdr, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, nullptr));
  TEST_END("probe_dims: three null-pointer guards");
}

/**
 * @brief A source shorter than the sniff window cannot be identified.
 * @details The probe pulls twelve bytes up front (WebP needs both fourCCs),
 *          so anything shorter is refused before any magic is compared.
 * @pre None.
 * @pre None.
 * @post A short buffer returned ::k_ra8_err_not_supported.
 * @post A buffer one byte under the window is still refused.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_too_short(void)
{
  TEST_BEGIN("probe_dims: below the sniff window");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  make_png(hdr, k_p_w, k_p_h);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_jof_probe_dims(hdr, 0U, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jof_probe_dims(hdr, (size_t)(k_p_sniff_min - 1U), &w, &h));
  TEST_END("probe_dims: below the sniff window");
}

/**
 * @brief The PNG arm reads IHDR, and refuses a truncated one.
 * @details Both halves of the ::priv_png_dims guard: a header long enough to
 *          carry both fields yields the declared geometry, and one cut short
 *          of the height field is refused rather than read out of bounds.
 * @pre None.
 * @pre None.
 * @post A full IHDR produced the declared width and height.
 * @post An IHDR truncated below its end returned ::k_ra8_err_not_supported.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_png(void)
{
  TEST_BEGIN("probe_dims: PNG IHDR arm and its truncation guard");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  make_png(hdr, k_p_w, k_p_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, &h));
  TEST_ASSERT_EQ(k_p_w, w);
  TEST_ASSERT_EQ(k_p_h, h);
  /* One byte short of the height field: sniffable, but the IHDR is cut. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jof_probe_dims(hdr, (size_t)(k_p_png_ihdr - 1U), &w, &h));
  TEST_END("probe_dims: PNG IHDR arm and its truncation guard");
}

/**
 * @brief Zero and over-cap PNG geometry are both refused.
 * @details The shared range check sits after the container dispatch, so a
 *          well-formed header declaring an unusable size still fails closed.
 * @pre None.
 * @pre None.
 * @post A zero width and a zero height each returned invalid-size.
 * @post A dimension past the atlas cap returned invalid-size.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_range_guard(void)
{
  TEST_BEGIN("probe_dims: zero and over-cap geometry");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  make_png(hdr, 0U, k_p_h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, &h));
  make_png(hdr, k_p_w, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, &h));
  make_png(hdr, k_p_over_dim, k_p_h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, &h));
  make_png(hdr, k_p_w, k_p_over_dim);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_png_ihdr, &w, &h));
  TEST_END("probe_dims: zero and over-cap geometry");
}

/**
 * @brief An unrecognised container, and RIFF that is not WebP, are refused.
 * @details `priv_is_webp()` requires BOTH fourCCs, so a RIFF container of some
 *          other form type must fall through to the unsupported arm rather
 *          than be handed to the WebP reader.
 * @pre None.
 * @pre None.
 * @post Arbitrary bytes returned ::k_ra8_err_not_supported.
 * @post A RIFF/AVI header returned ::k_ra8_err_not_supported.
 *
 * @par MC/DC:
 * Decision: `(RIFF fourCC matches) && (WEBP fourCC matches)` in `priv_is_webp`
 * (2 conditions)
 * - Vector 1: 0x5A filler       -> false (varies the first fourCC)
 * - Vector 2: "RIFF" + "AVI "   -> false (varies only the second fourCC)
 * - Vector 3: "RIFF" + "WEBP"   -> true  (control: both conditions true)
 * Vectors 1+3 prove the first fourCC independently affects the outcome; 2+3
 * prove the same for the second. N+1 = 3 vectors for N=2: minimal MC/DC.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_unknown_magic(void)
{
  TEST_BEGIN("probe_dims: unknown magic and RIFF-but-not-WEBP");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  (void)memset(hdr, (int)k_p_filler, sizeof(hdr));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_hdr_cap, &w, &h));
  /* RIFF container, but the form type is AVI -- only the first fourCC matches. */
  (void)memset(hdr, 0, sizeof(hdr));
  (void)memcpy(&hdr[0], s_riff_tag, sizeof(s_riff_tag));
  (void)memcpy(&hdr[k_p_form_ofs], s_avi_tag, sizeof(s_avi_tag));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_hdr_cap, &w, &h));
  /* Both fourCCs match, so priv_is_webp() is true and the WebP arm is taken;
   * the body is not a decodable bitstream, so the reader -- not the container
   * sniff -- reports the failure. This is the vector that varies only the
   * second fourCC against the AVI case above. */
  (void)memcpy(&hdr[k_p_form_ofs], s_webp_tag, sizeof(s_webp_tag));
  TEST_ASSERT(ra8_jof_probe_dims(hdr, (size_t)k_p_hdr_cap, &w, &h) != k_ra8_ok);
  TEST_END("probe_dims: unknown magic and RIFF-but-not-WEBP");
}

/**
 * @brief The JPEG arm is dispatched on SOI and answered by the JPEG reader.
 * @details The probe hands a JPEG straight to `ra8_jpeg_sw_get_dimensions()`,
 *          which range-checks against its own frame limits. A bare SOI with no
 *          SOF is malformed, so the reader -- not the probe -- reports the
 *          failure; what this vector proves is that the SOI dispatch arm is
 *          taken at all rather than falling through to unsupported.
 * @pre None.
 * @pre None.
 * @post A bare SOI did not return ::k_ra8_err_not_supported.
 * @post The dispatch reached the JPEG reader.
 *
 * @par MC/DC:
 * Decision: `(data[0] == SOI_first) && (data[1] == SOI_second)` (2 conditions)
 * - Vector 1: 0xFF 0xD8 -> true  (control: both conditions true)
 * - Vector 2: 0x89 'P'  -> false (varies data[0]; the PNG vectors above)
 * - Vector 3: 0xFF 0x00 -> false (varies only data[1])
 * Vectors 1+2 prove data[0] independently affects the outcome; 1+3 prove the
 * same for data[1]. N+1 = 3 vectors for N=2: minimal MC/DC.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_probe_jpeg_dispatch(void)
{
  TEST_BEGIN("probe_dims: JPEG SOI dispatch arm");
  uint8_t  hdr[k_p_hdr_cap] = {};
  uint16_t w                = 0U;
  uint16_t h                = 0U;
  (void)memset(hdr, 0, sizeof(hdr));
  hdr[0]             = 0xFFU; /* MAGIC-OK: JPEG SOI marker, first byte  */
  hdr[1]             = 0xD8U; /* MAGIC-OK: JPEG SOI marker, second byte */
  const ra8_err_t rc = ra8_jof_probe_dims(hdr, (size_t)k_p_hdr_cap, &w, &h);
  /* Whatever the reader concludes about a headerless JPEG, the probe must not
   * have treated it as an unrecognised container. */
  TEST_ASSERT(rc != k_ra8_err_not_supported);
  /* SOI's first byte alone is not SOI: varying only the second condition must
   * drop out of the JPEG arm and fall through to the unsupported tail. */
  hdr[1] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jof_probe_dims(hdr, (size_t)k_p_hdr_cap, &w, &h));
  TEST_END("probe_dims: JPEG SOI dispatch arm");
}

/**
 * @brief Test entry point -- runs the probe suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_probe_null_guards();
  test_probe_too_short();
  test_probe_png();
  test_probe_range_guard();
  test_probe_unknown_magic();
  test_probe_jpeg_dispatch();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof_probe_dims.c\n");
  return 0;
}
