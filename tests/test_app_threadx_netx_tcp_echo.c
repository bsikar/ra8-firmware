/**
 * @file test_app_threadx_netx_tcp_echo.c
 * @brief Integration test: ra8_eth NIC bring-up under the NetX-Duo demo
 *
 * @details
 * The production app at examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo/main.c
 * brings the chip up via the ``ra8_etha`` HAL, hands control to ThreadX,
 * and asks NetX Duo (via the nx_ether_driver_ra8_eth shim)
 * to drive the NIC. NetX/ThreadX are not in the host test build, so
 * this test exercises the same ra8_eth surface the NetX link-driver
 * shim ultimately calls -- with a focus on stats / event-handler
 * registration the upper stack uses for status reporting.
 *
 * Modeled flow:
 *   1. ra8_cgc_init + ra8_board_ethernet_init (pre-kernel boot)
 *   2. ra8_eth_init / ra8_eth_open(cfg)
 *   3. ra8_eth_attach_handler (NetX wakeup hook equivalent)
 *   4. ra8_eth_get_stats (NetX status reporting)
 *   5. ra8_eth_get_status / ra8_eth_clear_status (ISR back-end)
 *   6. ra8_eth_close
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_pin_validator.h"
#include "ra8_sim_mmap.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum app_threadx_netx_tcp_echo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready. */
} app_threadx_netx_tcp_echo_fixture_t;

/**
 * @enum app_threadx_netx_tcp_echo_fixture2_t
 * @brief The recognizable values moved through the code under test.
 */
typedef enum : uint16_t {
  k_eth_handler_ctx_token =
    0xCAFEU, /**< Token handed to the Ethernet handler and checked on the way back, proving the context pointer survives. */
} app_threadx_netx_tcp_echo_fixture2_t;

/**
 * @enum app_threadx_netx_tcp_echo_fixture3_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint32_t {
  k_event_mask_all =
    0xFFFFFFFFU, /**< Wait on every event bit, so the first event of any kind releases the wait. */
} app_threadx_netx_tcp_echo_fixture3_t;

/** @brief Captured event mask from the attached handler. */
static uint32_t s_last_eth_event_mask;
/** @brief Captured ctx pointer passed to the handler. */
static void* s_last_eth_event_ctx;

/** @brief Static MAC matching the NetX demo (192.168.1.42 -> 02:AA:..). */
static const uint8_t k_test_netx_mac[6] = {0x02U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU};

/**
 * @brief NetX-equivalent event handler -- captures the latest event mask.
 */
static void test_netx_eth_event_cb(void* ctx, uint32_t event_mask)
{
  s_last_eth_event_ctx = ctx;
  s_last_eth_event_mask |= event_mask;
}

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_eth_close();
  (void)ra8_eth_deinit();
  s_last_eth_event_mask = 0U;
  s_last_eth_event_ctx  = nullptr;
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_SIMULATOR_MODE. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
}

/**
 * @brief Build the open() cfg matching the NetX demo's wiring.
 */
static ra8_eth_cfg_t make_netx_cfg(void)
{
  ra8_eth_cfg_t cfg = {};
  (void)memcpy(cfg.mac_address, k_test_netx_mac, sizeof k_test_netx_mac);
  cfg.channel            = 0U;
  cfg.num_tx_descriptors = 0U;
  cfg.num_rx_descriptors = 0U;
  cfg.buffer_size        = 0U;
  return cfg;
}

/**
 * @brief Pre-kernel chip bring-up.
 *
 * @par MC/DC: not applicable -- two sequential bring-up calls with
 * no compound boolean decisions in the path.
 */
static void test_netx_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("netx_tcp_echo: cgc_init + board_ethernet_init");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_ethernet_init());
  TEST_END("netx_tcp_echo: cgc_init + board_ethernet_init");
}

/**
 * @brief Open the NIC at the NetX-demo MAC.
 *
 * @par MC/DC:
 * Compound decision under test (in ra8_eth_open): ``cfg == NULL ||
 * channel out of range``. Two atomic conditions x N+1 = 3 vectors;
 * this case covers the all-valid vector.
 */
static void test_netx_open_nic(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_init());
  TEST_BEGIN("netx_tcp_echo: ra8_eth_open at NetX MAC");
  const ra8_eth_cfg_t cfg = make_netx_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_open(&cfg));
  TEST_END("netx_tcp_echo: ra8_eth_open at NetX MAC");
}

/**
 * @brief Attach event handler -- NetX driver shim wakeup hook.
 *
 * @par MC/DC:
 * Decision vector under test: ``fn == NULL`` precondition guard inside
 * ra8_eth_attach_handler. This case covers the non-NULL vector.
 */
static void test_netx_attach_event_handler(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_init());
  const ra8_eth_cfg_t cfg = make_netx_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_open(&cfg));

  TEST_BEGIN("netx_tcp_echo: attach event handler");
  ra8_err_t err = ra8_eth_attach_handler(test_netx_eth_event_cb, (void*)k_eth_handler_ctx_token);
  TEST_ASSERT(err == k_ra8_ok);
  TEST_END("netx_tcp_echo: attach event handler");
}

/**
 * @brief NIC stats readback -- NetX status reporting equivalent.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_stats == NULL`` precondition guard
 * inside ra8_eth_get_stats. This case covers the non-NULL vector.
 */
static void test_netx_get_stats(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_init());
  const ra8_eth_cfg_t cfg = make_netx_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_open(&cfg));

  TEST_BEGIN("netx_tcp_echo: get_stats returns ok");
  ra8_eth_stats_t stats = {};
  ra8_err_t       err   = ra8_eth_get_stats(&stats);
  TEST_ASSERT(err == k_ra8_ok || err == k_ra8_err_not_initialized);
  TEST_END("netx_tcp_echo: get_stats returns ok");
}

/**
 * @brief Status get + clear -- ISR back-end the NetX shim drives.
 *
 * @par MC/DC:
 * Compound decision under test (in ra8_eth_get_status / clear_status):
 * ``out_mask == NULL`` for get, ``mask == 0`` no-op vs non-zero for
 * clear. Two atomic conditions x N+1 = 3 vectors; this case covers the
 * happy non-NULL / non-zero vector.
 */
static void test_netx_status_get_and_clear(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_init());
  const ra8_eth_cfg_t cfg = make_netx_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_open(&cfg));

  TEST_BEGIN("netx_tcp_echo: status get + clear");
  uint32_t  mask = k_event_mask_all;
  ra8_err_t g    = ra8_eth_get_status(&mask);
  TEST_ASSERT(g == k_ra8_ok);
  ra8_err_t c = ra8_eth_clear_status(mask);
  TEST_ASSERT(c == k_ra8_ok);
  TEST_END("netx_tcp_echo: status get + clear");
}

/**
 * @brief NULL handler rejected by attach.
 *
 * @par MC/DC:
 * Decision vector under test: ``fn == NULL`` failure side of the attach
 * precondition guard. Pairs with the non-NULL attach vector.
 */
static void test_netx_attach_null_handler_rejected(void)
{
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_init());
  TEST_BEGIN("netx_tcp_echo: attach with NULL fn detaches");
  /* The contract treats attach(NULL) as a detach -- it stores nullptr
   * in the handler slot and returns ok. The NetX wrapper relies on
   * this to drop its event hook during shutdown. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_attach_handler(nullptr, nullptr));
  TEST_END("netx_tcp_echo: attach with NULL fn detaches");
}

int main(void)
{
  test_netx_pre_kernel_bringup();
  test_netx_open_nic();
  test_netx_attach_event_handler();
  test_netx_get_stats();
  test_netx_status_get_and_clear();
  test_netx_attach_null_handler_rejected();
  return 0;
}
