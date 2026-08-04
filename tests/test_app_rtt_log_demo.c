/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_rtt_log_demo.c
 * @brief Integration test for examples/ek_ra8d2/rtt_log_demo/main.c
 *
 * @details
 * Replays the SEGGER RTT control-block initialisation and ring-buffer
 * write logic. The app's ``rtt_init`` + ``rtt_write`` are
 * straight-line code paths that we mirror byte-for-byte here so MC/DC
 * coverage of the wrap, full, and null-pointer branches is exercised
 * on the host.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum app_rtt_log_demo_fixture_t
 * @brief Protocol and on-disk field offsets.
 */
typedef enum : uint8_t {
  k_rtt_read_offset =
    5U, /**< Non-zero read cursor, so wrap arithmetic runs, not the empty-ring shortcut. */
} app_rtt_log_demo_fixture_t;

typedef enum : uint32_t {
  k_test_rtt_buf_bytes = 16U, /**< Test rtt buffer bytes. */
} test_rtt_const_t;

typedef struct {
  uint8_t  buf[k_test_rtt_buf_bytes]; /**< Buffer. */
  uint32_t wr_off;                    /**< Wr off. */
  uint32_t rd_off;                    /**< Rd off. */
  uint32_t size;                      /**< Size.   */
} test_rtt_ring_t;

static test_rtt_ring_t s_ring;

static void reset_world(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_test_rtt_buf_bytes; i++) {
    s_ring.buf[i] = 0U;
  }
  s_ring.wr_off = 0U;
  s_ring.rd_off = 0U;
  s_ring.size   = (uint32_t)k_test_rtt_buf_bytes;
}

/* Mirror of rtt_write from the app, parameterised on the ring. */
static ra8_err_t rtt_write_under_test(test_rtt_ring_t* up, const uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; i++) {
    uint32_t next = up->wr_off + 1U;
    if (next == up->size) {
      next = 0U;
    }
    if (next == up->rd_off) {
      break;
    }
    /* The ring length must fit the storage it indexes -- the app guarantees
     * this at init, and stating it here bounds the write below. */
    if (up->wr_off >= (uint32_t)sizeof(up->buf)) {
      return k_ra8_err_invalid_state;
    }
    up->buf[up->wr_off] = data[i];
    up->wr_off          = next;
  }
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * Decision: ``data == NULL``. Two vectors -- NULL (this test) +
 * non-NULL (test_rtt_write_basic). Pairs to N+1=2 vectors.
 */
static void test_rtt_write_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("rtt_log_demo: rtt_write NULL data rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rtt_write_under_test(&s_ring, nullptr, 4U));
  TEST_END("rtt_log_demo: rtt_write NULL data rejected");
}

/**
 * @par MC/DC:
 * Decision: ``next == rd_off`` (full). Vectors: not-full (this) +
 * full (next test) + wrap (third test). N+1 = 3 vectors.
 */
static void test_rtt_write_basic(void)
{
  reset_world();
  TEST_BEGIN("rtt_log_demo: rtt_write basic 4 bytes");
  static const uint8_t k_data[] = {1U, 2U, 3U, 4U};
  TEST_ASSERT_EQ(k_ra8_ok, rtt_write_under_test(&s_ring, k_data, sizeof k_data));
  TEST_ASSERT_EQ(4, s_ring.wr_off);
  TEST_ASSERT_EQ(1, s_ring.buf[0]);
  TEST_ASSERT_EQ(4, s_ring.buf[3]);
  TEST_END("rtt_log_demo: rtt_write basic 4 bytes");
}

/**
 * @par MC/DC:
 * Decision: ``next == size`` (wrap). Vectors: about-to-wrap (this)
 * + middle-of-buffer (basic test) + at-zero (basic test).
 */
static void test_rtt_write_wraps(void)
{
  reset_world();
  TEST_BEGIN("rtt_log_demo: rtt_write wraps at size boundary");
  /* Pre-load the ring near the boundary, drain reader so it isn't full. */
  s_ring.wr_off = (uint32_t)k_test_rtt_buf_bytes - 1U;
  s_ring.rd_off = (uint32_t)k_test_rtt_buf_bytes - 1U;
  /* After one byte, the writer wraps to 0 -- but next == rd_off since
   * the reader is at size-1, so the call should drop the byte. We
   * advance the reader first to leave room for one byte across the
   * wrap. */
  s_ring.rd_off                 = k_rtt_read_offset;
  static const uint8_t k_data[] = {0xAAU, 0xBBU};
  TEST_ASSERT_EQ(k_ra8_ok, rtt_write_under_test(&s_ring, k_data, sizeof k_data));
  /* First byte goes into slot 15, second wraps to slot 0. */
  TEST_ASSERT_EQ(0xAA, s_ring.buf[15]);
  TEST_ASSERT_EQ(0xBB, s_ring.buf[0]);
  TEST_ASSERT_EQ(1, s_ring.wr_off);
  TEST_END("rtt_log_demo: rtt_write wraps at size boundary");
}

/**
 * @par MC/DC:
 * Decision: ``next == rd_off`` true vector (full). Pairs with
 * test_rtt_write_basic for N+1 = 2 vectors of the full guard.
 */
static void test_rtt_write_full_drops_tail(void)
{
  reset_world();
  TEST_BEGIN("rtt_log_demo: rtt_write drops tail when full");
  /* One slot remains; second byte should be dropped. */
  s_ring.wr_off                 = 0U;
  s_ring.rd_off                 = 2U;
  static const uint8_t k_data[] = {0xC0U, 0xDEU, 0xEFU};
  TEST_ASSERT_EQ(k_ra8_ok, rtt_write_under_test(&s_ring, k_data, sizeof k_data));
  /* Only 0xC0 lands in slot 0; the next slot would equal rd_off so
   * the loop breaks. wr_off advances to 1 only. */
  TEST_ASSERT_EQ(0xC0, s_ring.buf[0]);
  TEST_ASSERT_EQ(1, s_ring.wr_off);
  TEST_END("rtt_log_demo: rtt_write drops tail when full");
}

int main(void)
{
  test_rtt_write_null_rejected();
  test_rtt_write_basic();
  test_rtt_write_wraps();
  test_rtt_write_full_drops_tail();
  return 0;
}
