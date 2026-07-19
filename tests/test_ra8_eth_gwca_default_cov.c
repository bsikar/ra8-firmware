/**
 * @file test_ra8_eth_gwca_default_cov.c
 * @brief Coverage-focused unit tests for ra8_eth_gwca_default.c
 *
 * @details
 * Companion to test_ra8_eth_gwca.c. This TU targets the one-call
 * default-state surface (default_open / default_send / default_recv /
 * rx_frame) and its static bring-up sub-helpers, driving the
 * validation-reject legs, the descriptor-queue re-arm paths, and the
 * step-trail error branches that the base test leaves uncovered. Every
 * uncovered branch is reached by pre-seeding the caller-owned
 * descriptor rings and MSTP ref counts, never by injecting a timed
 * hardware event, so the suite is deterministic on the Linux host
 * runner (no SIGALRM, no wall-clock spin).
 *
 * A handful of legs remain genuinely host-unreachable and are left
 * uncovered rather than excluded (see the file-level note at the
 * bottom): the two ::ra8_eth_gwca_set_operation_mode call sites and the
 * three ::ra8_eth_gwca_reload_queue / ::ra8_eth_gwca_kick_tx failure
 * returns can only fail on real silicon (the RA8_SIMULATOR_MODE build
 * short-circuits the poll to k_ra8_ok, and every index that would make
 * a reload/kick reject is already rejected by the identical range
 * guard earlier in the same call chain).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_eth_gwca.h"
#include "ra8_eth_gwca_internal.h"
#include "ra8_ether_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum eth_gwca_default_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_gwca_desc_size_hi = 0xFU, /**< High nibble of a descriptor's 12-bit data size. */
  k_gwca_desc_size_lo =
    0xFFU, /**< Its low byte; together they give the largest size the field can express. */
  k_gwca_drain_attempts =
    255U, /**< Poll attempts when draining the chain: more than it can hold, so the empty arm is reached. */
  k_gwca_linkfix_over_max =
    33U, /**< A link-fix count above the 32-entry maximum, which bring_up must reject. */
  k_gwca_slot_bytes_valid =
    128U, /**< A slot size the driver accepts, used for both directions so a rejection can only come from the field under test. */
  k_gwca_queue_index_over_max =
    40U, /**< A queue index at or past the 32-queue maximum, which reload and configure_queue must both reject. */
  k_gwca_slot_bytes_small =
    64U, /**< A smaller accepted slot size, proving the check is a bound and not an equality. */
  k_gwca_tx_pool_bytes = 128, /**< TX pool capacity: one slot's worth. */
  k_gwca_frame_bytes   = 64,  /**< Staging frame capacity.             */
} eth_gwca_default_cov_uint8_const_t;

/**
 * @enum eth_gwca_default_cov_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_gwca_desc_ptr_lo =
    0x100U, /**< A non-zero buffer pointer in the descriptor, so a walker that ignored the chain is visible. */
  k_gwca_slot_bytes_over_max =
    4096U, /**< A slot size above the 2048-byte maximum, which the extended TX init must reject. */
  k_gwca_out_bytes =
    256, /**< Receive-side output buffer, over one slot so a short read is visible. */
} eth_gwca_default_cov_uint16_const_t;

/**
 * @brief Reset the mmap'd peripheral window + MSTP ref counts.
 *
 * @details Mirrors the base test's setUp: zero the simulated register
 * space and re-gate every module so ::ra8_eth_gwca_init can re-enable
 * the ESWM stop bit cleanly.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Populate a state block whose RX/TX rings + LINKFIX are valid.
 *
 * @details Points every field at the caller-supplied storage using a
 * 4-entry RX ring, a 4-entry extended TX ring, an 8-entry LINKFIX
 * table, distinct in-range queue indices, and 128-byte slots. Callers
 * mutate a single field afterward to steer ::ra8_eth_gwca_default_open
 * down a specific error leg.
 *
 * @param[out] st         State block to fill.
 * @param[in]  linkfix    LINKFIX table storage (>= 8 entries).
 * @param[in]  rx_chain   RX basic-descriptor ring (>= 4 entries).
 * @param[in]  rx_pool    RX buffer pool (>= 3 * 128 bytes).
 * @param[in]  tx_chain   TX extended-descriptor ring (>= 4 entries).
 * @param[in]  tx_pool    TX buffer pool (>= 3 * 128 bytes).
 */
static void make_valid_state(ra8_eth_gwca_default_state_t* st,
                             ra8_gwca_basic_descriptor_t*  linkfix,
                             ra8_gwca_basic_descriptor_t*  rx_chain,
                             uint8_t*                      rx_pool,
                             ra8_gwca_ext_descriptor_t*    tx_chain,
                             uint8_t*                      tx_pool)
{
  *st                = (ra8_eth_gwca_default_state_t){};
  st->linkfix_table  = linkfix;
  st->linkfix_count  = 8U;
  st->rx_chain       = rx_chain;
  st->rx_depth       = 4U;
  st->rx_pool        = rx_pool;
  st->rx_slot_bytes  = k_gwca_slot_bytes_valid;
  st->rx_queue_index = 0U;
  st->tx_chain       = tx_chain;
  st->tx_depth       = 4U;
  st->tx_pool        = tx_pool;
  st->tx_slot_bytes  = k_gwca_slot_bytes_valid;
  st->tx_queue_index = 1U;
  st->mac_port       = 0U;
}

/**
 * @test default_send argument guards + idle-queue re-arm.
 *
 * @par MC/DC:
 * Two single-condition guards in ::ra8_eth_gwca_default_send:
 *   D1: `if (len == 0U)`
 *     - V_F: len = 64          -> false (falls through)
 *     - V_T: len = 0           -> true  (returns k_ra8_err_invalid_arg)
 *   D2: `if (len > state->tx_slot_bytes)`
 *     - V_F: len = 64, cap 64  -> false (falls through)
 *     - V_T: len = 65, cap 64  -> true  (returns k_ra8_err_invalid_arg)
 * Each atomic guard gets its own T/F pair, the minimal MC/DC set.
 * The re-arm sub-case then seeds the TX terminator to LEMPTY so
 * ::internal_tx_ext_rearm rewrites it back to LINK and pulses the
 * queue before slot 0 is filled.
 */
static void test_default_send_guards_and_rearm(void)
{
  TEST_BEGIN("default_send guards + rearm");
  prep();
  ra8_eth_gwca_default_state_t                          st = {};
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t s_tx_chain[2];
  static uint8_t                                        s_tx_pool[k_gwca_tx_pool_bytes];
  static uint8_t                                        s_frame[k_gwca_frame_bytes];

  s_tx_chain[0]     = (ra8_gwca_ext_descriptor_t){};
  s_tx_chain[1]     = (ra8_gwca_ext_descriptor_t){};
  st.tx_chain       = s_tx_chain;
  st.tx_depth       = 2U;
  st.tx_pool        = s_tx_pool;
  st.tx_slot_bytes  = k_gwca_slot_bytes_small;
  st.tx_queue_index = 0U;
  st.mac_port       = 0U;

  /* Null-pointer guards (RA8_CHECK_NULL_PTR). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gwca_default_send(nullptr, s_frame, 64U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gwca_default_send(&st, nullptr, 64U));

  /* D1 V_T: len == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_send(&st, s_frame, 0U));
  /* D2 V_T: len > tx_slot_bytes. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_send(&st, s_frame, 65U));

  /* Re-arm leg: terminator idle-disabled to LEMPTY. default_send must
   * restore it to LINK, fill slot 0 (FSINGLE), and return ok under
   * RA8_SIMULATOR_MODE (the write-back spin is host-compiled-out). */
  s_tx_chain[1].dt = (uint8_t)k_ra8_gwdcc_dt_lempty;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_default_send(&st, s_frame, 64U));
  TEST_ASSERT_EQ(k_ra8_gwdcc_dt_link, s_tx_chain[1].dt);
  TEST_ASSERT_EQ(k_ra8_gwdcc_dt_fsingle, s_tx_chain[0].dt);
  TEST_END("default_send guards + rearm");
}

/**
 * @test default_send reload-queue failure leg.
 *
 * @par MC/DC:
 * Single condition `if (reload_err != k_ra8_ok)` in
 * ::ra8_eth_gwca_default_send:
 *   - V_T: tx_queue_index = 40 (>= 32, no GWDCC register) ->
 *     ::ra8_eth_gwca_reload_queue returns k_ra8_err_invalid_arg ->
 *     default_send returns it.
 * The V_F control (reload ok) is exercised by the re-arm test above.
 * The terminator is pre-seeded to LINK so the re-arm helper is a
 * no-op and the failure is attributed purely to the mid-send reload.
 */
static void test_default_send_reload_failure(void)
{
  TEST_BEGIN("default_send reload failure");
  prep();
  ra8_eth_gwca_default_state_t                          st = {};
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t s_tx_chain[2];
  static uint8_t                                        s_tx_pool[k_gwca_tx_pool_bytes];
  static uint8_t                                        s_frame[k_gwca_frame_bytes];

  s_tx_chain[0]     = (ra8_gwca_ext_descriptor_t){};
  s_tx_chain[1]     = (ra8_gwca_ext_descriptor_t){};
  s_tx_chain[1].dt  = (uint8_t)k_ra8_gwdcc_dt_link; /* not LEMPTY -> rearm no-op */
  st.tx_chain       = s_tx_chain;
  st.tx_depth       = 2U;
  st.tx_pool        = s_tx_pool;
  st.tx_slot_bytes  = k_gwca_slot_bytes_small;
  st.tx_queue_index = k_gwca_queue_index_over_max; /* >= 32 -> reload rejects */
  st.mac_port       = 0U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_send(&st, s_frame, 64U));
  TEST_END("default_send reload failure");
}

/**
 * @test default_recv self-heals a GWCA-disabled RX queue.
 *
 * @par MC/DC:
 * Two single conditions on the drain path:
 *   In ::ra8_eth_gwca_default_recv, `if (err == k_ra8_err_no_data)`:
 *     - V_T: no FSINGLE slot -> rx_frame returns no_data -> re-arm runs.
 *   In ::internal_rearm_queue_if_disabled, `if (term->dt != LEMPTY)`:
 *     - V_F: terminator seeded to LEMPTY -> body runs (restore LINK,
 *       snap cursor to 0, reload the queue).
 * The ring is primed by init_ring (data slots FEMPTY), then the LINK
 * terminator is forced to LEMPTY so the disabled-queue heal fires.
 */
static void test_default_recv_rearm_disabled(void)
{
  TEST_BEGIN("default_recv rearm disabled");
  prep();
  ra8_eth_gwca_default_state_t                            st = {};
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  static uint8_t                                          s_out[k_gwca_out_bytes];
  uint32_t                                                got_len = 0U;

  st.rx_chain       = s_rx_chain;
  st.rx_depth       = 4U;
  st.rx_pool        = s_rx_pool;
  st.rx_slot_bytes  = k_gwca_slot_bytes_valid;
  st.rx_queue_index = 0U;
  st.rx_head        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_rx_chain, 4U, 128U));

  /* Force the terminator into the GWCA-disabled state. */
  s_rx_chain[3].dt = (uint8_t)k_ra8_gwdcc_dt_lempty;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_eth_gwca_default_recv(&st, s_out, sizeof(s_out), &got_len));
  /* Terminator re-armed to LINK, software cursor snapped to 0. */
  TEST_ASSERT_EQ(k_ra8_gwdcc_dt_link, s_rx_chain[3].dt);
  TEST_ASSERT_EQ(0U, st.rx_head);
  TEST_END("default_recv rearm disabled");
}

/**
 * @test rx_frame drain rejects an oversized / unbacked FSINGLE slot.
 *
 * @par MC/DC:
 * Compound decision in ::internal_drain_rx_slot:
 *   `if (buf == nullptr || frame_ds > out_capacity)`
 *   - V_T_-: buf == nullptr (PTR field zero), ds <= capacity ->
 *     first condition drives the reject (short-circuit).
 *   - V_F_T: buf != nullptr (PTR seeded), ds = 4095 > capacity 1 ->
 *     second condition drives the reject.
 * These two vectors prove each condition independently forces the
 * k_ra8_err_invalid_arg return that ::ra8_eth_gwca_rx_frame propagates.
 * The both-false control vector (a successful drain) dereferences the
 * decoded buffer pointer in `memcpy`; on x86_64 the 40-bit PTR field
 * cannot round-trip a >40-bit host address, so that leg is host-unsafe
 * and is covered on the chip target instead.
 */
static void test_rx_drain_reject(void)
{
  TEST_BEGIN("rx_frame drain reject");
  prep();
  ra8_eth_gwca_default_state_t                            st = {};
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  static uint8_t                                          s_out[k_gwca_out_bytes];
  uint32_t                                                got_len = 0U;

  st.rx_chain       = s_rx_chain;
  st.rx_depth       = 4U;
  st.rx_pool        = s_rx_pool;
  st.rx_slot_bytes  = k_gwca_slot_bytes_valid;
  st.rx_queue_index = 0U;

  /* V_T_-: buf decodes to nullptr; ds within capacity so only the
   * null test drives the reject. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_rx_chain, 4U, 128U));
  s_rx_chain[0].dt    = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  s_rx_chain[0].ptr_h = 0U;
  s_rx_chain[0].ptr_l = 0U;
  s_rx_chain[0].ds_l  = k_gwca_slot_bytes_valid;
  s_rx_chain[0].ds_h  = 0U;
  st.rx_head          = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gwca_default_recv(&st, s_out, sizeof(s_out), &got_len));

  /* V_F_T: buf non-null; ds = 4095 exceeds out_capacity = 1 so the
   * size test drives the reject (buffer is never dereferenced). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_rx_chain, 4U, 128U));
  s_rx_chain[0].dt    = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  s_rx_chain[0].ptr_h = 0U;
  s_rx_chain[0].ptr_l = k_gwca_desc_ptr_lo;
  s_rx_chain[0].ds_l  = k_gwca_desc_size_lo;
  s_rx_chain[0].ds_h  = k_gwca_desc_size_hi;
  st.rx_head          = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_recv(&st, s_out, 1U, &got_len));
  TEST_END("rx_frame drain reject");
}

/**
 * @test default_open rejects a TX ring shallower than 2 entries.
 *
 * @par MC/DC:
 * Single condition `if (depth < k_tx_ext_min_depth)` in
 * ::internal_tx_ext_init (reached only through default_open):
 *   - V_T: tx_depth = 1 -> returns k_ra8_err_invalid_arg, which the
 *     rings helper, the pre helper, and default_open each propagate,
 *     stamping the fail-2 / fail-1 step trail.
 * The RX ring is valid so control reaches the TX-init guard.
 */
static void test_default_open_bad_tx_depth(void)
{
  TEST_BEGIN("default_open bad tx_depth");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[2];
  static uint8_t                                          s_tx_pool[k_gwca_tx_pool_bytes];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);
  st.tx_depth = 1U; /* < 2 -> internal_tx_ext_init rejects */

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_2, g_ra8_eth_gwca_pre_step);
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_1, g_ra8_eth_gwca_open_step);
  TEST_END("default_open bad tx_depth");
}

/**
 * @test default_open rejects a TX slot size over the 12-bit DS field.
 *
 * @par MC/DC:
 * Single condition `if (slot_bytes > k_tx_ext_max_bytes)` in
 * ::internal_tx_ext_init:
 *   - V_T: tx_slot_bytes = 4096 (> 2048) -> k_ra8_err_invalid_arg.
 * The depth guard is satisfied (2) so control reaches the size guard;
 * the reject propagates through the rings + pre helpers to
 * default_open with the fail-2 / fail-1 trail.
 */
static void test_default_open_bad_tx_slot_bytes(void)
{
  TEST_BEGIN("default_open bad tx_slot_bytes");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[4];
  static uint8_t                                          s_tx_pool[3U * k_gwca_slot_bytes_valid];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);
  st.tx_depth      = 2U;
  st.tx_slot_bytes = k_gwca_slot_bytes_over_max; /* > 2048 -> internal_tx_ext_init rejects */

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_2, g_ra8_eth_gwca_pre_step);
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_1, g_ra8_eth_gwca_open_step);
  TEST_END("default_open bad tx_slot_bytes");
}

/**
 * @test default_open propagates a bring-up (LINKFIX) failure.
 *
 * @par MC/DC:
 * Single condition `if (err != k_ra8_ok)` guarding the bring_up call
 * in ::internal_default_open_pre:
 *   - V_T: linkfix_count = 33 (> 32) -> ::ra8_eth_gwca_bring_up rejects
 *     the count during install_linkfix -> pre stamps fail-3 and
 *     returns, default_open stamps fail-1.
 * The rings are valid so control reaches bring_up.
 */
static void test_default_open_bad_linkfix(void)
{
  TEST_BEGIN("default_open bad linkfix_count");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[4];
  static uint8_t                                          s_tx_pool[3U * k_gwca_slot_bytes_valid];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);
  st.linkfix_count = k_gwca_linkfix_over_max; /* > 32 -> bring_up rejects */

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_3, g_ra8_eth_gwca_pre_step);
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_1, g_ra8_eth_gwca_open_step);
  TEST_END("default_open bad linkfix_count");
}

/**
 * @test default_open propagates a per-queue configure failure.
 *
 * @par MC/DC:
 * Single condition `if (err != k_ra8_ok)` guarding the queues helper
 * in ::ra8_eth_gwca_default_open:
 *   - V_T: rx_queue_index = 40 (>= 32, no GWDCC register) ->
 *     ::ra8_eth_gwca_configure_queue rejects -> the queues helper
 *     returns it and default_open stamps fail-2.
 * The pre-phase (init + rings + bring_up + CONFIG) succeeds first, so
 * the failure is isolated to the queue-configuration step.
 */
static void test_default_open_bad_rx_queue(void)
{
  TEST_BEGIN("default_open bad rx_queue_index");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[4];
  static uint8_t                                          s_tx_pool[3U * k_gwca_slot_bytes_valid];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);
  st.rx_queue_index = k_gwca_queue_index_over_max; /* >= 32 -> configure_queue rejects */

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_2, g_ra8_eth_gwca_open_step);
  TEST_END("default_open bad rx_queue_index");
}

/**
 * @test default_open full happy path reaches OPERATION.
 *
 * @par MC/DC:
 * (no compound decision under test -- every guard evaluates false and
 * control walks the success trail: pre ok, queues ok, OPERATION ok,
 * both queue reloads ok.) Confirms the step trail lands on ok-3 and
 * both software cursors reset to 0.
 */
static void test_default_open_happy(void)
{
  TEST_BEGIN("default_open happy path");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[4];
  static uint8_t                                          s_tx_pool[3U * k_gwca_slot_bytes_valid];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_ok_3, g_ra8_eth_gwca_open_step);
  TEST_ASSERT_EQ(0U, st.rx_head);
  TEST_ASSERT_EQ(0U, st.tx_tail);
  TEST_END("default_open happy path");
}

/**
 * @test default_open propagates a ra8_eth_gwca_init failure.
 *
 * @par MC/DC:
 * Single condition `if (err != k_ra8_ok)` guarding the init call in
 * ::internal_default_open_pre:
 *   - V_T: the ESWM MSTP ref count is pre-saturated to UINT8_MAX by
 *     255 successful ::ra8_eth_gwca_init calls, so the 256th enable
 *     inside default_open returns k_ra8_err_invalid_state -> pre stamps
 *     fail-1 and returns, default_open stamps fail-1.
 * Runs last and drives its own reset so the saturated ref count does
 * not leak into earlier cases.
 */
static void test_default_open_init_saturation(void)
{
  TEST_BEGIN("default_open init saturation");
  /* Deliberately does NOT call prep() after saturating: ra8_mstp_init
   * would zero the ref count we are about to fill. */
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  for (uint32_t i = 0U; i < k_gwca_drain_attempts; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init());
  }

  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_linkfix[8];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_rx_chain[4];
  static uint8_t                                          s_rx_pool[3U * k_gwca_slot_bytes_valid];
  [[gnu::aligned(16)]] static ra8_gwca_ext_descriptor_t   s_tx_chain[4];
  static uint8_t                                          s_tx_pool[3U * k_gwca_slot_bytes_valid];
  ra8_eth_gwca_default_state_t                            st = {};

  make_valid_state(&st, s_linkfix, s_rx_chain, s_rx_pool, s_tx_chain, s_tx_pool);

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_eth_gwca_default_open(&st));
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_1, g_ra8_eth_gwca_pre_step);
  TEST_ASSERT_EQ(k_ra8_eth_gwca_step_fail_1, g_ra8_eth_gwca_open_step);
  TEST_END("default_open init saturation");
}

int32_t main(void)
{
  test_default_send_guards_and_rearm();
  test_default_send_reload_failure();
  test_default_recv_rearm_disabled();
  test_rx_drain_reject();
  test_default_open_bad_tx_depth();
  test_default_open_bad_tx_slot_bytes();
  test_default_open_bad_linkfix();
  test_default_open_bad_rx_queue();
  test_default_open_happy();
  test_default_open_init_saturation();
  (void)fprintf(stderr, "[OK  ] test_ra8_eth_gwca_default_cov.c\n");
  return 0;
}
