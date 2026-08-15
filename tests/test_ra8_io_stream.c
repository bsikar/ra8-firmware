/**
 * @file test_ra8_io_stream.c
 * @brief Unit tests for the ra8_io targetable byte-stream layer (issue #157).
 *
 * @details
 * Covers the RAM sink, the no-varargs formatted helpers, the block-device sink
 * end-to-end over a RAM block device (stream bytes -> sectors -> device), the
 * "same call, two targets" property, uart/usb-cdc bind + NULL-guard, handle
 * validation, and the `ra8_log` -> stream redirect captured into a RAM buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_backend.h"
#include "ra8_io_stream_blockdev.h"
#include "ra8_io_stream_ram.h"
#include "ra8_io_stream_uart.h"
#include "ra8_io_stream_usbcdc.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum io_stream_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} io_stream_fixture_t;

/**
 * @enum t_stream_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_ram_cap      = 64,    /**< RAM sink capture capacity.             */
  k_t_bd_blocks    = 4,     /**< RAM block device size for the bd sink. */
  k_t_stream_bytes = 600,   /**< Streamed byte count (one sector + 88). */
  k_t_log_cap      = 256,   /**< RAM sink capacity for the log capture. */
  k_t_first_chunk  = 512,   /**< Bytes landing in the first sector.     */
  k_t_tail_chunk   = 88,    /**< Bytes landing in the padded sector.    */
  k_t_puts_bound   = 65535, /**< Facade string-scan bound.              */
} t_stream_const_t;

/** @brief Behavior selected for the injected implementer-contract probe. */
typedef enum : uint8_t {
  k_probe_exact = 0U,    /**< Accept the requested bytes and succeed.      */
  k_probe_short_success, /**< Claim success after accepting a short write. */
  k_probe_over_success,  /**< Publish an impossible over-request count.    */
  k_probe_error_partial, /**< Fail after accepting one byte.               */
} stream_probe_mode_t;

/** @brief Caller-owned state for the injected stream backend. */
typedef struct {
  stream_probe_mode_t mode;  /**< Response behavior.       */
  uint32_t            calls; /**< Observed callback calls. */
} stream_probe_t;

/** @brief Non-terminated scan-bound fixture for fail-closed puts coverage. */
static char s_overlong[(size_t)k_t_puts_bound];

/**
 * @brief Emit one configured backend result for facade contract tests.
 * @param[in,out] context Probe state.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @param[out] written Published accepted count.
 * @return Configured canonical status.
 * @pre All pointers are non-null and @p bytes spans @p length bytes.
 * @pre @p length is non-zero for the exercised vectors.
 * @post The probe call count advances once.
 * @post @p written reflects the selected contract shape.
 * @note Thread-safe across distinct probe states. @details Exercises the probe write path with bounded caller-owned fixture state and verifies its documented result. @retval k_ra8_ok The fixture operation completed successfully. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_probe_write(void* context, const uint8_t* bytes, uint32_t length, uint32_t* written)
{
  stream_probe_t* probe = (stream_probe_t*)context;
  (void)bytes;
  ++probe->calls;
  if (probe->mode == k_probe_short_success) {
    *written = length - 1U;
    return k_ra8_ok;
  }
  if (probe->mode == k_probe_over_success) {
    *written = length + 1U;
    return k_ra8_ok;
  }
  if (probe->mode == k_probe_error_partial) {
    *written = 1U;
    return k_ra8_fail;
  }
  *written = length;
  return k_ra8_ok;
}

/** @brief Immutable implementer-contract probe operations. */
static const ra8_io_stream_iface_t s_probe_iface = {
  .write = internal_probe_write,
  .flush = nullptr,
};

/** @brief Invalid probe operations with the mandatory writer absent. */
static const ra8_io_stream_iface_t s_missing_write_iface = {
  .write = nullptr,
  .flush = nullptr,
};

/**
 * @brief Discard expected validation logs before the redirect vector runs.
 * @param[in] context Unused logger context.
 * @param[in] byte Unused emitted byte.
 * @return Nothing.
 * @post Host tests never fall through to target ITM MMIO. @details Exercises the log sink path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

/* =============================================================================
 * RAM sink + formatting
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- puts/putc append, ram_used reports the
 * captured length, and an overflowing write reports no_mem) @brief Verify ram sink behavior. @details Executes the ram sink scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_sink(void)
{
  TEST_BEGIN("ram sink");
  uint8_t                   buf[(size_t)k_t_ram_cap] = {};
  ra8_io_stream_ram_state_t st                       = {};
  ra8_io_stream_t           s                        = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, buf, k_t_ram_cap));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_puts(&s, "n="));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_u32(&s, 42U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&s, '\n'));
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT_EQ(5, used);
  TEST_ASSERT(memcmp(buf, "n=42\n", 5) == 0);
  TEST_END("ram sink");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a full buffer rejects the overflowing
 * write with no_mem, after accepting what fits) @brief Verify ram sink full behavior. @details Executes the ram sink full scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_sink_full(void)
{
  TEST_BEGIN("ram sink full");
  uint8_t                   buf[2] = {};
  ra8_io_stream_ram_state_t st     = {};
  ra8_io_stream_t           s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, buf, 2));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_stream_puts(&s, "abc"));
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT_EQ(2, used);
  TEST_END("ram sink full");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- decimal/hex rendering boundary values
 * and min_digits range rejection are independent single-condition checks) @brief Verify formatting behavior. @details Executes the formatting scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_formatting(void)
{
  TEST_BEGIN("formatting");
  uint8_t                   buf[(size_t)k_t_ram_cap] = {};
  ra8_io_stream_ram_state_t st                       = {};
  ra8_io_stream_t           s                        = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, buf, k_t_ram_cap));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_u32(&s, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&s, ' '));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_u32(&s, 4294967295U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&s, ' '));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_hex(&s, 0xDEADBEEFU, 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&s, ' '));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_hex(&s, 0xABU, 4U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&s, ' '));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_put_u64(&s, UINT64_MAX));
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  static const char expected[] = "0 4294967295 deadbeef 00ab 18446744073709551615";
  TEST_ASSERT_EQ(sizeof(expected) - 1U, used);
  TEST_ASSERT(memcmp(buf, expected, sizeof(expected) - 1U) == 0);
  /* min_digits range */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_stream_put_hex(&s, 1U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_stream_put_hex(&s, 1U, 9U));
  TEST_END("formatting");
}

/* =============================================================================
 * Block-device sink end-to-end
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- stream 600 bytes through the sink, flush
 * the padded tail sector, and read both blocks back off the device) @brief Verify blockdev sink behavior. @details Executes the blockdev sink scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_blockdev_sink(void)
{
  TEST_BEGIN("blockdev sink");
  static uint8_t              disk[(size_t)k_t_bd_blocks * (size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &bstate, disk, k_t_bd_blocks, false));

  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&s, &sstate, &bd, 0));

  uint8_t payload[(size_t)k_t_stream_bytes];
  for (uint32_t i = 0; i < (uint32_t)k_t_stream_bytes; ++i) {
    payload[i] = (uint8_t)(((i * 3U) + 1U) & k_byte_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, payload, k_t_stream_bytes, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&s));

  uint8_t blk0[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 0, 1, blk0));
  TEST_ASSERT(memcmp(blk0, payload, (size_t)k_t_first_chunk) == 0);

  uint8_t blk1[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 1, 1, blk1));
  TEST_ASSERT(memcmp(blk1, &payload[(size_t)k_t_first_chunk], (size_t)k_t_tail_chunk) == 0);
  /* tail of the second sector is zero-padded */
  for (uint32_t i = (uint32_t)k_t_tail_chunk; i < (uint32_t)k_ra8_io_block_size_bytes; ++i) {
    TEST_ASSERT_EQ(0, blk1[i]);
  }
  TEST_END("blockdev sink");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the same write call is aimed at two
 * different sinks and both receive the bytes) @brief Verify same call two targets behavior. @details Executes the same call two targets scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_same_call_two_targets(void)
{
  TEST_BEGIN("one call, two targets");
  uint8_t                   rbuf[(size_t)k_t_ram_cap] = {};
  ra8_io_stream_ram_state_t rst                       = {};
  ra8_io_stream_t           a                         = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&a, &rst, rbuf, k_t_ram_cap));

  static uint8_t              disk[(size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &bstate, disk, 1, false));
  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                b      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&b, &sstate, &bd, 0));

  const char* msg = "hello";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_puts(&a, msg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_puts(&b, msg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&b));
  TEST_ASSERT(memcmp(rbuf, msg, 5) == 0);
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 0, 1, blk));
  TEST_ASSERT(memcmp(blk, msg, 5) == 0);
  TEST_END("one call, two targets");
}

/* =============================================================================
 * uart / usb-cdc bind, validation, ra8_log redirect
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- each transport init rejects NULL and
 * binds a valid handle) @brief Verify uart usbcdc bind behavior. @details Executes the uart usbcdc bind scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_uart_usbcdc_bind(void)
{
  TEST_BEGIN("uart/usbcdc bind");
  ra8_io_stream_t            s   = {};
  ra8_io_stream_uart_state_t ust = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_uart_init(nullptr, &ust, 8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &ust, 8));
  ra8_io_stream_t              s2  = {};
  ra8_io_stream_usbcdc_state_t cst = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_usbcdc_init(nullptr, &cst, 0x81));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_usbcdc_init(&s2, &cst, 0x81));
  TEST_END("uart/usbcdc bind");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an unbound / NULL handle is rejected on
 * every entry point) @brief Verify stream validation behavior. @details Executes the stream validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_stream_validation(void)
{
  TEST_BEGIN("stream validation");
  ra8_io_stream_t unbound = {}; /* iface == nullptr */
  uint8_t         b       = 'x';
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_stream_write(&unbound, &b, 1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_stream_flush(&unbound));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_stream_putc(&unbound, 'y'));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(nullptr, &b, 1, nullptr));
  uint8_t                   buf[(size_t)k_t_ram_cap] = {};
  ra8_io_stream_ram_state_t st                       = {};
  ra8_io_stream_t           s                        = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, buf, k_t_ram_cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(&s, nullptr, 1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_puts(&s, nullptr));
  TEST_END("stream validation");
}

/**
 * @brief Reject invalid bindings and impossible backend success reports.
 * @details Proves binding failure is non-mutating, exact success is accepted,
 *          short/over success is classified as a backend protocol defect, and
 *          a real partial error preserves its accepted count.
 * @pre The immutable probe vtables remain valid for the test duration.
 * @pre The probe state is exclusively owned by this test.
 * @post Every vector leaves the handle bound to the same caller state.
 * @post No storage outside local fixtures is modified.
 * @note Single-threaded test helper. @since 0.1.0 */
RA8_INTERNAL static void internal_test_backend_contract(void)
{
  TEST_BEGIN("backend contract");
  stream_probe_t  probe  = {};
  ra8_io_stream_t stream = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_bind(nullptr, &s_probe_iface, &probe));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_bind(&stream, nullptr, &probe));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_bind(&stream, &s_probe_iface, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_stream_bind(&stream, &s_missing_write_iface, &probe));
  TEST_ASSERT(stream.iface == nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_bind(&stream, &s_probe_iface, &probe));

  const uint8_t bytes[2] = {1U, 2U};
  uint32_t      written  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&stream, bytes, sizeof(bytes), &written));
  TEST_ASSERT_EQ(sizeof(bytes), written);
  probe.mode = k_probe_short_success;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_io_stream_write(&stream, bytes, sizeof(bytes), &written));
  TEST_ASSERT_EQ(1U, written);
  probe.mode = k_probe_over_success;
  written    = 77U;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_io_stream_write(&stream, bytes, sizeof(bytes), &written));
  TEST_ASSERT_EQ(77U, written);
  probe.mode = k_probe_error_partial;
  TEST_ASSERT_EQ(k_ra8_fail, ra8_io_stream_write(&stream, bytes, sizeof(bytes), &written));
  TEST_ASSERT_EQ(1U, written);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&stream));
  TEST_END("backend contract");
}

/**
 * @brief Reject a string without a terminator inside the documented bound.
 * @pre ::s_overlong provides exactly ::k_t_puts_bound writable bytes.
 * @pre The probe state is exclusively owned by this test.
 * @post The backend is not called and no prefix is silently emitted.
 * @post The fixture contains no NUL byte.
 * @note Single-threaded test helper. @details Executes the puts bound scenario with bounded fixture state and asserts the contract-specific result. @since 0.1.0 */
RA8_INTERNAL static void internal_test_puts_bound(void)
{
  TEST_BEGIN("puts bound");
  (void)memset(s_overlong, 'x', sizeof(s_overlong));
  stream_probe_t  probe  = {};
  ra8_io_stream_t stream = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_bind(&stream, &s_probe_iface, &probe));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_io_stream_puts(&stream, s_overlong));
  TEST_ASSERT_EQ(0U, probe.calls);
  TEST_END("puts bound");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- attach a stream to ra8_log, emit a line,
 * confirm the capture buffer holds the tag and message, then detach) @brief Verify ra8 log redirect behavior. @details Executes the ra8 log redirect scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ra8_log_redirect(void)
{
  TEST_BEGIN("ra8_log redirect");
  static char               buffer[(size_t)k_t_log_cap] = {};
  ra8_io_stream_ram_state_t st                          = {};
  ra8_io_stream_t           s                           = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, (uint8_t*)buffer, k_t_log_cap - 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_log_attach(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_log_attach(&s));
  ra8_log_init();
  ra8_log_error("TST", "boom");
  ra8_io_log_detach();
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT(used > 0U);
  buffer[used] = '\0';
  TEST_ASSERT(strstr(buffer, "TST") != nullptr);
  TEST_ASSERT(strstr(buffer, "boom") != nullptr);
  TEST_END("ra8_log redirect");
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_ram_sink();
  internal_test_ram_sink_full();
  internal_test_formatting();
  internal_test_blockdev_sink();
  internal_test_same_call_two_targets();
  internal_test_uart_usbcdc_bind();
  internal_test_stream_validation();
  internal_test_backend_contract();
  internal_test_puts_bound();
  internal_test_ra8_log_redirect();
  return 0;
}
