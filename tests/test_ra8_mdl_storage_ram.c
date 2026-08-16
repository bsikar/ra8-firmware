/**
 * @file test_ra8_mdl_storage_ram.c
 * @brief Contract tests for the caller-buffer media transaction adapter.
 * @details Exercises publication, capacity rejection, abort cleanup, lifecycle
 *          guards, and immutable committed views without transport mocking.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_mdl_storage_ram.h"
#include "unity_minimal.h"

/** @brief Fixed test destination capacity. */
typedef enum : uint8_t {
  k_ram_test_capacity = 8U, /**< Bytes in the caller-owned fixture. */
} ram_test_limit_t;

/** @brief Caller-owned fixture backing. */
static uint8_t s_backing[k_ram_test_capacity];

/**
 * @brief Bind a fresh adapter and interface over the shared fixture bytes.
 * @details Clears prior bytes, initializes both caller-owned objects, and
 *          asserts that the production constructor accepts the exact span.
 * @param[out] storage Fresh adapter state.
 * @param[out] interface Fresh coordinator callbacks.
 * @pre File-scope backing is exclusively owned by this sequential test.
 * @pre Output pointers are non-NULL and writable.
 * @post Both outputs are initialized and the backing bytes are zero.
 * @post The adapter is idle with no committed view.
 * @note Test-only helper; not thread-safe through shared fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(ra8_mdl_storage_ram_t*   storage,
                                       ra8_mdl_storage_iface_t* interface)
{
  (void)memset(s_backing, 0, sizeof s_backing);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_storage_ram_init(storage, interface, s_backing, sizeof s_backing));
}

/**
 * @brief A committed transaction exposes exactly its appended source bytes.
 * @test Begin, two writes, commit, and view preserve order and extent.
 * @details Uses the public coordinator callbacks as the transfer loop would,
 *          then consumes the result only through the immutable view API.
 * @pre Shared fixture storage is exclusively owned.
 * @pre The adapter starts idle.
 * @post The view contains five exact bytes in append order.
 * @post No writable handle is exposed after commit.
 * @note This is the primary success-path contract proof.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_commit_view(void)
{
  TEST_BEGIN("mdl storage ram: committed view");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  internal_bind(&storage, &interface);
  static const uint8_t k_first[]  = {1U, 2U};
  static const uint8_t k_second[] = {3U, 4U, 5U};
  uint16_t             written    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, k_first, sizeof k_first, &written));
  TEST_ASSERT_EQ(sizeof k_first, written);
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, k_second, sizeof k_second, &written));
  TEST_ASSERT_EQ(sizeof k_second, written);
  TEST_ASSERT_EQ(k_ra8_ok, interface.commit(interface.ctx));
  const uint8_t* view   = nullptr;
  size_t         length = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_storage_ram_view(&storage, &view, &length));
  TEST_ASSERT_EQ(5U, length);
  TEST_ASSERT_EQ(0, memcmp(view, "\x01\x02\x03\x04\x05", length));
  TEST_END("mdl storage ram: committed view");
}

/**
 * @brief Capacity rejection and abort never publish a partial object.
 * @test An oversized second write preserves the prefix, then abort hides it.
 * @details Fills most of the buffer, injects a fragment larger than the
 *          remaining capacity, and verifies both zero progress and cleanup.
 * @pre Shared fixture storage is exclusively owned.
 * @pre The adapter starts idle.
 * @post The rejected write reports no progress and does not change length.
 * @post Abort leaves no committed view and permits a fresh transaction.
 * @note Models a source image larger than its bounded SDRAM allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_capacity_abort(void)
{
  TEST_BEGIN("mdl storage ram: capacity and abort");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  internal_bind(&storage, &interface);
  static const uint8_t k_prefix[]  = {1U, 2U, 3U, 4U, 5U, 6U};
  static const uint8_t k_too_big[] = {7U, 8U, 9U};
  uint16_t             written     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, k_prefix, sizeof k_prefix, &written));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 interface.write(interface.ctx, k_too_big, sizeof k_too_big, &written));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(sizeof k_prefix, storage.length);
  TEST_ASSERT_EQ(k_ra8_ok, interface.abort(interface.ctx));
  const uint8_t* view   = nullptr;
  size_t         length = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mdl_storage_ram_view(&storage, &view, &length));
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "again"));
  TEST_ASSERT_EQ(k_ra8_ok, interface.abort(interface.ctx));
  TEST_END("mdl storage ram: capacity and abort");
}

/**
 * @brief Lifecycle guards reject empty labels, empty commits, and overlap.
 * @test Invalid transitions preserve a recoverable transaction state.
 * @details Exercises the state errors a miswired coordinator could otherwise
 *          turn into a stale or empty committed source view.
 * @pre Shared fixture storage is exclusively owned.
 * @pre The adapter starts idle.
 * @post Every invalid transition returns its canonical status.
 * @post Final abort restores the idle zero-length state.
 * @note Complements transfer-level validation tests with adapter-local guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_lifecycle_guards(void)
{
  TEST_BEGIN("mdl storage ram: lifecycle guards");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  internal_bind(&storage, &interface);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, interface.begin(interface.ctx, ""));
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, interface.begin(interface.ctx, "overlap"));
  uint16_t written = UINT16_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.write(interface.ctx, nullptr, 1U, &written));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, nullptr, 0U, &written));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, interface.commit(interface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, interface.abort(interface.ctx));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, interface.abort(interface.ctx));
  TEST_END("mdl storage ram: lifecycle guards");
}

int main(void)
{
  internal_test_commit_view();
  internal_test_capacity_abort();
  internal_test_lifecycle_guards();
  return 0;
}
