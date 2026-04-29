/**
 * @file test_ra_eth.c
 * @brief Unit tests for ra_eth.c (Ethernet Switch Module + NIC API)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_mstp.h"
#include "ra_rmac.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_eth_test_t
 * @brief Driver-private constants used by the tests.
 */
typedef enum : uint32_t {
  k_ra_eth_test_tact        = 0x80000000UL, /**< TX descriptor active bit. */
  k_ra_eth_test_ract        = 0x80000000UL, /**< RX descriptor active bit. */
  k_ra_eth_test_gwca_run    = 0x00000003UL, /**< GWCA EDTR | EDRR.         */
  k_ra_eth_test_mmis1_pracs = 0x00000004UL, /**< MMIS1.PRACS.              */
  k_ra_eth_test_pkt_size    = 64U,          /**< Test frame size (60..).   */
  k_ra_eth_test_short_size  = 32U,          /**< Below min-frame.          */
} ra_eth_test_t;

static uint32_t s_eth_cb_count;
static uint32_t s_eth_cb_last_mask;

static void stub_eth_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_eth_cb_count;
  s_eth_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_eth_cb_count     = 0U;
  s_eth_cb_last_mask = 0U;
}

/* --- Lifecycle / status (existing scaffold) --- */

static void test_init(void)
{
  TEST_BEGIN("eth init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_END("eth init");
}

static void test_deinit(void)
{
  TEST_BEGIN("eth deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_deinit());
  TEST_END("eth deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("eth status read + clear");
  prep();
  ra_eswm()->ESWM_STS = 0xDEADBEEFU;
  uint32_t mask       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_get_status(nullptr));
  TEST_END("eth status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("eth attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_eth_attach_handler(stub_eth_cb, (void*)(uintptr_t)0xE0U));
  ra_eswm()->ESWM_STS = 0xCAFEU;
  ra_eth_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_eth_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_eth_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_attach_handler(nullptr, nullptr));
  ra_eswm()->ESWM_STS = 0xBABEU;
  ra_eth_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_eth_cb_count);
  TEST_END("eth attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("eth power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_exit_stop());
  TEST_END("eth power transition");
}

/* --- NIC API (new for Sweep 2 / Task 1) --- */

static const ra_eth_cfg_t s_test_cfg = {
  .mac_address        = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U},
  .channel            = 0U,
  .num_tx_descriptors = 0U,
  .num_rx_descriptors = 0U,
  .buffer_size        = 0U,
};

static void test_open_null_rejected(void)
{
  TEST_BEGIN("eth open null rejection");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_open(nullptr));
  TEST_END("eth open null rejection");
}

static void test_open_bad_channel(void)
{
  TEST_BEGIN("eth open bad channel");
  prep();
  ra_eth_cfg_t bad = s_test_cfg;
  bad.channel      = 7U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_eth_open(&bad));
  TEST_END("eth open bad channel");
}

static void test_open_happy_path(void)
{
  TEST_BEGIN("eth open happy path");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));
  /* GWCA control bits asserted (EDTR | EDRR). HUM Ch 34 p 1787 */
  TEST_ASSERT_EQ((int32_t)k_ra_eth_test_gwca_run, (int32_t)ra_gwca()->GWCA_CTRL);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth open happy path");
}

static void test_close_without_open(void)
{
  TEST_BEGIN("eth close without open");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_eth_close());
  TEST_END("eth close without open");
}

static void test_write_null_rejected(void)
{
  TEST_BEGIN("eth write null rejection");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_write(nullptr, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth write null rejection");
}

static void test_write_bad_length(void)
{
  TEST_BEGIN("eth write bad length");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));
  uint8_t pkt[64];
  (void)memset(pkt, 0xA5, sizeof(pkt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_eth_write(pkt, (uint32_t)k_ra_eth_test_short_size));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_eth_write(pkt, 9999U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth write bad length");
}

static void test_write_enqueues_and_advances(void)
{
  TEST_BEGIN("eth write enqueues + advances");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));

  uint8_t pkt[64];
  for (uint16_t i = 0U; i < (uint16_t)sizeof(pkt); ++i) {
    pkt[i] = (uint8_t)i;
  }

  /* GWCA_CTRL was set on open; clear it to confirm write re-asserts EDTR. */
  ra_gwca()->GWCA_CTRL = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));
  TEST_ASSERT((ra_gwca()->GWCA_CTRL & 0x1U) != 0U);

  /* Stats should reflect one tx_ok. */
  ra_eth_stats_t stats = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_stats(&stats));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)stats.tx_ok);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth write enqueues + advances");
}

static void test_write_busy_when_full(void)
{
  TEST_BEGIN("eth write busy when ring full");
  prep();
  /* Use a smaller TX ring to make ring-full quicker. */
  ra_eth_cfg_t cfg       = s_test_cfg;
  cfg.num_tx_descriptors = 2U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&cfg));

  uint8_t pkt[64];
  (void)memset(pkt, 0x5A, sizeof(pkt));

  /* Fill the ring. Each write leaves a TACT-set descriptor behind. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));
  /* Third write should land back on slot 0 which is still hardware-owned. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));

  ra_eth_stats_t stats = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_stats(&stats));
  TEST_ASSERT_EQ((int32_t)2, (int32_t)stats.tx_ok);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)stats.tx_err);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth write busy when ring full");
}

static void test_read_no_data(void)
{
  TEST_BEGIN("eth read no data");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));
  uint8_t  buf[128];
  uint32_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)got);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth read no data");
}

static void test_read_null_rejected(void)
{
  TEST_BEGIN("eth read null rejection");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));
  uint8_t  buf[16];
  uint32_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_eth_read(nullptr, (uint32_t)sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_eth_read(buf, 0U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth read null rejection");
}

static void test_read_returns_frame(void)
{
  TEST_BEGIN("eth read returns frame");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));

  uint8_t expected[64];
  for (uint16_t i = 0U; i < (uint16_t)sizeof(expected); ++i) {
    expected[i] = (uint8_t)(0xC0U + (i & 0x0FU));
  }
  /* Test-only hook: pretend the EDMAC engine released a frame on the
   * head RX descriptor. The hook drops the bytes into the buffer and
   * clears RACT so the next ra_eth_read pops it. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_eth_test_inject_rx(expected, (uint32_t)sizeof(expected)));

  uint8_t  buf[128];
  uint32_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)sizeof(expected), (int32_t)got);
  TEST_ASSERT_EQ(0, memcmp(buf, expected, sizeof(expected)));

  ra_eth_stats_t stats = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_stats(&stats));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)stats.rx_ok);

  /* A second read with no frame staged returns no_data. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), &got));

  /* test_inject_rx null-arg rejection. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_test_inject_rx(nullptr, 8U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth read returns frame");
}

static void test_link_status_via_mdio(void)
{
  TEST_BEGIN("eth link status via mdio");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));

  /* The MDIO read polls MMIS1.PRACS; pre-arm it so the first read
   * (BMSR) completes. The driver does two MDIO reads in a row -- arm
   * PRACS twice (the driver's wait clears it via MMID1 each time). */
  ra_rmac(k_ra_rmac_port_0)->MMIS1 = (uint32_t)k_ra_eth_test_mmis1_pracs;
  ra_rmac(k_ra_rmac_port_0)->MPSM  = 0U; /* PRD = 0 -> link not up */
  /* Stage second arm via a sticky write so the second wait sees it. */
  ra_eth_link_t out = {};
  /* Issue the link query -- it does two MDIO transactions. The
   * simulator strips MMIS1.PRACS on the first read; we re-arm via
   * a peek. Use a test path where MMIS1 stays set. */
  ra_rmac(k_ra_rmac_port_0)->MMIS1 = (uint32_t)k_ra_eth_test_mmis1_pracs | (uint32_t)0x40000000UL;
  /* Arm twice: simulate both reads completing. */
  ra_rmac(k_ra_rmac_port_0)->MMIS1 = 0xFFFFFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_link_status(&out));
  /* link_up reflects whatever PRD was; default zero -> 0. */

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_link_status(nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth link status via mdio");
}

static void test_get_stats_after_io(void)
{
  TEST_BEGIN("eth get_stats after writes + reads");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_open(&s_test_cfg));

  uint8_t pkt[64];
  (void)memset(pkt, 0x77, sizeof(pkt));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_write(pkt, (uint32_t)sizeof(pkt)));

  uint8_t  buf[128];
  uint32_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), &got));

  ra_eth_stats_t stats = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_stats(&stats));
  TEST_ASSERT_EQ((int32_t)2, (int32_t)stats.tx_ok);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)stats.rx_ok);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)stats.tx_err);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)stats.rx_err);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_get_stats(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_close());
  TEST_END("eth get_stats after writes + reads");
}

static void test_apis_require_open(void)
{
  TEST_BEGIN("eth apis require open");
  prep();
  uint8_t        buf[64];
  uint32_t       got   = 0U;
  ra_eth_link_t  link  = {};
  ra_eth_stats_t stats = {};
  /* Without ra_eth_open: every NIC API should reject with not_initialized. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_eth_write(buf, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_eth_read(buf, (uint32_t)sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_eth_link_status(&link));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_eth_get_stats(&stats));
  TEST_END("eth apis require open");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();

  test_open_null_rejected();
  test_open_bad_channel();
  test_open_happy_path();
  test_close_without_open();

  test_write_null_rejected();
  test_write_bad_length();
  test_write_enqueues_and_advances();
  test_write_busy_when_full();

  test_read_no_data();
  test_read_null_rejected();
  test_read_returns_frame();

  test_link_status_via_mdio();
  test_get_stats_after_io();
  test_apis_require_open();

  (void)fprintf(stderr, "[OK  ] test_ra_eth.c\n");
  return 0;
}
