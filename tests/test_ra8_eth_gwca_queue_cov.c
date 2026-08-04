/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_eth_gwca_queue_cov.c
 * @brief Coverage-focused unit tests for ra8_eth_gwca_queue.c
 *
 * @details
 * Companion to test_ra8_eth_gwca.c. That base suite deliberately skips
 * the ::ra8_eth_gwca_tx_frame happy path on the host: it assumed a
 * BSS-resident TX pool sits above the descriptor's 40-bit PTR field, so
 * the decoded destination pointer would not round-trip on x86_64 and the
 * real memcpy would fault. This TU removes that assumption by placing the
 * TX buffer pool INSIDE the fake's SRAM window (0x22000000, mapped RW by
 * ra8_fake_mmap's constructor). A pointer into that window is below 2^32,
 * so ::ra8_eth_gwca_set_descriptor_buffer / ::ra8_eth_gwca_decode_ptr
 * round-trip it exactly and the frame copy lands in real, writable,
 * host-backed memory. That lets the full enqueue path (find slot ->
 * decode buffer -> memcpy -> stamp DS/DT -> advance tail) run
 * deterministically on the Linux runner with no timed hardware event and
 * no wall-clock spin (no SIGALRM).
 *
 * The remaining uncovered legs are reached by pre-seeding the
 * caller-owned descriptor ring: the queue-full leg (every data slot
 * FSINGLE so find_slot returns no_data) and the unbacked-slot leg (a
 * FEMPTY slot whose PTR is still zero so decode returns nullptr). Both
 * are legitimate reachable error returns, not split statements.
 */

#include "ra8_err.h"
#include "ra8_eth_gwca.h"
#include "ra8_eth_gwca_internal.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum eth_gwca_queue_cov_fixture_t
 * @brief The payload generators and their seeds, plus buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_gwca_frame_fill_len = 64U, /**< Bytes of that buffer the generator fills, which is all of it. */
  k_gwca_frame_seed =
    0xA5U, /**< Seed of the frame generator, `seed ^ i`, giving a payload with no repeated byte. */
  /** Frame buffer capacity. */
  k_gwca_frame_bytes = 64,
} eth_gwca_queue_cov_fixture_t;

/**
 * @brief SRAM-window base for host-round-trippable TX buffers.
 *
 * @details ra8_fake_mmap maps 0x22000000..0x221FFFFF (2 MiB SRAM) as
 * ordinary RW host RAM. An address in this window fits in 32 bits, so it
 * survives the descriptor's 40-bit ptr_h/ptr_l split-and-rejoin exactly.
 * 0x22080000 is 512 KiB into the window, clear of the head that other
 * SRAM-touching helpers might use.
 */
enum : uintptr_t {
  k_sram_tx_pool_addr = 0x22080000UL, /**< SRAM TX pool address. */
};

/**
 * @brief Reset the mmap'd peripheral window + MSTP ref counts.
 *
 * @details Mirrors the base suite's setUp: scrub the fake register
 * space and re-gate every module so descriptor-register writes land in a
 * known-zero backing.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @test internal_compose_gwdcc sets the SL bit when stop_on_last is true.
 *
 * @par MC/DC:
 * Single condition `if (cfg->stop_on_last)` inside internal_compose_gwdcc
 * (reached through ::ra8_eth_gwca_configure_queue):
 *   - V_T: stop_on_last = true -> the SL bit (GWDCC bit 10) is OR'd in.
 * The V_F control (SL left clear) is exercised by the base suite's
 * test_configure_queue, which passes stop_on_last = false. Together the
 * two vectors prove the guard body executes only when the field is set.
 * The sibling `is_tx` guard is also true here so DQT is set as well;
 * that is an independent single-condition guard, not part of a compound.
 */
static void test_compose_stop_on_last(void)
{
  TEST_BEGIN("compose stop_on_last -> SL");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_table[4];
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_chain[2];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_install_linkfix(s_table, 4U));

  ra8_eth_gwca_queue_cfg_t cfg = {
    .priority     = 3U,
    .is_tx        = true,
    .stop_on_last = true,
    .extended     = false,
    .chain_head   = s_chain,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_configure_queue(s_table, 0U, &cfg));

  /* The composed GWDCC[0] must carry the SL bit and the DQT (TX) bit. */
  const uint32_t gwdcc0 = *ra8_gwca_gwdcc(0U);
  TEST_ASSERT_EQ(k_ra8_gwdcc_sl, gwdcc0 & (uint32_t)k_ra8_gwdcc_sl);
  TEST_ASSERT_EQ(k_ra8_gwdcc_dqt, gwdcc0 & (uint32_t)k_ra8_gwdcc_dqt);
  TEST_ASSERT_EQ(k_ra8_gwdcc_dt_linkfix, s_table[0].dt);
  TEST_END("compose stop_on_last -> SL");
}

/**
 * @test ra8_eth_gwca_decode_ptr rejects a null descriptor.
 *
 * @par MC/DC:
 * Single condition `if (desc == nullptr)` in ::ra8_eth_gwca_decode_ptr:
 *   - V_T: desc = nullptr -> returns nullptr.
 * The V_F control (a non-null descriptor reconstructs its 40-bit PTR) is
 * exercised by the tx_frame and rx-drain paths in the other suites.
 */
static void test_decode_ptr_null(void)
{
  TEST_BEGIN("decode_ptr null guard");
  prep();
  TEST_ASSERT_NULL(ra8_eth_gwca_decode_ptr(nullptr));
  TEST_END("decode_ptr null guard");
}

/**
 * @test ra8_eth_gwca_tx_frame full enqueue happy path (host-round-trippable).
 *
 * @par MC/DC:
 * The tx_frame length guard `if (frame_len == 0U || frame_len >
 * slot_bytes)` is a compound decision fully vectored by the base suite's
 * test_tx_frame (the two true legs) -- here it evaluates false || false
 * (frame_len = 64, slot_bytes = 128), the control vector that lets
 * execution fall through into the enqueue body. The two single-condition
 * guards on the enqueue path both take their false branch on this happy
 * run: `if (err != k_ra8_ok)` (find_slot succeeded) and
 * `if (buf == nullptr)` (the slot has a backing buffer). Their true
 * branches are driven by test_tx_frame_queue_full and
 * test_tx_frame_unbacked_slot below.
 *
 * @details Places the TX pool at ::k_sram_tx_pool_addr inside the fake
 * SRAM window so the decoded destination pointer is valid, writable host
 * memory. Confirms the frame bytes actually land, DS is stamped to
 * frame_len, DT flips to FSINGLE, and the tail cursor advances.
 */
static void test_tx_frame_happy(void)
{
  TEST_BEGIN("tx_frame happy path");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_chain[4];
  uint8_t* const                                          pool = (uint8_t*)k_sram_tx_pool_addr;
  static uint8_t                                          s_frame[k_gwca_frame_bytes];
  uint32_t                                                tail = 0U;

  for (uint32_t i = 0U; i < k_gwca_frame_fill_len; ++i) {
    s_frame[i] = (uint8_t)(k_gwca_frame_seed ^ i);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_chain, 4U, 128U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_attach_buffers(s_chain, 4U, 128U, pool));

  /* Slot 0 is FEMPTY and backed by pool[0]; enqueue must succeed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_tx_frame(s_chain, 4U, &tail, s_frame, 64U, 128U));

  /* Frame bytes copied into the slot buffer. */
  for (uint32_t i = 0U; i < k_gwca_frame_fill_len; ++i) {
    TEST_ASSERT_EQ(s_frame[i], pool[i]);
  }
  /* DS stamped to frame_len, DT flipped to FSINGLE, tail advanced. */
  const uint32_t ds_actual = (uint32_t)s_chain[0].ds_l | ((uint32_t)s_chain[0].ds_h << 8U);
  TEST_ASSERT_EQ(64U, ds_actual);
  TEST_ASSERT_EQ(k_ra8_gwdcc_dt_fsingle, s_chain[0].dt);
  TEST_ASSERT_EQ(1U, tail);
  TEST_END("tx_frame happy path");
}

/**
 * @test ra8_eth_gwca_tx_frame propagates find_slot's queue-full return.
 *
 * @par MC/DC:
 * Single condition `if (err != k_ra8_ok)` on the tx_frame enqueue path:
 *   - V_T: every data slot pre-seeded FSINGLE so ::ra8_eth_gwca_find_slot
 *     finds no FEMPTY slot and returns k_ra8_err_no_data, which tx_frame
 *     forwards unchanged.
 * The length guard passes first (frame_len = 64 <= slot_bytes = 128) so
 * control reaches the find_slot call before this guard fires.
 */
static void test_tx_frame_queue_full(void)
{
  TEST_BEGIN("tx_frame queue full");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_chain[4];
  uint8_t* const                                          pool = (uint8_t*)k_sram_tx_pool_addr;
  static uint8_t                                          s_frame[k_gwca_frame_bytes];
  uint32_t                                                tail = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_chain, 4U, 128U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_attach_buffers(s_chain, 4U, 128U, pool));

  /* Mark all three data slots FSINGLE -> no FEMPTY slot remains. */
  s_chain[0].dt = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  s_chain[1].dt = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  s_chain[2].dt = (uint8_t)k_ra8_gwdcc_dt_fsingle;

  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_eth_gwca_tx_frame(s_chain, 4U, &tail, s_frame, 64U, 128U));
  TEST_END("tx_frame queue full");
}

/**
 * @test ra8_eth_gwca_tx_frame rejects a FEMPTY slot with no backing buffer.
 *
 * @par MC/DC:
 * Single condition `if (buf == nullptr)` on the tx_frame enqueue path:
 *   - V_T: the ring is init'd but attach_buffers is NOT called, so slot 0
 *     is FEMPTY with ptr_h/ptr_l still zero; ::ra8_eth_gwca_decode_ptr
 *     returns nullptr and tx_frame returns k_ra8_err_invalid_arg before
 *     any memcpy runs.
 * find_slot succeeds first (slot 0 is FEMPTY), so control reaches the
 * buffer-decode guard.
 */
static void test_tx_frame_unbacked_slot(void)
{
  TEST_BEGIN("tx_frame unbacked slot");
  prep();
  [[gnu::aligned(16)]] static ra8_gwca_basic_descriptor_t s_chain[4];
  static uint8_t                                          s_frame[k_gwca_frame_bytes];
  uint32_t                                                tail = 0U;

  /* init_ring leaves ptr_h/ptr_l at zero (buffers not yet attached). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gwca_init_ring(s_chain, 4U, 128U));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gwca_tx_frame(s_chain, 4U, &tail, s_frame, 64U, 128U));
  TEST_END("tx_frame unbacked slot");
}

int32_t main(void)
{
  test_compose_stop_on_last();
  test_decode_ptr_null();
  test_tx_frame_happy();
  test_tx_frame_queue_full();
  test_tx_frame_unbacked_slot();
  (void)fprintf(stderr, "[OK  ] test_ra8_eth_gwca_queue_cov.c\n");
  return 0;
}
