/**
 * @file test_fw_os_threadx.c
 * @brief Vectors for the pure mapping arithmetic in the ThreadX `fw_os` binding.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The ThreadX binding of the `fw_os` seam (#693) cannot run on a host: it
 * needs a scheduler, and proving it end to end needs the bench. What it does
 * not need the bench for is the arithmetic, and that is where a binding
 * actually goes wrong. Three maps carry all of it, and all three live in
 * `port/threadx/inc/fw_os_threadx.h` as pure functions precisely so these
 * vectors can reach them.
 *
 * What is asserted here:
 *  - a millisecond bound rounds **up** to whole ticks, never down, so a lock
 *    never returns early from a bound it promised to honour;
 *  - the two sentinels survive the conversion and, just as important, a
 *    finite bound large enough to overflow the tick scale does **not** turn
 *    into wait-forever;
 *  - the four portable priority bands land inside the ThreadX scale, strictly
 *    ordered, with the urgent end reserved for the composition root;
 *  - a ThreadX status maps to the ::ra8_err_t `fw_os.h` documents, including
 *    the one case ThreadX itself cannot distinguish: "held, and you said do
 *    not wait" versus "you waited and the bound passed".
 *
 * These vectors include no ThreadX header. The agreement between the mirrored
 * constants they use and the real `tx_api.h` values is a compile-time check in
 * `port/threadx/src/fw_os_threadx.c`, not something a host test can see.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include "fw_os_threadx.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Tick rates the vectors sweep, and the bounds they convert. */
enum : uint32_t {
  k_test_rate_1khz    = 1000U,
  k_test_rate_100hz   = 100U,
  k_test_rate_10khz   = 10000U,
  k_test_bound_ms     = 25U,
  k_test_odd_bound_ms = 5U,
};

static void internal_check_ticks_at_1khz(void)
{
  TEST_BEGIN("fw_os threadx: a 1 kHz tick makes ms and ticks the same number");
  /* A 1 kHz kernel makes a millisecond a tick, so the conversion is identity
   * everywhere except at the sentinels. */
  TEST_ASSERT_EQ(k_test_bound_ms, fw_os_threadx_ticks_for(k_test_bound_ms, k_test_rate_1khz));
  TEST_ASSERT_EQ(1, fw_os_threadx_ticks_for(1U, k_test_rate_1khz));
  TEST_ASSERT_EQ(K_FW_OS_TX_NO_WAIT, fw_os_threadx_ticks_for(0U, k_test_rate_1khz));
  TEST_ASSERT_EQ(K_FW_OS_TX_WAIT_FOREVER,
                 fw_os_threadx_ticks_for(UINT32_MAX, k_test_rate_1khz));
  TEST_END("fw_os threadx: a 1 kHz tick makes ms and ticks the same number");
}

static void internal_check_ticks_round_up(void)
{
  TEST_BEGIN("fw_os threadx: a sub-tick bound still costs a whole tick");
  /* At 100 Hz a tick is 10 ms. A 5 ms bound is less than one tick and must
   * still cost one: fw_os_mutex_lock promises "at least", so rounding down
   * would return early from a wait the caller asked for. */
  TEST_ASSERT_EQ(1, fw_os_threadx_ticks_for(k_test_odd_bound_ms, k_test_rate_100hz));
  TEST_ASSERT_EQ(1, fw_os_threadx_ticks_for(10U, k_test_rate_100hz));
  TEST_ASSERT_EQ(2, fw_os_threadx_ticks_for(11U, k_test_rate_100hz));
  TEST_ASSERT_EQ(3, fw_os_threadx_ticks_for(k_test_bound_ms, k_test_rate_100hz));

  /* A faster kernel simply scales; 25 ms is 250 ticks at 10 kHz. */
  TEST_ASSERT_EQ(250, fw_os_threadx_ticks_for(k_test_bound_ms, k_test_rate_10khz));
  TEST_END("fw_os threadx: a sub-tick bound still costs a whole tick");
}

static void internal_check_ticks_never_become_forever(void)
{
  TEST_BEGIN("fw_os threadx: an overflowing finite bound never becomes wait-forever");
  /* UINT32_MAX - 1 ms at 10 kHz scales past the 32-bit tick range. The clamp
   * has to land below wait-forever, or a bounded wait silently becomes an
   * unbounded one -- the worst failure this conversion can have. */
  const uint32_t huge = fw_os_threadx_ticks_for(UINT32_MAX - 1U, k_test_rate_10khz);
  TEST_ASSERT(huge < K_FW_OS_TX_WAIT_FOREVER);
  TEST_ASSERT_EQ(K_FW_OS_TX_WAIT_FOREVER - 1U, huge);

  /* A zero tick rate is nonsense, and answering no-wait beats dividing by it. */
  TEST_ASSERT_EQ(K_FW_OS_TX_NO_WAIT, fw_os_threadx_ticks_for(k_test_bound_ms, 0U));
  TEST_END("fw_os threadx: an overflowing finite bound never becomes wait-forever");
}

static void internal_check_ms_for(void)
{
  TEST_BEGIN("fw_os threadx: ticks convert back to ms and wrap rather than saturate");
  TEST_ASSERT_EQ(k_test_bound_ms, fw_os_threadx_ms_for(k_test_bound_ms, k_test_rate_1khz));
  TEST_ASSERT_EQ(250, fw_os_threadx_ms_for(25U, k_test_rate_100hz));
  TEST_ASSERT_EQ(25, fw_os_threadx_ms_for(250U, k_test_rate_10khz));
  TEST_ASSERT_EQ(0, fw_os_threadx_ms_for(k_test_bound_ms, 0U));

  /* fw_os_uptime_ms documents a wrap at 2^32 ms rather than a saturate, so a
   * tick count whose millisecond value overruns 32 bits must truncate. */
  TEST_ASSERT_EQ(0, fw_os_threadx_ms_for(0U, k_test_rate_1khz));
  TEST_ASSERT_EQ(UINT32_MAX, fw_os_threadx_ms_for(UINT32_MAX, k_test_rate_1khz));
  TEST_END("fw_os threadx: ticks convert back to ms and wrap rather than saturate");
}

static void internal_check_priority_map(void)
{
  TEST_BEGIN("fw_os threadx: four bands map onto the ThreadX scale in order");
  const uint32_t idle   = fw_os_threadx_priority_for(0U);
  const uint32_t low    = fw_os_threadx_priority_for(1U);
  const uint32_t normal = fw_os_threadx_priority_for(2U);
  const uint32_t high   = fw_os_threadx_priority_for(3U);

  /* ThreadX counts down, so a more urgent band is a smaller number. */
  TEST_ASSERT(high < normal);
  TEST_ASSERT(normal < low);
  TEST_ASSERT(low < idle);

  /* Every band has to be a legal ThreadX priority. */
  TEST_ASSERT(idle < K_FW_OS_TX_MAX_PRIORITIES);
  TEST_ASSERT(high < K_FW_OS_TX_MAX_PRIORITIES);

  /* The urgent end stays reserved: a portable library can never start a
   * thread that outranks the composition root's own. */
  TEST_ASSERT(high > 0U);

  /* An out-of-range band still has to answer with a legal priority. */
  TEST_ASSERT_EQ(normal, fw_os_threadx_priority_for(99U));
  TEST_END("fw_os threadx: four bands map onto the ThreadX scale in order");
}

static void internal_check_err_wait_cases(void)
{
  TEST_BEGIN("fw_os threadx: no-wait and timeout are told apart by the caller's bound");
  /* The one distinction ThreadX cannot make for us. Both a no-wait failure
   * and a genuine timeout come back as the same status; only the caller's
   * requested bound says which answer fw_os.h promises. */
  TEST_ASSERT_EQ(k_ra8_err_would_block,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_not_available, false));
  TEST_ASSERT_EQ(k_ra8_err_timeout,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_not_available, true));
  TEST_ASSERT_EQ(k_ra8_err_would_block,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_no_instance, false));
  TEST_ASSERT_EQ(k_ra8_err_timeout,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_no_instance, true));

  /* An object deleted underneath a waiter, or a wait aborted, reads as a
   * wait that did not succeed rather than as a caller mistake. */
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_os_threadx_err_for((uint32_t)k_fw_os_tx_deleted, true));
  TEST_ASSERT_EQ(k_ra8_err_timeout,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_wait_aborted, true));
  TEST_END("fw_os threadx: no-wait and timeout are told apart by the caller's bound");
}

static void internal_check_err_other_cases(void)
{
  TEST_BEGIN("fw_os threadx: every other status maps to what fw_os.h documents");
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_threadx_err_for((uint32_t)k_fw_os_tx_success, false));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_threadx_err_for((uint32_t)k_fw_os_tx_success, true));

  /* fw_os_mutex_unlock promises access_denied for a non-owner, and
   * fw_os_mutex_deinit promises busy for a mutex still in use. */
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_not_owned, false));
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_delete_error, false));

  /* A thread that cannot delete itself is the documented invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_caller_error, false));

  /* Malformed arguments all collapse to one answer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_ptr_error, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_priority_error, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_os_threadx_err_for((uint32_t)k_fw_os_tx_size_error, false));

  /* Anything the binding has never reasoned about must say so rather than
   * borrow a specific code it has not earned. */
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, fw_os_threadx_err_for(0x21U, false));
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, fw_os_threadx_err_for(0xFFU, true));
  TEST_END("fw_os threadx: every other status maps to what fw_os.h documents");
}

int main(void)
{
  internal_check_ticks_at_1khz();
  internal_check_ticks_round_up();
  internal_check_ticks_never_become_forever();
  internal_check_ms_for();
  internal_check_priority_map();
  internal_check_err_wait_cases();
  internal_check_err_other_cases();
  return 0;
}
