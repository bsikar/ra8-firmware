/**
 * @file test_ra8_usb_device_open.c
 * @brief Host tests for ``ra8_usb_device_open`` against USBX-shaped C stubs (RA8FW-317).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------- */
/* USBX-shaped C stubs exercised through ra8_usb_stack_ops_t */
/* -------------------------------------------------------------------------- */

static struct {
  char                      seq[16];
  size_t                    seq_len;
  uint32_t                  ux_success_code;
  uint32_t                  stub_sys_rc;
  uint32_t                  stub_stack_rc;
  uint32_t                  stub_reg_rc;
  ra8_err_t                 dcd_rc;
  void*                     last_pool;
  unsigned long             last_pool_bytes;
  uint8_t*                  last_hs_fw;
  unsigned long             last_hs_len;
  uint8_t*                  last_fs_fw;
  unsigned long             last_fs_len;
  uint8_t*                  last_strings_fw;
  unsigned long             last_strings_len;
  uint8_t*                  last_langid_fw;
  unsigned long             last_langid_len;
  char                      last_class_name[64];
  void*                     last_entry;
  unsigned long             last_cfg;
  unsigned long             last_iface;
  void*                     last_param;
  ra8_usb_device_port_t     last_dcd_port;
  ra8_usb_device_error_cb_t registered_error_cb;
  uint32_t                  err_cb_hits;
  uint32_t                  err_cb_code;
} s_stub;

static void stub_reset(void)
{
  (void)memset(&s_stub, 0, sizeof(s_stub));
  s_stub.last_dcd_port = (ra8_usb_device_port_t)255U;
}

static void stub_push(char ch)
{
  if (s_stub.seq_len + 1U < sizeof(s_stub.seq)) {
    s_stub.seq[s_stub.seq_len++] = ch;
    s_stub.seq[s_stub.seq_len]   = '\0';
  }
}

static uint32_t stub_ux_system_initialize(void*         pool,
                                          unsigned long pool_bytes,
                                          void*         cache_safe_pool,
                                          unsigned long cache_safe_size)
{
  (void)cache_safe_pool;
  (void)cache_safe_size;
  stub_push('S');
  s_stub.last_pool       = pool;
  s_stub.last_pool_bytes = pool_bytes;
  return s_stub.stub_sys_rc;
}

static void stub_ux_utility_error_callback_register(ra8_usb_device_error_cb_t cb)
{
  stub_push('E');
  s_stub.registered_error_cb = cb;
}

static uint32_t stub_ux_device_stack_initialize(uint8_t*      hs_fw,
                                                unsigned long hs_len,
                                                uint8_t*      fs_fw,
                                                unsigned long fs_len,
                                                uint8_t*      strings_fw,
                                                unsigned long strings_len,
                                                uint8_t*      langid_fw,
                                                unsigned long langid_len,
                                                void*         change_cb)
{
  (void)change_cb;
  stub_push('D');
  s_stub.last_hs_fw       = hs_fw;
  s_stub.last_hs_len      = hs_len;
  s_stub.last_fs_fw       = fs_fw;
  s_stub.last_fs_len      = fs_len;
  s_stub.last_strings_fw  = strings_fw;
  s_stub.last_strings_len = strings_len;
  s_stub.last_langid_fw   = langid_fw;
  s_stub.last_langid_len  = langid_len;
  return s_stub.stub_stack_rc;
}

static uint32_t stub_ux_device_stack_class_register(const char*   name,
                                                    void*         entry,
                                                    unsigned long cfg,
                                                    unsigned long iface,
                                                    void*         param)
{
  stub_push('R');
  (void)memset(s_stub.last_class_name, 0, sizeof(s_stub.last_class_name));
  if (name != nullptr) {
    for (size_t i = 0U; (i + 1U < sizeof(s_stub.last_class_name)) && (name[i] != '\0'); i++) {
      s_stub.last_class_name[i] = name[i];
    }
  }
  s_stub.last_entry = entry;
  s_stub.last_cfg   = cfg;
  s_stub.last_iface = iface;
  s_stub.last_param = param;
  return s_stub.stub_reg_rc;
}

static ra8_err_t ops_system_init(void* pool, uint32_t pool_bytes)
{
  return (stub_ux_system_initialize(pool, (unsigned long)pool_bytes, nullptr, 0UL) == 0U)
           ? k_ra8_ok
           : k_ra8_err_invalid_state;
}

static ra8_err_t ops_error_cb_register(ra8_usb_device_error_cb_t cb)
{
  stub_ux_utility_error_callback_register(cb);
  return k_ra8_ok;
}

static ra8_err_t ops_device_stack_init(uint8_t* hs_fw,
                                       uint32_t hs_len,
                                       uint8_t* fs_fw,
                                       uint32_t fs_len,
                                       uint8_t* strings_fw,
                                       uint32_t strings_len,
                                       uint8_t* langid_fw,
                                       uint32_t langid_len)
{
  uint32_t rc = stub_ux_device_stack_initialize(hs_fw,
                                                (unsigned long)hs_len,
                                                fs_fw,
                                                (unsigned long)fs_len,
                                                strings_fw,
                                                (unsigned long)strings_len,
                                                langid_fw,
                                                (unsigned long)langid_len,
                                                nullptr);
  return (rc == 0U) ? k_ra8_ok : k_ra8_err_invalid_state;
}

static ra8_err_t ops_class_register(const char*                     name,
                                    ra8_usb_device_class_entry_fn_t entry,
                                    uint8_t                         configuration,
                                    uint8_t                         interface,
                                    void*                           param)
{
  uint32_t rc = stub_ux_device_stack_class_register(name,
                                                    (void*)(uintptr_t)entry,
                                                    (unsigned long)configuration,
                                                    (unsigned long)interface,
                                                    param);
  return (rc == 0U) ? k_ra8_ok : k_ra8_err_invalid_state;
}

static ra8_err_t ops_dcd_open(ra8_usb_device_port_t port)
{
  stub_push('O');
  s_stub.last_dcd_port = port;
  return s_stub.dcd_rc;
}

static uint32_t stub_cdc_entry(void* cmd)
{
  (void)cmd;
  return 0U;
}

static uint32_t stub_msc_entry(void* cmd)
{
  (void)cmd;
  return 0U;
}

static void stub_app_error_cb(uint32_t level, uint32_t ctx, uint32_t code)
{
  (void)level;
  (void)ctx;
  s_stub.err_cb_hits++;
  s_stub.err_cb_code = code;
}

static const ra8_usb_stack_ops_t k_full_ops = {
  .system_init       = ops_system_init,
  .error_cb_register = ops_error_cb_register,
  .device_stack_init = ops_device_stack_init,
  .class_register    = ops_class_register,
  .dcd_open          = ops_dcd_open,
};

static const ra8_usb_stack_ops_t k_no_dcd_ops = {
  .system_init       = ops_system_init,
  .error_cb_register = nullptr,
  .device_stack_init = ops_device_stack_init,
  .class_register    = ops_class_register,
  .dcd_open          = nullptr,
};

static const ra8_usb_device_class_driver_t k_cdc_driver = {
  .name  = "ux_slave_class_cdc_acm",
  .entry = stub_cdc_entry,
};

static const ra8_usb_device_class_driver_t k_msc_driver = {
  .name  = "ux_slave_class_storage",
  .entry = stub_msc_entry,
};

static const ra8_usb_desc_device_t k_test_dev = {
  .vid           = 0x1209U,
  .pid           = 0x000AU,
  .bcd_device    = 0x0100U,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "EK-RA8D2 USB Open",
  .serial        = "00000001",
  .langid        = k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = 100U,
  .self_powered  = false,
  .remote_wakeup = false,
};

static const ra8_usb_class_t k_test_cdc = {
  .kind = k_ra8_usb_class_cdc_acm,
  .cdc_acm =
    {
      .notify_ep          = 0x83U,
      .notify_bytes       = 8U,
      .notify_interval_ms = 255U,
      .out_ep             = 0x02U,
      .in_ep              = 0x81U,
      .data_bytes         = 64U,
      .high_speed         = false,
    },
};

typedef struct {
  uint8_t pool[16384];
  uint8_t fs_device[k_ra8_usb_desc_framework_bytes_max];
  uint8_t hs_device[k_ra8_usb_desc_framework_bytes_max];
  uint8_t strings[k_ra8_usb_desc_strings_bytes_max];
  uint8_t langid[k_ra8_usb_desc_langid_bytes];
  uint8_t class_param[32];
} test_storage_t;

static test_storage_t s_store;

static ra8_usb_device_frameworks_t make_fw(void)
{
  return (ra8_usb_device_frameworks_t){
    .device      = s_store.fs_device,
    .device_cap  = (uint32_t)sizeof(s_store.fs_device),
    .strings     = s_store.strings,
    .strings_cap = (uint32_t)sizeof(s_store.strings),
    .langid      = s_store.langid,
    .langid_cap  = (uint32_t)sizeof(s_store.langid),
  };
}

static void test_open_composes_and_initialises_fs_cdc(void)
{
  TEST_BEGIN("ra8_usb_device_open composes frameworks and runs stack sequence for FS CDC");
  stub_reset();

  const ra8_usb_device_cfg_t dev_cfg = {
    .desc        = &k_test_dev,
    .classes     = &k_test_cdc,
    .class_count = 1U,
  };
  ra8_usb_device_frameworks_t     fw  = make_fw();
  const ra8_usb_device_open_cfg_t cfg = {
    .port          = k_ra8_usb_device_port_fs,
    .configuration = 1U,
    .interface     = 0U,
    .pool_bytes    = (uint32_t)sizeof(s_store.pool),
    .pool          = s_store.pool,
    .device_cfg    = &dev_cfg,
    .frameworks    = &fw,
    .ops           = &k_full_ops,
    .class_driver  = &k_cdc_driver,
    .class_param   = s_store.class_param,
    .error_cb      = stub_app_error_cb,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_open(&cfg));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "SEDRO"));
  TEST_ASSERT_EQ(s_store.pool, s_stub.last_pool);
  TEST_ASSERT_EQ((unsigned long)sizeof(s_store.pool), s_stub.last_pool_bytes);
  TEST_ASSERT_EQ(NULL, s_stub.last_hs_fw);
  TEST_ASSERT_EQ(0UL, s_stub.last_hs_len);
  TEST_ASSERT_EQ(s_store.fs_device, s_stub.last_fs_fw);
  TEST_ASSERT_EQ((unsigned long)fw.device_len, s_stub.last_fs_len);
  TEST_ASSERT_EQ((unsigned long)fw.strings_len, s_stub.last_strings_len);
  TEST_ASSERT_EQ((unsigned long)fw.langid_len, s_stub.last_langid_len);
  TEST_ASSERT_EQ(0, strcmp(s_stub.last_class_name, "ux_slave_class_cdc_acm"));
  TEST_ASSERT_EQ((void*)(uintptr_t)stub_cdc_entry, s_stub.last_entry);
  TEST_ASSERT_EQ(1UL, s_stub.last_cfg);
  TEST_ASSERT_EQ(0UL, s_stub.last_iface);
  TEST_ASSERT_EQ(s_store.class_param, s_stub.last_param);
  TEST_ASSERT_EQ(k_ra8_usb_device_port_fs, s_stub.last_dcd_port);
  TEST_ASSERT_EQ((void*)(uintptr_t)stub_app_error_cb, (void*)(uintptr_t)s_stub.registered_error_cb);

  s_stub.registered_error_cb(1U, 2U, 0x29U);
  TEST_ASSERT_EQ(1U, s_stub.err_cb_hits);
  TEST_ASSERT_EQ(0x29U, s_stub.err_cb_code);
  TEST_END("FS CDC compose+open passes exact pool, frameworks, class, and callback");
}

static void test_open_dual_speed_hs_with_prebuilt_frameworks_and_no_dcd(void)
{
  TEST_BEGIN("ra8_usb_device_open supports dual-speed HS pre-built frameworks and optional dcd");
  stub_reset();

  const ra8_usb_class_t msc_fs = {
    .kind = k_ra8_usb_class_msc,
    .msc  = {.in_ep = 0x81U, .out_ep = 0x02U, .data_bytes = 64U, .high_speed = false},
  };
  const ra8_usb_class_t msc_hs = {
    .kind = k_ra8_usb_class_msc,
    .msc  = {.in_ep = 0x81U, .out_ep = 0x02U, .data_bytes = 512U, .high_speed = true},
  };

  const ra8_usb_device_cfg_t  fs_cfg = {.desc = &k_test_dev, .classes = &msc_fs, .class_count = 1U};
  ra8_usb_device_frameworks_t fw     = make_fw();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_compose(&fs_cfg, &fw));

  uint32_t hs_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_desc_build_msc(&k_test_dev,
                                        &msc_hs.msc,
                                        s_store.hs_device,
                                        sizeof(s_store.hs_device),
                                        &hs_len));

  const ra8_usb_device_open_cfg_t cfg = {
    .port                    = k_ra8_usb_device_port_hs,
    .configuration           = 1U,
    .interface               = 0U,
    .pool_bytes              = (uint32_t)sizeof(s_store.pool),
    .pool                    = s_store.pool,
    .device_cfg              = nullptr,
    .frameworks              = &fw,
    .hs_device_framework     = s_store.hs_device,
    .hs_device_framework_len = hs_len,
    .ops                     = &k_no_dcd_ops,
    .class_driver            = &k_msc_driver,
    .class_param             = s_store.class_param,
    .error_cb                = nullptr,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_open(&cfg));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "SDR"));
  TEST_ASSERT_EQ(s_store.hs_device, s_stub.last_hs_fw);
  TEST_ASSERT_EQ((unsigned long)hs_len, s_stub.last_hs_len);
  TEST_ASSERT_EQ(s_store.fs_device, s_stub.last_fs_fw);
  TEST_ASSERT_EQ(0, strcmp(s_stub.last_class_name, "ux_slave_class_storage"));
  TEST_END("HS pre-built MSC open forwarded both speed frameworks and skipped NULL DCD/err hooks");
}

static void test_open_refusals_and_failure_propagation(void)
{
  TEST_BEGIN("ra8_usb_device_open refuses bad args and propagates stage failures cleanly");
  stub_reset();

  const ra8_usb_device_cfg_t dev_cfg = {
    .desc        = &k_test_dev,
    .classes     = &k_test_cdc,
    .class_count = 1U,
  };
  ra8_usb_device_frameworks_t     fw   = make_fw();
  const ra8_usb_device_open_cfg_t good = {
    .port          = k_ra8_usb_device_port_fs,
    .configuration = 1U,
    .interface     = 0U,
    .pool_bytes    = (uint32_t)sizeof(s_store.pool),
    .pool          = s_store.pool,
    .device_cfg    = &dev_cfg,
    .frameworks    = &fw,
    .ops           = &k_full_ops,
    .class_driver  = &k_cdc_driver,
    .class_param   = s_store.class_param,
  };

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_open(nullptr));

  ra8_usb_device_open_cfg_t bad = good;
  bad.port                      = (ra8_usb_device_port_t)3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_open(&bad));

  bad      = good;
  bad.pool = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_open(&bad));

  bad                         = good;
  bad.hs_device_framework     = s_store.hs_device;
  bad.hs_device_framework_len = 64U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_open(&bad));

  bad          = good;
  bad.ops      = &k_no_dcd_ops;
  bad.error_cb = stub_app_error_cb;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_open(&bad));
  TEST_ASSERT_EQ(0U, (uint32_t)s_stub.seq_len);

  fw.device_cap = 8U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_usb_device_open(&good));
  TEST_ASSERT_EQ(0U, (uint32_t)s_stub.seq_len);

  fw                 = make_fw();
  s_stub.stub_sys_rc = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_device_open(&good));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "S"));

  stub_reset();
  s_stub.stub_stack_rc = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_device_open(&good));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "SD"));

  stub_reset();
  s_stub.stub_reg_rc = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_device_open(&good));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "SDR"));

  stub_reset();
  s_stub.dcd_rc = k_ra8_err_hw_init_failed;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_usb_device_open(&good));
  TEST_ASSERT_EQ(0, strcmp(s_stub.seq, "SDRO"));

  TEST_END("All invalid configurations, compose refusals, and stack stage errors fail cleanly");
}

int main(void)
{
  test_open_composes_and_initialises_fs_cdc();
  test_open_dual_speed_hs_with_prebuilt_frameworks_and_no_dcd();
  test_open_refusals_and_failure_propagation();
  return 0;
}
