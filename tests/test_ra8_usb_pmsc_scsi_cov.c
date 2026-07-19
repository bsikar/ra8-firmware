/**
 * @file test_ra8_usb_pmsc_scsi_cov.c
 * @brief Branch-coverage tests for the device-MSC SCSI command
 *        handlers in `libs/ra8_hal/src/ra8_usb_pmsc_scsi.c`
 *
 * @details
 * The sibling `test_ra8_usb_pmsc.c` drives the happy paths through the
 * public BOT dispatcher. This companion suite reaches the error /
 * degenerate legs that the public path cannot present deterministically:
 * the buffer-too-small guards, the storage-backend error returns, the
 * zero-block-count short-circuits, and the zero-block-size default. It
 * calls the TU-external `internal_handle_*` handlers directly (they are
 * declared non-static in `ra8_usb_pmsc_internal.h` precisely for this
 * MC/DC access), pre-seeding the extern `s_usb_pmsc_state` shadow
 * singleton so each handler sees a deterministic CDB + storage backend.
 *
 * Every branch here is driven by ordinary function inputs and mocked
 * storage callbacks; none of it touches the USB controller registers,
 * so no lines require GCOVR_EXCL markers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_pmsc.h"
#include "ra8_usb_pmsc_internal.h"
#include "unity_minimal.h"

/**
 * @enum usb_pmsc_scsi_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_usb_pmsc_scsi_cov_lba_24 = 24U,
  k_usb_pmsc_scsi_cov_val_ff = 0xFFU,
} usb_pmsc_scsi_cov_uint8_const_t;

/* =============================================================================
 * Test fixtures
 * =============================================================================
 */

typedef enum : uint16_t {
  k_test_buf_capacity = 1024U, /**< Generous response buffer. */
} test_buf_lim_t;

typedef enum : uint32_t {
  k_test_block_count = 1000U, /**< Reported total block count.  */
  k_test_block_size  = 512U,  /**< Reported block size (bytes). */
} test_capacity_t;

/**
 * @enum test_cdb_off_t
 * @brief READ(10) / WRITE(10) CDB byte offsets the decoder reads.
 *
 * @details Mirrors the private offsets in `ra8_usb_pmsc_scsi.c` so the
 * test can pre-seed the cached CDB without going through `feed_cbw`.
 */
typedef enum : uint8_t {
  k_test_cdb_lba_msb = 2U, /**< LBA byte 3 (big-endian). */
  k_test_cdb_lba_b1  = 3U, /**< LBA byte 2.              */
  k_test_cdb_lba_b2  = 4U, /**< LBA byte 1.              */
  k_test_cdb_lba_lsb = 5U, /**< LBA byte 0.              */
  k_test_cdb_cnt_msb = 7U, /**< Transfer count high.     */
  k_test_cdb_cnt_lsb = 8U, /**< Transfer count low.      */
} test_cdb_off_t;

typedef enum : uint16_t {
  k_test_small_cap    = 4U,  /**< Below INQUIRY / SENSE minima. */
  k_test_tiny_cap     = 2U,  /**< Below MODE SENSE(6) minimum.  */
  k_test_partial_read = 100U /**< Below one 512-byte block.     */
} test_small_cap_t;

/**
 * @var s_test_error
 * @brief Distinct non-success code the failing mock backends return.
 *
 * @details Chosen distinct from `k_ra8_err_invalid_size` so a test can
 * tell a backend-propagated failure apart from a size-guard rejection.
 * @note Read-only after definition.
 * @since 0.1.0
 */
static const ra8_err_t s_test_error = k_ra8_err_hw_error;

/* ---- Mock storage callbacks ---- */

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t cb_read_ok(void* ctx, uint32_t lba, uint32_t block_count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)block_count;
  (void)buf;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t cb_read_err(void* ctx, uint32_t lba, uint32_t block_count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)block_count;
  (void)buf;
  return k_ra8_err_hw_error;
}

static ra8_err_t cb_write_ok(void* ctx, uint32_t lba, uint32_t block_count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)block_count;
  (void)buf;
  return k_ra8_ok;
}

static ra8_err_t cb_write_err(void* ctx, uint32_t lba, uint32_t block_count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)block_count;
  (void)buf;
  return k_ra8_err_hw_error;
}

static ra8_err_t cb_cap_normal(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = (uint32_t)k_test_block_count;
  *block_size  = (uint32_t)k_test_block_size;
  return k_ra8_ok;
}

static ra8_err_t cb_cap_err(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = 0U;
  *block_size  = 0U;
  return k_ra8_err_hw_error;
}

static ra8_err_t cb_cap_bs_zero(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = (uint32_t)k_test_block_count;
  *block_size  = 0U; /* Forces the k_ra8_pmsc_block_size_default fallback. */
  return k_ra8_ok;
}

static ra8_err_t cb_cap_count_zero(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = 0U; /* Exercises the last-LBA "degrade to 0" branch. */
  *block_size  = (uint32_t)k_test_block_size;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t cb_inq_ok(void* ctx, uint8_t* vendor8, uint8_t* product16, uint8_t* revision4)
{
  (void)ctx;
  (void)vendor8;
  (void)product16;
  (void)revision4;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t cb_inq_err(void* ctx, uint8_t* vendor8, uint8_t* product16, uint8_t* revision4)
{
  (void)ctx;
  (void)vendor8;
  (void)product16;
  (void)revision4;
  return k_ra8_err_hw_error;
}

static const ra8_usb_pmsc_storage_t s_ok_storage = {
  .read_block   = cb_read_ok,
  .write_block  = cb_write_ok,
  .get_capacity = cb_cap_normal,
  .get_inquiry  = cb_inq_ok,
  .ctx          = nullptr,
};

/* ---- Harness helpers ---- */

/**
 * @brief Reset the driver, bring it up FS, and attach `storage`.
 *
 * @details Mirrors `test_ra8_usb_pmsc.c`'s `prep()` sequence so the
 * shadow singleton (`s_usb_pmsc_state.storage`) is populated exactly as
 * production would leave it, then lets each test pre-seed the cached CDB
 * directly.
 *
 * @param[in] storage Backend to bind. Must have four non-null hooks.
 */
static void setup_with(const ra8_usb_pmsc_storage_t* storage)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pmsc_close();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(storage));
}

/**
 * @brief Seed the cached READ/WRITE(10) CDB in the shadow singleton.
 *
 * @param[in] lba Logical block address to encode (big-endian).
 * @param[in] block_count Transfer block count to encode.
 */
static void seed_rw_cdb(uint32_t lba, uint16_t block_count)
{
  for (uint8_t i = 0U; i < k_ra8_pmsc_cdb_max_len; ++i) {
    s_usb_pmsc_state.cbw_cdb[i] = 0U;
  }
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_lba_msb] =
    (uint8_t)((lba >> k_usb_pmsc_scsi_cov_lba_24) & k_usb_pmsc_scsi_cov_val_ff);
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_lba_b1] =
    (uint8_t)((lba >> 16U) & k_usb_pmsc_scsi_cov_val_ff);
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_lba_b2] = (uint8_t)((lba >> 8U) & k_usb_pmsc_scsi_cov_val_ff);
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_lba_lsb] = (uint8_t)(lba & k_usb_pmsc_scsi_cov_val_ff);
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_cnt_msb] =
    (uint8_t)((block_count >> 8U) & k_usb_pmsc_scsi_cov_val_ff);
  s_usb_pmsc_state.cbw_cdb[k_test_cdb_cnt_lsb] =
    (uint8_t)(block_count & k_usb_pmsc_scsi_cov_val_ff);
}

/* =============================================================================
 * INQUIRY handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- the size guard `capacity < resp_len` is a
 * single condition; this vector drives it true.)
 */
static void test_inquiry_buffer_too_small(void)
{
  TEST_BEGIN("INQUIRY rejects a buffer smaller than the 36-byte response");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_handle_inquiry(buf, (uint32_t)k_test_small_cap, &out_len));
  TEST_END("INQUIRY rejects a buffer smaller than the 36-byte response");
}

/**
 * @par MC/DC:
 * (no compound decisions -- the guard `err != k_ra8_ok` is a single
 * condition; the failing get_inquiry backend drives it true.)
 */
static void test_inquiry_backend_error_propagates(void)
{
  TEST_BEGIN("INQUIRY propagates a get_inquiry backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_inquiry            = cb_inq_err;
  setup_with(&st);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error,
                 internal_handle_inquiry(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_END("INQUIRY propagates a get_inquiry backend error");
}

/**
 * @par MC/DC:
 * (no compound decisions -- happy path; asserts the response length.)
 */
static void test_inquiry_happy_path_len(void)
{
  TEST_BEGIN("INQUIRY happy path yields a 36-byte response");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_handle_inquiry(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_ASSERT_EQ(k_ra8_pmsc_inquiry_resp_len, out_len);
  /* Byte 1 removable-medium bit is always set. */
  TEST_ASSERT_EQ(0x80U, buf[1]);
  TEST_END("INQUIRY happy path yields a 36-byte response");
}

/* =============================================================================
 * READ_CAPACITY(10) handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- single size guard driven true.)
 */
static void test_read_capacity_buffer_too_small(void)
{
  TEST_BEGIN("READ_CAPACITY(10) rejects a buffer smaller than 8 bytes");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_handle_read_capacity(buf, (uint32_t)k_test_small_cap, &out_len));
  TEST_END("READ_CAPACITY(10) rejects a buffer smaller than 8 bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `err != k_ra8_ok` guard driven true.)
 */
static void test_read_capacity_backend_error_propagates(void)
{
  TEST_BEGIN("READ_CAPACITY(10) propagates a get_capacity backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_err;
  setup_with(&st);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error,
                 internal_handle_read_capacity(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_END("READ_CAPACITY(10) propagates a get_capacity backend error");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the `block_count == 0` degrade
 * branch of the last-LBA ternary; last LBA must clamp to 0.)
 */
static void test_read_capacity_zero_block_count_degrades(void)
{
  TEST_BEGIN("READ_CAPACITY(10) reports last LBA 0 when block count is 0");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_count_zero;
  setup_with(&st);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_handle_read_capacity(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_ASSERT_EQ(k_ra8_pmsc_read_capacity_resp_len, out_len);
  /* Last LBA (bytes 0..3) is the big-endian zero. */
  TEST_ASSERT_EQ(0x00U, buf[0]);
  TEST_ASSERT_EQ(0x00U, buf[1]);
  TEST_ASSERT_EQ(0x00U, buf[2]);
  TEST_ASSERT_EQ(0x00U, buf[3]);
  TEST_END("READ_CAPACITY(10) reports last LBA 0 when block count is 0");
}

/* =============================================================================
 * REQUEST SENSE handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- single size guard driven true.)
 */
static void test_request_sense_buffer_too_small(void)
{
  TEST_BEGIN("REQUEST SENSE rejects a buffer smaller than 18 bytes");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_handle_request_sense(buf, (uint32_t)k_test_small_cap, &out_len));
  TEST_END("REQUEST SENSE rejects a buffer smaller than 18 bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions -- happy path; asserts the canned sense
 * response code and additional-length bytes.)
 */
static void test_request_sense_happy_path(void)
{
  TEST_BEGIN("REQUEST SENSE returns a canned no-sense response");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_handle_request_sense(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_ASSERT_EQ(k_ra8_pmsc_request_sense_resp_len, out_len);
  /* Byte 0 = response code 0x70; byte 7 = additional length 0x0A. */
  TEST_ASSERT_EQ(0x70U, buf[0]);
  TEST_ASSERT_EQ(0x0AU, buf[7]);
  TEST_END("REQUEST SENSE returns a canned no-sense response");
}

/* =============================================================================
 * MODE SENSE(6) handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- single size guard driven true.)
 */
static void test_mode_sense_buffer_too_small(void)
{
  TEST_BEGIN("MODE SENSE(6) rejects a buffer smaller than 4 bytes");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_handle_mode_sense(buf, (uint32_t)k_test_tiny_cap, &out_len));
  TEST_END("MODE SENSE(6) rejects a buffer smaller than 4 bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions -- happy path; asserts the header-only reply.)
 */
static void test_mode_sense_happy_path(void)
{
  TEST_BEGIN("MODE SENSE(6) returns a header-only 4-byte response");
  setup_with(&s_ok_storage);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_handle_mode_sense(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_ASSERT_EQ(k_ra8_pmsc_mode_sense_resp_len, out_len);
  /* Byte 0 = mode data length 3 (header-only). */
  TEST_ASSERT_EQ(0x03U, buf[0]);
  TEST_END("MODE SENSE(6) returns a header-only 4-byte response");
}

/* =============================================================================
 * READ(10) handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- single `block_count == 0` guard driven
 * true; a zero-count READ(10) is a legal no-op.)
 */
static void test_read10_zero_block_count_noop(void)
{
  TEST_BEGIN("READ(10) with zero block count is a no-op");
  setup_with(&s_ok_storage);
  seed_rw_cdb(0U, 0U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_ok, internal_handle_read10(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_ASSERT_EQ(0U, out_len);
  TEST_END("READ(10) with zero block count is a no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `cap_err != k_ra8_ok` guard driven
 * true by the failing get_capacity backend.)
 */
static void test_read10_capacity_error_propagates(void)
{
  TEST_BEGIN("READ(10) propagates a get_capacity backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_err;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error,
                 internal_handle_read10(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_END("READ(10) propagates a get_capacity backend error");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `block_size == 0` guard driven true;
 * the handler must fall back to the 512-byte default block size.)
 */
static void test_read10_zero_block_size_defaults_512(void)
{
  TEST_BEGIN("READ(10) falls back to 512-byte blocks when backend reports 0");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_bs_zero;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_handle_read10(buf, (uint32_t)k_test_buf_capacity, &out_len));
  /* One block at the default 512-byte size. */
  TEST_ASSERT_EQ(k_test_block_size, out_len);
  TEST_END("READ(10) falls back to 512-byte blocks when backend reports 0");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `bytes > capacity` guard driven
 * true; the request exceeds the destination buffer.)
 */
static void test_read10_exceeds_capacity(void)
{
  TEST_BEGIN("READ(10) rejects a request larger than the destination buffer");
  setup_with(&s_ok_storage);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  /* One 512-byte block will not fit in a 100-byte window. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_handle_read10(buf, (uint32_t)k_test_partial_read, &out_len));
  TEST_END("READ(10) rejects a request larger than the destination buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `err != k_ra8_ok` guard driven true
 * by the failing read_block backend.)
 */
static void test_read10_backend_error_propagates(void)
{
  TEST_BEGIN("READ(10) propagates a read_block backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.read_block             = cb_read_err;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error,
                 internal_handle_read10(buf, (uint32_t)k_test_buf_capacity, &out_len));
  TEST_END("READ(10) propagates a read_block backend error");
}

/* =============================================================================
 * WRITE(10) handler
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- single `block_count == 0` guard driven
 * true; a zero-count WRITE(10) is a legal no-op.)
 */
static void test_write10_zero_block_count_noop(void)
{
  TEST_BEGIN("WRITE(10) with zero block count is a no-op");
  setup_with(&s_ok_storage);
  seed_rw_cdb(0U, 0U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(k_ra8_ok, internal_handle_write10(buf, &out_len));
  TEST_ASSERT_EQ(0U, out_len);
  TEST_END("WRITE(10) with zero block count is a no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `cap_err != k_ra8_ok` guard driven
 * true by the failing get_capacity backend.)
 */
static void test_write10_capacity_error_propagates(void)
{
  TEST_BEGIN("WRITE(10) propagates a get_capacity backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_err;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error, internal_handle_write10(buf, &out_len));
  TEST_END("WRITE(10) propagates a get_capacity backend error");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `block_size == 0` guard driven true;
 * the handler must fall back to the 512-byte default block size.)
 */
static void test_write10_zero_block_size_defaults_512(void)
{
  TEST_BEGIN("WRITE(10) falls back to 512-byte blocks when backend reports 0");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.get_capacity           = cb_cap_bs_zero;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_handle_write10(buf, &out_len));
  /* One block at the default 512-byte size. */
  TEST_ASSERT_EQ(k_test_block_size, out_len);
  TEST_END("WRITE(10) falls back to 512-byte blocks when backend reports 0");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single `err != k_ra8_ok` guard driven true
 * by the failing write_block backend.)
 */
static void test_write10_backend_error_propagates(void)
{
  TEST_BEGIN("WRITE(10) propagates a write_block backend error");
  ra8_usb_pmsc_storage_t st = s_ok_storage;
  st.write_block            = cb_write_err;
  setup_with(&st);
  seed_rw_cdb(0U, 1U);

  uint8_t  buf[k_test_buf_capacity] = {};
  uint32_t out_len                  = k_usb_pmsc_scsi_cov_val_ff;
  TEST_ASSERT_EQ(s_test_error, internal_handle_write10(buf, &out_len));
  TEST_END("WRITE(10) propagates a write_block backend error");
}

int32_t main(void)
{
  test_inquiry_buffer_too_small();
  test_inquiry_backend_error_propagates();
  test_inquiry_happy_path_len();

  test_read_capacity_buffer_too_small();
  test_read_capacity_backend_error_propagates();
  test_read_capacity_zero_block_count_degrades();

  test_request_sense_buffer_too_small();
  test_request_sense_happy_path();

  test_mode_sense_buffer_too_small();
  test_mode_sense_happy_path();

  test_read10_zero_block_count_noop();
  test_read10_capacity_error_propagates();
  test_read10_zero_block_size_defaults_512();
  test_read10_exceeds_capacity();
  test_read10_backend_error_propagates();

  test_write10_zero_block_count_noop();
  test_write10_capacity_error_propagates();
  test_write10_zero_block_size_defaults_512();
  test_write10_backend_error_propagates();

  (void)fprintf(stderr, "[OK ] test_ra8_usb_pmsc_scsi_cov.c\n");
  return 0;
}
