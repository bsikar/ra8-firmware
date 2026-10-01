/**
 * @file test_ra8_usb_desc.c
 * @brief Host tests for the synthesised USB device frameworks (RA8FW-317).
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
 * (the decision under test is ``cdc->high_speed``: this case covers the true
 * branch, and ::internal_test_cdc_matches_app covers the false one)
 * @brief A high-speed CDC-ACM framework carries the device qualifier.
 *
 * @details tz_secure_only_usb_hs is the one CDC app in the tree that
 * negotiates high speed, so it publishes a qualifier the full-speed apps must
 * not. Asserts the qualifier lands between the device descriptor and the
 * configuration, that it repeats the device descriptor's MISC / common / IAD
 * class triple rather than the per-interface zeros a mass-storage device
 * publishes, and that wTotalLength still counts only the configuration.
 *
 * @pre None; the encoder is pure.
 * @post Both speeds have been encoded from the same device identity.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cdc_high_speed(void)
{
  TEST_BEGIN("a high-speed CDC framework publishes the device qualifier");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;
  uint32_t fs_used                                 = 0U;

  ra8_usb_desc_cdc_acm_t hs = k_fixture_cdc;
  hs.high_speed             = true;
  hs.data_bytes             = 512U;
  hs.notify_interval_ms     = 0x08U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_cdc_acm(&k_fixture_dev,
                                            &k_fixture_cdc,
                                            got,
                                            (uint32_t)sizeof(got),
                                            &fs_used));
  TEST_ASSERT_EQ(0x09, got[18]); /* configuration follows directly */

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_cdc_acm(&k_fixture_dev, &hs, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(fs_used + 10U, used);

  /* The qualifier itself, USB 2.0 sec 9.6.2. */
  TEST_ASSERT_EQ(0x0A, got[18]);
  TEST_ASSERT_EQ(0x06, got[19]);
  TEST_ASSERT_EQ(0x00, got[20]);
  TEST_ASSERT_EQ(0x02, got[21]);
  TEST_ASSERT_EQ(got[4], got[22]); /* class triple repeats the device */
  TEST_ASSERT_EQ(got[5], got[23]);
  TEST_ASSERT_EQ(got[6], got[24]);
  TEST_ASSERT_EQ(0x40, got[25]);
  TEST_ASSERT_EQ(0x01, got[26]);
  TEST_ASSERT_EQ(0x00, got[27]);

  /* wTotalLength is measured from the configuration, not from byte zero. */
  TEST_ASSERT_EQ(0x09, got[28]);
  TEST_ASSERT_EQ(0x4B, got[30]);
  TEST_ASSERT_EQ(0x00, got[31]);

  TEST_END("the qualifier is the only difference between the two speeds");
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

/* =============================================================================
 * Human interface
 * =============================================================================
 */

/** @brief Sizes the HID fixtures below share. */
enum : uint32_t {
  k_fixture_hid_bytes        = 52U, /**< Length of every HID framework here.  */
  k_fixture_hid_mouse_report = 52U, /**< usb_hid_device report descriptor.    */
  k_fixture_hid_plain_report = 21U, /**< usb_selftest_hid report descriptor.  */
  k_fixture_hid_kbd_report   = 45U, /**< usb_host_keyboard report descriptor. */
};

/**
 * @brief The FS device framework of usb_hid_device, byte for byte.
 *
 * @details Transcribed from `s_device_framework_fs[]` in that app's `main.c`
 * at merge-base. A boot mouse: interface subclass 1, protocol 2, an 8-byte
 * interrupt endpoint polled every 10 ms, and a 0x0034 = 52 byte report
 * descriptor. The configuration block is 0x0022 = 34 bytes.
 */
static const uint8_t k_oracle_hid_mouse[k_fixture_hid_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x01U, 0x00U, 0x00U,
  0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x22U, 0x00U, 0x01U, 0x01U, 0x00U, 0x80U,
  0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x01U, 0x02U, 0x00U, 0x09U, 0x21U, 0x11U,
  0x01U, 0x00U, 0x01U, 0x22U, 0x34U, 0x00U, 0x07U, 0x05U, 0x81U, 0x03U, 0x08U, 0x00U, 0x0AU,
};

/**
 * @brief The FS device framework of usb_selftest_hid, byte for byte.
 *
 * @details The same shape with no boot profile at all: subclass and protocol
 * are both 0, the endpoint carries 64 bytes and is polled every frame, and the
 * report descriptor is 0x0015 = 21 bytes.
 */
static const uint8_t k_oracle_hid_plain[k_fixture_hid_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x18U, 0x00U, 0x00U,
  0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x22U, 0x00U, 0x01U, 0x01U, 0x00U, 0x80U,
  0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x09U, 0x21U, 0x11U,
  0x01U, 0x00U, 0x01U, 0x22U, 0x15U, 0x00U, 0x07U, 0x05U, 0x81U, 0x03U, 0x40U, 0x00U, 0x01U,
};

/**
 * @brief The FS device framework of usb_host_keyboard's device half, byte for byte.
 *
 * @details A boot keyboard: subclass 1, protocol 1, and a 0x002D = 45 byte
 * report descriptor. It shares its PID with usb_selftest_hid, which is what
 * the tree carries; only the class triple and the report length differ.
 */
static const uint8_t k_oracle_hid_keyboard[k_fixture_hid_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x18U, 0x00U, 0x00U,
  0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x22U, 0x00U, 0x01U, 0x01U, 0x00U, 0x80U,
  0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x01U, 0x01U, 0x00U, 0x09U, 0x21U, 0x11U,
  0x01U, 0x00U, 0x01U, 0x22U, 0x2DU, 0x00U, 0x07U, 0x05U, 0x81U, 0x03U, 0x40U, 0x00U, 0x01U,
};

/** @brief The identity usb_hid_device publishes. */
static const ra8_usb_desc_device_t k_fixture_hid_dev = {
  .vid           = 0x1209U,
  .pid           = 0x0001U,
  .bcd_device    = 0x0100U,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "EK-RA8D2 HID Mouse",
  .serial        = "00000001",
  .langid        = 0U,
  .max_power_ma  = 100U,
  .self_powered  = false,
  .remote_wakeup = false,
};

/** @brief The boot-mouse endpoint layout that app publishes. */
static const ra8_usb_desc_hid_t k_fixture_hid_mouse = {
  .in_ep            = 0x81U,
  .data_bytes       = 8U,
  .poll_interval_ms = 10U,
  .report_bytes     = (uint16_t)k_fixture_hid_mouse_report,
  .boot_interface   = true,
  .protocol         = k_ra8_usb_desc_hid_protocol_mouse,
};

/**
 * @brief The HID builder reproduces the frameworks three shipped apps carry.
 *
 * @details All three at once, because the class triple is the whole difference
 * between them: a boot mouse, a plain interface with no boot profile, and a
 * boot keyboard. The report length reaches the wire from a field rather than
 * from a hand-counted literal, which is the drift this builder exists to stop.
 */
RA8_INTERNAL static void internal_test_hid_matches_app(void)
{
  TEST_BEGIN("the HID frameworks of usb_hid_device, usb_selftest_hid and usb_host_keyboard");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_hid(&k_fixture_hid_dev,
                                        &k_fixture_hid_mouse,
                                        got,
                                        (uint32_t)sizeof(got),
                                        &used));
  TEST_ASSERT_EQ(k_fixture_hid_bytes, used);
  for (uint32_t i = 0U; i < k_fixture_hid_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_hid_mouse[i], got[i]);
  }

  ra8_usb_desc_device_t test_dev = k_fixture_hid_dev;
  test_dev.pid                   = 0x0018U;
  test_dev.product               = "RA8D2 HID TEST";
  test_dev.serial                = "00000018";

  ra8_usb_desc_hid_t plain = k_fixture_hid_mouse;
  plain.data_bytes         = 64U;
  plain.poll_interval_ms   = 1U;
  plain.report_bytes       = (uint16_t)k_fixture_hid_plain_report;
  plain.boot_interface     = false;
  plain.protocol           = k_ra8_usb_desc_hid_protocol_none;

  used = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_hid(&test_dev, &plain, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(k_fixture_hid_bytes, used);
  for (uint32_t i = 0U; i < k_fixture_hid_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_hid_plain[i], got[i]);
  }

  ra8_usb_desc_hid_t keyboard = plain;
  keyboard.report_bytes       = (uint16_t)k_fixture_hid_kbd_report;
  keyboard.boot_interface     = true;
  keyboard.protocol           = k_ra8_usb_desc_hid_protocol_keyboard;

  used = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_hid(&test_dev, &keyboard, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(k_fixture_hid_bytes, used);
  for (uint32_t i = 0U; i < k_fixture_hid_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_hid_keyboard[i], got[i]);
  }

  /* Every HID framework here is one interface and one endpoint, so the
   * configuration block is 0x0022 = 34 bytes whatever the class triple says. */
  TEST_ASSERT_EQ(0x22, got[20]);
  TEST_ASSERT_EQ(0x00, got[21]);

  TEST_END("all three reproduce the shipped bytes");
}

/**
 * @brief The HID builder refuses what it cannot encode.
 *
 * @details The pairing rule is the interesting one: bInterfaceProtocol is
 * reserved unless the interface declares the boot subclass, so a boot protocol
 * on a non-boot interface is refused rather than written out and ignored.
 */
RA8_INTERNAL static void internal_test_hid_refusals(void)
{
  TEST_BEGIN("the HID builder refuses a config it cannot put on the wire");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_desc_build_hid(nullptr, &k_fixture_hid_mouse, got, (uint32_t)sizeof(got), &used));

  ra8_usb_desc_hid_t out_dir = k_fixture_hid_mouse;
  out_dir.in_ep              = 0x01U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_hid(&k_fixture_hid_dev, &out_dir, got, (uint32_t)sizeof(got), &used));

  ra8_usb_desc_hid_t no_report = k_fixture_hid_mouse;
  no_report.report_bytes       = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_hid(&k_fixture_hid_dev, &no_report, got, (uint32_t)sizeof(got), &used));

  ra8_usb_desc_hid_t no_poll = k_fixture_hid_mouse;
  no_poll.poll_interval_ms   = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_hid(&k_fixture_hid_dev, &no_poll, got, (uint32_t)sizeof(got), &used));

  ra8_usb_desc_hid_t stray_protocol = k_fixture_hid_mouse;
  stray_protocol.boot_interface     = false;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_hid(&k_fixture_hid_dev, &stray_protocol, got, (uint32_t)sizeof(got), &used));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_usb_desc_build_hid(&k_fixture_hid_dev, &k_fixture_hid_mouse, got, 8U, &used));

  ra8_usb_desc_device_t greedy = k_fixture_hid_dev;
  greedy.max_power_ma          = 600U;
  TEST_ASSERT_EQ(
    k_ra8_err_range_check_failed,
    ra8_usb_desc_build_hid(&greedy, &k_fixture_hid_mouse, got, (uint32_t)sizeof(got), &used));

  TEST_END("a framework it cannot encode is refused, not truncated");
}

/**
 * @enum test_fixture_dfu_t
 * @brief Wire sizes of the DFU framework all five bootloader apps carry.
 */
enum : uint16_t {
  k_fixture_dfu_bytes    = 45U,     /**< Device framework wire length.   */
  k_fixture_dfu_xfer     = 64U,     /**< wTransferSize, bytes per block. */
  k_fixture_dfu_detach   = 255U,    /**< wDetachTimeOut, milliseconds.   */
  k_fixture_dfu_bcd      = 0x0110U, /**< bcdDFUVersion 1.1.              */
  k_fixture_dfu_vid      = 0x1209U, /**< idVendor.                       */
  k_fixture_dfu_pid      = 0x0019U, /**< idProduct.                      */
  k_fixture_dfu_bcd_dev  = 0x0100U, /**< bcdDevice.                      */
  k_fixture_dfu_power_ma = 100U,    /**< Bus draw.                       */
};

/**
 * @brief The device framework all five DFU apps carry, byte for byte.
 *
 * @details Transcribed from `s_device_framework[]` at merge-base. dfu_bootloader,
 * dfu_selftest_boot, dfu_selftest_fs_host, dfu_selftest_hs_host and
 * usb_selftest_dfu carry this array identically, which is five copies of the
 * same 45 bytes. The interface declares zero endpoints because DFU runs over
 * the control pipe, so the configuration block is only 0x001B = 27 bytes.
 */
static const uint8_t k_oracle_dfu[k_fixture_dfu_bytes] = {
  0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x09U, 0x12U, 0x19U, 0x00U,
  0x00U, 0x01U, 0x01U, 0x02U, 0x03U, 0x01U, 0x09U, 0x02U, 0x1BU, 0x00U, 0x01U, 0x01U,
  0x00U, 0x80U, 0x32U, 0x09U, 0x04U, 0x00U, 0x00U, 0x00U, 0xFEU, 0x01U, 0x02U, 0x00U,
  0x09U, 0x21U, 0x07U, 0xFFU, 0x00U, 0x40U, 0x00U, 0x10U, 0x01U,
};

/** @brief The identity the five DFU apps publish. */
static const ra8_usb_desc_device_t k_fixture_dfu_dev = {
  .vid           = (uint16_t)k_fixture_dfu_vid,
  .pid           = (uint16_t)k_fixture_dfu_pid,
  .bcd_device    = (uint16_t)k_fixture_dfu_bcd_dev,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 DFU",
  .serial        = "00000019",
  .langid        = 0U,
  .max_power_ma  = (uint16_t)k_fixture_dfu_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/** @brief The DFU function those apps publish: download, upload, tolerant. */
static const ra8_usb_desc_dfu_t k_fixture_dfu = {
  .can_download           = true,
  .can_upload             = true,
  .manifestation_tolerant = true,
  .will_detach            = false,
  .dfu_mode               = true,
  .detach_timeout_ms      = (uint16_t)k_fixture_dfu_detach,
  .transfer_bytes         = (uint16_t)k_fixture_dfu_xfer,
  .bcd_dfu                = (uint16_t)k_fixture_dfu_bcd,
};

/**
 * @brief The DFU builder reproduces the framework five shipped apps carry.
 *
 * @details One case covers all five, because the arrays are byte-identical:
 * the same vendor, product, serial and capability set in every copy. The
 * assertions single out the two fields a hand-editor gets wrong, bNumEndpoints
 * and wTotalLength, since a DFU interface having no endpoints at all is the
 * one thing that separates this layout from every other in this header.
 */
RA8_INTERNAL static void internal_test_dfu_matches_app(void)
{
  TEST_BEGIN("the DFU framework of all five bootloader apps");
  uint8_t  got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t used                                    = 0U;

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &k_fixture_dfu, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ((uint32_t)k_fixture_dfu_bytes, used);
  for (uint32_t i = 0U; i < (uint32_t)k_fixture_dfu_bytes; i++) {
    TEST_ASSERT_EQ(k_oracle_dfu[i], got[i]);
  }
  TEST_ASSERT_EQ(0U, got[30]);    /* bNumEndpoints                           */
  TEST_ASSERT_EQ(0x1BU, got[20]); /* wTotalLength low                        */
  TEST_ASSERT_EQ(0x00U, got[21]); /* wTotalLength high                       */
  TEST_ASSERT_EQ(0x07U, got[38]); /* bmAttributes, the three capability bits */

  TEST_END("forty-five bytes reproduced, and one array replaces five copies");
}

/**
 * @brief The DFU builder refuses a config it cannot put on the wire.
 *
 * @details The run-time protocol and the will-detach bit are exercised here
 * rather than in the oracle case, because no app in the tree publishes them
 * yet and an encoder with an untested branch is an encoder that will be wrong
 * the first time someone uses it.
 */
RA8_INTERNAL static void internal_test_dfu_refusals(void)
{
  TEST_BEGIN("DFU refusals, and the two branches no app exercises yet");
  uint8_t            got[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t           used                                    = 0U;
  ra8_usb_desc_dfu_t bad                                     = k_fixture_dfu;

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_desc_build_dfu(nullptr, &k_fixture_dfu, got, (uint32_t)sizeof(got), &used));

  bad                = k_fixture_dfu;
  bad.transfer_bytes = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &bad, got, (uint32_t)sizeof(got), &used));

  bad         = k_fixture_dfu;
  bad.bcd_dfu = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &bad, got, (uint32_t)sizeof(got), &used));

  bad              = k_fixture_dfu;
  bad.can_download = false;
  bad.can_upload   = false;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &bad, got, (uint32_t)sizeof(got), &used));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &k_fixture_dfu, got, 1U, &used));

  /* Run-time descriptor: bInterfaceProtocol 1, not 2. */
  bad          = k_fixture_dfu;
  bad.dfu_mode = false;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &bad, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(0x01U, got[34]);

  /* will_detach sets bit 3 and leaves the other three where they were. */
  bad             = k_fixture_dfu;
  bad.will_detach = true;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_desc_build_dfu(&k_fixture_dfu_dev, &bad, got, (uint32_t)sizeof(got), &used));
  TEST_ASSERT_EQ(0x0FU, got[38]);

  TEST_END("every documented refusal returns its code, both protocols encode");
}

int main(void)
{
  internal_test_cdc_matches_app();
  internal_test_cdc_high_speed();
  internal_test_strings_match_app();
  internal_test_refusals();
  internal_test_optional_fields();
  internal_test_msc_matches_app();
  internal_test_msc_refusals();
  internal_test_hid_matches_app();
  internal_test_hid_refusals();
  internal_test_dfu_matches_app();
  internal_test_dfu_refusals();
  return 0;
}
