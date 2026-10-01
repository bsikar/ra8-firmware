/**
 * @file test_ra8_usb_compose.c
 * @brief Host tests for the one-call framework composition (RA8FW-317).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The composition layer adds dispatch and argument checking on top of the
 * encoders, so the oracle for it is the encoders themselves: for each of the
 * four class kinds, composing must produce exactly the bytes the app-facing
 * builder produces, and the three lengths must be the three the app used to
 * carry by hand. The refusal cases cover the arguments a caller can get wrong
 * that the encoders never see, because composition checks them first.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "unity_minimal.h"

/** @brief The identity every case below composes. */
static const ra8_usb_desc_device_t k_fixture_dev = {
  .vid           = 0x1209U,
  .pid           = 0x000AU,
  .bcd_device    = 0x0100U,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "EK-RA8D2 CDC Echo!",
  .serial        = "00000001",
  .langid        = k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = 100U,
  .self_powered  = false,
  .remote_wakeup = false,
};

/** @brief A CDC-ACM port, the shape usb_cdc_echo publishes. */
static const ra8_usb_desc_cdc_acm_t k_fixture_cdc = {
  .notify_ep          = 0x83U,
  .notify_bytes       = 8U,
  .notify_interval_ms = 255U,
  .out_ep             = 0x02U,
  .in_ep              = 0x81U,
  .data_bytes         = 64U,
  .high_speed         = false,
};

/** @brief A mass-storage function, the shape usb_msc_device publishes. */
static const ra8_usb_desc_msc_t k_fixture_msc = {
  .in_ep      = 0x81U,
  .out_ep     = 0x02U,
  .data_bytes = 64U,
  .high_speed = false,
};

/** @brief A human-interface function, the shape usb_hid_device publishes. */
static const ra8_usb_desc_hid_t k_fixture_hid = {
  .in_ep            = 0x81U,
  .data_bytes       = 8U,
  .poll_interval_ms = 10U,
  .report_bytes     = 50U,
  .boot_interface   = true,
  .protocol         = k_ra8_usb_desc_hid_protocol_keyboard,
};

/** @brief A DFU function in DFU mode. */
static const ra8_usb_desc_dfu_t k_fixture_dfu = {
  .can_download           = true,
  .can_upload             = true,
  .manifestation_tolerant = true,
  .will_detach            = false,
  .dfu_mode               = true,
  .detach_timeout_ms      = 1000U,
  .transfer_bytes         = 64U,
  .bcd_dfu                = 0x0110U,
};

/** @brief Scratch buffers one composition writes into. */
typedef struct {
  uint8_t device[k_ra8_usb_desc_framework_bytes_max];
  uint8_t strings[k_ra8_usb_desc_strings_bytes_max];
  uint8_t langid[k_ra8_usb_desc_langid_bytes];
} internal_bufs_t;

/**
 * @brief Point a frameworks struct at @p bufs with full caps.
 * @param[in,out] bufs Storage that outlives the returned struct.
 * @return ra8_usb_device_frameworks_t Buffers and caps, lengths zeroed.
 */
static ra8_usb_device_frameworks_t internal_fw(internal_bufs_t* bufs)
{
  return (ra8_usb_device_frameworks_t){
    .device      = bufs->device,
    .device_cap  = (uint32_t)sizeof(bufs->device),
    .strings     = bufs->strings,
    .strings_cap = (uint32_t)sizeof(bufs->strings),
    .langid      = bufs->langid,
    .langid_cap  = (uint32_t)sizeof(bufs->langid),
  };
}

/**
 * @brief Composing a class must equal calling that class's builder by hand.
 * @param[in] name Case label for the failure message.
 * @param[in] cls  The single class entry to compose.
 * @param[in] want Bytes the matching builder produced.
 * @param[in] want_len Length of @p want.
 */
static void internal_expect_same(const char*            name,
                                 const ra8_usb_class_t* cls,
                                 const uint8_t*         want,
                                 uint32_t               want_len)
{
  internal_bufs_t             bufs = {};
  ra8_usb_device_frameworks_t fw   = internal_fw(&bufs);
  const ra8_usb_device_cfg_t  cfg  = {
    .desc        = &k_fixture_dev,
    .classes     = cls,
    .class_count = 1U,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_compose(&cfg, &fw));
  TEST_ASSERT_EQ(want_len, fw.device_len);
  for (uint32_t i = 0U; i < want_len; i++) {
    if (fw.device[i] != want[i]) {
      printf("  %s byte %u: got %02X want %02X\n", name, i, fw.device[i], want[i]);
    }
    TEST_ASSERT_EQ(want[i], fw.device[i]);
  }
}

/** @brief All four class kinds compose to their builder's bytes. */
static void internal_test_every_kind_matches_its_builder(void)
{
  TEST_BEGIN("every class kind composes to the bytes its own builder emits");

  uint8_t  want[k_ra8_usb_desc_framework_bytes_max] = {};
  uint32_t want_len                                 = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_cdc_acm(&k_fixture_dev,
                                            &k_fixture_cdc,
                                            want,
                                            (uint32_t)sizeof(want),
                                            &want_len));
  const ra8_usb_class_t cdc = {.kind = k_ra8_usb_class_cdc_acm, .cdc_acm = k_fixture_cdc};
  internal_expect_same("cdc", &cdc, want, want_len);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_msc(&k_fixture_dev,
                                        &k_fixture_msc,
                                        want,
                                        (uint32_t)sizeof(want),
                                        &want_len));
  const ra8_usb_class_t msc = {.kind = k_ra8_usb_class_msc, .msc = k_fixture_msc};
  internal_expect_same("msc", &msc, want, want_len);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_hid(&k_fixture_dev,
                                        &k_fixture_hid,
                                        want,
                                        (uint32_t)sizeof(want),
                                        &want_len));
  const ra8_usb_class_t hid = {.kind = k_ra8_usb_class_hid, .hid = k_fixture_hid};
  internal_expect_same("hid", &hid, want, want_len);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_dfu(&k_fixture_dev,
                                        &k_fixture_dfu,
                                        want,
                                        (uint32_t)sizeof(want),
                                        &want_len));
  const ra8_usb_class_t dfu = {.kind = k_ra8_usb_class_dfu, .dfu = k_fixture_dfu};
  internal_expect_same("dfu", &dfu, want, want_len);

  TEST_END("cdc, msc, hid and dfu each match their builder byte for byte");
}

/** @brief The strings and language id come out with the device framework. */
static void internal_test_all_three_frameworks(void)
{
  TEST_BEGIN("one call fills all three frameworks the stack needs");

  internal_bufs_t             bufs = {};
  ra8_usb_device_frameworks_t fw   = internal_fw(&bufs);
  const ra8_usb_class_t       cdc  = {.kind = k_ra8_usb_class_cdc_acm, .cdc_acm = k_fixture_cdc};
  const ra8_usb_device_cfg_t  cfg  = {
    .desc        = &k_fixture_dev,
    .classes     = &cdc,
    .class_count = 1U,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_compose(&cfg, &fw));

  /* The three lengths usb_cdc_echo carried by hand. */
  TEST_ASSERT_EQ(93U, fw.device_len);
  TEST_ASSERT_EQ(56U, fw.strings_len);
  TEST_ASSERT_EQ(2U, fw.langid_len);

  /* String framework: LANGID, index, length, then the manufacturer. */
  TEST_ASSERT_EQ(0x09U, fw.strings[0]);
  TEST_ASSERT_EQ(0x04U, fw.strings[1]);
  TEST_ASSERT_EQ(k_ra8_usb_desc_str_manufacturer, fw.strings[2]);
  TEST_ASSERT_EQ('B', fw.strings[4]);

  /* Language-id framework: 0x0409 little-endian. */
  TEST_ASSERT_EQ(0x09U, fw.langid[0]);
  TEST_ASSERT_EQ(0x04U, fw.langid[1]);

  TEST_END("device 93, strings 56, langid 2, contents as the app published them");
}

/** @brief Every documented refusal returns its documented code. */
static void internal_test_refusals(void)
{
  TEST_BEGIN("a configuration the encoders cannot serve is refused, not guessed");

  internal_bufs_t             bufs = {};
  ra8_usb_device_frameworks_t fw   = internal_fw(&bufs);
  const ra8_usb_class_t       cdc  = {.kind = k_ra8_usb_class_cdc_acm, .cdc_acm = k_fixture_cdc};
  const ra8_usb_device_cfg_t  good = {
    .desc        = &k_fixture_dev,
    .classes     = &cdc,
    .class_count = 1U,
  };

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(NULL, &fw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&good, NULL));

  ra8_usb_device_cfg_t bad = good;
  bad.desc                 = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&bad, &fw));

  bad         = good;
  bad.classes = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&bad, &fw));

  bad             = good;
  bad.class_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&bad, &fw));

  /* An unset kind is a half-filled array entry, not a CDC port. */
  const ra8_usb_class_t unset = {.kind = k_ra8_usb_class_none, .cdc_acm = k_fixture_cdc};
  bad                         = good;
  bad.classes                 = &unset;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&bad, &fw));

  /* Composite is the shape the config allows and the encoders do not model. */
  const ra8_usb_class_t pair[2] = {cdc, cdc};
  bad                           = good;
  bad.classes                   = pair;
  bad.class_count               = 2U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_device_compose(&bad, &fw));

  TEST_END("NULLs, a zero count, an unset kind and a composite set all refuse");
}

/** @brief A missing or undersized buffer refuses before any byte is written. */
static void internal_test_buffer_refusals(void)
{
  TEST_BEGIN("a missing or undersized buffer refuses instead of writing part of a framework");

  internal_bufs_t            bufs = {};
  const ra8_usb_class_t      cdc  = {.kind = k_ra8_usb_class_cdc_acm, .cdc_acm = k_fixture_cdc};
  const ra8_usb_device_cfg_t good = {
    .desc        = &k_fixture_dev,
    .classes     = &cdc,
    .class_count = 1U,
  };

  /* A NULL buffer is caught before any encode runs. */
  ra8_usb_device_frameworks_t no_langid = internal_fw(&bufs);
  no_langid.langid                      = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_compose(&good, &no_langid));
  TEST_ASSERT_EQ(0U, no_langid.device_len);

  /* A cap too small surfaces the encoder's own refusal, one per framework. */
  ra8_usb_device_frameworks_t tight = internal_fw(&bufs);
  tight.device_cap                  = 8U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_usb_device_compose(&good, &tight));

  tight             = internal_fw(&bufs);
  tight.strings_cap = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_usb_device_compose(&good, &tight));

  tight            = internal_fw(&bufs);
  tight.langid_cap = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_usb_device_compose(&good, &tight));

  TEST_END("a NULL buffer and each undersized cap return their documented code");
}

int main(void)
{
  internal_test_every_kind_matches_its_builder();
  internal_test_all_three_frameworks();
  internal_test_refusals();
  internal_test_buffer_refusals();
  return 0;
}
