/**
 * @file test_ra8_nsc.c
 * @brief Unit tests for libs/ra8_nsc (NSC veneer scaffold)
 *
 * @details Exercises XSPI, Ethernet, logging, and substrate-initialization
 *          veneers against the host fake hardware and network PAL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_xspi_flash.h"
#include "ra8_mstp.h"
#include "ra8_net_pal.h"
#include "ra8_nsc.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi.h"
#include "unity_minimal.h"

/**
 * @enum t_nsc_t
 * @brief Buffer capacity and the mask out-parameter seed.
 */
typedef enum : uint16_t {
  k_t_buf_cap    = 64U,     /**< Scratch and frame buffers, bytes. */
  k_t_mask_unset = 0xDEADU, /**< Pre-set mask; a veneer that rejects its input
                                  must leave it rather than report a real mask. */
} t_nsc_t;

/**
 * @brief Reset the host fakes used by the general NSC veneer tests.
 *
 * @details Clears fake MMIO, installs the XSPI flash model, initializes the
 *          module-stop controller, and returns the network PAL to its
 *          uninitialized state.
 *
 * @pre The test runs in the single-threaded host-test process.
 * @pre No other test is concurrently using the global fake peripherals.
 * @post Fake MMIO and the XSPI model are ready for the next vector.
 * @post The network PAL is deinitialized and owns no queued frame.
 *
 * @note This helper intentionally resets process-global test state.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  /* The xspi veneers forward to ra8_xspi_flash_*, whose real register
   * sequence needs the tests/mocks NOR model to service TRREQ kicks. */
  ra8_fake_xspi_flash_install();
  (void)ra8_mstp_init();
  (void)ra8_net_pal_deinit();
}

/* =============================================================================
 * ra8_nsc_xspi_*
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/**
 * @brief Verify XSPI-read argument validation and the healthy forward path.
 *
 * @details Drives null, zero-length, over-limit, and valid reads through the
 *          NSC veneer while the register-level NOR model services the driver.
 *
 * @pre The host fake address space is available to the test process.
 * @pre The XSPI model can be installed by ::internal_prep.
 * @post Invalid requests report their documented errors.
 * @post A valid 64-byte request reaches the modeled flash and succeeds.
 *
 * @note Runs synchronously and owns its stack destination buffer.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_xspi_read_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_xspi_read: arg validation + model-flash forward");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init(0U, k_ra8_xspi_lio_1s1s1s));

  uint8_t buf[k_t_buf_cap] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_xspi_read(0U, nullptr, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_xspi_read(0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_xspi_read(0U, buf, (uint32_t)k_ra8_nsc_xspi_max_read + 1U));

  /* Valid args -> veneer forwards to ra8_xspi_flash_read, serviced by
   * the tests/mocks register-level NOR model. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_xspi_read(0U, buf, 64U));
  TEST_END("ra8_nsc_xspi_read: arg validation + model-flash forward");
}

/**
 * @brief Verify that the XSPI-status veneer forwards to the driver.
 *
 * @details Initializes the modeled XSPI instance, queries status through the
 *          veneer, and verifies that a null output pointer is rejected.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 *
 * @pre The host fake address space is available to the test process.
 * @pre The XSPI model can be installed by ::internal_prep.
 * @post A valid status query returns k_ra8_ok.
 * @post A null status destination returns k_ra8_err_null_ptr.
 *
 * @note The inactive modeled device may report a zero status mask.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_xspi_status_forwards_to_driver(void)
{
  TEST_BEGIN("ra8_nsc_xspi_status: forwards to ra8_xspi");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init(0U, k_ra8_xspi_lio_1s1s1s));

  uint32_t mask = k_t_mask_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_xspi_status(0U, &mask));
  /* Stub: reading status from an inactive xspi returns 0; the
   * point of the test is that the veneer doesn't fail. */

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_xspi_status(0U, nullptr));
  TEST_END("ra8_nsc_xspi_status: forwards to ra8_xspi");
}

/* =============================================================================
 * ra8_nsc_eth_*
 * =============================================================================
 */

static const ra8_net_pal_mac_t s_test_mac = {
  .bytes = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U},
};

/**
 * @brief Verify Ethernet-send validation and forwarding.
 *
 * @details Initializes the network PAL and exercises null, zero-length,
 *          over-limit, and valid frame submissions through the NSC veneer.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 *
 * @pre The host fake address space is available to the test process.
 * @pre The test MAC address is valid for local administration.
 * @post Invalid frames return their documented validation errors.
 * @post A valid frame is accepted by the PAL queue.
 *
 * @note The test owns the PAL until the next ::internal_prep call.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_eth_send_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_eth_send: arg validation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&s_test_mac));

  uint8_t frame[k_t_buf_cap] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_send(nullptr, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_send(frame, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_eth_send(frame, (uint16_t)(k_ra8_nsc_eth_frame_max + 1U)));

  /* Valid args -> veneer forwards to ra8_net_pal, frame is queued. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_eth_send(frame, (uint16_t)sizeof(frame)));
  TEST_END("ra8_nsc_eth_send: arg validation");
}

/**
 * @brief Verify Ethernet-receive validation and the empty-ring result.
 *
 * @details Exercises null output arguments, undersized capacities, a zero
 *          capacity, and a valid receive attempt against an empty PAL ring.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 *
 * @pre The host fake address space is available to the test process.
 * @pre The network PAL can be initialized with ::s_test_mac.
 * @post Invalid destinations return their documented validation errors.
 * @post The valid empty-ring request returns k_ra8_err_no_data.
 *
 * @note The receive buffer is confined to this test invocation.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_eth_recv_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_eth_recv: arg validation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&s_test_mac));

  uint8_t  buf[k_ra8_nsc_eth_frame_max] = {};
  uint16_t len                          = (uint16_t)k_ra8_nsc_eth_frame_max;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_recv(nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_recv(buf, nullptr));
  uint16_t small = 16U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_recv(buf, &small));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_recv(buf, &zero));

  /* Valid args, empty ring -> veneer forwards no_data from the PAL. */
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_nsc_eth_recv(buf, &len));
  TEST_END("ra8_nsc_eth_recv: arg validation");
}

/* =============================================================================
 * ra8_nsc_log_emit
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/**
 * @brief Verify secure logging for ordinary and truncated messages.
 *
 * @details Sends a normal message and an over-capacity message through the
 *          logging veneer and verifies that both accepted calls succeed.
 *
 * @pre The host logging substrate is available.
 * @pre The supplied tag and message strings are NUL-terminated.
 * @post Both logging requests return k_ra8_ok.
 * @post The over-capacity request is handled without reading past its string.
 *
 * @note Exact log rendering is outside this veneer-forwarding test.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_log_emit_happy(void)
{
  TEST_BEGIN("ra8_nsc_log_emit: copies tag + message and returns ok");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_log_emit("TAG", "hello secure world"));
  /* Long message gets truncated; must still return k_ra8_ok. */
  static const char k_long_message[] =
    "this is a very long message that exceeds the secure scratch buffer "
    "k_ra8_nsc_log_msg_max_len cap so the veneer should truncate it before "
    "calling ra8_log_info from the secure side; the return code stays k_ra8_ok.";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_log_emit("LONG", k_long_message));
  TEST_END("ra8_nsc_log_emit: copies tag + message and returns ok");
}

/**
 * @brief Verify that the logging veneer rejects null strings.
 *
 * @details Supplies a null tag and a null message in separate calls and
 *          checks the secure veneer validation result for each case.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 *
 * @pre The host logging substrate is available.
 * @pre Each vector supplies exactly one valid NUL-terminated string.
 * @post A null tag returns k_ra8_err_null_ptr.
 * @post A null message returns k_ra8_err_null_ptr.
 *
 * @note No log payload should be forwarded by either vector.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_log_emit_null(void)
{
  TEST_BEGIN("ra8_nsc_log_emit: NULL pointers rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_log_emit(nullptr, "msg"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_log_emit("tag", nullptr));
  TEST_END("ra8_nsc_log_emit: NULL pointers rejected");
}

/* =============================================================================
 * ra8_nsc_periph_init
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/**
 * @brief Verify idempotent secure peripheral initialization.
 *
 * @details Resets the host fixture, executes the secure peripheral substrate
 *          initializer three times, and checks every result.
 *
 * @pre The fake MMIO mappings are available.
 * @pre The module-stop substrate can be initialized by ::internal_prep.
 * @post The first initialization request returns k_ra8_ok.
 * @post Both repeated requests return k_ra8_ok without failure.
 *
 * @note The production module intentionally retains its initialized latch.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_periph_init_idempotent(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: idempotent");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  /* Second call returns k_ra8_ok via the s_initialized fast-path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_END("ra8_nsc_periph_init: idempotent");
}

int main(void)
{
  internal_test_xspi_read_validates_args();
  internal_test_xspi_status_forwards_to_driver();
  internal_test_eth_send_validates_args();
  internal_test_eth_recv_validates_args();
  internal_test_log_emit_happy();
  internal_test_log_emit_null();
  internal_test_periph_init_idempotent();
  return 0;
}
