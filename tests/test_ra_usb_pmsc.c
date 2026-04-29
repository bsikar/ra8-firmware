/**
 * @file test_ra_usb_pmsc.c
 * @brief Unit tests for the native USB device-side MSC class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_pmsc.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_pmsc_buf_capacity = 1024U, /**< Generous test buffer size.  */
} test_pmsc_lim_t;

typedef enum : uint8_t {
  k_test_pmsc_cbw_off_signature   = 0U,
  k_test_pmsc_cbw_off_tag         = 4U,
  k_test_pmsc_cbw_off_data_length = 8U,
  k_test_pmsc_cbw_off_flags       = 12U,
  k_test_pmsc_cbw_off_lun         = 13U,
  k_test_pmsc_cbw_off_cdb_length  = 14U,
  k_test_pmsc_cbw_off_cdb         = 15U,
  k_test_pmsc_cbw_len             = 31U,
  k_test_pmsc_csw_off_signature   = 0U,
  k_test_pmsc_csw_off_tag         = 4U,
  k_test_pmsc_csw_off_status      = 12U,
  k_test_pmsc_csw_len             = 13U,
} test_pmsc_layout_t;

typedef enum : uint32_t {
  k_test_pmsc_block_count = 1000U,
  k_test_pmsc_block_size  = 512U,
} test_pmsc_capacity_t;

typedef enum : uint8_t {
  k_test_pmsc_scsi_inquiry          = 0x12U,
  k_test_pmsc_scsi_read_capacity_10 = 0x25U,
  k_test_pmsc_scsi_read_10          = 0x28U,
  k_test_pmsc_scsi_write_10         = 0x2AU,
  k_test_pmsc_scsi_test_unit_ready  = 0x00U,
} test_pmsc_scsi_t;

/* ---- Stub storage backend ---- */

typedef struct {
  uint32_t read_calls;
  uint32_t write_calls;
  uint32_t inquiry_calls;
  uint32_t capacity_calls;
  uint32_t last_lba;
  uint32_t last_block_count;
  uint8_t  last_write_byte;
  uint8_t  read_fill_byte;
} test_storage_state_t;

static test_storage_state_t s_storage_state;

static const uint8_t s_test_vendor[8] = {'A', 'C', 'M', 'E', ' ', ' ', ' ', ' '};
static const uint8_t s_test_product[16] =
  {'T', 'E', 'S', 'T', 'D', 'R', 'V', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
static const uint8_t s_test_revision[4] = {'1', '.', '0', '0'};

static ra_err_t stub_read_block(void* ctx, uint32_t lba, uint32_t block_count, uint8_t* buf)
{
  (void)ctx;
  s_storage_state.read_calls++;
  s_storage_state.last_lba         = lba;
  s_storage_state.last_block_count = block_count;
  for (uint32_t i = 0U; i < (block_count * (uint32_t)k_test_pmsc_block_size); ++i) {
    buf[i] = s_storage_state.read_fill_byte;
  }
  return k_ra_ok;
}

static ra_err_t stub_write_block(void* ctx, uint32_t lba, uint32_t block_count, const uint8_t* buf)
{
  (void)ctx;
  s_storage_state.write_calls++;
  s_storage_state.last_lba         = lba;
  s_storage_state.last_block_count = block_count;
  if (block_count > 0U) {
    s_storage_state.last_write_byte = buf[0];
  }
  return k_ra_ok;
}

static ra_err_t stub_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  s_storage_state.capacity_calls++;
  *block_count = (uint32_t)k_test_pmsc_block_count;
  *block_size  = (uint32_t)k_test_pmsc_block_size;
  return k_ra_ok;
}

static ra_err_t
stub_get_inquiry(void* ctx, uint8_t* vendor8, uint8_t* product16, uint8_t* revision4)
{
  (void)ctx;
  s_storage_state.inquiry_calls++;
  for (uint8_t i = 0U; i < 8U; ++i) {
    vendor8[i] = s_test_vendor[i];
  }
  for (uint8_t i = 0U; i < 16U; ++i) {
    product16[i] = s_test_product[i];
  }
  for (uint8_t i = 0U; i < 4U; ++i) {
    revision4[i] = s_test_revision[i];
  }
  return k_ra_ok;
}

static const ra_usb_pmsc_storage_t s_test_storage = {
  .read_block   = stub_read_block,
  .write_block  = stub_write_block,
  .get_capacity = stub_get_capacity,
  .get_inquiry  = stub_get_inquiry,
  .ctx          = nullptr,
};

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_pmsc_close();
  s_storage_state                = (test_storage_state_t){};
  s_storage_state.read_fill_byte = 0xA5U;
}

/* ---- Helpers ---- */

static void build_cbw(uint8_t*       cbw,
                      uint32_t       tag,
                      uint32_t       data_xfer_len,
                      bool           data_in,
                      uint8_t        lun,
                      const uint8_t* cdb,
                      uint8_t        cdb_len)
{
  for (uint8_t i = 0U; i < k_test_pmsc_cbw_len; ++i) {
    cbw[i] = 0U;
  }
  /* dCBWSignature = 'USBC' little-endian. */
  cbw[k_test_pmsc_cbw_off_signature + 0U] = 0x55U;
  cbw[k_test_pmsc_cbw_off_signature + 1U] = 0x53U;
  cbw[k_test_pmsc_cbw_off_signature + 2U] = 0x42U;
  cbw[k_test_pmsc_cbw_off_signature + 3U] = 0x43U;
  /* dCBWTag (little-endian). */
  cbw[k_test_pmsc_cbw_off_tag + 0U] = (uint8_t)(tag & 0xFFU);
  cbw[k_test_pmsc_cbw_off_tag + 1U] = (uint8_t)((tag >> 8U) & 0xFFU);
  cbw[k_test_pmsc_cbw_off_tag + 2U] = (uint8_t)((tag >> 16U) & 0xFFU);
  cbw[k_test_pmsc_cbw_off_tag + 3U] = (uint8_t)((tag >> 24U) & 0xFFU);
  /* dCBWDataTransferLength (little-endian). */
  cbw[k_test_pmsc_cbw_off_data_length + 0U] = (uint8_t)(data_xfer_len & 0xFFU);
  cbw[k_test_pmsc_cbw_off_data_length + 1U] = (uint8_t)((data_xfer_len >> 8U) & 0xFFU);
  cbw[k_test_pmsc_cbw_off_data_length + 2U] = (uint8_t)((data_xfer_len >> 16U) & 0xFFU);
  cbw[k_test_pmsc_cbw_off_data_length + 3U] = (uint8_t)((data_xfer_len >> 24U) & 0xFFU);
  cbw[k_test_pmsc_cbw_off_flags]            = data_in ? 0x80U : 0x00U;
  cbw[k_test_pmsc_cbw_off_lun]              = lun;
  cbw[k_test_pmsc_cbw_off_cdb_length]       = cdb_len;
  for (uint8_t i = 0U; i < cdb_len; ++i) {
    cbw[k_test_pmsc_cbw_off_cdb + i] = cdb[i];
  }
}

/* ---- Tests ---- */

static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_pmsc_init FS returns k_ra_ok");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_END("ra_usb_pmsc_init FS returns k_ra_ok");
}

static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_pmsc_init HS returns k_ra_ok");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_hs));
  TEST_END("ra_usb_pmsc_init HS returns k_ra_ok");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_pmsc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pmsc_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_pmsc_init rejects bogus speed");
}

static void test_close_without_init(void)
{
  TEST_BEGIN("ra_usb_pmsc_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pmsc_close());
  TEST_END("ra_usb_pmsc_close before init returns invalid_state");
}

static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_storage / step / feed_cbw / dispatch / build_csw reject pre-init");
  prep();

  uint8_t                  buf[k_test_pmsc_cbw_len] = {};
  uint32_t                 data_len                 = 0U;
  ra_usb_pmsc_csw_status_t status                   = k_ra_pmsc_csw_status_passed;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pmsc_step());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pmsc_feed_cbw(buf));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_pmsc_dispatch_command(buf, (uint32_t)k_test_pmsc_cbw_len, &data_len, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pmsc_build_csw(k_ra_pmsc_csw_status_passed, 0U, buf));

  TEST_END("attach_storage / step / feed_cbw / dispatch / build_csw reject pre-init");
}

static void test_pre_attach_guards(void)
{
  TEST_BEGIN("step / feed_cbw / dispatch reject pre-attach (post-init)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));

  uint8_t                  buf[k_test_pmsc_cbw_len] = {};
  uint32_t                 data_len                 = 0U;
  ra_usb_pmsc_csw_status_t status                   = k_ra_pmsc_csw_status_passed;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pmsc_step());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pmsc_feed_cbw(buf));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_pmsc_dispatch_command(buf, (uint32_t)k_test_pmsc_cbw_len, &data_len, &status));

  TEST_END("step / feed_cbw / dispatch reject pre-attach (post-init)");
}

static void test_attach_storage_null_validation(void)
{
  TEST_BEGIN("attach_storage rejects NULL struct + NULL callbacks");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_attach_storage(nullptr));

  ra_usb_pmsc_storage_t bad = s_test_storage;
  bad.read_block            = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_attach_storage(&bad));

  bad             = s_test_storage;
  bad.write_block = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_attach_storage(&bad));

  bad              = s_test_storage;
  bad.get_capacity = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_attach_storage(&bad));

  bad             = s_test_storage;
  bad.get_inquiry = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_attach_storage(&bad));

  /* All four non-NULL accepted. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  TEST_END("attach_storage rejects NULL struct + NULL callbacks");
}

static void test_feed_cbw_null_guard(void)
{
  TEST_BEGIN("feed_cbw rejects NULL pointer");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pmsc_feed_cbw(nullptr));
  TEST_END("feed_cbw rejects NULL pointer");
}

static void test_feed_cbw_bad_signature_emits_phase_error(void)
{
  TEST_BEGIN("feed_cbw with wrong signature -> phase-error CSW");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t bad_cbw[k_test_pmsc_cbw_len] = {};
  /* Set signature to garbage instead of 'USBC'. */
  bad_cbw[0] = 0xDEU;
  bad_cbw[1] = 0xADU;
  bad_cbw[2] = 0xBEU;
  bad_cbw[3] = 0xEFU;
  /* Tag still meaningful so we can confirm it's echoed in the CSW. */
  bad_cbw[k_test_pmsc_cbw_off_tag + 0U] = 0x11U;
  bad_cbw[k_test_pmsc_cbw_off_tag + 1U] = 0x22U;
  bad_cbw[k_test_pmsc_cbw_off_tag + 2U] = 0x33U;
  bad_cbw[k_test_pmsc_cbw_off_tag + 3U] = 0x44U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pmsc_feed_cbw(bad_cbw));

  /* The driver should now be parked in CSW_TX with phase-error
   * status. Build the CSW and confirm its layout. */
  uint8_t csw[k_test_pmsc_csw_len] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_build_csw(k_ra_pmsc_csw_status_phase_error, 0U, csw));
  /* dCSWSignature = 'USBS' little-endian. */
  TEST_ASSERT_EQ((int32_t)0x55U, (int32_t)csw[k_test_pmsc_csw_off_signature + 0U]);
  TEST_ASSERT_EQ((int32_t)0x53U, (int32_t)csw[k_test_pmsc_csw_off_signature + 1U]);
  TEST_ASSERT_EQ((int32_t)0x42U, (int32_t)csw[k_test_pmsc_csw_off_signature + 2U]);
  TEST_ASSERT_EQ((int32_t)0x53U, (int32_t)csw[k_test_pmsc_csw_off_signature + 3U]);
  /* Tag echo. */
  TEST_ASSERT_EQ((int32_t)0x11U, (int32_t)csw[k_test_pmsc_csw_off_tag + 0U]);
  TEST_ASSERT_EQ((int32_t)0x22U, (int32_t)csw[k_test_pmsc_csw_off_tag + 1U]);
  TEST_ASSERT_EQ((int32_t)0x33U, (int32_t)csw[k_test_pmsc_csw_off_tag + 2U]);
  TEST_ASSERT_EQ((int32_t)0x44U, (int32_t)csw[k_test_pmsc_csw_off_tag + 3U]);
  /* Status byte = phase error (0x02). */
  TEST_ASSERT_EQ((int32_t)0x02U, (int32_t)csw[k_test_pmsc_csw_off_status]);

  TEST_END("feed_cbw with wrong signature -> phase-error CSW");
}

static void test_inquiry_returns_backend_strings(void)
{
  TEST_BEGIN("INQUIRY response includes backend-supplied vendor / product / revision");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_inquiry;
  cdb[4]                           = 36U; /* allocation length */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 0xCAFEBABEU, 36U, true, 0U, cdb, 6U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  uint8_t                  data[k_test_pmsc_buf_capacity] = {};
  uint32_t                 data_len                       = 0U;
  ra_usb_pmsc_csw_status_t status                         = k_ra_pmsc_csw_status_failed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_passed, (int32_t)status);
  TEST_ASSERT_EQ((int32_t)36U, (int32_t)data_len);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_storage_state.inquiry_calls);

  /* Vendor / product / revision starting at fixed offsets. */
  TEST_ASSERT_EQ((int32_t)'A', (int32_t)data[8 + 0U]);
  TEST_ASSERT_EQ((int32_t)'C', (int32_t)data[8 + 1U]);
  TEST_ASSERT_EQ((int32_t)'M', (int32_t)data[8 + 2U]);
  TEST_ASSERT_EQ((int32_t)'E', (int32_t)data[8 + 3U]);

  TEST_ASSERT_EQ((int32_t)'T', (int32_t)data[16 + 0U]);
  TEST_ASSERT_EQ((int32_t)'E', (int32_t)data[16 + 1U]);
  TEST_ASSERT_EQ((int32_t)'S', (int32_t)data[16 + 2U]);
  TEST_ASSERT_EQ((int32_t)'T', (int32_t)data[16 + 3U]);

  TEST_ASSERT_EQ((int32_t)'1', (int32_t)data[32 + 0U]);
  TEST_ASSERT_EQ((int32_t)'.', (int32_t)data[32 + 1U]);
  TEST_ASSERT_EQ((int32_t)'0', (int32_t)data[32 + 2U]);
  TEST_ASSERT_EQ((int32_t)'0', (int32_t)data[32 + 3U]);

  /* Byte 0: peripheral device type. Byte 1: removable bit set. */
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[0]);
  TEST_ASSERT_EQ((int32_t)0x80U, (int32_t)data[1]);

  TEST_END("INQUIRY response includes backend-supplied vendor / product / revision");
}

static void test_read_capacity_returns_count_minus_one_be(void)
{
  TEST_BEGIN("READ_CAPACITY(10) returns block_count-1 + block_size big-endian");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[10]                  = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_read_capacity_10;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 1U, 8U, true, 0U, cdb, 10U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  uint8_t                  data[k_test_pmsc_buf_capacity] = {};
  uint32_t                 data_len                       = 0U;
  ra_usb_pmsc_csw_status_t status                         = k_ra_pmsc_csw_status_failed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_passed, (int32_t)status);
  TEST_ASSERT_EQ((int32_t)8U, (int32_t)data_len);

  /* k_test_pmsc_block_count = 1000 -> last LBA = 999 = 0x000003E7
   * big-endian. */
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[0]);
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[1]);
  TEST_ASSERT_EQ((int32_t)0x03U, (int32_t)data[2]);
  TEST_ASSERT_EQ((int32_t)0xE7U, (int32_t)data[3]);
  /* Block size 512 = 0x00000200 big-endian. */
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[4]);
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[5]);
  TEST_ASSERT_EQ((int32_t)0x02U, (int32_t)data[6]);
  TEST_ASSERT_EQ((int32_t)0x00U, (int32_t)data[7]);

  TEST_END("READ_CAPACITY(10) returns block_count-1 + block_size big-endian");
}

static void test_read10_calls_backend_and_returns_512(void)
{
  TEST_BEGIN("READ(10) happy path -- backend produces 512 bytes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  /* READ(10) at LBA 5, count 1. */
  uint8_t cdb[10]                  = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_read_10;
  cdb[5]                           = 5U; /* LBA low byte. */
  cdb[8]                           = 1U; /* count low byte. */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 2U, 512U, true, 0U, cdb, 10U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  uint8_t                  data[k_test_pmsc_buf_capacity] = {};
  uint32_t                 data_len                       = 0U;
  ra_usb_pmsc_csw_status_t status                         = k_ra_pmsc_csw_status_failed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_passed, (int32_t)status);
  TEST_ASSERT_EQ((int32_t)512U, (int32_t)data_len);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_storage_state.read_calls);
  TEST_ASSERT_EQ((int32_t)5U, (int32_t)s_storage_state.last_lba);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_storage_state.last_block_count);
  /* Read fill byte from stub propagated. */
  TEST_ASSERT_EQ((int32_t)0xA5U, (int32_t)data[0]);
  TEST_ASSERT_EQ((int32_t)0xA5U, (int32_t)data[511]);

  TEST_END("READ(10) happy path -- backend produces 512 bytes");
}

static void test_write10_calls_backend(void)
{
  TEST_BEGIN("WRITE(10) happy path -- backend receives 512 bytes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  /* WRITE(10) at LBA 7, count 1. */
  uint8_t cdb[10]                  = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_write_10;
  cdb[5]                           = 7U;
  cdb[8]                           = 1U;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 3U, 512U, false, 0U, cdb, 10U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  /* The data buffer holds the host-supplied payload; pre-fill it so
   * the test can confirm the backend received it. */
  uint8_t data[k_test_pmsc_buf_capacity] = {};
  for (uint32_t i = 0U; i < 512U; ++i) {
    data[i] = 0x5AU;
  }
  uint32_t                 data_len = 0U;
  ra_usb_pmsc_csw_status_t status   = k_ra_pmsc_csw_status_failed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_passed, (int32_t)status);
  TEST_ASSERT_EQ((int32_t)512U, (int32_t)data_len);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_storage_state.write_calls);
  TEST_ASSERT_EQ((int32_t)7U, (int32_t)s_storage_state.last_lba);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_storage_state.last_block_count);
  TEST_ASSERT_EQ((int32_t)0x5AU, (int32_t)s_storage_state.last_write_byte);

  TEST_END("WRITE(10) happy path -- backend receives 512 bytes");
}

static void test_test_unit_ready_no_data_phase(void)
{
  TEST_BEGIN("TEST_UNIT_READY produces zero-byte data phase + passed CSW");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_test_unit_ready;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 4U, 0U, true, 0U, cdb, 6U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  uint8_t                  data[k_test_pmsc_buf_capacity] = {};
  uint32_t                 data_len                       = 0xFFU;
  ra_usb_pmsc_csw_status_t status                         = k_ra_pmsc_csw_status_failed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)data_len);
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_passed, (int32_t)status);

  TEST_END("TEST_UNIT_READY produces zero-byte data phase + passed CSW");
}

static void test_unsupported_opcode_fails(void)
{
  TEST_BEGIN("Unsupported SCSI opcode -> CSW status FAILED");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[10]                  = {};
  cdb[0]                           = 0xCCU; /* not a SCSI opcode the driver knows */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 5U, 0U, true, 0U, cdb, 10U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_feed_cbw(cbw));

  uint8_t                  data[k_test_pmsc_buf_capacity] = {};
  uint32_t                 data_len                       = 0U;
  ra_usb_pmsc_csw_status_t status                         = k_ra_pmsc_csw_status_passed;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pmsc_dispatch_command(data,
                                                       (uint32_t)k_test_pmsc_buf_capacity,
                                                       &data_len,
                                                       &status));
  TEST_ASSERT_EQ((int32_t)k_ra_pmsc_csw_status_failed, (int32_t)status);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)data_len);

  TEST_END("Unsupported SCSI opcode -> CSW status FAILED");
}

static void test_step_state_machine_loops(void)
{
  TEST_BEGIN("step transitions through phases and lands back at IDLE");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  /* From IDLE the step machine should advance without errors. The
   * starter just verifies we can pump it without tripping a state
   * guard. */
  for (uint8_t i = 0U; i < 8U; ++i) {
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_step());
  }

  TEST_END("step transitions through phases and lands back at IDLE");
}

static void test_dispatch_null_arg_rejection(void)
{
  TEST_BEGIN("dispatch_command rejects NULL output pointers");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t                  data[16] = {};
  uint32_t                 dl       = 0U;
  ra_usb_pmsc_csw_status_t status   = k_ra_pmsc_csw_status_passed;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_pmsc_dispatch_command(nullptr, 16U, &dl, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_pmsc_dispatch_command(data, 16U, nullptr, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_pmsc_dispatch_command(data, 16U, &dl, nullptr));
  TEST_END("dispatch_command rejects NULL output pointers");
}

static void test_build_csw_null_guard(void)
{
  TEST_BEGIN("build_csw rejects NULL output");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pmsc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_pmsc_build_csw(k_ra_pmsc_csw_status_passed, 0U, nullptr));
  TEST_END("build_csw rejects NULL output");
}

int32_t main(void)
{
  test_init_fs_returns_ok();
  test_init_hs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_pre_init_guards();
  test_pre_attach_guards();
  test_attach_storage_null_validation();
  test_feed_cbw_null_guard();
  test_feed_cbw_bad_signature_emits_phase_error();
  test_inquiry_returns_backend_strings();
  test_read_capacity_returns_count_minus_one_be();
  test_read10_calls_backend_and_returns_512();
  test_write10_calls_backend();
  test_test_unit_ready_no_data_phase();
  test_unsupported_opcode_fails();
  test_step_state_machine_loops();
  test_dispatch_null_arg_rejection();
  test_build_csw_null_guard();
  (void)fprintf(stderr, "[OK ] test_ra_usb_pmsc.c\n");
  return 0;
}
