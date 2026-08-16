/**
 * @file test_ra8_esp_hosted_rtos_pool.c
 * @brief Host tests for the allocator, queue, mutex and semaphore vtable rows.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * The memory-facing half of the esp-hosted RTOS slice:
 * ``ra8_esp_hosted_rtos_pool.c`` (the two ThreadX byte pools, the aligned
 * allocator and the queues carved out of them) and
 * ``ra8_esp_hosted_rtos_sync.c`` (the mutex and semaphore tables). The thread,
 * timer, clock and lifecycle rows are covered by the sibling
 * ``test_ra8_esp_hosted_rtos.c``; the two files exist separately because one
 * would exceed the project's thousand-line cap.
 *
 * Everything is driven through the bound vtable, which is how the vendored
 * core reaches these functions, and through the recording ThreadX model,
 * whose one-shot failure injection is what makes pool exhaustion and a
 * refused create reachable rather than assumed unreachable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_attributes.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_esp_hosted_tx_shim_sync_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_pool_const_t
 * @brief Fixture sizes and fixed vectors the allocator and queue tests use.
 *
 * @details
 * Each names the role the number plays rather than its value: a queue message
 * is not a realloc target even when both are byte counts, and the drain loop's
 * attempt cap is not the size of the blocks it asks for. ::k_t_pool_msg_bytes
 * is the one that most needs a name -- it appears as a queue element size, as
 * three stack-buffer extents and as a fill-loop bound, and every one of those
 * has to move together for the round-trip assertion to stay meaningful.
 *
 * @invariant ::k_t_pool_calloc_bytes equals the product of the element count
 *            and element size the calloc control vector requests, so the
 *            zero-scan covers the whole block and not a prefix of it.
 * @invariant ::k_t_pool_drain_attempts multiplied by ::k_t_pool_drain_chunk
 *            exceeds ::k_ra8_esp_hosted_pool_bytes, so the drain loop is
 *            guaranteed to reach exhaustion before it runs out of attempts.
 *
 * @par Example:
 * @code
 * uint8_t sent[(size_t)k_t_pool_msg_bytes] = {};
 * @endcode
 *
 * @see priv_ra8_esp_hosted_rtos_pool_stats
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_pool_foreign_bytes  = 64U,         /**< Stack buffer offered to the release guard.  */
  k_t_pool_grow_bytes     = 128U,        /**< Size the realloc grow vector asks for.      */
  k_t_pool_calloc_bytes   = 128U,        /**< Bytes calloc must have zeroed (8 x 16).     */
  k_t_pool_drain_attempts = 64U,         /**< Cap on the pool-draining loop.              */
  k_t_pool_drain_chunk    = 8192U,       /**< Largest block the drain loop requests.      */
  k_t_pool_stat_poison    = 0xFFFFFFFFU, /**< Pre-set value the stats row must overwrite. */
  k_t_pool_msg_bytes      = 28U,         /**< Queue element size, in bytes.               */
  k_t_pool_msg_over_bytes = 68U,         /**< Element size past the sixteen-word cap.     */
  k_t_pool_depth_over     = 65U,         /**< Queue depth past the element-count cap.     */
  k_t_pool_msg_seed       = 0xA0U,       /**< First byte of the queue message pattern.    */
  k_t_pool_fill_byte      = 0xABU,       /**< Byte `_h_memset` is asked to write.         */
} t_pool_const_t;

/** Vtable every test drives the port through. */
static hosted_osi_funcs_t s_funcs;

/** Bring the port down (if up), clear the ThreadX model, bring it back up.
 * @brief Verify reset port.
 * @details Implements the fixture-only reset port operation with bounded static state.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_port(void)
{
  if (priv_ra8_esp_hosted_rtos_is_ready()) {
    (void)priv_ra8_esp_hosted_rtos_deinit();
  }
  internal_ra8_esp_hosted_tx_shim_reset();
  (void)memset(&s_funcs, 0, sizeof(s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_init());
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_bind(&s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_bind_pool(&s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_bind_sync(&s_funcs));
}

/* ---------------------------------------------------------------------------
 * Allocator
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_aligned_alloc_returns_aligned_and_frees
 *
 * @brief A 64-byte-aligned request really is aligned and releases correctly.
 *
 * @details
 * The allocator over-allocates and hides the ThreadX base pointer in a header
 * immediately below the payload, so this checks both halves: the payload
 * address is a multiple of the requested alignment, the recorded size is the
 * size asked for, and the release is accepted. A release of a pointer the
 * port never handed out must be refused by the header sentinel rather than
 * passed to ThreadX.
 *
 * @par MC/DC:
 * Decision `(align == 0U) || (align > k_align_max) || ((align & (align-1U)) != 0U)`
 * in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@priv_ra8_esp_hosted_rtos_alloc`
 * (3 conditions):
 * - Vector 1: align=64  -> false,false,false -> allocates (control).
 * - Vector 2: align=0   -> true              -> refused (varies zero test).
 * - Vector 3: align=128 -> false,true        -> refused (varies the cap).
 * - Vector 4: align=24  -> false,false,true  -> refused (varies power-of-two).
 * Pairs 1+2, 1+3 and 1+4 prove each condition's independent influence.
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decision `(size == 0U) || (size > k_alloc_max)` (2 conditions) is covered by
 * vectors size=100 (control), size=0 and size=k_alloc_max+1.
 *
 * @pre The port is initialised.
 * @post Every block allocated here is released.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_aligned_alloc_returns_aligned_and_frees(void)
{
  TEST_BEGIN("malloc_align returns a genuinely aligned block that frees");
  internal_reset_port();
  void* const p = s_funcs._h_malloc_align(100U, HOSTED_MEM_ALIGNMENT_64);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(0U, (uintptr_t)p % (uintptr_t)HOSTED_MEM_ALIGNMENT_64);

  size_t recorded = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_block_size(p, &recorded));
  TEST_ASSERT_EQ(100U, recorded);
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_release(p));

  TEST_ASSERT_NULL(s_funcs._h_malloc_align(100U, 0U));
  TEST_ASSERT_NULL(s_funcs._h_malloc_align(100U, 128U));
  TEST_ASSERT_NULL(s_funcs._h_malloc_align(100U, 24U));
  TEST_ASSERT_NULL(s_funcs._h_malloc(0U));
  TEST_ASSERT_NULL(s_funcs._h_malloc(8193U));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_ra8_esp_hosted_rtos_release(NULL));
  uint8_t foreign[(size_t)k_t_pool_foreign_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_rtos_release(&foreign[32]));
  TEST_END("malloc_align returns a genuinely aligned block that frees");
}

/**
 * @test internal_test_realloc_preserves_contents_growing_and_shrinking
 *
 * @brief realloc copies min(old,new) bytes and honours its null/zero cases.
 *
 * @details
 * ThreadX byte pools cannot grow a block in place and do not record a block's
 * size, so the port keeps the size in its own header and copies. Both
 * directions are checked with a known byte pattern, because a size taken from
 * the wrong side of the min() shows up only on one of them.
 *
 * @par MC/DC:
 * Three single-condition decisions in sequence -- `mem == nullptr`,
 * `newsize == 0U`, and the `(oldsize < newsize)` select inside the copy
 * length. Vectors: realloc(NULL, 32) (first true), realloc(p, 0) (first
 * false, second true), grow 32->::k_t_pool_grow_bytes (128) (both false, third
 * true), shrink ::k_t_pool_grow_bytes->16 (both false, third false).
 *
 * @pre The port is initialised.
 * @post Every block allocated here is released.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_realloc_preserves_contents_growing_and_shrinking(void)
{
  TEST_BEGIN("realloc preserves contents growing and shrinking");
  internal_reset_port();
  uint8_t* small = (uint8_t*)s_funcs._h_realloc(NULL, 32U);
  TEST_ASSERT_NOT_NULL(small);
  for (uint32_t i = 0U; i < 32U; ++i) {
    small[i] = (uint8_t)(i + 1U);
  }

  uint8_t* grown = (uint8_t*)s_funcs._h_realloc(small, k_t_pool_grow_bytes);
  TEST_ASSERT_NOT_NULL(grown);
  for (uint32_t i = 0U; i < 32U; ++i) {
    TEST_ASSERT_EQ((i + 1U), grown[i]);
  }
  size_t recorded = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_block_size(grown, &recorded));
  TEST_ASSERT_EQ(k_t_pool_grow_bytes, recorded);

  uint8_t* shrunk = (uint8_t*)s_funcs._h_realloc(grown, 16U);
  TEST_ASSERT_NOT_NULL(shrunk);
  for (uint32_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ((i + 1U), shrunk[i]);
  }
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_block_size(shrunk, &recorded));
  TEST_ASSERT_EQ(16U, recorded);

  TEST_ASSERT_NULL(s_funcs._h_realloc(shrunk, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_ra8_esp_hosted_rtos_block_size(NULL, &recorded));
  TEST_END("realloc preserves contents growing and shrinking");
}

/**
 * @test internal_test_calloc_zeroes_and_refuses_overflow
 *
 * @brief calloc zeroes its block and refuses a product that cannot be served.
 *
 * @details
 * The overflow guard divides rather than multiplying, so a product that would
 * wrap is rejected before it becomes a small allocation the caller then
 * overruns.
 *
 * @par MC/DC:
 * Decision `(blk_no == 0U) || (size == 0U)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_h_calloc`
 * (2 conditions):
 * - Vector 1: 8 x 16 -> false,false -> allocates (control).
 * - Vector 2: 0 x 16 -> true        -> refused (varies the count).
 * - Vector 3: 8 x 0  -> false,true  -> refused (varies the element size).
 * Vectors 1+2 and 1+3 prove each condition's independent influence.
 * The second decision, `blk_no > (k_alloc_max / size)`, is covered by vectors
 * 8 x 16 (false) and 0x40000000 x 16 (true).
 *
 * @pre The port is initialised.
 * @post The allocated block is released.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_calloc_zeroes_and_refuses_overflow(void)
{
  TEST_BEGIN("calloc zeroes its block and refuses an overflowing product");
  internal_reset_port();
  uint8_t* const p = (uint8_t*)s_funcs._h_calloc(8U, 16U);
  TEST_ASSERT_NOT_NULL(p);
  for (uint32_t i = 0U; i < k_t_pool_calloc_bytes; ++i) {
    TEST_ASSERT_EQ(0U, p[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_calloc(0U, 16U));
  TEST_ASSERT_NULL(s_funcs._h_calloc(8U, 0U));
  TEST_ASSERT_NULL(s_funcs._h_calloc(0x40000000U, 16U));
  s_funcs._h_free(p);
  s_funcs._h_free(NULL);
  TEST_END("calloc zeroes its block and refuses an overflowing product");
}

/**
 * @test internal_test_pool_exhaustion_reports_null
 *
 * @brief A refused pool allocation becomes a null return, never a fault.
 *
 * @details
 * Driven three ways: by arming the modelled pool family to refuse once, by
 * arming it to report SUCCESS while handing back no block, and by draining the
 * real fixed pool with repeated maximum-size requests until it genuinely
 * cannot serve another. The last is the case a badly sized
 * ::k_ra8_esp_hosted_pool_bytes would produce on the bench.
 *
 * The middle one is a contract break by the allocator rather than a refusal:
 * `tx_byte_allocate` writes its out-parameter only on success, so a TX_SUCCESS
 * with the block pointer still null must become a null return and not an
 * address computed from zero. The clang static analyzer found that path before
 * any test did (docs/STATIC_ANALYSIS.md).
 *
 * @par MC/DC:
 * Decision: `(tx_byte_allocate(...) != TX_SUCCESS) || (base == nullptr)`
 * (2 conditions).
 * - Vector 1: armed TX_NO_MEMORY        -> C1=T (short-circuits) -> true
 * - Vector 2: armed TX_SUCCESS, no block -> C1=F, C2=T           -> true
 * - Vector 3: ordinary success           -> C1=F, C2=F           -> false
 * Vectors 2+3 prove C1=F holds while C2 alone flips the outcome; 1+3 prove C1
 * flips it with C2 unable to contribute (short-circuit). N+1 = 3 vectors for
 * N=2: minimal MC/DC. The natural pool exhaustion below re-drives vector 1
 * through the real allocator rather than the injection seam.
 *
 * @pre The port is initialised.
 * @post The pool is left drained; the next test resets the port.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_pool_exhaustion_reports_null(void)
{
  TEST_BEGIN("pool exhaustion reports null");
  internal_reset_port();
  internal_ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_pool, TX_NO_MEMORY);
  TEST_ASSERT_NULL(s_funcs._h_malloc(64U));
  internal_ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_pool, TX_SUCCESS);
  TEST_ASSERT_NULL(s_funcs._h_malloc(64U));
  TEST_ASSERT_NOT_NULL(s_funcs._h_malloc(64U));

  bool exhausted = false;
  for (uint32_t i = 0U; i < k_t_pool_drain_attempts; ++i) {
    if (s_funcs._h_malloc(k_t_pool_drain_chunk) == NULL) {
      exhausted = true;
      break;
    }
  }
  TEST_ASSERT_EQ(true, exhausted);
  TEST_END("pool exhaustion reports null");
}

/**
 * @test internal_test_pool_stats_report_live_numbers
 *
 * @brief Pool statistics come from ThreadX and move when the pool moves.
 *
 * @details
 * The reported figures back ``ra8_esp_hosted_mem_dump``, so they have to be
 * the live byte-pool numbers rather than a port-side tally; the test proves
 * that by allocating and watching the available count fall. An uninitialised
 * port must report zeroes rather than reading a dead control block.
 *
 * @par MC/DC:
 * Three single-condition decisions: `s_pool.ready`, `out_available != nullptr`
 * and `out_fragments != nullptr`. Vectors: initialised with both outputs,
 * initialised with both null, and uninitialised with both outputs.
 *
 * @pre The port is initialised.
 * @post The port is left initialised.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_pool_stats_report_live_numbers(void)
{
  TEST_BEGIN("pool stats report live ThreadX byte-pool numbers");
  internal_reset_port();
  uint32_t before_avail = 0U;
  uint32_t before_frags = 0U;
  priv_ra8_esp_hosted_rtos_pool_stats(&before_avail, &before_frags);
  TEST_ASSERT(before_avail > 0U);

  TEST_ASSERT_NOT_NULL(s_funcs._h_malloc(1600U));
  uint32_t after_avail = 0U;
  uint32_t after_frags = 0U;
  priv_ra8_esp_hosted_rtos_pool_stats(&after_avail, &after_frags);
  TEST_ASSERT(after_avail < before_avail);
  TEST_ASSERT(after_frags > before_frags);

  priv_ra8_esp_hosted_rtos_pool_stats(NULL, NULL);

  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_deinit());
  uint32_t down_avail = k_t_pool_stat_poison;
  uint32_t down_frags = k_t_pool_stat_poison;
  priv_ra8_esp_hosted_rtos_pool_stats(&down_avail, &down_frags);
  TEST_ASSERT_EQ(0U, down_avail);
  TEST_ASSERT_EQ(0U, down_frags);
  TEST_END("pool stats report live ThreadX byte-pool numbers");
}

/**
 * @test internal_test_memcpy_and_memset_guard_null
 *
 * @brief The copy and fill rows work and report a null argument by returning null.
 *
 * @details
 * These two rows have no error channel -- they return the destination -- so
 * the only honest way to report a null argument is a null return, and that is
 * what the test pins.
 *
 * @par MC/DC:
 * Decision `(dest == nullptr) || (src == nullptr)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_h_memcpy`
 * (2 conditions):
 * - Vector 1: both real -> false,false -> copies (control).
 * - Vector 2: dest null -> true        -> null (varies dest).
 * - Vector 3: src null  -> false,true  -> null (varies src).
 * Vectors 1+2 and 1+3 prove each condition's independent influence.
 * N+1 = 3 vectors for N=2: minimal MC/DC. `_h_memset`'s single-condition
 * guard is covered by a real buffer and a null one.
 *
 * @pre The port is initialised.
 * @post Only local buffers are written.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_memcpy_and_memset_guard_null(void)
{
  TEST_BEGIN("memcpy and memset rows work and guard null arguments");
  internal_reset_port();
  const uint8_t src[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t       dst[8] = {};
  TEST_ASSERT(s_funcs._h_memcpy(dst, src, sizeof(src)) == dst);
  TEST_ASSERT_EQ(0, memcmp(dst, src, sizeof(src)));
  TEST_ASSERT_NULL(s_funcs._h_memcpy(NULL, src, sizeof(src)));
  TEST_ASSERT_NULL(s_funcs._h_memcpy(dst, NULL, sizeof(src)));

  TEST_ASSERT(s_funcs._h_memset(dst, (int)k_t_pool_fill_byte, sizeof(dst)) == dst);
  for (uint32_t i = 0U; i < sizeof(dst); ++i) {
    TEST_ASSERT_EQ(k_t_pool_fill_byte, dst[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_memset(NULL, 0, sizeof(dst)));
  TEST_END("memcpy and memset rows work and guard null arguments");
}

/* ---------------------------------------------------------------------------
 * Queues
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_queue_roundtrip_and_nonblocking_dequeue
 *
 * @brief A message survives the ring, and an empty queue fails without waiting.
 *
 * @details
 * `spi_drv.c` chains three zero-timeout dequeues with `if (a) if (b) if (c)`,
 * so a non-zero return on an empty queue is load-bearing and is asserted
 * directly. The blocking send on a full queue is checked through the model's
 * recorded wait option, which proves the port asked ThreadX to block rather
 * than polling.
 *
 * @par MC/DC:
 * A queue call reaches three functions, so this case covers the decisions of
 * all three: the enqueue and dequeue slots
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_h_queue_item` and
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_h_dequeue_item`,
 * and the handle resolver they both call,
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_queue_slot`.
 * Decision `(slot == nullptr) || (item == nullptr)` (2 conditions), one copy
 * in each of the two slots:
 * - Vector 1: live handle, real item -> false,false -> proceeds (control).
 * - Vector 2: null handle, real item -> true        -> RET_INVALID.
 * - Vector 3: live handle, null item -> false,true  -> RET_INVALID.
 * Vectors 1+2 and 1+3 prove each condition's independent influence.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * The resolver carries two more compound decisions. Its entry guard
 * `(handle == nullptr) || !s_pool.ready` takes the false-false control and
 * the null-handle arm here -- every live call and every `NULL` call above --
 * and its not-ready arm in ::internal_test_queue_create_bounds_and_table_exhaustion,
 * which presents a handle that was live before the pool was torn down.
 * Its row match `(handle == &s_pool.queues[i]) && s_pool.queues[i].used`
 * takes the occupied-and-matching control here, and the stale-handle vector
 * from the final call above: after `_h_destroy_queue` the address still
 * matches a row whose `used` flag has been cleared, so the second condition
 * alone turns the outcome false and a stale handle reports RET_INVALID
 * instead of reaching a freed ring. The address-mismatch vector comes from
 * ::internal_test_queue_create_bounds_and_table_exhaustion, where eight live queues
 * mean the scan crosses non-matching rows before it reaches the wanted one.
 *
 * @pre The port is initialised.
 * @post The queue is destroyed.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_queue_roundtrip_and_nonblocking_dequeue(void)
{
  TEST_BEGIN("queue round-trip; empty dequeue fails without blocking");
  internal_reset_port();
  void* const q = s_funcs._h_create_queue(2U, k_t_pool_msg_bytes);
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQ(0, s_funcs._h_queue_msg_waiting(q));

  uint8_t sent[(size_t)k_t_pool_msg_bytes];
  uint8_t got[(size_t)k_t_pool_msg_bytes] = {};
  for (uint32_t i = 0U; i < k_t_pool_msg_bytes; ++i) {
    sent[i] = (uint8_t)(k_t_pool_msg_seed + i);
  }
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_queue_item(q, sent, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(1, s_funcs._h_queue_msg_waiting(q));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_dequeue_item(q, got, 0));
  TEST_ASSERT_EQ(0, memcmp(sent, got, sizeof(got)));
  TEST_ASSERT_EQ(0, s_funcs._h_queue_msg_waiting(q));

  /* Empty, non-blocking: must fail, and must be a non-zero value so the
   * vendored `if (dequeue(...))` chain reads it as "nothing here". */
  TEST_ASSERT_EQ(RET_FAIL_TIMEOUT, s_funcs._h_dequeue_item(q, got, 0));

  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_queue_item(NULL, sent, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_queue_item(q, NULL, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_dequeue_item(NULL, got, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_dequeue_item(q, NULL, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_queue_msg_waiting(NULL));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_reset_queue(NULL));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_destroy_queue(NULL));

  TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_queue(q));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_queue_msg_waiting(q));
  TEST_END("queue round-trip; empty dequeue fails without blocking");
}

/**
 * @test internal_test_queue_full_send_requests_an_unbounded_wait
 *
 * @brief A full queue reports a timeout, having asked ThreadX to block.
 *
 * @details
 * The host model cannot suspend, so a blocking send on a full ring returns
 * TX_QUEUE_FULL; what the test can still prove is that the port converted
 * HOSTED_BLOCK_MAX into TX_WAIT_FOREVER rather than into a zero-tick poll,
 * which is the bug the "any negative value blocks" rule exists to prevent.
 *
 * @par MC/DC:
 * Single-condition decision `rc == TX_QUEUE_FULL` selecting RET_FAIL_TIMEOUT
 * over RET_FAIL. Vectors: the full ring here, and the ordinary success in
 * internal_test_queue_roundtrip_and_nonblocking_dequeue.
 *
 * @pre The port is initialised.
 * @post The queue is destroyed and its ring returned.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_queue_full_send_requests_an_unbounded_wait(void)
{
  TEST_BEGIN("full queue reports a timeout after asking to block forever");
  internal_reset_port();
  void* const q = s_funcs._h_create_queue(2U, k_t_pool_msg_bytes);
  TEST_ASSERT_NOT_NULL(q);
  uint8_t msg[(size_t)k_t_pool_msg_bytes] = {};
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_queue_item(q, msg, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_queue_item(q, msg, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(RET_FAIL_TIMEOUT, s_funcs._h_queue_item(q, msg, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(TX_WAIT_FOREVER, ((const TX_QUEUE*)q)->last_wait);

  TEST_ASSERT_EQ(2, s_funcs._h_queue_msg_waiting(q));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_reset_queue(q));
  TEST_ASSERT_EQ(0, s_funcs._h_queue_msg_waiting(q));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_queue(q));
  TEST_END("full queue reports a timeout after asking to block forever");
}

/**
 * @test internal_test_queue_create_bounds_and_table_exhaustion
 *
 * @brief Bad geometry is refused and the fixed queue table never grows.
 *
 * @details
 * The word cap and the table budget are separate limits and are checked
 * separately: a 68-byte element is refused for exceeding sixteen ThreadX
 * words, and the ninth queue is refused because ::k_ra8_esp_hosted_max_queues
 * is eight and the port allocates nothing beyond it.
 *
 * @par MC/DC:
 * Decision `!s_pool.ready || (words == 0U)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@internal_h_create_queue`
 * (2 conditions):
 * - Vector 1: ready, ::k_t_pool_msg_bytes (28) -> false,false -> creates
 *   (control).
 * - Vector 2: not ready -> true -> refused (varies readiness).
 * - Vector 3: ready, ::k_t_pool_msg_over_bytes (68) -> false,true -> refused
 *   (varies the size).
 * Decision `(qnum_elem == 0U) || (qnum_elem > k_queue_elems_max)`
 * (2 conditions) is covered by depth 4 (control), 0, and
 * ::k_t_pool_depth_over (65).
 *
 * @pre The port is initialised.
 * @post Every queue created here is destroyed.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_queue_create_bounds_and_table_exhaustion(void)
{
  TEST_BEGIN("queue create bounds and fixed-table exhaustion");
  internal_reset_port();
  TEST_ASSERT_NULL(s_funcs._h_create_queue(4U, k_t_pool_msg_over_bytes));
  TEST_ASSERT_NULL(s_funcs._h_create_queue(0U, k_t_pool_msg_bytes));
  TEST_ASSERT_NULL(s_funcs._h_create_queue(k_t_pool_depth_over, k_t_pool_msg_bytes));

  void* queues[k_ra8_esp_hosted_max_queues] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_queues; ++i) {
    queues[i] = s_funcs._h_create_queue(4U, k_t_pool_msg_bytes);
    TEST_ASSERT_NOT_NULL(queues[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_create_queue(4U, k_t_pool_msg_bytes));
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_queues; ++i) {
    TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_queue(queues[i]));
  }
  void* const survivor = s_funcs._h_create_queue(4U, k_t_pool_msg_bytes);
  TEST_ASSERT_NOT_NULL(survivor);

  /* Not ready: the port must refuse rather than touch a dead pool. */
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_NULL(s_funcs._h_create_queue(4U, k_t_pool_msg_bytes));
  /* A handle that was live until the teardown is refused on the readiness
     condition alone -- its address still names a real row. */
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_queue_msg_waiting(survivor));
  TEST_END("queue create bounds and fixed-table exhaustion");
}

/* ---------------------------------------------------------------------------
 * Semaphores and mutexes
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_semaphore_starts_at_one_so_the_vendored_drain_empties_it
 *
 * @brief Create then one zero-timeout take leaves the semaphore empty.
 *
 * @details
 * Every vendored call site creates a semaphore and immediately drains it once
 * with `_h_get_semaphore(sem, 0)` so the waiting task blocks. This asserts
 * exactly that sequence: the drain succeeds, and the take after it fails --
 * which would not hold if the port had started the count at max_count.
 *
 * @par MC/DC:
 * Decision `!s_rtos.ready || (max_count <= 0)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c@internal_h_create_semaphore`
 * (2 conditions):
 * - Vector 1: ready, max_count=3 -> false,false -> creates (control).
 * - Vector 2: not ready          -> true        -> refused (varies readiness).
 * - Vector 3: ready, max_count=0 -> false,true  -> refused (varies the count).
 * Vectors 1+2 and 1+3 prove each condition's independent influence.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre The port is initialised.
 * @post Every semaphore created here is destroyed.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void
internal_test_semaphore_starts_at_one_so_the_vendored_drain_empties_it(void)
{
  TEST_BEGIN("semaphore create + one try-take leaves it empty");
  internal_reset_port();
  void* const sem = s_funcs._h_create_semaphore(3);
  TEST_ASSERT_NOT_NULL(sem);
  /* The drain every vendored call site performs. */
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_get_semaphore(sem, 0));
  /* Empty now: the "is a message queued?" test must read false. */
  TEST_ASSERT_EQ(RET_FAIL_TIMEOUT, s_funcs._h_get_semaphore(sem, 0));

  TEST_ASSERT_EQ(RET_OK, s_funcs._h_post_semaphore(sem));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_get_semaphore(sem, 0));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_post_semaphore_from_isr(sem));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_get_semaphore(sem, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(TX_WAIT_FOREVER, ((const TX_SEMAPHORE*)sem)->last_wait);

  TEST_ASSERT_NULL(s_funcs._h_create_semaphore(0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_get_semaphore(NULL, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_post_semaphore(NULL));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_post_semaphore_from_isr(NULL));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_destroy_semaphore(NULL));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_semaphore(sem));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_get_semaphore(sem, 0));
  TEST_END("semaphore create + one try-take leaves it empty");
}

/**
 * @test internal_test_semaphore_table_exhaustion
 *
 * @brief The semaphore table refuses the ninth request and never grows.
 *
 * @details
 * ::k_ra8_esp_hosted_max_semaphores is eight, which covers the SPI transport's
 * three plus the serial and RPC layers; the ninth must fail cleanly so a
 * budgeting mistake surfaces as a refused create rather than as memory the
 * board does not have.
 *
 * @par MC/DC:
 * Single-condition decision `idx == k_max_semaphores` after the table claim.
 * Vectors: the eight successful claims and the ninth refusal.
 *
 * @pre The port is initialised.
 * @post Every semaphore created here is destroyed.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_semaphore_table_exhaustion(void)
{
  TEST_BEGIN("semaphore table refuses beyond its fixed budget");
  internal_reset_port();
  void* sems[k_ra8_esp_hosted_max_semaphores] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_semaphores; ++i) {
    sems[i] = s_funcs._h_create_semaphore(1);
    TEST_ASSERT_NOT_NULL(sems[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_create_semaphore(1));
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_semaphores; ++i) {
    TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_semaphore(sems[i]));
  }
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_semaphore(1));
  TEST_END("semaphore table refuses beyond its fixed budget");
}

/**
 * @test internal_test_mutex_lock_unlock_and_unbalanced_release
 *
 * @brief Locks nest, an unbalanced release is reported, and the table is fixed.
 *
 * @details
 * An unbalanced release is always a caller defect, so the port reports
 * RET_FAIL for it rather than swallowing it. The table budget is checked the
 * same way as the semaphore one.
 *
 * @par MC/DC:
 * Single-condition decisions throughout: `!s_sync.ready` in the create,
 * `idx == k_max_mutexes` in every handle lookup, and
 * `tx_mutex_put(...) == TX_SUCCESS` in the release. Vectors: a live handle, a
 * null handle, a destroyed handle, a balanced release and an unbalanced one.
 *
 * @pre The port is initialised.
 * @post Every mutex created here is destroyed.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_mutex_lock_unlock_and_unbalanced_release(void)
{
  TEST_BEGIN("mutex lock/unlock, unbalanced release, fixed table");
  internal_reset_port();
  void* const m = s_funcs._h_create_mutex();
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQ(TX_INHERIT, ((const TX_MUTEX*)m)->inherit);
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_lock_mutex(m, (int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_unlock_mutex(m));
  TEST_ASSERT_EQ(RET_FAIL, s_funcs._h_unlock_mutex(m));

  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_lock_mutex(NULL, 0));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_unlock_mutex(NULL));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_destroy_mutex(NULL));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_mutex(m));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_lock_mutex(m, 0));

  void* mutexes[k_ra8_esp_hosted_max_mutexes] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_mutexes; ++i) {
    mutexes[i] = s_funcs._h_create_mutex();
    TEST_ASSERT_NOT_NULL(mutexes[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_create_mutex());
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_mutexes; ++i) {
    TEST_ASSERT_EQ(RET_OK, s_funcs._h_destroy_mutex(mutexes[i]));
  }

  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_NULL(s_funcs._h_create_mutex());
  TEST_END("mutex lock/unlock, unbalanced release, fixed table");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

int main(void)
{
  internal_test_aligned_alloc_returns_aligned_and_frees();
  internal_test_realloc_preserves_contents_growing_and_shrinking();
  internal_test_calloc_zeroes_and_refuses_overflow();
  internal_test_pool_exhaustion_reports_null();
  internal_test_pool_stats_report_live_numbers();
  internal_test_memcpy_and_memset_guard_null();
  internal_test_queue_roundtrip_and_nonblocking_dequeue();
  internal_test_queue_full_send_requests_an_unbounded_wait();
  internal_test_queue_create_bounds_and_table_exhaustion();
  internal_test_semaphore_starts_at_one_so_the_vendored_drain_empties_it();
  internal_test_semaphore_table_exhaustion();
  internal_test_mutex_lock_unlock_and_unbalanced_release();
  if (priv_ra8_esp_hosted_rtos_is_ready()) {
    (void)priv_ra8_esp_hosted_rtos_deinit();
  }
  return 0;
}
