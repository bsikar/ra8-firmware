/**
 * @file test_ra8_usb_pmsc.c
 * @brief Unit tests for the native USB device-side MSC class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_pmsc.h"
#include "ra8_usb_pmsc_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_pmsc_bot_t
 * @brief Command Block Wrapper and SCSI values one BOT exchange is built from.
 *
 * @details
 * A CBW opens with the ASCII signature "USBC"; the driver must reject any
 * wrapper that does not. The `k_t_bad_sig_*` bytes spell 0xDEADBEEF instead,
 * which is the negative vector for that check. The two fill bytes differ so a
 * payload that survives a round trip is distinguishable from one the mock
 * invented.
 */
typedef enum : uint8_t {
  k_t_cbw_sig_b0       = 0x55U, /**< CBW signature byte 0, ASCII 'U'.            */
  k_t_cbw_sig_b1       = 0x53U, /**< CBW signature byte 1, ASCII 'S'.            */
  k_t_cbw_sig_b2       = 0x42U, /**< CBW signature byte 2, ASCII 'B'.            */
  k_t_cbw_sig_b3       = 0x43U, /**< CBW signature byte 3, ASCII 'C'.            */
  k_t_bad_sig_b0       = 0xDEU, /**< Byte 0 of the deliberately wrong signature. */
  k_t_bad_sig_b1       = 0xADU, /**< Byte 1 of it.                               */
  k_t_bad_sig_b2       = 0xBEU, /**< Byte 2 of it.                               */
  k_t_bad_sig_b3       = 0xEFU, /**< Byte 3 of it.                               */
  k_t_bad_tag_b0       = 0x11U, /**< Tag byte 0 of the malformed wrapper.        */
  k_t_bad_tag_b1       = 0x22U, /**< Tag byte 1.                                 */
  k_t_bad_tag_b2       = 0x33U, /**< Tag byte 2.                                 */
  k_t_bad_tag_b3       = 0x44U, /**< Tag byte 3.                                 */
  k_t_cbw_flag_data_in = 0x80U, /**< CBW flags bit 7: device-to-host transfer.   */
  k_t_le32_hi_shift    = 24U,   /**< Top-byte shift of a 32-bit LE field.        */
  k_t_byte_mask        = 0xFFU, /**< Low-byte mask while serialising it.         */
  k_t_cdb_len_10       = 10U,   /**< CDB length of the 10-byte SCSI commands.    */
  k_t_cdb10_off_lba_b3 = 5U,    /**< Least-significant LBA byte in that CDB.     */
  k_t_inquiry_len      = 36U,   /**< Standard INQUIRY response length, bytes.    */
  k_t_sense_len        = 18U,   /**< Fixed-format REQUEST SENSE length, bytes.   */
  k_t_read_lba         = 5U,    /**< LBA the read arm asks for.                  */
  k_t_write_lba        = 7U,    /**< LBA the write arm targets.                  */
  k_t_opcode_unknown   = 0xCCU, /**< Opcode outside the supported set; stalls.   */
  k_t_fill_written     = 0x5AU, /**< Byte pattern the write arm sends.           */
  k_t_fill_returned    = 0xA5U, /**< Byte pattern the mock storage reads back.   */
} t_pmsc_bot_t;

/**
 * @enum t_pmsc_tag_t
 * @brief CBW tags, opaque to the protocol -- the device must echo each in the
 *        CSW and nothing more -- so they need only be distinct per arm.
 */
typedef enum : uint32_t {
  k_t_tag_inquiry = 0xCAFEBABEU, /**< Tag for the INQUIRY arm.            */
  k_t_tag_phase_a = 0x30U,       /**< Tag for the first phase-error arm.  */
  k_t_tag_phase_b = 0x40U,       /**< Tag for the second phase-error arm. */
} t_pmsc_tag_t;

typedef enum : uint16_t {
  k_test_pmsc_buf_capacity = 1024U, /**< Generous test buffer size. */
} test_pmsc_lim_t;

typedef enum : uint8_t {
  k_test_pmsc_cbw_off_signature   = 0U,  /**< Test pmsc cbw off signature.   */
  k_test_pmsc_cbw_off_tag         = 4U,  /**< Test pmsc cbw off tag.         */
  k_test_pmsc_cbw_off_data_length = 8U,  /**< Test pmsc cbw off data length. */
  k_test_pmsc_cbw_off_flags       = 12U, /**< Test pmsc cbw off flags.       */
  k_test_pmsc_cbw_off_lun         = 13U, /**< Test pmsc cbw off lun.         */
  k_test_pmsc_cbw_off_cdb_length  = 14U, /**< Test pmsc cbw off cdb length.  */
  k_test_pmsc_cbw_off_cdb         = 15U, /**< Test pmsc cbw off cdb.         */
  k_test_pmsc_cbw_len             = 31U, /**< Test pmsc cbw length.          */
  k_test_pmsc_csw_off_signature   = 0U,  /**< Test pmsc csw off signature.   */
  k_test_pmsc_csw_off_tag         = 4U,  /**< Test pmsc csw off tag.         */
  k_test_pmsc_csw_off_status      = 12U, /**< Test pmsc csw off status.      */
  k_test_pmsc_csw_len             = 13U, /**< Test pmsc csw length.          */
} test_pmsc_layout_t;

typedef enum : uint32_t {
  k_test_pmsc_block_count = 1000U, /**< Test pmsc block count. */
  k_test_pmsc_block_size  = 512U,  /**< Test pmsc block size.  */
} test_pmsc_capacity_t;

typedef enum : uint8_t {
  k_test_pmsc_scsi_inquiry          = 0x12U, /**< Test pmsc scsi inquiry.          */
  k_test_pmsc_scsi_read_capacity_10 = 0x25U, /**< Test pmsc scsi read capacity 10. */
  k_test_pmsc_scsi_read_10          = 0x28U, /**< Test pmsc scsi read 10.          */
  k_test_pmsc_scsi_write_10         = 0x2AU, /**< Test pmsc scsi write 10.         */
  k_test_pmsc_scsi_test_unit_ready  = 0x00U, /**< Test pmsc scsi test unit ready.  */
  k_test_pmsc_scsi_request_sense    = 0x03U, /**< Test pmsc scsi request sense.    */
  k_test_pmsc_scsi_mode_sense_6     = 0x1AU, /**< Test pmsc scsi mode sense 6.     */
} test_pmsc_scsi_t;

typedef enum : uint16_t {
  /* One below the ra8_mstp refcount ceiling (UINT8_MAX). Enabling the
   * USB module this many times leaves the refcount saturated so the
   * next enable (inside ra8_usb_pmsc_init) returns an error. */
  k_test_pmsc_mstp_saturate = 255U, /**< Test pmsc mstp saturate. */
  /* Buffer capacity below the 36-byte INQUIRY response but non-zero,
   * so the capacity guard passes and the handler's size guard fires. */
  k_test_pmsc_tiny_cap = 4U, /**< Test pmsc tiny cap. */
} test_pmsc_sat_t;

/* ---- Stub storage backend ---- */

typedef struct {
  uint32_t read_calls;       /**< Read calls.       */
  uint32_t write_calls;      /**< Write calls.      */
  uint32_t inquiry_calls;    /**< Inquiry calls.    */
  uint32_t capacity_calls;   /**< Capacity calls.   */
  uint32_t last_lba;         /**< Last lba.         */
  uint32_t last_block_count; /**< Last block count. */
  uint8_t  last_write_byte;  /**< Last write byte.  */
  uint8_t  read_fill_byte;   /**< Read fill byte.   */
} test_storage_state_t;

static test_storage_state_t s_storage_state;

static const uint8_t s_test_vendor[8] = {'A', 'C', 'M', 'E', ' ', ' ', ' ', ' '};
static const uint8_t s_test_product[16] =
  {'T', 'E', 'S', 'T', 'D', 'R', 'V', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
static const uint8_t s_test_revision[4] = {'1', '.', '0', '0'};

static ra8_err_t stub_read_block(void* ctx, uint32_t lba, uint32_t block_count, uint8_t* buf)
{
  (void)ctx;
  s_storage_state.read_calls++;
  s_storage_state.last_lba         = lba;
  s_storage_state.last_block_count = block_count;
  for (uint32_t i = 0U; i < (block_count * (uint32_t)k_test_pmsc_block_size); ++i) {
    buf[i] = s_storage_state.read_fill_byte;
  }
  return k_ra8_ok;
}

static ra8_err_t stub_write_block(void* ctx, uint32_t lba, uint32_t block_count, const uint8_t* buf)
{
  (void)ctx;
  s_storage_state.write_calls++;
  s_storage_state.last_lba         = lba;
  s_storage_state.last_block_count = block_count;
  if (block_count > 0U) {
    s_storage_state.last_write_byte = buf[0];
  }
  return k_ra8_ok;
}

static ra8_err_t stub_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  s_storage_state.capacity_calls++;
  *block_count = (uint32_t)k_test_pmsc_block_count;
  *block_size  = (uint32_t)k_test_pmsc_block_size;
  return k_ra8_ok;
}

static ra8_err_t
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
  return k_ra8_ok;
}

static const ra8_usb_pmsc_storage_t s_test_storage = {
  .read_block   = stub_read_block,
  .write_block  = stub_write_block,
  .get_capacity = stub_get_capacity,
  .get_inquiry  = stub_get_inquiry,
  .ctx          = nullptr,
};

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pmsc_close();
  s_storage_state                = (test_storage_state_t){};
  s_storage_state.read_fill_byte = k_t_fill_returned;
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
  cbw[k_test_pmsc_cbw_off_signature + 0U] = k_t_cbw_sig_b0;
  cbw[k_test_pmsc_cbw_off_signature + 1U] = k_t_cbw_sig_b1;
  cbw[k_test_pmsc_cbw_off_signature + 2U] = k_t_cbw_sig_b2;
  cbw[k_test_pmsc_cbw_off_signature + 3U] = k_t_cbw_sig_b3;
  /* dCBWTag (little-endian). */
  cbw[k_test_pmsc_cbw_off_tag + 0U] = (uint8_t)(tag & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_tag + 1U] = (uint8_t)((tag >> 8U) & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_tag + 2U] = (uint8_t)((tag >> 16U) & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_tag + 3U] = (uint8_t)((tag >> k_t_le32_hi_shift) & k_t_byte_mask);
  /* dCBWDataTransferLength (little-endian). */
  cbw[k_test_pmsc_cbw_off_data_length + 0U] = (uint8_t)(data_xfer_len & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_data_length + 1U] = (uint8_t)((data_xfer_len >> 8U) & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_data_length + 2U] = (uint8_t)((data_xfer_len >> 16U) & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_data_length + 3U] =
    (uint8_t)((data_xfer_len >> k_t_le32_hi_shift) & k_t_byte_mask);
  cbw[k_test_pmsc_cbw_off_flags]      = data_in ? k_t_cbw_flag_data_in : 0x00U;
  cbw[k_test_pmsc_cbw_off_lun]        = lun;
  cbw[k_test_pmsc_cbw_off_cdb_length] = cdb_len;
  for (uint8_t i = 0U; i < cdb_len; ++i) {
    cbw[k_test_pmsc_cbw_off_cdb + i] = cdb[i];
  }
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
  TEST_BEGIN("ra8_usb_pmsc_init FS returns k_ra8_ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_END("ra8_usb_pmsc_init FS returns k_ra8_ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_pmsc_init HS returns k_ra8_ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_hs));
  TEST_END("ra8_usb_pmsc_init HS returns k_ra8_ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_pmsc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pmsc_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_pmsc_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_pmsc_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_close());
  TEST_END("ra8_usb_pmsc_close before init returns invalid_state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_storage / step / feed_cbw / dispatch / build_csw reject pre-init");
  prep();

  uint8_t                   buf[k_test_pmsc_cbw_len] = {};
  uint32_t                  data_len                 = 0U;
  ra8_usb_pmsc_csw_status_t status                   = k_ra8_pmsc_csw_status_passed;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_attach_storage(&s_test_storage));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_step());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_feed_cbw(buf));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_usb_pmsc_dispatch_command(buf, (uint32_t)k_test_pmsc_cbw_len, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pmsc_build_csw(k_ra8_pmsc_csw_status_passed, 0U, buf));

  TEST_END("attach_storage / step / feed_cbw / dispatch / build_csw reject pre-init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_attach_guards(void)
{
  TEST_BEGIN("step / feed_cbw / dispatch reject pre-attach (post-init)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));

  uint8_t                   buf[k_test_pmsc_cbw_len] = {};
  uint32_t                  data_len                 = 0U;
  ra8_usb_pmsc_csw_status_t status                   = k_ra8_pmsc_csw_status_passed;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_step());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pmsc_feed_cbw(buf));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_usb_pmsc_dispatch_command(buf, (uint32_t)k_test_pmsc_cbw_len, &data_len, &status));

  TEST_END("step / feed_cbw / dispatch reject pre-attach (post-init)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_storage_null_validation(void)
{
  TEST_BEGIN("attach_storage rejects NULL struct + NULL callbacks");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_attach_storage(nullptr));

  ra8_usb_pmsc_storage_t bad = s_test_storage;
  bad.read_block             = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_attach_storage(&bad));

  bad             = s_test_storage;
  bad.write_block = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_attach_storage(&bad));

  bad              = s_test_storage;
  bad.get_capacity = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_attach_storage(&bad));

  bad             = s_test_storage;
  bad.get_inquiry = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_attach_storage(&bad));

  /* All four non-NULL accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  TEST_END("attach_storage rejects NULL struct + NULL callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_feed_cbw_null_guard(void)
{
  TEST_BEGIN("feed_cbw rejects NULL pointer");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_feed_cbw(nullptr));
  TEST_END("feed_cbw rejects NULL pointer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_feed_cbw_bad_signature_emits_phase_error(void)
{
  TEST_BEGIN("feed_cbw with wrong signature -> phase-error CSW");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t bad_cbw[k_test_pmsc_cbw_len] = {};
  /* Set signature to garbage instead of 'USBC'. */
  bad_cbw[0] = k_t_bad_sig_b0;
  bad_cbw[1] = k_t_bad_sig_b1;
  bad_cbw[2] = k_t_bad_sig_b2;
  bad_cbw[3] = k_t_bad_sig_b3;
  /* Tag still meaningful so we can confirm it's echoed in the CSW. */
  bad_cbw[k_test_pmsc_cbw_off_tag + 0U] = k_t_bad_tag_b0;
  bad_cbw[k_test_pmsc_cbw_off_tag + 1U] = k_t_bad_tag_b1;
  bad_cbw[k_test_pmsc_cbw_off_tag + 2U] = k_t_bad_tag_b2;
  bad_cbw[k_test_pmsc_cbw_off_tag + 3U] = k_t_bad_tag_b3;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pmsc_feed_cbw(bad_cbw));

  /* The driver should now be parked in CSW_TX with phase-error
   * status. Build the CSW and confirm its layout. */
  uint8_t csw[k_test_pmsc_csw_len] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_build_csw(k_ra8_pmsc_csw_status_phase_error, 0U, csw));
  /* dCSWSignature = 'USBS' little-endian. */
  TEST_ASSERT_EQ(0x55U, csw[k_test_pmsc_csw_off_signature + 0U]);
  TEST_ASSERT_EQ(0x53U, csw[k_test_pmsc_csw_off_signature + 1U]);
  TEST_ASSERT_EQ(0x42U, csw[k_test_pmsc_csw_off_signature + 2U]);
  TEST_ASSERT_EQ(0x53U, csw[k_test_pmsc_csw_off_signature + 3U]);
  /* Tag echo. */
  TEST_ASSERT_EQ(0x11U, csw[k_test_pmsc_csw_off_tag + 0U]);
  TEST_ASSERT_EQ(0x22U, csw[k_test_pmsc_csw_off_tag + 1U]);
  TEST_ASSERT_EQ(0x33U, csw[k_test_pmsc_csw_off_tag + 2U]);
  TEST_ASSERT_EQ(0x44U, csw[k_test_pmsc_csw_off_tag + 3U]);
  /* Status byte = phase error (0x02). */
  TEST_ASSERT_EQ(0x02U, csw[k_test_pmsc_csw_off_status]);

  TEST_END("feed_cbw with wrong signature -> phase-error CSW");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_inquiry_returns_backend_strings(void)
{
  TEST_BEGIN("INQUIRY response includes backend-supplied vendor / product / revision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_inquiry;
  cdb[4]                           = k_t_inquiry_len; /* allocation length */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, k_t_tag_inquiry, k_t_inquiry_len, true, 0U, cdb, 6U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(36U, data_len);
  TEST_ASSERT_EQ(1U, s_storage_state.inquiry_calls);

  /* Vendor / product / revision starting at fixed offsets. */
  TEST_ASSERT_EQ('A', data[8 + 0U]);
  TEST_ASSERT_EQ('C', data[8 + 1U]);
  TEST_ASSERT_EQ('M', data[8 + 2U]);
  TEST_ASSERT_EQ('E', data[8 + 3U]);

  TEST_ASSERT_EQ('T', data[16 + 0U]);
  TEST_ASSERT_EQ('E', data[16 + 1U]);
  TEST_ASSERT_EQ('S', data[16 + 2U]);
  TEST_ASSERT_EQ('T', data[16 + 3U]);

  TEST_ASSERT_EQ('1', data[32 + 0U]);
  TEST_ASSERT_EQ('.', data[32 + 1U]);
  TEST_ASSERT_EQ('0', data[32 + 2U]);
  TEST_ASSERT_EQ('0', data[32 + 3U]);

  /* Byte 0: peripheral device type. Byte 1: removable bit set. */
  TEST_ASSERT_EQ(0x00U, data[0]);
  TEST_ASSERT_EQ(0x80U, data[1]);

  TEST_END("INQUIRY response includes backend-supplied vendor / product / revision");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_capacity_returns_count_minus_one_be(void)
{
  TEST_BEGIN("READ_CAPACITY(10) returns block_count-1 + block_size big-endian");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[k_t_cdb_len_10]      = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_read_capacity_10;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 1U, 8U, true, 0U, cdb, k_t_cdb_len_10);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(8U, data_len);

  /* k_test_pmsc_block_count = 1000 -> last LBA = 999 = 0x000003E7
   * big-endian. */
  TEST_ASSERT_EQ(0x00U, data[0]);
  TEST_ASSERT_EQ(0x00U, data[1]);
  TEST_ASSERT_EQ(0x03U, data[2]);
  TEST_ASSERT_EQ(0xE7U, data[3]);
  /* Block size 512 = 0x00000200 big-endian. */
  TEST_ASSERT_EQ(0x00U, data[4]);
  TEST_ASSERT_EQ(0x00U, data[5]);
  TEST_ASSERT_EQ(0x02U, data[6]);
  TEST_ASSERT_EQ(0x00U, data[7]);

  TEST_END("READ_CAPACITY(10) returns block_count-1 + block_size big-endian");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read10_calls_backend_and_returns_512(void)
{
  TEST_BEGIN("READ(10) happy path -- backend produces 512 bytes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  /* READ(10) at LBA 5, count 1. */
  uint8_t cdb[k_t_cdb_len_10]      = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_read_10;
  cdb[k_t_cdb10_off_lba_b3]        = k_t_read_lba; /* LBA low byte.   */
  cdb[8]                           = 1U;           /* count low byte. */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 2U, k_test_pmsc_block_size, true, 0U, cdb, k_t_cdb_len_10);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(512U, data_len);
  TEST_ASSERT_EQ(1U, s_storage_state.read_calls);
  TEST_ASSERT_EQ(5U, s_storage_state.last_lba);
  TEST_ASSERT_EQ(1U, s_storage_state.last_block_count);
  /* Read fill byte from stub propagated. */
  TEST_ASSERT_EQ(0xA5U, data[0]);
  TEST_ASSERT_EQ(0xA5U, data[511]);

  TEST_END("READ(10) happy path -- backend produces 512 bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write10_calls_backend(void)
{
  TEST_BEGIN("WRITE(10) happy path -- backend receives 512 bytes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  /* WRITE(10) at LBA 7, count 1. */
  uint8_t cdb[k_t_cdb_len_10]      = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_write_10;
  cdb[k_t_cdb10_off_lba_b3]        = k_t_write_lba;
  cdb[8]                           = 1U;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 3U, k_test_pmsc_block_size, false, 0U, cdb, k_t_cdb_len_10);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  /* The data buffer holds the host-supplied payload; pre-fill it so
   * the test can confirm the backend received it. */
  uint8_t data[k_test_pmsc_buf_capacity] = {};
  for (uint32_t i = 0U; i < k_test_pmsc_block_size; ++i) {
    data[i] = k_t_fill_written;
  }
  uint32_t                  data_len = 0U;
  ra8_usb_pmsc_csw_status_t status   = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(512U, data_len);
  TEST_ASSERT_EQ(1U, s_storage_state.write_calls);
  TEST_ASSERT_EQ(7U, s_storage_state.last_lba);
  TEST_ASSERT_EQ(1U, s_storage_state.last_block_count);
  TEST_ASSERT_EQ(0x5AU, s_storage_state.last_write_byte);

  TEST_END("WRITE(10) happy path -- backend receives 512 bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_test_unit_ready_no_data_phase(void)
{
  TEST_BEGIN("TEST_UNIT_READY produces zero-byte data phase + passed CSW");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_test_unit_ready;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 4U, 0U, true, 0U, cdb, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = k_t_byte_mask;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(0U, data_len);
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);

  TEST_END("TEST_UNIT_READY produces zero-byte data phase + passed CSW");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unsupported_opcode_fails(void)
{
  TEST_BEGIN("Unsupported SCSI opcode -> CSW status FAILED");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[k_t_cdb_len_10]      = {};
  cdb[0]                           = k_t_opcode_unknown; /* not a SCSI opcode the driver knows */
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, k_t_read_lba, 0U, true, 0U, cdb, k_t_cdb_len_10);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_failed, status);
  TEST_ASSERT_EQ(0U, data_len);

  TEST_END("Unsupported SCSI opcode -> CSW status FAILED");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_step_state_machine_loops(void)
{
  TEST_BEGIN("step transitions through phases and lands back at IDLE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  /* From IDLE the step machine should advance without errors. The
   * starter just verifies we can pump it without tripping a state
   * guard. */
  for (uint8_t i = 0U; i < 8U; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_step());
  }

  TEST_END("step transitions through phases and lands back at IDLE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_null_arg_rejection(void)
{
  TEST_BEGIN("dispatch_command rejects NULL output pointers");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t                   data[16] = {};
  uint32_t                  dl       = 0U;
  ra8_usb_pmsc_csw_status_t status   = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_dispatch_command(nullptr, 16U, &dl, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_dispatch_command(data, 16U, nullptr, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pmsc_dispatch_command(data, 16U, &dl, nullptr));
  TEST_END("dispatch_command rejects NULL output pointers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_build_csw_null_guard(void)
{
  TEST_BEGIN("build_csw rejects NULL output");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_pmsc_build_csw(k_ra8_pmsc_csw_status_passed, 0U, nullptr));
  TEST_END("build_csw rejects NULL output");
}

/**
 * @par MC/DC:
 * (no compound decisions -- drives the REQUEST SENSE + MODE SENSE(6)
 * arms of the SCSI dispatch `switch`, which is a multi-way selection,
 * not a boolean `&&`/`||` decision.)
 */
static void test_dispatch_request_sense_and_mode_sense(void)
{
  TEST_BEGIN("dispatch routes REQUEST SENSE + MODE SENSE(6) to their handlers");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;

  /* REQUEST SENSE (0x03) -> 18-byte canned no-sense response. */
  uint8_t cdb_sense[6]             = {};
  cdb_sense[0]                     = (uint8_t)k_test_pmsc_scsi_request_sense;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 0x10U, k_t_sense_len, true, 0U, cdb_sense, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(18U, data_len);
  TEST_ASSERT_EQ(0x70U, data[0]); /* current-error response code */

  /* MODE SENSE(6) (0x1A) -> 4-byte header-only response. feed_cbw
   * re-arms CDB_DECODE regardless of the prior data-phase state. */
  uint8_t cdb_mode[6] = {};
  cdb_mode[0]         = (uint8_t)k_test_pmsc_scsi_mode_sense_6;
  build_cbw(cbw, k_t_bad_tag_b0, 4U, true, 0U, cdb_mode, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));
  data_len = 0U;
  status   = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_passed, status);
  TEST_ASSERT_EQ(4U, data_len);
  TEST_ASSERT_EQ(0x03U, data[0]); /* mode data length (header-only) */

  TEST_END("dispatch routes REQUEST SENSE + MODE SENSE(6) to their handlers");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the single `bot_state !=
 * CDB_DECODE` precondition guard; dispatching before feed_cbw leaves
 * the machine in IDLE so the guard fires.)
 */
static void test_dispatch_wrong_bot_state_rejected(void)
{
  TEST_BEGIN("dispatch before feed_cbw is rejected with invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  /* No feed_cbw -> bot_state is IDLE, not CDB_DECODE. */
  uint8_t                   data[16] = {};
  uint32_t                  data_len = 0U;
  ra8_usb_pmsc_csw_status_t status   = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pmsc_dispatch_command(data, 16U, &data_len, &status));
  TEST_END("dispatch before feed_cbw is rejected with invalid_state");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the single `data_buf_capacity ==
 * 0` precondition guard after a valid CBW put the machine in
 * CDB_DECODE.)
 */
static void test_dispatch_zero_capacity_rejected(void)
{
  TEST_BEGIN("dispatch with zero capacity is rejected with invalid_size");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_inquiry;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, 0x20U, k_t_inquiry_len, true, 0U, cdb, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[16] = {};
  uint32_t                  data_len = 0U;
  ra8_usb_pmsc_csw_status_t status   = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_usb_pmsc_dispatch_command(data, 0U, &data_len, &status));
  TEST_END("dispatch with zero capacity is rejected with invalid_size");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the single `err != k_ra8_ok`
 * guard after dispatch: a non-zero but too-small buffer makes the
 * INQUIRY handler return invalid_size, forcing CSW status FAILED and a
 * zeroed data length.)
 */
static void test_dispatch_handler_error_marks_failed(void)
{
  TEST_BEGIN("dispatch handler error -> CSW FAILED + zero data_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_inquiry;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, k_t_tag_phase_a, k_t_inquiry_len, true, 0U, cdb, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  /* Capacity 4 clears the "== 0" guard but is below the 36-byte
   * INQUIRY response, so internal_handle_inquiry returns invalid_size
   * and dispatch flips the CSW to FAILED. */
  uint8_t                   data[k_test_pmsc_tiny_cap] = {};
  uint32_t                  data_len                   = k_t_byte_mask;
  ra8_usb_pmsc_csw_status_t status                     = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_tiny_cap, &data_len, &status));
  TEST_ASSERT_EQ(k_ra8_pmsc_csw_status_failed, status);
  TEST_ASSERT_EQ(0U, data_len);
  TEST_END("dispatch handler error -> CSW FAILED + zero data_len");
}

/**
 * @par MC/DC:
 * (no compound decisions -- drives the DATA_TX/DATA_RX arm of the step
 * `switch` (a multi-way selection). A data-producing command parks the
 * machine in DATA_TX; one step advances it to CSW_TX.)
 */
static void test_step_from_data_phase_advances_to_csw(void)
{
  TEST_BEGIN("step from DATA_TX advances to CSW_TX");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t cdb[6]                   = {};
  cdb[0]                           = (uint8_t)k_test_pmsc_scsi_inquiry;
  uint8_t cbw[k_test_pmsc_cbw_len] = {};
  build_cbw(cbw, k_t_tag_phase_b, k_t_inquiry_len, true, 0U, cdb, 6U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_feed_cbw(cbw));

  uint8_t                   data[k_test_pmsc_buf_capacity] = {};
  uint32_t                  data_len                       = 0U;
  ra8_usb_pmsc_csw_status_t status                         = k_ra8_pmsc_csw_status_failed;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_buf_capacity, &data_len, &status));
  /* Data-IN command with 36 bytes -> machine is now in DATA_TX. */
  TEST_ASSERT_EQ(k_ra8_pmsc_state_data_tx, s_usb_pmsc_state.bot_state);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_step());
  TEST_ASSERT_EQ(k_ra8_pmsc_state_csw_tx, s_usb_pmsc_state.bot_state);
  TEST_END("step from DATA_TX advances to CSW_TX");
}

/**
 * @par MC/DC:
 * (no compound decisions -- drives the CSW_TX arm of the step
 * `switch`. An invalid-signature CBW parks the machine in CSW_TX; one
 * step rewinds it to IDLE.)
 */
static void test_step_from_csw_tx_rewinds_to_idle(void)
{
  TEST_BEGIN("step from CSW_TX rewinds to IDLE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  /* Bad signature parks the BOT machine in CSW_TX (phase error). */
  uint8_t bad_cbw[k_test_pmsc_cbw_len] = {};
  bad_cbw[0]                           = k_t_bad_sig_b0;
  bad_cbw[1]                           = k_t_bad_sig_b1;
  bad_cbw[2]                           = k_t_bad_sig_b2;
  bad_cbw[3]                           = k_t_bad_sig_b3;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pmsc_feed_cbw(bad_cbw));
  TEST_ASSERT_EQ(k_ra8_pmsc_state_csw_tx, s_usb_pmsc_state.bot_state);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_step());
  TEST_ASSERT_EQ(k_ra8_pmsc_state_idle, s_usb_pmsc_state.bot_state);
  TEST_END("step from CSW_TX rewinds to IDLE");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the single `usb_err != k_ra8_ok`
 * guard in ra8_usb_pmsc_init. Saturating the USB module-stop refcount
 * makes the underlying ra8_usb_device_init fail, which the init maps to
 * k_ra8_err_hw_init_failed.)
 */
static void test_init_hw_init_failed_on_mstp_saturation(void)
{
  TEST_BEGIN("init maps a device_init failure to hw_init_failed");
  prep();

  /* Drive the USB module-stop refcount to its UINT8_MAX ceiling by
   * enabling the controller directly. The next enable -- the one
   * ra8_usb_pmsc_init issues via ra8_usb_device_init -- then returns an
   * error, exercising the init failure leg. */
  for (uint16_t i = 0U; i < (uint16_t)k_test_pmsc_mstp_saturate; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  }

  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_END("init maps a device_init failure to hw_init_failed");
}

/**
 * @test test_mcdc_pmsc
 *
 * @par MC/DC:
 * Covers the compound boolean decision flagged in docs/MCDC_GAPS.csv
 * for libs/ra8_hal/src/ra8_usb_pmsc.c.
 *
 * Decision A (line 817, 2 conds): pmsc_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3:
 *   - V1 FS -> C1=F (short circuit) -> dec=F (init ok)
 *   - V2 HS -> C1=T, C2=F           -> dec=F (init ok)
 *   - V3 9  -> C1=T, C2=T           -> dec=T (invalid_arg)
 * Vectors V1+V3 vary C1 with C2 implicit (decision flips); V2+V3 vary
 * C2 with C1 held T (decision flips). N+1=3 minimal MC/DC.
 */
static void test_mcdc_pmsc(void)
{
  TEST_BEGIN("pmsc MC/DC: init speed gate");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pmsc_init((ra8_usb_speed_t)9U));
  TEST_END("pmsc MC/DC: init speed gate");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_fs_returns_ok,
  test_init_hs_returns_ok,
  test_init_bad_speed,
  test_close_without_init,
  test_pre_init_guards,
  test_pre_attach_guards,
  test_attach_storage_null_validation,
  test_feed_cbw_null_guard,
  test_feed_cbw_bad_signature_emits_phase_error,
  test_inquiry_returns_backend_strings,
  test_read_capacity_returns_count_minus_one_be,
  test_read10_calls_backend_and_returns_512,
  test_write10_calls_backend,
  test_test_unit_ready_no_data_phase,
  test_unsupported_opcode_fails,
  test_step_state_machine_loops,
  test_dispatch_null_arg_rejection,
  test_build_csw_null_guard,
  test_dispatch_request_sense_and_mode_sense,
  test_dispatch_wrong_bot_state_rejected,
  test_dispatch_zero_capacity_rejected,
  test_dispatch_handler_error_marks_failed,
  test_step_from_data_phase_advances_to_csw,
  test_step_from_csw_tx_rewinds_to_idle,
  test_init_hw_init_failed_on_mstp_saturation,
  test_mcdc_pmsc,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_usb_pmsc.c\n");
  return 0;
}
