/**
 * @file test_ra8_usb_pmsc_guards.c
 * @brief Argument- and state-guard rejection vectors for the device-MSC
 *        (USB Mass Storage Class) public API in `libs/ra8_hal/src/ra8_usb_pmsc.c`
 *
 * @details
 * The sibling `test_ra8_usb_pmsc.c` drives the happy paths through the public
 * BOT (Bulk-Only Transport) dispatcher, and `test_ra8_usb_pmsc_scsi_cov.c`
 * reaches the SCSI handlers' error legs. This suite covers the third family:
 * the entry guards every public call makes before it does any work -- the
 * pre-init and pre-attach lifecycle rejections, the NULL-struct and
 * NULL-callback validation in `ra8_usb_pmsc_attach_storage()`, and the NULL
 * output-pointer rejections in `ra8_usb_pmsc_feed_cbw()`,
 * `ra8_usb_pmsc_dispatch_command()` and `ra8_usb_pmsc_build_csw()`.
 *
 * These arms only ever assert that a call is *refused*, so the storage backend
 * below is deliberately inert: the four callbacks exist to be non-NULL (that is
 * exactly what the validation checks) and are never invoked. Following the
 * sibling suites, this translation unit shares no state with them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_pmsc.h"
#include "unity_minimal.h"

/** @brief Command-block sizes the guard arms hand to the public API. */
typedef enum : uint8_t {
  k_test_pmsc_cbw_len  = 31U, /**< CBW length, fixed by the BOT spec. */
  k_test_pmsc_data_len = 16U, /**< Scratch data-buffer length for the NULL
                                   output-pointer dispatch vectors.          */
} test_pmsc_len_t;

/* ---- Inert storage backend -------------------------------------------------
 * Present so `attach_storage` sees four non-NULL callbacks; never invoked,
 * because every arm here asserts a call is refused before any I/O happens.
 * ---------------------------------------------------------------------------
 */

/** @brief Inert ::ra8_usb_pmsc_storage_t read callback (never invoked). */
static ra8_err_t stub_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_ok;
}

/** @brief Inert ::ra8_usb_pmsc_storage_t write callback (never invoked). */
static ra8_err_t stub_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_ok;
}

/** @brief Inert ::ra8_usb_pmsc_storage_t capacity callback (never invoked). */
static ra8_err_t stub_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  (void)block_count;
  (void)block_size;
  return k_ra8_ok;
}

/** @brief Inert ::ra8_usb_pmsc_storage_t inquiry callback (never invoked). */
static ra8_err_t
stub_get_inquiry(void* ctx, uint8_t* vendor8, uint8_t* product16, uint8_t* revision4)
{
  (void)ctx;
  (void)vendor8;
  (void)product16;
  (void)revision4;
  return k_ra8_ok;
}

/** @brief A structurally valid storage backend: four non-NULL callbacks. */
static const ra8_usb_pmsc_storage_t s_test_storage = {
  .read_block   = stub_read_block,
  .write_block  = stub_write_block,
  .get_capacity = stub_get_capacity,
  .get_inquiry  = stub_get_inquiry,
  .ctx          = nullptr,
};

/** @brief Return the driver to a closed, freshly-mapped state before each arm. */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pmsc_close();
}

/* ---- Tests ---- */

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
static void test_dispatch_null_arg_rejection(void)
{
  TEST_BEGIN("dispatch_command rejects NULL output pointers");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pmsc_attach_storage(&s_test_storage));

  uint8_t                   data[k_test_pmsc_data_len] = {};
  uint32_t                  dl                         = 0U;
  ra8_usb_pmsc_csw_status_t status                     = k_ra8_pmsc_csw_status_passed;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_pmsc_dispatch_command(nullptr, (uint32_t)k_test_pmsc_data_len, &dl, &status));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_data_len, nullptr, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_pmsc_dispatch_command(data, (uint32_t)k_test_pmsc_data_len, &dl, nullptr));
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
/** @brief Roster of the guard arms, run in order by main(). */
static void (*const s_roster[])(void) = {
  test_pre_init_guards,
  test_pre_attach_guards,
  test_attach_storage_null_validation,
  test_feed_cbw_null_guard,
  test_dispatch_null_arg_rejection,
  test_build_csw_null_guard,
};

/**
 * @brief Test entry point -- runs the pmsc guard suite in order.
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
  for (size_t i = 0U; i < (sizeof(s_roster) / sizeof(s_roster[0])); ++i) {
    s_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_usb_pmsc_guards.c\n");
  return 0;
}
