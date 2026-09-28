/**
 * @file test_ra8_usb_desc.c
 * @brief Host tests for the synthesised USB device frameworks (#766).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The oracle is not a hand-written expectation: it is the framework array
 * `examples/ek_ra8d2/hw_validated/manual/usb_cdc_echo` already carries,
 * transcribed verbatim. A synthesiser that cannot reproduce a framework the
 * board has actually enumerated with is not a synthesiser worth converting an
 * app onto, so the first vector compares all ninety-three bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb_desc.h"
#include "unity_minimal.h"

/** @brief Sizes the fixtures below share. */
enum : uint32_t {
  k_fixture_framework_bytes = 93U, /**< Length of the usb_cdc_echo oracle. */
  k_fixture_strings_bytes   = 56U, /**< Length of its string framework.    */
};

/**
 * @brief The FS device framework of usb_cdc_echo, byte for byte.
 *
 * @details Transcribed from `s_device_framework_fs[]` in that app's `main.c`
 * at merge-base. Its offset 20..21 is the `wTotalLength` the app counted by
 * hand (0x004B = 75); the encoder derives the same value from what it emits.
 */
static const uint8_t k_oracle_framework[k_fixture_framework_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0xEFU, 0x02U, 0x01U, 0x40U, 0x09U, 0x12U, 0x0AU, 0x00U, 0x00U, 0x01U,
  0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x4BU, 0x00U, 0x02U, 0x01U, 0x00U, 0x80U, 0x32U, 0x08U,
  0x0BU, 0x00U, 0x02U, 0x02U, 0x02U, 0x01U, 0x00U, 0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x02U, 0x02U,
  0x01U, 0x00U, 0x05U, 0x24U, 0x00U, 0x20U, 0x01U, 0x05U, 0x24U, 0x01U, 0x01U, 0x01U, 0x04U, 0x24U,
  0x02U, 0x02U, 0x05U, 0x24U, 0x06U, 0x00U, 0x01U, 0x07U, 0x05U, 0x83U, 0x03U, 0x08U, 0x00U, 0xFFU,
  0x09U, 0x04U, 0x01U, 0x00U, 0x02U, 0x0AU, 0x00U, 0x00U, 0x00U, 0x07U, 0x05U, 0x02U, 0x02U, 0x40U,
  0x00U, 0x00U, 0x07U, 0x05U, 0x81U, 0x02U, 0x40U, 0x00U, 0x00U,
};

/** @brief The string framework of the same app, byte for byte. */
static const uint8_t k_oracle_strings[k_fixture_strings_bytes] = {
  0x09U, 0x04U, 0x01U, 0x12U, 'B',   'r',   'i', 'g', 'h',   't',   'o',   'n',   ' ', 'S',
  'i',   'k',   'a',   'r',   's',   'k',   'i', 'e', 0x09U, 0x04U, 0x02U, 0x12U, 'E', 'K',
  '-',   'R',   'A',   '8',   'D',   '2',   ' ', 'C', 'D',   'C',   ' ',   'E',   'c', 'h',
  'o',   '!',   0x09U, 0x04U, 0x03U, 0x08U, '0', '0', '0',   '0',   '0',   '0',   '0', '1',
};

/** @brief The identity usb_cdc_echo publishes. */
static const ra8_usb_desc_device_t k_fixture_dev = {
  .vid           = 0x1209U,
  .pid           = 0x000AU,
  .bcd_device    = 0x0100U,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "EK-RA8D2 CDC Echo!",
  .serial        = "00000001",
  .langid        = 0U,
  .max_power_ma  = 100U,
  .self_powered  = false,
  .remote_wakeup = false,
};

/** @brief The endpoint layout usb_cdc_echo publishes. */
static const ra8_usb_desc_cdc_acm_t k_fixture_cdc = {
  .notify_ep          = 0x83U,
  .notify_bytes       = 8U,
  .notify_interval_ms = 0xFFU,
  .out_ep             = 0x02U,
  .in_ep              = 0x81U,
  .data_bytes         = 64U,
};

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it compares synthesised bytes with a
 * committed oracle array; no `&&` or `||` in the code under test that this
 * case touches)
 * @brief The synthesised CDC-ACM framework equals the app's own array.
 *
 * @details Asserts the length first so a size drift reports as a size drift,
 * then every byte with its index, so a mismatch names the field.
 *
 * @pre None; the encoder is pure.
 * @post Every one of the ninety-three bytes has been asserted.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cdc_matches_app(void)
{
  TEST_BEGIN("the synthesised framework is usb_cdc_echo's array, byte for byte");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &k_fixture_cdc, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ((int)k_fixture_framework_bytes, (int)used);
  for (uint32_t i = 0U; i < k_fixture_framework_bytes; i++) {
    TEST_ASSERT_EQ((int)k_oracle_framework[i], (int)got[i]);
  }

  /* wTotalLength is the field a human counts wrong. It is derived here, and
   * it matches the 0x004B the app's comment says it had to be. */
  TEST_ASSERT_EQ(0x4B, (int)got[20]);
  TEST_ASSERT_EQ(0x00, (int)got[21]);

  TEST_END("ninety-three bytes reproduced from a config struct");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the string and language-id
 * encoders against committed oracle arrays)
 * @brief The string and language-id frameworks match the app's arrays.
 *
 * @pre None; the encoders are pure.
 * @post Both frameworks have been compared byte for byte.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_strings_match_app(void)
{
  TEST_BEGIN("the string and language-id frameworks match too");
  uint8_t  got[k_ra8_usb_desc_strings_bytes_max] = {};
  uint32_t used                                  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_strings(&k_fixture_dev, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ((int)k_fixture_strings_bytes, (int)used);
  for (uint32_t i = 0U; i < k_fixture_strings_bytes; i++) {
    TEST_ASSERT_EQ((int)k_oracle_strings[i], (int)got[i]);
  }

  uint8_t  lang[4] = {};
  uint32_t lang_n  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_desc_build_langid(0U, lang, 4U, &lang_n));
  TEST_ASSERT_EQ(2, (int)lang_n);
  TEST_ASSERT_EQ(0x09, (int)lang[0]);
  TEST_ASSERT_EQ(0x04, (int)lang[1]);

  /* An explicit language id is honoured rather than silently defaulted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_desc_build_langid(0x0809U, lang, 4U, &lang_n));
  TEST_ASSERT_EQ(0x09, (int)lang[0]);
  TEST_ASSERT_EQ(0x08, (int)lang[1]);

  TEST_END("both auxiliary frameworks reproduced");
}

/**
 * @par MC/DC:
 * The endpoint-direction guard in ::ra8_usb_desc_build_cdc_acm is
 * `(notify_ep & IN) == 0 || (in_ep & IN) == 0 || (out_ep & IN) != 0`.
 * Vectors below drive each of the three operands true with the other two
 * false, plus the all-false case in ::internal_test_cdc_matches_app, which
 * is independence for all three.
 * @brief Malformed configs are refused rather than encoded.
 *
 * @pre None; the encoder is pure.
 * @post Every refusal path has returned its documented code.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_refusals(void)
{
  TEST_BEGIN("a malformed config is refused, not encoded");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_desc_build_cdc_acm(nullptr, &k_fixture_cdc, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, nullptr, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &k_fixture_cdc, nullptr, 8U, &used));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_desc_build_cdc_acm(&k_fixture_dev,
                                            &k_fixture_cdc,
                                            got,
                                            (uint32_t)sizeof(got),
                                            nullptr));

  /* A buffer one byte short of the framework fails as a size, and does not
   * leave a plausible-looking truncated descriptor behind a success code. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_usb_desc_build_cdc_acm(&k_fixture_dev,
                                            &k_fixture_cdc,
                                            got,
                                            k_fixture_framework_bytes - 1U,
                                            &used));

  /* Operand 1: the notify endpoint is missing its IN bit. */
  ra8_usb_desc_cdc_acm_t bad = k_fixture_cdc;
  bad.notify_ep              = 0x03U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &bad, got, (uint32_t)sizeof(got), &used));

  /* Operand 2: the bulk-IN endpoint is missing its IN bit. */
  bad       = k_fixture_cdc;
  bad.in_ep = 0x01U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &bad, got, (uint32_t)sizeof(got), &used));

  /* Operand 3: the bulk-OUT endpoint wrongly carries the IN bit. */
  bad        = k_fixture_cdc;
  bad.out_ep = 0x82U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &bad, got, (uint32_t)sizeof(got), &used));

  bad            = k_fixture_cdc;
  bad.data_bytes = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &bad, got, (uint32_t)sizeof(got), &used));

  ra8_usb_desc_device_t hungry = k_fixture_dev;
  hungry.max_power_ma          = 502U;
  TEST_ASSERT_EQ(
    k_ra8_err_range_check_failed,
    ra8_usb_desc_build_cdc_acm(&hungry, &k_fixture_cdc, got, (uint32_t)sizeof(got), &used));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_usb_desc_build_langid(0U, got, 1U, &used));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_desc_build_langid(0U, nullptr, 8U, &used));

  TEST_END("every documented refusal returns its documented code");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- covers the optional-field and
 * rounding contracts the header publishes)
 * @brief Omitted strings, odd power draws and flags encode as documented.
 *
 * @pre None; the encoder is pure.
 * @post The three published contracts have been asserted on the wire bytes.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_optional_fields(void)
{
  TEST_BEGIN("omitted strings, odd power and the attribute flags");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  ra8_usb_desc_device_t bare = k_fixture_dev;
  bare.manufacturer          = nullptr;
  bare.product               = "";
  bare.serial                = "S";
  bare.bcd_device            = 0U;
  bare.max_power_ma          = 101U;
  bare.self_powered          = true;
  bare.remote_wakeup         = true;

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_cdc_acm(&bare, &k_fixture_cdc, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ((int)k_fixture_framework_bytes, (int)used);

  /* An omitted slot publishes index 0, so the descriptor never points at a
   * string the framework does not carry. */
  TEST_ASSERT_EQ(0, (int)got[14]);
  TEST_ASSERT_EQ(0, (int)got[15]);
  TEST_ASSERT_EQ((int)k_ra8_usb_desc_str_serial, (int)got[16]);

  /* bcdDevice 0 means 0x0100. */
  TEST_ASSERT_EQ(0x00, (int)got[12]);
  TEST_ASSERT_EQ(0x01, (int)got[13]);

  /* 101 mA rounds up to 51 units, never down to 50. */
  TEST_ASSERT_EQ(51, (int)got[26]);
  TEST_ASSERT_EQ(0xE0, (int)got[25]);

  uint8_t  strings[k_ra8_usb_desc_strings_bytes_max] = {};
  uint32_t strings_n                                 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_strings(&bare, strings, (uint32_t)sizeof(strings), &strings_n));
  TEST_ASSERT_EQ((int)(k_ra8_usb_desc_string_hdr + 1U), (int)strings_n);
  TEST_ASSERT_EQ((int)k_ra8_usb_desc_str_serial, (int)strings[2]);
  TEST_ASSERT_EQ(1, (int)strings[3]);
  TEST_ASSERT_EQ((int)'S', (int)strings[4]);

  /* A device that publishes nothing gets an empty framework, not a refusal. */
  ra8_usb_desc_device_t silent = bare;
  silent.serial                = nullptr;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_strings(&silent, strings, (uint32_t)sizeof(strings), &strings_n));
  TEST_ASSERT_EQ(0, (int)strings_n);

  TEST_END("the optional-field contracts hold on the wire bytes");
}

/* =============================================================================
 * Mass storage
 * =============================================================================
 */

/** @brief Sizes the mass-storage fixtures below share. */
enum : uint32_t {
  k_fixture_msc_fs_bytes = 50U, /**< Length of the usb_msc_device oracle.  */
  k_fixture_msc_hs_bytes = 60U, /**< Length of the usb_msc_mram_hs oracle. */
};

/**
 * @brief The FS device framework of usb_msc_device, byte for byte.
 *
 * @details Transcribed from `s_device_framework_fs[]` in that app's `main.c`
 * at merge-base. Device class is 0 here, not the MISC / common / IAD triple a
 * CDC composite publishes, and the configuration block is 0x0020 = 32 bytes.
 */
static const uint8_t k_oracle_msc_fs[k_fixture_msc_fs_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x0BU, 0x00U, 0x00U,
  0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x20U, 0x00U, 0x01U, 0x01U, 0x00U, 0x80U,
  0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x02U, 0x08U, 0x06U, 0x50U, 0x00U, 0x07U, 0x05U, 0x81U,
  0x02U, 0x40U, 0x00U, 0x00U, 0x07U, 0x05U, 0x02U, 0x02U, 0x40U, 0x00U, 0x00U,
};

/**
 * @brief The HS device framework of usb_msc_mram_hs, byte for byte.
 *
 * @details Same app family at high speed: a ten-byte device qualifier sits
 * between the device descriptor and the configuration block, and the two bulk
 * endpoints carry a 512-byte max packet size. The qualifier is outside the
 * configuration block, so `wTotalLength` stays 0x0020.
 */
static const uint8_t k_oracle_msc_hs[k_fixture_msc_hs_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x0DU, 0x00U,
  0x00U, 0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x0AU, 0x06U, 0x00U, 0x02U, 0x00U, 0x00U,
  0x00U, 0x40U, 0x01U, 0x00U, 0x09U, 0x02U, 0x20U, 0x00U, 0x01U, 0x01U, 0x00U, 0x80U,
  0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x02U, 0x08U, 0x06U, 0x50U, 0x00U, 0x07U, 0x05U,
  0x81U, 0x02U, 0x00U, 0x02U, 0x00U, 0x07U, 0x05U, 0x02U, 0x02U, 0x00U, 0x02U, 0x00U,
};

/** @brief The identity usb_msc_device publishes. */
static const ra8_usb_desc_device_t k_fixture_msc_dev = {
  .vid           = 0x1209U,
  .pid           = 0x000BU,
  .bcd_device    = 0x0100U,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "EK-RA8D2 RAM Disk",
  .serial        = "00000001",
  .langid        = 0U,
  .max_power_ma  = 100U,
  .self_powered  = false,
  .remote_wakeup = false,
};

/** @brief The full-speed endpoint layout that app publishes. */
static const ra8_usb_desc_msc_t k_fixture_msc_fs = {
  .in_ep      = 0x81U,
  .out_ep     = 0x02U,
  .data_bytes = 64U,
  .high_speed = false,
};

/**
 * @brief The mass-storage builder reproduces the framework a shipped app carries.
 *
 * @details Both speeds, because the high-speed variant is the only descriptor
 * in this header that is not a straight-line append: the device qualifier goes
 * before the configuration block and must stay out of its `wTotalLength`.
 */
RA8_INTERNAL static void internal_test_msc_matches_app(void)
{
  TEST_BEGIN("the mass-storage framework of usb_msc_device and usb_msc_mram_hs");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_msc(&k_fixture_msc_dev,
                                        &k_fixture_msc_fs,
                                        got,
                                        (uint32_t)sizeof(got),
                                        &used));
  TEST_ASSERT_EQ(k_fixture_msc_fs_bytes, used);
  for (uint32_t i = 0U; i < k_fixture_msc_fs_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_msc_fs[i], got[i]);
  }

  ra8_usb_desc_device_t hs_dev = k_fixture_msc_dev;
  hs_dev.pid                   = 0x000DU;
  hs_dev.product               = "EK-RA8D2 MRAM HS";
  hs_dev.serial                = "00000003";

  ra8_usb_desc_msc_t hs = k_fixture_msc_fs;
  hs.data_bytes         = 512U;
  hs.high_speed         = true;

  used = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_desc_build_msc(&hs_dev, &hs, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(k_fixture_msc_hs_bytes, used);
  for (uint32_t i = 0U; i < k_fixture_msc_hs_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_msc_hs[i], got[i]);
  }

  /* The qualifier is not part of the configuration, so wTotalLength is the
   * same 0x0020 at both speeds even though the framework grew by ten bytes. */
  TEST_ASSERT_EQ(0x20, got[30]);
  TEST_ASSERT_EQ(0x00, got[31]);

  TEST_END("both speeds reproduce the shipped bytes");
}

/**
 * @brief The mass-storage builder refuses what it cannot encode.
 *
 * @details The direction-bit rule is the one a converted app is most likely to
 * get wrong, because the two bulk endpoints differ only in that bit.
 */
RA8_INTERNAL static void internal_test_msc_refusals(void)
{
  TEST_BEGIN("mass-storage refusals");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  ra8_usb_desc_msc_t bad = k_fixture_msc_fs;
  bad.in_ep              = 0x01U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_msc(&k_fixture_msc_dev, &bad, got, (uint32_t)sizeof(got), &used));

  bad        = k_fixture_msc_fs;
  bad.out_ep = 0x82U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_msc(&k_fixture_msc_dev, &bad, got, (uint32_t)sizeof(got), &used));

  bad            = k_fixture_msc_fs;
  bad.data_bytes = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_msc(&k_fixture_msc_dev, &bad, got, (uint32_t)sizeof(got), &used));

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_desc_build_msc(nullptr, &k_fixture_msc_fs, got, (uint32_t)sizeof(got), &used));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_usb_desc_build_msc(&k_fixture_msc_dev, &k_fixture_msc_fs, got, 8U, &used));

  ra8_usb_desc_device_t greedy = k_fixture_msc_dev;
  greedy.max_power_ma          = 501U;
  TEST_ASSERT_EQ(
    k_ra8_err_range_check_failed,
    ra8_usb_desc_build_msc(&greedy, &k_fixture_msc_fs, got, (uint32_t)sizeof(got), &used));

  TEST_END("a framework it cannot encode is refused, not truncated");
}

int main(void)
{
  internal_test_cdc_matches_app();
  internal_test_strings_match_app();
  internal_test_refusals();
  internal_test_optional_fields();
  internal_test_msc_matches_app();
  internal_test_msc_refusals();
  return 0;
}
