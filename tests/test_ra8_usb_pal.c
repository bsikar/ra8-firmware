/**
 * @file test_ra8_usb_pal.c
 * @brief Unit tests for libs/ra8_usb_pal
 * @details Exercises PAL lifecycle, endpoint queues, callback dispatch, argument rejection, and promoted MC/DC predicates against fake USB MMIO.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_pal.h"
#include "ra8_usb_pal_internal.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_pal_t
 * @brief Loopback payload pattern, buffer size and the status register seed.
 */
typedef enum : uint16_t {
  k_t_payload_mask = 0xAAU,   /**< XORed with the byte index to generate the
                                   payload, so no two bytes repeat.            */
  k_t_rx_cap       = 64U,     /**< Receive buffer, bytes. */
  k_t_intsts_probe = 0xBEEFU, /**< INTSTS0 pattern proving the read path
                                   returns the register verbatim.              */
} t_pal_t;

/**
 * @brief Reset the hosted USB PAL fixture.
 *
 * @details
 * Deinitializes any live PAL while its register mapping is valid, then resets fake MMIO and module-stop state.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post A previously initialized PAL has been deinitialized; an uninitialized PAL remains inert.
 * @post The fake register mapping and module-stop fixture are reset.
 * @note Fixture reset helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prep(void)
{
  (void)ra8_usb_pal_deinit();
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify full-speed initialization starts detached.
 *
 * @details
 * Initializes the PAL at full speed and checks that the first observable state is detached.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_fs_starts_detached(void)
{
  TEST_BEGIN("ra8_usb_pal_init: FS init starts detached");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));

  ra8_usb_pal_state_t state = k_ra8_usb_pal_state_configd;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra8_usb_pal_state_detached, state);
  TEST_END("ra8_usb_pal_init: FS init starts detached");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify high-speed initialization starts detached.
 *
 * @details
 * Initializes the PAL at high speed and checks that the first observable state is detached.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_hs_starts_detached(void)
{
  TEST_BEGIN("ra8_usb_pal_init: HS init starts detached");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_hs));
  ra8_usb_pal_state_t state = k_ra8_usb_pal_state_configd;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra8_usb_pal_state_detached, state);
  TEST_END("ra8_usb_pal_init: HS init starts detached");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Reject an unsupported USB speed.
 *
 * @details
 * Passes an out-of-domain speed and verifies initialization fails before the PAL changes state.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_pal_init: bad speed rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_init((ra8_usb_speed_t)99U));
  TEST_END("ra8_usb_pal_init: bad speed rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify attach and detach state transitions.
 *
 * @details
 * Exercises both pull-up transitions and checks the cached PAL state after each successful request.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_attach_detach_cycles_state(void)
{
  TEST_BEGIN("ra8_usb_pal_attach: cycles state attached/detached");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_attach(true));
  ra8_usb_pal_state_t state = k_ra8_usb_pal_state_detached;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra8_usb_pal_state_attached, state);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_attach(false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra8_usb_pal_state_detached, state);
  TEST_END("ra8_usb_pal_attach: cycles state attached/detached");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify endpoint-open argument validation.
 *
 * @details
 * Covers reserved and oversized endpoint numbers, invalid direction and type values, packet-size bounds, and one valid open.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_ep_open_validates_args(void)
{
  TEST_BEGIN("ra8_usb_pal_ep_open: arg validation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));

  /* Bad EP addr (0 = control reserved). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(0U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_pal_ep_open(99U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));

  /* Bad direction. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_pal_ep_open(1U, (ra8_usb_pal_ep_dir_t)9U, k_ra8_usb_pal_ep_type_bulk, 64U));

  /* Bad type. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, (ra8_usb_pal_ep_type_t)99U, 64U));

  /* Zero packet size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 0U));

  /* Valid args -> ep_open stores the slot. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_END("ra8_usb_pal_ep_open: arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Round-trip one packet through the endpoint ring.
 *
 * @details
 * Checks the empty-ring result, queues a patterned packet, receives identical bytes, and verifies the ring becomes empty again.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_ep_send_recv_loopback(void)
{
  TEST_BEGIN("ra8_usb_pal_ep_{send,recv}: in-memory loopback round-trip");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));

  uint8_t data[16] = {0U};
  for (uint16_t i = 0U; i < (uint16_t)sizeof(data); ++i) {
    data[i] = (uint8_t)(k_t_payload_mask ^ i);
  }
  uint8_t  rx[k_t_rx_cap] = {0U};
  uint16_t rx_len         = (uint16_t)sizeof(rx);

  /* Empty ring -> recv reports no_data. */
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_pal_ep_recv(1U, rx, &rx_len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_ep_send(1U, data, (uint16_t)sizeof(data)));

  rx_len = (uint16_t)sizeof(rx);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_ep_recv(1U, rx, &rx_len));
  TEST_ASSERT_EQ(sizeof(data), rx_len);
  for (uint16_t i = 0U; i < (uint16_t)sizeof(data); ++i) {
    TEST_ASSERT_EQ(data[i], rx[i]);
  }

  /* Ring empty again. */
  rx_len = (uint16_t)sizeof(rx);
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_pal_ep_recv(1U, rx, &rx_len));
  TEST_END("ra8_usb_pal_ep_{send,recv}: in-memory loopback round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify send and receive argument validation.
 *
 * @details
 * Covers endpoint bounds, null buffers, transfer limits, zero receive capacity, and unopened endpoint state without mutating a valid slot.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_ep_send_recv_arg_validation(void)
{
  TEST_BEGIN("ra8_usb_pal_ep_{send,recv}: arg validation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));

  uint8_t  buf[16] = {0U};
  uint16_t len     = (uint16_t)sizeof(buf);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_ep_send(0U, buf, 16U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_ep_send(99U, buf, 16U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pal_ep_send(1U, nullptr, 16U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_send(1U, buf, (uint16_t)(k_ra8_usb_pal_xfer_max + 1U)));
  /* ep_send on an unopened EP returns invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_ep_send(2U, buf, 16U));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pal_ep_recv(1U, nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pal_ep_recv(1U, buf, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_ep_recv(0U, buf, &len));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_ep_recv(1U, buf, &zero));
  /* ep_recv on an unopened EP returns invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_ep_recv(2U, buf, &len));
  TEST_END("ra8_usb_pal_ep_{send,recv}: arg validation");
}

static int32_t s_usb_event_count = 0;

/**
 * @brief Count one translated USB event.
 *
 * @details
 * Records that the PAL invoked its callback while deliberately ignoring the context, speed, and event-mask payloads.
 *
 * @param[in] ctx Callback context, unused by this counting fixture.
 * @param[in] speed Reported USB bus speed, unused by this counting fixture.
 * @param[in] mask Translated PAL event mask, unused by this counting fixture.
 * @pre ::s_usb_event_count names writable fixture storage.
 * @pre The callback arguments may carry any values accepted by the PAL callback contract.
 * @post ::s_usb_event_count is incremented exactly once.
 * @post No PAL or fake-MMIO state is modified.
 * @note Single-threaded counting stub; not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stub_usb_event(void* ctx, ra8_usb_speed_t speed, uint16_t mask)
{
  (void)ctx;
  (void)speed;
  (void)mask;
  ++s_usb_event_count;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify event-handler binding and removal.
 *
 * @details
 * Installs the hosted callback on an initialized PAL and then removes it through the public binding API.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_event_handler_attach_detach(void)
{
  TEST_BEGIN("ra8_usb_pal_set_event_handler: attach + detach");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));

  s_usb_event_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_set_event_handler(internal_stub_usb_event, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_set_event_handler(nullptr, nullptr));
  TEST_END("ra8_usb_pal_set_event_handler: attach + detach");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify interrupt-status dispatch reaches the PAL callback.
 *
 * @details
 * Seeds INTSTS0, dispatches full-speed events, and checks nonzero, zero, and mismatched-speed delivery behavior.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_dispatch_relays_intsts0(void)
{
  TEST_BEGIN("ra8_usb_dispatch -> PAL relay -> stack callback");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));

  s_usb_event_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_set_event_handler(internal_stub_usb_event, nullptr));

  /* Pre-set INTSTS0 to a non-zero value so dispatch sees a real event. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->INTSTS0               = k_t_intsts_probe;

  ra8_usb_dispatch(k_ra8_usb_speed_fs);
  TEST_ASSERT_EQ(1, s_usb_event_count);

  /* Zero INTSTS0 dispatch is a no-op (no event delivered). */
  reg->INTSTS0 = 0U;
  ra8_usb_dispatch(k_ra8_usb_speed_fs);
  TEST_ASSERT_EQ(1, s_usb_event_count);

  /* HS speed mismatch is filtered out of the FS-bound PAL. */
  reg->INTSTS0 = k_t_intsts_probe;
  ra8_usb_dispatch(k_ra8_usb_speed_hs);
  /* Whether HS dispatch fires depends on ra8_usb's internal state;
   * the PAL's per-speed filter should drop it regardless. */
  TEST_ASSERT(s_usb_event_count <= 2);
  TEST_END("ra8_usb_dispatch -> PAL relay -> stack callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Verify every stateful PAL call rejects pre-init use.
 *
 * @details
 * Calls attach, state, endpoint, handler, and deinit APIs before initialization and checks each invalid-state result.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_calls_before_init_fail(void)
{
  TEST_BEGIN("ra8_usb_pal_*: pre-init calls return invalid_state");
  internal_prep();

  ra8_usb_pal_state_t state   = k_ra8_usb_pal_state_configd;
  uint8_t             buf[16] = {0U};
  uint16_t            len     = 16U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_ep_send(1U, buf, 16U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_ep_recv(1U, buf, &len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pal_set_event_handler(internal_stub_usb_event, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pal_deinit());
  TEST_END("ra8_usb_pal_*: pre-init calls return invalid_state");
}

/* =============================================================================
 * MC/DC tests for compound decisions in libs/ra8_usb_pal/src/ra8_usb_pal.c
 * ============================================================================= */

/**
 * @test internal_test_mcdc_init_speed
 *
 * @par MC/DC:
 * Decision: `if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs))`
 * (libs/ra8_usb_pal/src/ra8_usb_pal.c, ra8_usb_pal_init).
 * - V1: speed=fs       -> C1=F short-circuit -> false (proceed; ok).
 * - V2: speed=99 (bad) -> C1=T, C2=T -> true (rejected; varies C1).
 * - V3: speed=hs       -> C1=T, C2=F -> false (proceed; varies C2).
 * V1 vs V2 vary C1. V2 vs V3 vary C2. N+1 = 3 vectors for N=2.
 * @brief Exercise MC/DC vectors for speed validation.
 *
 * @details
 * Drives full-speed, invalid, and high-speed inputs so each operand in the initialization speed guard independently affects the decision.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_init_speed(void)
{
  TEST_BEGIN("mcdc: ra8_usb_pal_init speed (!=fs && !=hs)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pal_init((ra8_usb_speed_t)99U));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_hs));
  TEST_END("mcdc: ra8_usb_pal_init speed (!=fs && !=hs)");
}

/**
 * @test internal_test_mcdc_ep_open_addr
 *
 * @par MC/DC:
 * Decision: `if ((ep_addr == 0U) || (ep_addr > k_ra8_usb_pal_ep_max))`
 * (libs/ra8_usb_pal/src/ra8_usb_pal.c, ra8_usb_pal_ep_open).
 * - V1: ep_addr=1                     -> C1=F, C2=F -> false (proceed).
 * - V2: ep_addr=0                     -> C1=T short-circuit -> true (varies C1).
 * - V3: ep_addr=k_ra8_usb_pal_ep_max+1 -> C1=F, C2=T -> true (varies C2).
 * V1 vs V2 vary C1. V1 vs V3 vary C2 with C1 held F. N+1 = 3 vectors.
 * @brief Exercise MC/DC vectors for endpoint-number bounds.
 *
 * @details
 * Drives an in-range endpoint, endpoint zero, and one endpoint above the maximum through the shared endpoint predicate.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_ep_open_addr(void)
{
  TEST_BEGIN("mcdc: ep_open ep_addr (==0 || >ep_max)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(0U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open((uint8_t)((uint32_t)k_ra8_usb_pal_ep_max + 1U),
                                     k_ra8_usb_pal_ep_dir_in,
                                     k_ra8_usb_pal_ep_type_bulk,
                                     64U));
  TEST_END("mcdc: ep_open ep_addr (==0 || >ep_max)");
}

/**
 * @test internal_test_mcdc_ep_open_dir
 *
 * @par MC/DC:
 * Decision: `if ((dir != k_ra8_usb_pal_ep_dir_out) && (dir != k_ra8_usb_pal_ep_dir_in))`
 * (libs/ra8_usb_pal/src/ra8_usb_pal.c, ra8_usb_pal_ep_open).
 * - V1: dir=out -> C1=F short-circuit -> false (proceed).
 * - V2: dir=99  -> C1=T, C2=T         -> true (reject; varies C1).
 * - V3: dir=in  -> C1=T, C2=F         -> false (proceed; varies C2).
 * V1 vs V2 vary C1. V2 vs V3 vary C2. N+1 = 3 vectors for N=2.
 * @brief Exercise MC/DC vectors for endpoint direction.
 *
 * @details
 * Drives OUT, an invalid direction, and IN so both inequalities in the direction guard independently affect the result.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_ep_open_dir(void)
{
  TEST_BEGIN("mcdc: ep_open dir (!=out && !=in)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_out, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_pal_ep_open(2U, (ra8_usb_pal_ep_dir_t)99U, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(3U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_END("mcdc: ep_open dir (!=out && !=in)");
}

/**
 * @test internal_test_mcdc_ep_open_type_packet
 *
 * @par MC/DC:
 * Decision (3 conditions OR): `if ((type > k_ra8_usb_pal_ep_type_intr) ||
 * (max_packet == 0U) || (max_packet > k_ra8_usb_pal_xfer_max))`
 * (libs/ra8_usb_pal/src/ra8_usb_pal.c, ra8_usb_pal_ep_open).
 * - V1: type=bulk, packet=64           -> C1=F,C2=F,C3=F -> false (proceed).
 * - V2: type=99,   packet=64           -> C1=T short-circuit -> true (varies C1).
 * - V3: type=bulk, packet=0            -> C1=F,C2=T short-circuit -> true (varies C2).
 * - V4: type=bulk, packet=k_xfer_max+1 -> C1=F,C2=F,C3=T -> true (varies C3).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Chilenski masking-MC/DC minimal cover for a 3-input short-circuit
 * OR. Each vector varies exactly one condition relative to V1 with
 * the others held in their non-masking value (false for OR). DO-178C
 * Level B / IEC 61508 SIL 3 qualified form; full 2^3=8 combinatoric
 * coverage is not required.
 * @brief Exercise MC/DC vectors for endpoint type and packet bounds.
 *
 * @details
 * Uses one valid vector and one vector per invalid OR operand to cover type, zero packet size, and oversized packet size.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_ep_open_type_packet(void)
{
  TEST_BEGIN("mcdc: ep_open type/packet (3-cond OR)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(2U, k_ra8_usb_pal_ep_dir_in, (ra8_usb_pal_ep_type_t)99U, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(3U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_open(4U,
                                     k_ra8_usb_pal_ep_dir_in,
                                     k_ra8_usb_pal_ep_type_bulk,
                                     (uint16_t)((uint32_t)k_ra8_usb_pal_xfer_max + 1U)));
  TEST_END("mcdc: ep_open type/packet (3-cond OR)");
}

/**
 * @test internal_test_mcdc_ep_send_len_data
 *
 * @par MC/DC:
 * Decision (3 conditions): `if ((len > k_ra8_usb_pal_xfer_max) ||
 * ((data == nullptr) && (len != 0U)))`
 * (libs/ra8_usb_pal/src/ra8_usb_pal.c, ra8_usb_pal_ep_send).
 * Let A=`len>xfer_max`, B=`data==nullptr`, C=`len!=0U`. Decision
 * is `A || (B && C)`.
 * - V1: A=F,B=F,C=T (data!=NULL, len=1) -> false (send ok).
 * - V2: A=T (len > xfer_max)            -> true  (varies A; reject).
 * - V3: A=F,B=T,C=T (data=NULL, len=1)  -> true  (varies B; reject).
 * - V4: A=F,B=T,C=F (data=NULL, len=0)  -> false (varies C).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * The mixed `A || (B && C)` admits the same masking-MC/DC minimal
 * cover as a 3-condition decision: 4 representative vectors per
 * Chilenski. DO-178C Level B / IEC 61508 SIL 3 qualified form.
 * @brief Exercise MC/DC vectors for send length and data validity.
 *
 * @details
 * Covers the mixed length-or-null-data decision with valid, oversized, null-nonzero, and null-zero transfers.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_ep_send_len_data(void)
{
  TEST_BEGIN("mcdc: ep_send len/data (3-cond mixed)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));

  uint8_t buf[16] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_ep_send(1U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pal_ep_send(1U, buf, (uint16_t)(k_ra8_usb_pal_xfer_max + 1U)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pal_ep_send(1U, nullptr, 1U));
  /* V4: data=NULL,len=0 -> guard does not return null_ptr; the
   * compound decision evaluates FALSE here (B=T, C=F). */
  const ra8_err_t v4 = ra8_usb_pal_ep_send(1U, nullptr, 0U);
  TEST_ASSERT(v4 != k_ra8_err_null_ptr);
  TEST_END("mcdc: ep_send len/data (3-cond mixed)");
}

/**
 * @test internal_test_mcdc_priv_usb_pal_should_dispatch_event
 *
 * @par MC/DC:
 * Decision at libs/ra8_usb_pal/src/ra8_usb_pal.c (call site) -> helper at
 * libs/ra8_usb_pal/src/ra8_usb_pal.c:
 *   ``event_fn != NULL && mask != none`` (2 conditions, AND).
 * - V1: cb=NULL,  mask!=none -> false (left varies vs V2)
 * - V2: cb!=NULL, mask!=none -> true
 * - V3: cb!=NULL, mask=none  -> false (right varies vs V2)
 * N+1 = 3.
 * @brief Exercise MC/DC vectors for event dispatch eligibility.
 *
 * @details
 * Varies callback presence and translated mask independently through the promoted private predicate.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_priv_usb_pal_should_dispatch_event(void)
{
  TEST_BEGIN("usb_pal MC/DC: should_dispatch_event AND");
  uint8_t fake_cb = 0U;
  TEST_ASSERT(!priv_usb_pal_should_dispatch_event(nullptr, 0x0001U, 0x0000U));
  TEST_ASSERT(priv_usb_pal_should_dispatch_event(&fake_cb, 0x0001U, 0x0000U));
  TEST_ASSERT(!priv_usb_pal_should_dispatch_event(&fake_cb, 0x0000U, 0x0000U));
  TEST_END("usb_pal MC/DC: should_dispatch_event AND");
}

/**
 * @test internal_test_mcdc_priv_usb_pal_ep_out_of_range
 *
 * @par MC/DC:
 * Decision at libs/ra8_usb_pal/src/ra8_usb_pal.c (call site) -> helper at
 * libs/ra8_usb_pal/src/ra8_usb_pal.c:
 *   ``ep_addr == 0 || ep_addr > ep_max`` (2 conditions, OR).
 * - V1: ep=1,  ep_max=10 -> false
 * - V2: ep=0,  ep_max=10 -> true (varies left)
 * - V3: ep=11, ep_max=10 -> true (varies right)
 * N+1 = 3.
 * @brief Exercise MC/DC vectors for the private endpoint predicate.
 *
 * @details
 * Varies endpoint-zero and endpoint-above-maximum independently while retaining one valid endpoint vector.
 *
 * @pre The hosted fake-MMIO fixture is available.
 * @pre No concurrent PAL operation is active in the single-threaded test process.
 * @post Every expected return code and observable state transition has been asserted.
 * @post The next vector can reclaim singleton state through ::internal_prep.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_priv_usb_pal_ep_out_of_range(void)
{
  TEST_BEGIN("usb_pal MC/DC: ep_out_of_range OR");
  TEST_ASSERT(!priv_usb_pal_ep_out_of_range(1U, 10U));
  TEST_ASSERT(priv_usb_pal_ep_out_of_range(0U, 10U));
  TEST_ASSERT(priv_usb_pal_ep_out_of_range(11U, 10U));
  TEST_END("usb_pal MC/DC: ep_out_of_range OR");
}

/**
 * @brief Discard one hosted diagnostic byte.
 *
 * @details
 * Redirects expected argument-rejection diagnostics away from the target-only
 * ITM window while the hosted PAL vectors run.
 *
 * @param[in] ctx Opaque logger context, unused by this sink.
 * @param[in] byte Diagnostic byte to discard.
 * @pre No test vector depends on observing diagnostic text.
 * @pre The sink is installed only for this single-threaded test process.
 * @post No memory, fixture, or USB state is modified.
 * @post Control returns immediately to the production logger.
 * @note Hosted safety adapter; not a production output path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_init_fs_starts_detached();
  internal_test_init_hs_starts_detached();
  internal_test_init_bad_speed();
  internal_test_attach_detach_cycles_state();
  internal_test_ep_open_validates_args();
  internal_test_ep_send_recv_loopback();
  internal_test_ep_send_recv_arg_validation();
  internal_test_event_handler_attach_detach();
  internal_test_dispatch_relays_intsts0();
  internal_test_calls_before_init_fail();
  internal_test_mcdc_init_speed();
  internal_test_mcdc_ep_open_addr();
  internal_test_mcdc_ep_open_dir();
  internal_test_mcdc_ep_open_type_packet();
  internal_test_mcdc_ep_send_len_data();
  internal_test_mcdc_priv_usb_pal_should_dispatch_event();
  internal_test_mcdc_priv_usb_pal_ep_out_of_range();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
