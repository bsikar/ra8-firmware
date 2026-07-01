/**
 * @file test_ra_ble_gatt_client_cov.c
 * @brief Coverage-completion unit tests for the GATT client wrapper.
 *
 * @details
 * Companion to test_ra_ble_gatt_client.c. The base suite leaves three
 * host-reachable regions of libs/ra_ble_host/src/ra_ble_gatt_client.c
 * uncovered because it never launches a successful read, never fills
 * the subscription table, and never calls the pending-count test hook:
 *
 *   - ra_ble_gatt_read() body past the NULL-callback guard: the busy
 *     re-entry check, the pending-slot latch, and the success return
 *     (the base suite only drives the NULL-callback early return).
 *   - the "subscription table full" leg of ra_ble_gatt_subscribe()
 *     that returns k_ra_err_no_mem.
 *   - ra_ble_gatt_client_test_pending_count(), the in-flight-request
 *     counter test hook, including each of its three increments.
 *
 * All three regions are ordinary host-mode logic -- this TU is built
 * with RA_SIMULATOR_MODE, so the RA_TARGET_BUILD NimBLE call sites are
 * compiled out and every line below is driven purely by pre-seeding
 * the module's static bookkeeping through the public API. No MMIO,
 * no timers, no SIGALRM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_ble_gatt_client.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern uint32_t ra_ble_gatt_client_test_pending_count(void);
extern void     ra_ble_gatt_client_test_reset(void);

/** @brief Read-completion callback sink (target build only invokes it). */
static void cov_read_cb(void* ctx, const uint8_t* data, uint16_t len, uint16_t status)
{
  (void)ctx;
  (void)data;
  (void)len;
  (void)status;
}

/** @brief Discovery-completion callback sink. */
static void cov_disc_cb(void* ctx, const ra_ble_gatt_service_t* svc, uint16_t status)
{
  (void)ctx;
  (void)svc;
  (void)status;
}

/** @brief Write-completion callback sink. */
static void cov_write_cb(void* ctx, uint16_t status)
{
  (void)ctx;
  (void)status;
}

/**
 * @test test_read_launch_then_busy
 *
 * @details Drives ra_ble_gatt_read() past the NULL-callback guard that
 * the base suite stops at. The first call latches the pending-read
 * singleton (in_use, conn_handle, read_cb, ctx) and returns k_ra_ok
 * via the host-build success path. The second call, with the singleton
 * still latched, takes the busy re-entry return.
 *
 * @par MC/DC:
 * Decision: `if (s_pending_read.in_use != 0U)`
 * (1 condition, ra_ble_gatt_read).
 * - V1 in_use == 0 (fresh reset) -> Decision F: request is launched,
 *   returns k_ra_ok.
 * - V2 in_use != 0 (latched by V1) -> Decision T: returns k_ra_err_busy.
 * Single-condition decision; V1+V2 give both outcomes -> minimal MC/DC.
 */
static void test_read_launch_then_busy(void)
{
  TEST_BEGIN("test_read_launch_then_busy");
  ra_ble_gatt_client_test_reset();
  /* V1: no read in flight -> launch succeeds, latches the singleton. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_read(0x40U, 0x21U, cov_read_cb, nullptr));
  /* One request is now in flight. */
  TEST_ASSERT_EQ(1U, ra_ble_gatt_client_test_pending_count());
  /* V2: a read is already in flight -> the second launch is rejected. */
  TEST_ASSERT_EQ(k_ra_err_busy, ra_ble_gatt_read(0x40U, 0x22U, cov_read_cb, nullptr));
  TEST_END("test_read_launch_then_busy");
}

/**
 * @test test_subscribe_table_full
 *
 * @details Exhausts the four-entry subscription table so the next
 * subscribe hits the "no free slot" return (k_ra_err_no_mem). Each of
 * the first four subscribes uses a distinct (conn_handle, cccd_handle)
 * pair, so the reuse-search never matches and every call consumes a
 * fresh slot. The slot is claimed before the inner CCCD Write_Request
 * runs, so the first subscribe latches the shared pending-write
 * singleton and later subscribes see that inner write return busy --
 * but the slot has already been allocated, so the table still fills.
 * The fifth subscribe finds no free slot and returns k_ra_err_no_mem.
 *
 * @par MC/DC:
 * Decision: `if (slot == nullptr)` (the allocation-failed guard,
 * 1 condition).
 * - The four filling calls leave a non-NULL slot -> Decision F.
 * - The fifth call finds the table full, slot stays NULL ->
 *   Decision T: returns k_ra_err_no_mem.
 * Both outcomes of the single-condition decision are exercised.
 */
static void test_subscribe_table_full(void)
{
  TEST_BEGIN("test_subscribe_table_full");
  ra_ble_gatt_client_test_reset();
  /* Fill all four slots with distinct (conn, cccd) pairs. The first
   * call latches the pending-write singleton (Write_Request response);
   * calls 2..4 therefore see the inner write return busy, but the slot
   * is already claimed, so either terminal code is acceptable here. */
  ra_err_t e1 = ra_ble_gatt_subscribe(0x40U, 0x10U, 1U, 0U, nullptr, nullptr);
  TEST_ASSERT(e1 == k_ra_ok || e1 == k_ra_err_busy);
  ra_err_t e2 = ra_ble_gatt_subscribe(0x41U, 0x11U, 1U, 0U, nullptr, nullptr);
  TEST_ASSERT(e2 == k_ra_ok || e2 == k_ra_err_busy);
  ra_err_t e3 = ra_ble_gatt_subscribe(0x42U, 0x12U, 1U, 0U, nullptr, nullptr);
  TEST_ASSERT(e3 == k_ra_ok || e3 == k_ra_err_busy);
  ra_err_t e4 = ra_ble_gatt_subscribe(0x43U, 0x13U, 1U, 0U, nullptr, nullptr);
  TEST_ASSERT(e4 == k_ra_ok || e4 == k_ra_err_busy);
  /* Table is full: a fifth distinct subscription cannot allocate. */
  TEST_ASSERT_EQ(k_ra_err_no_mem, ra_ble_gatt_subscribe(0x44U, 0x14U, 1U, 0U, nullptr, nullptr));
  TEST_END("test_subscribe_table_full");
}

/**
 * @test test_pending_count_none_then_all
 *
 * @details Exercises ra_ble_gatt_client_test_pending_count() across
 * both extremes so every branch of its three independent in_use
 * checks is taken. Immediately after reset all three singletons are
 * clear, so the counter returns 0 (each `if` false). Then a discovery,
 * a read, and a Write_Request are launched to latch all three
 * singletons, and the counter returns 3 (each `if` true, each
 * increment taken).
 *
 * @par MC/DC:
 * Three independent single-condition decisions
 * (`s_pending_disc.in_use != 0`, `s_pending_read.in_use != 0`,
 * `s_pending_write.in_use != 0`).
 * - Post-reset call: all three false -> counter 0.
 * - All-latched call: all three true -> counter 3.
 * Each condition is observed both false and true; no compound `&&`/`||`.
 */
static void test_pending_count_none_then_all(void)
{
  TEST_BEGIN("test_pending_count_none_then_all");
  ra_ble_gatt_client_test_reset();
  /* Nothing in flight: every in_use check is false. */
  TEST_ASSERT_EQ(0U, ra_ble_gatt_client_test_pending_count());

  /* Latch all three singletons via the public API. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_discover_services(0x40U, cov_disc_cb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_read(0x40U, 0x21U, cov_read_cb, nullptr));
  uint8_t val = 0xA5U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_write(0x40U, 0x30U, &val, 1U, 1U, cov_write_cb, nullptr));

  /* All three in flight: every in_use check is true, all increments run. */
  TEST_ASSERT_EQ(3U, ra_ble_gatt_client_test_pending_count());
  TEST_END("test_pending_count_none_then_all");
}

int main(void)
{
  test_read_launch_then_busy();
  test_subscribe_table_full();
  test_pending_count_none_then_all();
  return 0;
}
