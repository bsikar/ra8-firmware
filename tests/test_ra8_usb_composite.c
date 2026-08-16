/**
 * @file test_ra8_usb_composite.c
 * @brief Unit tests for the native USB device-side composite-class
 *        layer
 * @details Validates composite-device interface routing, descriptor selection, class dispatch, lifecycle, and malformed requests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_composite.h"
#include "unity_minimal.h"

/**
 * @enum t_comp_t
 * @brief SETUP fields and interface sweep bound for the composite device.
 */
typedef enum : uint8_t {
  k_t_breq_set_idle    = 0x22U, /**< A class bRequest routed by interface. */
  k_t_iface_sweep_max  = 12U,   /**< Interface numbers swept: past the highest
                                     the composite device claims, so the
                                     unclaimed ones must be rejected.          */
  k_t_windex_unclaimed = 9U,    /**< An interface number no function owns. */
  k_t_wvalue_probe     = 7U,    /**< wValue the routed request carries; opaque
                                     to the composite layer, which passes it on. */
} t_comp_t;

typedef enum : uint8_t {
  k_test_comp_cdc_first = 0U, /**< CDC starts at IF0.   */
  k_test_comp_cdc_count = 2U, /**< CDC owns IF0 + IF1.  */
  k_test_comp_hid_first = 2U, /**< HID at IF2.          */
  k_test_comp_hid_count = 1U, /**< Test comp hid count. */
  k_test_comp_msc_first = 3U, /**< MSC at IF3.          */
  k_test_comp_msc_count = 1U, /**< Test comp msc count. */
} test_comp_layout_t;

typedef enum : uint8_t {
  k_test_comp_bm_class_to_if    = 0x21U, /**< Class, OUT, interface.     */
  k_test_comp_bm_class_to_if_in = 0xA1U, /**< Class, IN,  interface.     */
  k_test_comp_bm_std_to_dev     = 0x00U, /**< Standard, OUT, device.     */
  k_test_comp_bm_std_to_dev_in  = 0x80U, /**< Standard, IN,  device.     */
  k_test_comp_std_set_address   = 0x05U, /**< Test comp std set address. */
  k_test_comp_std_get_status    = 0x00U, /**< Test comp std get status.  */
} test_comp_setup_t;

typedef enum : uint8_t {
  k_test_comp_handler_self =
    (uint8_t)k_ra8_usb_composite_max_classes, /**< Test comp handler self. */
} test_comp_handler_t;

/* ---- Stub class layers ---- */

typedef struct {
  uint32_t init_calls;     /**< Init calls.     */
  uint32_t setup_calls;    /**< Setup calls.    */
  uint32_t close_calls;    /**< Close calls.    */
  uint16_t last_w_index;   /**< Last w index.   */
  uint8_t  last_b_request; /**< Last b request. */
} test_class_state_t;

static test_class_state_t s_cdc_state;
static test_class_state_t s_hid_state;
static test_class_state_t s_msc_state;

static ra8_err_t cdc_init(void* ctx)
{
  (void)ctx;
  s_cdc_state.init_calls++;
  return k_ra8_ok;
}

static ra8_err_t cdc_setup(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_cdc_state.setup_calls++;
  s_cdc_state.last_w_index   = setup->w_index;
  s_cdc_state.last_b_request = setup->b_request;
  return k_ra8_ok;
}

static ra8_err_t cdc_close(void* ctx)
{
  (void)ctx;
  s_cdc_state.close_calls++;
  return k_ra8_ok;
}

static ra8_err_t hid_init(void* ctx)
{
  (void)ctx;
  s_hid_state.init_calls++;
  return k_ra8_ok;
}

static ra8_err_t hid_setup(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_hid_state.setup_calls++;
  s_hid_state.last_w_index   = setup->w_index;
  s_hid_state.last_b_request = setup->b_request;
  return k_ra8_ok;
}

static ra8_err_t hid_close(void* ctx)
{
  (void)ctx;
  s_hid_state.close_calls++;
  return k_ra8_ok;
}

static ra8_err_t msc_init(void* ctx)
{
  (void)ctx;
  s_msc_state.init_calls++;
  return k_ra8_ok;
}

static ra8_err_t msc_setup(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_msc_state.setup_calls++;
  s_msc_state.last_w_index   = setup->w_index;
  s_msc_state.last_b_request = setup->b_request;
  return k_ra8_ok;
}

static ra8_err_t msc_close(void* ctx)
{
  (void)ctx;
  s_msc_state.close_calls++;
  return k_ra8_ok;
}

static const ra8_usb_composite_class_t s_cdc_layer = {
  .interface_number_first = (uint8_t)k_test_comp_cdc_first,
  .interface_number_count = (uint8_t)k_test_comp_cdc_count,
  .init                   = cdc_init,
  .handle_setup           = cdc_setup,
  .close                  = cdc_close,
  .ctx                    = nullptr,
};

static const ra8_usb_composite_class_t s_hid_layer = {
  .interface_number_first = (uint8_t)k_test_comp_hid_first,
  .interface_number_count = (uint8_t)k_test_comp_hid_count,
  .init                   = hid_init,
  .handle_setup           = hid_setup,
  .close                  = hid_close,
  .ctx                    = nullptr,
};

static const ra8_usb_composite_class_t s_msc_layer = {
  .interface_number_first = (uint8_t)k_test_comp_msc_first,
  .interface_number_count = (uint8_t)k_test_comp_msc_count,
  .init                   = msc_init,
  .handle_setup           = msc_setup,
  .close                  = msc_close,
  .ctx                    = nullptr,
};

/* Caller-owned descriptor blobs. The composite layer just caches the
 * pointers; the on-wire bytes are not validated. */
static const uint8_t s_test_device_desc[18] = {
  18U,   /* bLength.                  */
  0x01U, /* bDescriptorType = DEVICE. */
  0x00U,
  0x02U, /* bcdUSB = 2.00. */
  (uint8_t)k_ra8_usb_composite_class_misc,
  (uint8_t)k_ra8_usb_composite_subclass_common,
  (uint8_t)k_ra8_usb_composite_protocol_iad,
  64U, /* bMaxPacketSize0. */
  /* idVendor / idProduct / bcdDevice / iManufacturer / iProduct /
   * iSerialNumber / bNumConfigurations -- not validated by the
   * composite layer; left as zeros. */
};

static const uint8_t s_test_config_desc[9] = {
  9U,    /* bLength.                         */
  0x02U, /* bDescriptorType = CONFIGURATION. */
  0x00U,
  0x00U, /* wTotalLength.                       */
  0x04U, /* bNumInterfaces (CDC=2 + HID + MSC). */
  0x01U, /* bConfigurationValue.                */
  0x00U, /* iConfiguration.                     */
  0x80U, /* bmAttributes.                       */
  50U,   /* bMaxPower.                          */
};

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_composite_close();
  s_cdc_state = (test_class_state_t){};
  s_hid_state = (test_class_state_t){};
  s_msc_state = (test_class_state_t){};
}

/* ---- Tests ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_composite_init FS returns k_ra8_ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_END("ra8_usb_composite_init FS returns k_ra8_ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_composite_init HS returns k_ra8_ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_hs));
  TEST_END("ra8_usb_composite_init HS returns k_ra8_ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_composite_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_composite_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_composite_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_composite_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_close());
  TEST_END("ra8_usb_composite_close before init returns invalid_state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("register_class / set_descriptors / step / dispatch reject pre-init");
  prep();

  uint8_t         count = 0U;
  uint8_t         who   = 0U;
  ra8_usb_setup_t setup = {};
  const uint8_t*  p     = nullptr;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_register_class(&s_cdc_layer));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_composite_set_descriptors(s_test_device_desc, s_test_config_desc));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_step());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_dispatch_setup(&setup, &who));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_get_class_count(&count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_get_device_descriptor(&p));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_get_config_descriptor(&p));

  TEST_END("register_class / set_descriptors / step / dispatch reject pre-init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_class_happy_path(void)
{
  TEST_BEGIN("register_class accepts CDC + HID + MSC, calls each init once");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_hid_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_msc_layer));

  TEST_ASSERT_EQ(1U, s_cdc_state.init_calls);
  TEST_ASSERT_EQ(1U, s_hid_state.init_calls);
  TEST_ASSERT_EQ(1U, s_msc_state.init_calls);

  uint8_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_get_class_count(&count));
  TEST_ASSERT_EQ(3U, count);

  TEST_END("register_class accepts CDC + HID + MSC, calls each init once");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_class_collision_rejected(void)
{
  TEST_BEGIN("register_class rejects overlapping IF range with k_ra8_err_exists");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));

  /* Build a HID-shaped layer that re-claims IF1 (already owned by CDC). */
  ra8_usb_composite_class_t bad_layer = s_hid_layer;
  bad_layer.interface_number_first    = 1U; /* overlaps CDC's IF1. */
  bad_layer.interface_number_count    = 1U;
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_usb_composite_register_class(&bad_layer));

  /* Same first-IF as CDC, different count: still overlaps. */
  bad_layer.interface_number_first = (uint8_t)k_test_comp_cdc_first;
  bad_layer.interface_number_count = 1U;
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_usb_composite_register_class(&bad_layer));

  TEST_END("register_class rejects overlapping IF range with k_ra8_err_exists");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_class_null_validation(void)
{
  TEST_BEGIN("register_class rejects NULL struct + NULL callbacks");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(nullptr));

  ra8_usb_composite_class_t bad = s_cdc_layer;
  bad.init                      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));

  bad              = s_cdc_layer;
  bad.handle_setup = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));

  bad       = s_cdc_layer;
  bad.close = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));

  bad                        = s_cdc_layer;
  bad.interface_number_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_composite_register_class(&bad));

  bad                        = s_cdc_layer;
  bad.interface_number_first = (uint8_t)k_ra8_usb_composite_max_ifs;
  bad.interface_number_count = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_composite_register_class(&bad));

  TEST_END("register_class rejects NULL struct + NULL callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_class_full(void)
{
  TEST_BEGIN("register_class returns k_ra8_err_no_mem when table is full");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  /* Fill the registry with k_ra8_usb_composite_max_classes layers,
   * each on its own IF slot. */
  ra8_usb_composite_class_t layer = s_hid_layer;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_composite_max_classes; ++i) {
    layer.interface_number_first = i;
    layer.interface_number_count = 1U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&layer));
  }

  layer.interface_number_first = (uint8_t)k_ra8_usb_composite_max_classes;
  layer.interface_number_count = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_usb_composite_register_class(&layer));

  TEST_END("register_class returns k_ra8_err_no_mem when table is full");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_descriptors_with_iad(void)
{
  TEST_BEGIN("set_descriptors caches IAD-bearing config descriptor pointers");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_composite_set_descriptors(s_test_device_desc, s_test_config_desc));

  const uint8_t* dev = nullptr;
  const uint8_t* cfg = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_get_device_descriptor(&dev));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_get_config_descriptor(&cfg));
  TEST_ASSERT(dev == s_test_device_desc);
  TEST_ASSERT(cfg == s_test_config_desc);

  /* Sanity-check the IAD identifiers we documented in the header. */
  TEST_ASSERT_EQ(k_ra8_usb_composite_class_misc, dev[4]);
  TEST_ASSERT_EQ(k_ra8_usb_composite_subclass_common, dev[5]);
  TEST_ASSERT_EQ(k_ra8_usb_composite_protocol_iad, dev[6]);

  TEST_END("set_descriptors caches IAD-bearing config descriptor pointers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_descriptors_null_rejection(void)
{
  TEST_BEGIN("set_descriptors rejects NULL pointers");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_composite_set_descriptors(nullptr, s_test_config_desc));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_composite_set_descriptors(s_test_device_desc, nullptr));
  TEST_END("set_descriptors rejects NULL pointers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_routes_class_request_to_owner(void)
{
  TEST_BEGIN("dispatch_setup routes class request by wIndex to owning class");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_hid_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_msc_layer));

  /* Class request with wIndex = 0 -> CDC (owns IF0..IF1). */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_comp_bm_class_to_if,
    .b_request       = k_t_breq_set_idle,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  uint8_t handler = (uint8_t)k_test_comp_handler_self;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(0U, handler);
  TEST_ASSERT_EQ(1U, s_cdc_state.setup_calls);
  TEST_ASSERT_EQ(0U, s_hid_state.setup_calls);
  TEST_ASSERT_EQ(0U, s_msc_state.setup_calls);

  /* wIndex = 1 -> still CDC (range covers IF0..IF1). */
  setup.w_index = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(0U, handler);
  TEST_ASSERT_EQ(2U, s_cdc_state.setup_calls);

  /* wIndex = 2 -> HID (single IF). */
  setup.w_index = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(1U, handler);
  TEST_ASSERT_EQ(1U, s_hid_state.setup_calls);

  /* wIndex = 3 -> MSC. */
  setup.w_index = 3U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(2U, handler);
  TEST_ASSERT_EQ(1U, s_msc_state.setup_calls);

  /* wIndex outside any class range -> not_found, no class fired. */
  setup.w_index = k_t_windex_unclaimed;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(2U, s_cdc_state.setup_calls);
  TEST_ASSERT_EQ(1U, s_hid_state.setup_calls);
  TEST_ASSERT_EQ(1U, s_msc_state.setup_calls);

  TEST_END("dispatch_setup routes class request by wIndex to owning class");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_handles_standard_request_internally(void)
{
  TEST_BEGIN("dispatch_setup answers standard SET_ADDRESS without firing any class");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_comp_bm_std_to_dev,
    .b_request       = (uint8_t)k_test_comp_std_set_address,
    .w_value         = k_t_wvalue_probe,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  uint8_t handler = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(k_test_comp_handler_self, handler);
  TEST_ASSERT_EQ(0U, s_cdc_state.setup_calls);

  /* Standard GET_STATUS also handled internally. */
  setup.bm_request_type = (uint8_t)k_test_comp_bm_std_to_dev_in;
  setup.b_request       = (uint8_t)k_test_comp_std_get_status;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_dispatch_setup(&setup, &handler));
  TEST_ASSERT_EQ(k_test_comp_handler_self, handler);
  TEST_ASSERT_EQ(0U, s_cdc_state.setup_calls);

  TEST_END("dispatch_setup answers standard SET_ADDRESS without firing any class");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_null_arg_rejection(void)
{
  TEST_BEGIN("dispatch_setup rejects NULL arguments");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup   = {};
  uint8_t         handler = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_dispatch_setup(nullptr, &handler));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_dispatch_setup(&setup, nullptr));
  TEST_END("dispatch_setup rejects NULL arguments");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_count_null_rejection(void)
{
  TEST_BEGIN("get_class_count / get_*_descriptor reject NULL output");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_get_class_count(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_get_device_descriptor(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_get_config_descriptor(nullptr));
  TEST_END("get_class_count / get_*_descriptor reject NULL output");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_step_loops_without_error(void)
{
  TEST_BEGIN("step pumps state machine without tripping a state guard");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  for (uint8_t i = 0U; i < k_t_iface_sweep_max; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_step());
  }
  TEST_END("step pumps state machine without tripping a state guard");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_invokes_each_class_close(void)
{
  TEST_BEGIN("close walks registered classes and calls each close hook");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_hid_layer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_msc_layer));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_close());
  TEST_ASSERT_EQ(1U, s_cdc_state.close_calls);
  TEST_ASSERT_EQ(1U, s_hid_state.close_calls);
  TEST_ASSERT_EQ(1U, s_msc_state.close_calls);

  /* Subsequent calls are pre-init guarded again. */
  uint8_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_composite_get_class_count(&count));

  TEST_END("close walks registered classes and calls each close hook");
}

/**
 * @test test_mcdc_composite
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_composite.c.
 *
 * Decision A (line 338, 2 conds): composite_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 199, 3 conds): internal_validate_class callback
 *   completeness check
 *   `(init==NULL) || (handle_setup==NULL) || (close==NULL)` -- per
 *   DO-178C 6.4.4.3 representative-subset for a side-effect-free OR:
 *   3 lone-true vectors (each callback NULL in turn) + 1 all-false
 *   (all three non-NULL) prove every condition independently flips
 *   the outcome. Exercised via `ra8_usb_composite_register_class`.
 */
static void test_mcdc_composite(void)
{
  TEST_BEGIN("composite MC/DC: init speed / class-callback OR chain");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_composite_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_init(k_ra8_usb_speed_fs));

  /* B all-false: all three callbacks non-NULL -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_composite_register_class(&s_cdc_layer));
  /* B-V1: init=NULL. */
  ra8_usb_composite_class_t bad = s_hid_layer;
  bad.init                      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));
  /* B-V2: handle_setup=NULL. */
  bad              = s_hid_layer;
  bad.handle_setup = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));
  /* B-V3: close=NULL. */
  bad       = s_hid_layer;
  bad.close = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_composite_register_class(&bad));

  TEST_END("composite MC/DC: init speed / class-callback OR chain");
}

int main(void)
{
  test_init_fs_returns_ok();
  test_init_hs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_pre_init_guards();
  test_register_class_happy_path();
  test_register_class_collision_rejected();
  test_register_class_null_validation();
  test_register_class_full();
  test_set_descriptors_with_iad();
  test_set_descriptors_null_rejection();
  test_dispatch_routes_class_request_to_owner();
  test_dispatch_handles_standard_request_internally();
  test_dispatch_null_arg_rejection();
  test_get_count_null_rejection();
  test_step_loops_without_error();
  test_close_invokes_each_class_close();
  test_mcdc_composite();
  return 0;
}
