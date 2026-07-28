/**
 * @file test_ra8_mipi_dsi_dispatch_cov.c
 * @brief Coverage-focused host unit tests for ra8_mipi_dsi_dispatch.c
 *
 * @details
 * Companion to `test_ra8_mipi_dsi.c`. That file exercises the public
 * contract; this file drives the residual uncovered lines of
 * `ra8_mipi_dsi_dispatch.c` deterministically by pre-seeding the
 * fake MIPI DSI-2 register block through `ra8_fake_mmap`:
 *
 *  - `ra8_mipi_dsi_video_start` with the HBP/HFP "no-LP" flags set
 *    (the sibling test only sets HSA);
 *  - `ra8_mipi_dsi_clear_status` for the sequence-channel-1, receive,
 *    fatal-error, and PHY-lane clear branches;
 *  - `ra8_mipi_dsi_rx_result_get` for receive-result slots 1, 2, and 3;
 *  - `ra8_mipi_dsi_dispatch_video` buffer over/underflow soft-reset leg;
 *  - the top-level `ra8_mipi_dsi_dispatch` fan-out for every interrupt
 *    class plus the legacy "nothing pending" always-invoke leg.
 *
 * Every register the driver reads is ordinary host RAM here, so there
 * is no hardware-only path among the targeted lines and no coverage
 * exclusion is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ra8_mipi_dsi_cov_const_t
 * @brief Numeric inputs used by the coverage cases (no magic numbers).
 *
 * @details
 * Tests are exempt from the magic-number gate, but these are still
 * named for readability. Slot payloads use a distinct data0 byte per
 * slot so each RXRSSnR read can be verified independently.
 */
typedef enum : uint32_t {
  k_cov_slot1_data0     = 0xA1U,        /**< data0 byte seeded into RXRSS1R. */
  k_cov_slot1_data1     = 0x22U,        /**< data1 byte seeded into RXRSS1R. */
  k_cov_slot2_data0     = 0xB2U,        /**< data0 byte seeded into RXRSS2R. */
  k_cov_slot2_data1     = 0x33U,        /**< data1 byte seeded into RXRSS2R. */
  k_cov_slot3_data0     = 0xC3U,        /**< data0 byte seeded into RXRSS3R. */
  k_cov_slot3_data1     = 0x44U,        /**< data1 byte seeded into RXRSS3R. */
  k_cov_slot_dt         = 0x06U,        /**< DSI data-type field (DCS read). */
  k_cov_rstcr_seed      = 0xFFFFFFFFUL, /**< Sentinel pre-seed of RSTCR.     */
  k_cov_all_isr         = (uint32_t)k_ra8_mipi_dsi_isr_sq0 | (uint32_t)k_ra8_mipi_dsi_isr_sq1 |
                          (uint32_t)k_ra8_mipi_dsi_isr_vm | (uint32_t)k_ra8_mipi_dsi_isr_rcv |
                          (uint32_t)k_ra8_mipi_dsi_isr_ferr |
                          (uint32_t)k_ra8_mipi_dsi_isr_ppi, /**< Cov all ISR. */
  k_cov_expected_fanout = 6U, /**< One user callback per interrupt class. */
} ra8_mipi_dsi_cov_const_t;

/**
 * @var s_cov_cb_count
 * @brief Number of times the stub callback has fired since the last reset.
 */
static uint32_t s_cov_cb_count;

/**
 * @var s_cov_cb_last_event
 * @brief Class enum captured by the most recent stub callback.
 */
static ra8_mipi_dsi_event_t s_cov_cb_last_event;

/**
 * @var s_cov_cb_last_mask
 * @brief Status mask captured by the most recent stub callback.
 */
static uint32_t s_cov_cb_last_mask;

/**
 * @brief Test callback that records the last dispatch and counts fan-out.
 *
 * @param[in] ctx         Unused opaque context.
 * @param[in] event       Interrupt-class enum reported by the driver.
 * @param[in] status_mask Status bits captured before clearing.
 */
static void stub_cov_cb(void* ctx, ra8_mipi_dsi_event_t event, uint32_t status_mask)
{
  (void)ctx;
  ++s_cov_cb_count;
  s_cov_cb_last_event = event;
  s_cov_cb_last_mask  = status_mask;
}

/**
 * @brief Reset the fake peripheral memory + MSTP table + counters.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_cov_cb_count      = 0U;
  s_cov_cb_last_event = k_ra8_mipi_dsi_event_phy;
  s_cov_cb_last_mask  = 0U;
}

/**
 * @brief Build a video config with all three "no-LP" flags asserted.
 *
 * @return A fully-populated ::ra8_mipi_dsi_video_cfg_t.
 */
static ra8_mipi_dsi_video_cfg_t make_video_cfg_all_no_lp(void)
{
  const ra8_mipi_dsi_video_cfg_t v = {
    .pixel_format             = k_ra8_mipi_dsi_dt_pixel_rgb888,
    .virtual_channel          = k_ra8_mipi_dsi_vc0,
    .sync_pulse               = false,
    .hsa_no_lp                = true,
    .hbp_no_lp                = true,
    .hfp_no_lp                = true,
    .vsync_active_high        = true,
    .hsync_active_high        = true,
    .vertical_sync_lines      = 4U,
    .vertical_active_lines    = 600U,
    .vertical_back_porch      = 8U,
    .vertical_front_porch     = 6U,
    .horizontal_sync_lines    = 12U,
    .horizontal_active_pixels = 1024U,
    .horizontal_back_porch    = 64U,
    .horizontal_front_porch   = 32U,
    .video_mode_delay         = 0U,
  };
  return v;
}

/**
 * @test test_cov_video_start_hbp_hfp_no_lp
 * @par MC/DC:
 * (no compound decision -- `ra8_mipi_dsi_video_start` uses three
 * independent single-condition `if` guards. This case sets all three
 * flags true so the HBP and HFP "no-LP" bodies both execute; the
 * sibling test covers the HSA body and the false legs.)
 */
static void test_cov_video_start_hbp_hfp_no_lp(void)
{
  TEST_BEGIN("mipi_dsi cov video_start hbp/hfp no-LP");
  prep();

  const ra8_mipi_dsi_video_cfg_t v = make_video_cfg_all_no_lp();

  /* Pre-seed VMSR.VIRDY so the bounded poll returns success at once. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->VMSR                       = (uint32_t)k_ra8_mipi_dsi_vmsr_virdy;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_start(&v));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_vstart) != 0U));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_hsanolp) != 0U));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_hbpnolp) != 0U));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_hfpnolp) != 0U));

  TEST_END("mipi_dsi cov video_start hbp/hfp no-LP");
}

/**
 * @test test_cov_clear_status_all_classes
 * @par MC/DC:
 * `ra8_mipi_dsi_clear_status` is a chain of six independent
 * single-condition `if (mask & bit)` guards (no `&&`/`||`). Passing a
 * mask with every interrupt-class bit set drives the true leg of each
 * guard, including the SQ1 / receive / fatal / PHY clears that the
 * sibling test's SQ0+VM-only seed leaves untouched.
 */
static void test_cov_clear_status_all_classes(void)
{
  TEST_BEGIN("mipi_dsi cov clear_status all classes");
  prep();

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_clear_status((uint32_t)k_cov_all_isr));

  /* Each RW1C status-clear register received its clear-all mask. */
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_sqch_clear_all, reg->SQCH1SCR);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_rxsr_clear_all, reg->RXSCR);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_ferrsr_clear_all, reg->FERRSCR);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_plsr_clear_all, reg->PLSCR);

  TEST_END("mipi_dsi cov clear_status all classes");
}

/**
 * @test test_cov_rx_result_slots_1_2_3
 * @par MC/DC:
 * (no compound decision -- exercises the `switch (slot)` register
 * selector for slots 1, 2, and 3. Each slot pre-seeds its own
 * RXRSSR valid bit and RXRSSnR word so the case that reads that
 * register is taken and its decoded data0 byte verified.)
 */
static void test_cov_rx_result_slots_1_2_3(void)
{
  TEST_BEGIN("mipi_dsi cov rx_result slots 1/2/3");
  prep();

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  ra8_mipi_dsi_rx_result_t    r   = {};

  /* Slot 1 -> RXRSS1R. */
  reg->RXRSSR  = (uint32_t)k_ra8_mipi_dsi_rxrssr_slt1vld;
  reg->RXRSS1R = (uint32_t)k_cov_slot1_data0 | ((uint32_t)k_cov_slot1_data1 << 8) |
                 ((uint32_t)k_cov_slot_dt << 16) | (uint32_t)k_ra8_mipi_dsi_rxrss_rxsuc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_rx_result_get(1U, &r));
  TEST_ASSERT_EQ(k_cov_slot1_data0, r.data[0]);
  TEST_ASSERT_EQ(k_cov_slot1_data1, r.data[1]);
  TEST_ASSERT(r.rx_success);

  /* Slot 2 -> RXRSS2R. */
  memset((void*)&r, 0, sizeof(r));
  reg->RXRSSR  = (uint32_t)k_ra8_mipi_dsi_rxrssr_slt2vld;
  reg->RXRSS2R = (uint32_t)k_cov_slot2_data0 | ((uint32_t)k_cov_slot2_data1 << 8) |
                 ((uint32_t)k_cov_slot_dt << 16) | (uint32_t)k_ra8_mipi_dsi_rxrss_rxsuc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_rx_result_get(2U, &r));
  TEST_ASSERT_EQ(k_cov_slot2_data0, r.data[0]);
  TEST_ASSERT_EQ(k_cov_slot2_data1, r.data[1]);

  /* Slot 3 -> RXRSS3R (the default arm of the switch). */
  memset((void*)&r, 0, sizeof(r));
  reg->RXRSSR  = (uint32_t)k_ra8_mipi_dsi_rxrssr_slt3vld;
  reg->RXRSS3R = (uint32_t)k_cov_slot3_data0 | ((uint32_t)k_cov_slot3_data1 << 8) |
                 ((uint32_t)k_cov_slot_dt << 16) | (uint32_t)k_ra8_mipi_dsi_rxrss_rxsuc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_rx_result_get(3U, &r));
  TEST_ASSERT_EQ(k_cov_slot3_data0, r.data[0]);
  TEST_ASSERT_EQ(k_cov_slot3_data1, r.data[1]);

  TEST_END("mipi_dsi cov rx_result slots 1/2/3");
}

/**
 * @test test_cov_dispatch_video_buffer_reset
 * @par MC/DC:
 * Decision `(bits & (VBUFOVF | VBUFUDF)) != 0U` is a single relational
 * condition on a fixed bitmask (the `|` forms the mask; it is not a
 * logical operator), so N+1 = 2 vectors suffice:
 *  - V1: VMSR has VBUFOVF set -> condition true  -> soft-reset leg runs.
 *  - V2: VMSR has neither bit -> condition false -> reset leg skipped.
 * A sentinel pre-seed of RSTCR distinguishes the two: the true leg
 * writes RSTCR (swrst then 0), the false leg leaves the sentinel.
 * A third vector with VBUFUDF confirms the other mask bit also fires.
 */
static void test_cov_dispatch_video_buffer_reset(void)
{
  TEST_BEGIN("mipi_dsi cov dispatch_video buffer reset");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(stub_cov_cb, nullptr));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* V1: overflow -> soft-reset leg executes, RSTCR ends at 0. */
  reg->RSTCR = (uint32_t)k_cov_rstcr_seed;
  reg->VMSR  = (uint32_t)k_ra8_mipi_dsi_vmsr_vbufovf;
  ra8_mipi_dsi_dispatch_video();
  TEST_ASSERT_EQ(0U, reg->RSTCR);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_video, s_cov_cb_last_event);

  /* V3: underflow -> the other mask bit also drives the reset leg. */
  reg->RSTCR = (uint32_t)k_cov_rstcr_seed;
  reg->VMSR  = (uint32_t)k_ra8_mipi_dsi_vmsr_vbufudf;
  ra8_mipi_dsi_dispatch_video();
  TEST_ASSERT_EQ(0U, reg->RSTCR);

  /* V2: neither bit -> reset leg skipped, sentinel survives. */
  reg->RSTCR = (uint32_t)k_cov_rstcr_seed;
  reg->VMSR  = 0U;
  ra8_mipi_dsi_dispatch_video();
  TEST_ASSERT_EQ(k_cov_rstcr_seed, reg->RSTCR);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(nullptr, nullptr));
  TEST_END("mipi_dsi cov dispatch_video buffer reset");
}

/**
 * @test test_cov_dispatch_fanout_all_and_none
 * @par MC/DC:
 * `ra8_mipi_dsi_dispatch` is a chain of seven independent
 * single-condition `if (snapshot & bit)` guards (no `&&`/`||`).
 *  - V1: ISR = every interrupt-class bit -> all six per-class dispatch
 *    branches run (one user callback each) and the "nothing pending"
 *    guard is false.
 *  - V2: ISR = 0 -> every per-class guard is false and the legacy
 *    always-invoke guard `(snapshot & isr_all) == 0` fires once.
 */
static void test_cov_dispatch_fanout_all_and_none(void)
{
  TEST_BEGIN("mipi_dsi cov dispatch fan-out all + none");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(stub_cov_cb, nullptr));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* V1: every class pending -> six per-class dispatches fire. VMSR is
   * left clean so dispatch_video takes its non-reset path here. */
  reg->ISR = (uint32_t)k_cov_all_isr;
  ra8_mipi_dsi_dispatch();
  TEST_ASSERT_EQ(k_cov_expected_fanout, s_cov_cb_count);

  /* V2: nothing pending -> single legacy always-invoke with mask 0. */
  s_cov_cb_count = 0U;
  reg->ISR       = 0U;
  ra8_mipi_dsi_dispatch();
  TEST_ASSERT_EQ(1U, s_cov_cb_count);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_phy, s_cov_cb_last_event);
  TEST_ASSERT_EQ(0U, s_cov_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(nullptr, nullptr));
  TEST_END("mipi_dsi cov dispatch fan-out all + none");
}

int32_t main(void)
{
  test_cov_video_start_hbp_hfp_no_lp();
  test_cov_clear_status_all_classes();
  test_cov_rx_result_slots_1_2_3();
  test_cov_dispatch_video_buffer_reset();
  test_cov_dispatch_fanout_all_and_none();
  (void)fprintf(stderr, "[OK  ] test_ra8_mipi_dsi_dispatch_cov.c\n");
  return 0;
}
