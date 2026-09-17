/**
 * @file test_mdl_storage_ram.c
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

/**
 * @brief Bind a fresh adapter and interface over the shared fixture bytes.
 * @details Clears prior bytes, initializes both caller-owned objects, and
 *          asserts that the production constructor accepts the exact span.
 * @param[out] storage Fresh adapter state.
 * @param[out] interface Fresh coordinator callbacks.
 * @pre Helper-local backing is exclusively owned by this sequential test.
 * @pre Output pointers are non-NULL and writable.
 * @post Both outputs are initialized and the backing bytes are zero.
 * @post The adapter is idle with no committed view.
 * @note Test-only helper; not thread-safe through shared fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(ra8_mdl_storage_ram_t*   storage,
                                       ra8_mdl_storage_iface_t* interface)
{
  static uint8_t s_backing[k_ram_test_capacity];
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
  static const uint8_t expected[] = {1U, 2U, 3U, 4U, 5U};
  TEST_ASSERT_EQ(sizeof(expected), length);
  TEST_ASSERT_EQ(0, memcmp(view, expected, length));
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

/**
 * @brief Initialization and begin callbacks independently reject null inputs.
 * @test N+1 vectors vary each term while every other term remains false.
 * @details Uses a valid constructor/begin control, then nulls each required
 *          output, buffer, callback context, and destination in isolation.
 * @par MC/DC:
 * `libs/ra8_c6link/src/ra8_mdl_storage_ram.c@ra8_mdl_storage_ram_init`
 * receives `(F,F,F)->F`, `(T,-,-)->T`, `(F,T,-)->T`, `(F,F,T)->T`.
 * `libs/ra8_c6link/src/ra8_mdl_storage_ram.c@internal_ram_begin` receives
 * `(F,F)->F`, `(T,-)->T`, and `(F,T)->T`.
 * @pre Shared fixture storage is exclusively owned.
 * @pre Stack outputs are writable for every non-null vector.
 * @post Every rejected vector returns the null-pointer status.
 * @post The successful transaction is aborted back to idle.
 * @note Calls production callbacks through the published storage interface.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_init_and_begin(void)
{
  TEST_BEGIN("mdl storage ram: init and begin MC/DC");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  uint8_t                 byte      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_init(nullptr, &interface, &byte, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_init(&storage, nullptr, &byte, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_init(&storage, &interface, nullptr, 1U));
  internal_bind(&storage, &interface);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.begin(nullptr, "source"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.begin(interface.ctx, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  TEST_ASSERT_EQ(k_ra8_ok, interface.abort(interface.ctx));
  TEST_END("mdl storage ram: init and begin MC/DC");
}

/**
 * @brief Write and commit decisions independently reject malformed state.
 * @test Callback pointer, zero-length, and lifecycle terms receive N+1 vectors.
 * @details Begins one transaction, varies callback inputs without corrupting
 *          it, publishes one byte, then constructs each invalid commit state.
 * @par MC/DC:
 * `libs/ra8_c6link/src/ra8_mdl_storage_ram.c@internal_ram_write` receives
 * `(F,F)->F`, `(T,-)->T`, `(F,T)->T` for its pointer OR and `(F,F)->F`,
 * `(T,F)->F`, `(T,T)->T` for its data/length AND.
 * `libs/ra8_c6link/src/ra8_mdl_storage_ram.c@internal_ram_commit` receives
 * `(F,F)->F`, `(T,-)->T`, and `(F,T)->T`.
 * @pre Shared fixture storage is exclusively owned.
 * @pre The valid byte remains readable throughout the callback vectors.
 * @post Only the valid write advances storage by one byte.
 * @post Final abort restores the empty transaction to idle.
 * @note Short-circuit vectors deliberately leave later terms unevaluated.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_write_and_commit(void)
{
  TEST_BEGIN("mdl storage ram: write and commit MC/DC");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  internal_bind(&storage, &interface);
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  const uint8_t byte    = 0x5AU;
  uint16_t      written = UINT16_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.write(nullptr, &byte, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.write(interface.ctx, &byte, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, &byte, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, nullptr, 0U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, interface.write(interface.ctx, nullptr, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_ok, interface.commit(interface.ctx));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, interface.commit(interface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "empty"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, interface.commit(interface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, interface.abort(interface.ctx));
  TEST_END("mdl storage ram: write and commit MC/DC");
}

/**
 * @brief Committed-view pointer and lifecycle terms vary independently.
 * @test A valid one-byte publication anchors both N+1 decision tables.
 * @details Rejects each null output independently, then mutates only the public
 *          lifecycle fields to isolate committed, active, and length terms.
 * @par MC/DC:
 * `libs/ra8_c6link/src/ra8_mdl_storage_ram.c@ra8_mdl_storage_ram_view`
 * receives `(F,F,F)->F` plus each one-true pointer vector, and state vectors
 * `(F,F,F)->F`, `(T,-,-)->T`, `(F,T,-)->T`, `(F,F,T)->T`.
 * @pre Shared fixture storage is exclusively owned.
 * @pre One byte is committed before view validation begins.
 * @post The valid control exposes the exact backing pointer and length one.
 * @post Every one-true state vector returns invalid-state.
 * @note Direct state mutation is confined to this transparent adapter fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_view(void)
{
  TEST_BEGIN("mdl storage ram: view MC/DC");
  ra8_mdl_storage_ram_t   storage   = {};
  ra8_mdl_storage_iface_t interface = {};
  internal_bind(&storage, &interface);
  const uint8_t byte    = 0xA5U;
  uint16_t      written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, interface.begin(interface.ctx, "source"));
  TEST_ASSERT_EQ(k_ra8_ok, interface.write(interface.ctx, &byte, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_ok, interface.commit(interface.ctx));
  const uint8_t* view   = nullptr;
  size_t         length = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_view(nullptr, &view, &length));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_view(&storage, nullptr, &length));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_ram_view(&storage, &view, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_storage_ram_view(&storage, &view, &length));
  TEST_ASSERT_EQ(1U, length);
  storage.committed = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mdl_storage_ram_view(&storage, &view, &length));
  storage.committed = true;
  storage.active    = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mdl_storage_ram_view(&storage, &view, &length));
  storage.active = false;
  storage.length = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mdl_storage_ram_view(&storage, &view, &length));
  TEST_END("mdl storage ram: view MC/DC");
}

int main(void)
{
  internal_test_commit_view();
  internal_test_capacity_abort();
  internal_test_lifecycle_guards();
  internal_test_mcdc_init_and_begin();
  internal_test_mcdc_write_and_commit();
  internal_test_mcdc_view();
  return 0;
}
