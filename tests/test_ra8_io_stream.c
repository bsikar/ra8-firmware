/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_io_stream.c
 * @brief Unit tests for the ra8_io targetable byte-stream layer (issue #157).
 *
 * @details
 * Covers the RAM sink, the no-varargs formatted helpers, the block-device sink
 * end-to-end over a RAM block device (stream bytes -> sectors -> device), the
 * "same call, two targets" property, uart/usb-cdc bind + NULL-guard, handle
 * validation, and the `ra8_log` -> stream redirect captured into a RAM buffer.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
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
  k_t_ram_cap      = 64,  /**< RAM sink capture capacity.             */
  k_t_bd_blocks    = 4,   /**< RAM block device size for the bd sink. */
  k_t_stream_bytes = 600, /**< Streamed byte count (one sector + 88). */
  k_t_log_cap      = 256, /**< RAM sink capacity for the log capture. */
  k_t_first_chunk  = 512, /**< Bytes landing in the first sector.     */
  k_t_tail_chunk   = 88,  /**< Bytes landing in the padded sector.    */
} t_stream_const_t;

/* =============================================================================
 * RAM sink + formatting
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- puts/putc append, ram_used reports the
 * captured length, and an overflowing write reports no_mem)
 */
static void test_ram_sink(void)
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
 * write with no_mem, after accepting what fits)
 */
static void test_ram_sink_full(void)
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
 * (no compound decisions under test -- decimal/hex rendering boundary values and
 * min_digits range rejection are independent single-condition checks)
 */
static void test_formatting(void)
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
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT(memcmp(buf, "0 4294967295 deadbeef 00ab", 26) == 0);
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
 * the padded tail sector, and read both blocks back off the device)
 */
static void test_blockdev_sink(void)
{
  TEST_BEGIN("blockdev sink");
  static uint8_t              s_disk[(size_t)k_t_bd_blocks * (size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &bstate, s_disk, k_t_bd_blocks, false));

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
 * different sinks and both receive the bytes)
 */
static void test_same_call_two_targets(void)
{
  TEST_BEGIN("one call, two targets");
  uint8_t                   rbuf[(size_t)k_t_ram_cap] = {};
  ra8_io_stream_ram_state_t rst                       = {};
  ra8_io_stream_t           a                         = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&a, &rst, rbuf, k_t_ram_cap));

  static uint8_t              s_disk[(size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &bstate, s_disk, 1, false));
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
 * binds a valid handle)
 */
static void test_uart_usbcdc_bind(void)
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
 * every entry point)
 */
static void test_stream_validation(void)
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
 * @par MC/DC:
 * (no compound decisions under test -- attach a stream to ra8_log, emit a line,
 * confirm the capture buffer holds the tag and message, then detach)
 */
static void test_ra8_log_redirect(void)
{
  TEST_BEGIN("ra8_log redirect");
  static char               s_buf[(size_t)k_t_log_cap] = {};
  ra8_io_stream_ram_state_t st                         = {};
  ra8_io_stream_t           s                          = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, (uint8_t*)s_buf, k_t_log_cap - 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_log_attach(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_log_attach(&s));
  ra8_log_init();
  ra8_log_error("TST", "boom");
  ra8_io_log_detach();
  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT(used > 0U);
  s_buf[used] = '\0';
  TEST_ASSERT(strstr(s_buf, "TST") != nullptr);
  TEST_ASSERT(strstr(s_buf, "boom") != nullptr);
  TEST_END("ra8_log redirect");
}

int32_t main(void)
{
  test_ram_sink();
  test_ram_sink_full();
  test_formatting();
  test_blockdev_sink();
  test_same_call_two_targets();
  test_uart_usbcdc_bind();
  test_stream_validation();
  test_ra8_log_redirect();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_stream.c\n");
  return 0;
}
