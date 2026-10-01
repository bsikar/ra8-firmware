/**
 * @file test_ra8_imgdec_name.c
 * @brief Host tests for the shared image naming table.
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_name.h"
#include "unity_minimal.h"

/** @brief Longest fixture prefix, one byte past the widest signature. */
enum : uint32_t { k_fixture_bytes = 16U };

/**
 * @brief Every defined format bit is nameable, `_none` and junk are not.
 */
RA8_INTERNAL static void internal_test_name_table(void)
{
  TEST_BEGIN("every format bit names itself, nothing else does");
  ra8_imgdec_name_t id = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_jpeg, &id));
  TEST_ASSERT_EQ(0, strcmp("jpg", id.ext));
  TEST_ASSERT_EQ(0, strcmp("image/jpeg", id.mime));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_jpeg, id.format);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_png, &id));
  TEST_ASSERT_EQ(0, strcmp("png", id.ext));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_webp, &id));
  TEST_ASSERT_EQ(0, strcmp("webp", id.ext));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_gif, &id));
  TEST_ASSERT_EQ(0, strcmp("gif", id.ext));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_bmp, &id));
  TEST_ASSERT_EQ(0, strcmp("bmp", id.ext));

  /* TGA has no signature, so it is nameable but never sniffable. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_name(k_ra8_imgdec_format_tga, &id));
  TEST_ASSERT_EQ(0, strcmp("tga", id.ext));
  TEST_ASSERT_EQ(0, strcmp("image/x-tga", id.mime));

  /* The empty set, a combination and an undefined bit are all refusals. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_name(k_ra8_imgdec_format_none, &id));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, id.format);
  TEST_ASSERT_NULL(id.ext);
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    ra8_imgdec_name((ra8_imgdec_format_t)(k_ra8_imgdec_format_png | k_ra8_imgdec_format_gif), &id));
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    ra8_imgdec_name((ra8_imgdec_format_t)((uint32_t)k_ra8_imgdec_format_mask + 1U), &id));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_name(k_ra8_imgdec_format_png, nullptr));

  TEST_END("every format bit names itself, nothing else does");
}

/**
 * @brief identify() agrees with sniff() and adds the names.
 */
RA8_INTERNAL static void internal_test_identify(void)
{
  TEST_BEGIN("a recognised buffer comes back named");
  ra8_imgdec_name_t id = {};

  const uint8_t jpeg[k_fixture_bytes] = {0xFFU, 0xD8U, 0xFFU, 0xE0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_identify(jpeg, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_jpeg, id.format);
  TEST_ASSERT_EQ(0, strcmp("image/jpeg", id.mime));

  const uint8_t png[k_fixture_bytes] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_identify(png, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_png, id.format);
  TEST_ASSERT_EQ(0, strcmp("png", id.ext));

  const uint8_t webp[k_fixture_bytes] =
    {'R', 'I', 'F', 'F', 0x10U, 0U, 0U, 0U, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_identify(webp, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(0, strcmp("webp", id.ext));

  const uint8_t gif[k_fixture_bytes] = {'G', 'I', 'F', '8', '9', 'a'};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_identify(gif, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(0, strcmp("gif", id.ext));

  const uint8_t bmp[k_fixture_bytes] = {'B', 'M', 0x36U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_identify(bmp, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(0, strcmp("image/bmp", id.mime));

  TEST_END("a recognised buffer comes back named");
}

/**
 * @brief A refusal leaves the record empty rather than half-written.
 */
RA8_INTERNAL static void internal_test_identify_refusals(void)
{
  TEST_BEGIN("an unrecognised buffer reports nothing at all");
  ra8_imgdec_name_t id                    = {};
  const uint8_t     junk[k_fixture_bytes] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'g'};

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_identify(junk, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, id.format);
  TEST_ASSERT_NULL(id.ext);
  TEST_ASSERT_NULL(id.mime);

  /* A RIFF that is not a WebP stays unnamed: the form type decides. */
  const uint8_t wave[k_fixture_bytes] =
    {'R', 'I', 'F', 'F', 0x10U, 0U, 0U, 0U, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_identify(wave, k_fixture_bytes, &id));

  /* A PNG one byte short of its signature is refused, not guessed. */
  const uint8_t short_png[] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU};
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_imgdec_identify(short_png, (uint32_t)sizeof(short_png), &id));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_identify(junk, 0U, &id));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_identify(nullptr, k_fixture_bytes, &id));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_identify(junk, k_fixture_bytes, nullptr));

  TEST_END("an unrecognised buffer reports nothing at all");
}

int main(void)
{
  internal_test_name_table();
  internal_test_identify();
  internal_test_identify_refusals();
  return 0;
}
