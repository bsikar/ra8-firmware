/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file test_ra8_wifi_c6link.c
 * @brief The ra8_wifi facade over the real ESP32-C6 backend and ra8_c6link.
 *
 * @details
 * The end-to-end host test: ::k_ra8_wifi_backend_c6link driving the whole
 * ra8_c6link stack -- payload header, checksum, TLV envelope, protobuf, the
 * transaction pump -- answered by tests/mocks/ra8_c6_model.c, the same
 * co-processor model the c6link tests use. Nothing is stubbed: a station
 * association is exercised by decoding a real `Event_StaConnected` frame the
 * model builds with the codec the ESP32-C6 runs.
 *
 * @par Working to the model's frame budget
 * The co-processor model holds at most ::k_c6m_queue frames for the host over
 * one reset (a deliberately small, non-wrapping buffer), and a full station
 * bring-up is more RPC responses than that. So this test does what the model is
 * built for: short, reset-bounded sequences. The individual backend operations
 * are driven directly through the vtable in budget-sized groups, and the facade
 * lifecycle is exercised by reaching association through ::ra8_wifi_poll (the
 * way an application observes an asynchronous join) rather than by blocking
 * ::ra8_wifi_connect, whose full six-RPC sequence the model cannot answer in one
 * reset. The blocking-connect path -- every branch of it -- is covered against a
 * mock backend in test_ra8_wifi.c.
 *
 * The IP provider is a canned lease: NetX Duo is not present in a host build, so
 * the seam the facade calls is a local function, exactly as the design intends.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "esp_hosted_rpc.pb-c.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_c6link_wifi.h"
#include "ra8_err.h"
#include "ra8_wifi.h"
#include "ra8_wifi_backend.h"
#include "ra8_wifi_c6link.h"
#include "unity_minimal.h"

/**
 * @enum t_c6_const_t
 * @brief Sizes and canned values this test owns.
 */
typedef enum : uint32_t {
  k_t_arena   = 4096U,       /**< Decode arena handed to the link.      */
  k_t_ip      = 0xC0A80164U, /**< 192.168.1.100, the canned lease.      */
  k_t_gw      = 0xC0A80101U, /**< 192.168.1.1 gateway.                  */
  k_t_channel = 6U,          /**< AP primary channel the model reports. */
} t_c6_const_t;

/** @brief The link handle the backend opens. */
static ra8_c6link_t s_link;
/** @brief The ESP32-C6 backend context. */
static ra8_wifi_c6link_t s_c6;
/** @brief The facade handle under test. */
static ra8_wifi_t s_wifi;
/** @brief Decode arena handed to the link. */
static uint8_t s_arena[(size_t)k_t_arena];
/** @brief What the canned IP provider returns. */
static ra8_wifi_lease_t s_lease;
/** @brief Result the canned IP provider reports. */
static ra8_err_t s_ip_ret;
/** @brief Times the IP provider was called. */
static int s_ip_calls;

/** @brief Canned IP provider: no NetX in a host build, so a local lease. */
static ra8_err_t t_ip_bind(void* ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  (void)ctx;
  (void)mac;
  s_ip_calls++;
  if (s_ip_ret != k_ra8_ok) {
    return s_ip_ret;
  }
  *out = s_lease;
  return k_ra8_ok;
}

/** @brief Fill a backend configuration bound to the co-processor model. */
static ra8_wifi_c6link_cfg_t t_bcfg(void)
{
  ra8_wifi_c6link_cfg_t bcfg = {};
  bcfg.link                  = &s_link;
  bcfg.arena                 = s_arena;
  bcfg.arena_bytes           = (uint32_t)k_t_arena;
  bcfg.rx_cb                 = nullptr;
  ra8_c6_model_bind(&bcfg.transport);
  return bcfg;
}

/*
 * Reset the model and bring the facade up over the c6 backend. Suppresses the
 * one-shot boot event so init spends only one queued frame on its fw probe,
 * leaving the rest of the model's small frame budget for the operation under
 * test.
 */
static ra8_err_t t_up(void)
{
  ra8_c6_model_reset();
  ra8_c6_model()->silent_boot = true;
  s_ip_calls                  = 0;
  s_ip_ret                    = k_ra8_ok;
  s_lease                     = (ra8_wifi_lease_t){};
  s_lease.ip                  = (uint32_t)k_t_ip;
  s_lease.gateway             = (uint32_t)k_t_gw;
  s_link                      = (ra8_c6link_t){};
  ra8_wifi_c6link_cfg_t bcfg  = t_bcfg();

  ra8_wifi_cfg_t  cfg   = {};
  const ra8_err_t setup = ra8_wifi_c6link_setup(&s_c6, &bcfg, &cfg);
  if (setup != k_ra8_ok) {
    return setup;
  }
  cfg.ip_bind = t_ip_bind;
  cfg.ip_ctx  = &s_link;
  return ra8_wifi_init(&s_wifi, &cfg);
}

/* --- setup validation ---------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_setup_validation(void)
{
  TEST_BEGIN("wifi-c6 setup validation");
  ra8_wifi_c6link_t     self = {};
  ra8_wifi_c6link_cfg_t bcfg = t_bcfg();
  ra8_wifi_cfg_t        cfg  = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_c6link_setup(nullptr, &bcfg, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_c6link_setup(&self, nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_c6link_setup(&self, &bcfg, nullptr));

  ra8_wifi_c6link_cfg_t no_link = bcfg;
  no_link.link                  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_c6link_setup(&self, &no_link, &cfg));

  ra8_wifi_c6link_cfg_t tiny = bcfg;
  tiny.arena_bytes           = (uint32_t)k_ra8_c6link_arena_min - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_wifi_c6link_setup(&self, &tiny, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_c6link_setup(&self, &bcfg, &cfg));
  TEST_ASSERT(cfg.backend == &k_ra8_wifi_backend_c6link);
  TEST_ASSERT(cfg.backend_ctx == &self);
  TEST_END("wifi-c6 setup validation");
}

/* --- init open failure --------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_open_fail(void)
{
  TEST_BEGIN("wifi-c6 init open failure");
  ra8_c6_model_reset();
  s_link                     = (ra8_c6link_t){};
  ra8_wifi_c6link_cfg_t bcfg = t_bcfg(); /* binds the model transport            */
  ra8_c6_model()->handshake  = false;    /* the co-processor never arms the line */

  ra8_wifi_cfg_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_c6link_setup(&s_c6, &bcfg, &cfg));
  cfg.ip_bind = t_ip_bind;
  /* open -> ra8_c6link_await_ready never sees HANDSHAKE -> hw_timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_wifi_init(&s_wifi, &cfg));
  TEST_END("wifi-c6 init open failure");
}

/* --- backend rows called directly, for their guards --------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_backend_direct_guards(void)
{
  TEST_BEGIN("wifi-c6 backend guards");
  const ra8_wifi_backend_t* b = &k_ra8_wifi_backend_c6link;

  /* Null context is rejected by every row. */
  ra8_wifi_mac_t  mac  = {};
  ra8_wifi_ap_t   ap   = {};
  ra8_wifi_link_t link = k_ra8_wifi_link_down;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->open(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->radio_up(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->radio_down(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->join(nullptr, "x", "y"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->leave(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->service(nullptr, &link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->get_mac(nullptr, &mac));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->get_ap(nullptr, &ap));

  /* A context with no link is rejected by the rows that dereference it. */
  ra8_wifi_c6link_t no_link = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->open(&no_link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->close(&no_link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->radio_up(&no_link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->radio_down(&no_link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->leave(&no_link));

  /* A valid context but null out-parameters, each rejected before any wire. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->join(&s_c6, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->service(&s_c6, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->get_mac(&s_c6, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, b->get_ap(&s_c6, nullptr));

  /* Opening an already-open link is refused by ra8_c6link, surfaced by open. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, b->open(&s_c6));

  /* The pacing row returns nothing, so its guards are proved by it declining to
   * reach a transport that is not there: neither call may fault, and neither
   * may reach the model's clock. */
  const uint32_t before = ra8_c6_model()->delays;
  b->idle(nullptr, 1U);
  ra8_wifi_c6link_t no_transport = {};
  b->idle(&no_transport, 1U);
  TEST_ASSERT_EQ(before, ra8_c6_model()->delays);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));
  TEST_END("wifi-c6 backend guards");
}

/* --- backend happy paths, in budget-sized groups ------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_backend_ops(void)
{
  TEST_BEGIN("wifi-c6 backend operations");
  const ra8_wifi_backend_t* b = &k_ra8_wifi_backend_c6link;

  /* Group A: start the radio (Req_WifiInit/SetWifiMode/WifiStart) and read the
   * station MAC (Req_GetMACAddress) -- four RPCs, inside the frame budget. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  TEST_ASSERT_EQ(k_ra8_ok, b->radio_up(&s_c6));
  ra8_wifi_mac_t mac = {};
  TEST_ASSERT_EQ(k_ra8_ok, b->get_mac(&s_c6, &mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* Group B: join (Req_WifiSetConfig/WifiConnect) -- two RPCs, fresh reset. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  TEST_ASSERT_EQ(k_ra8_ok, b->join(&s_c6, "ra8-bench", "hunter2hunter2"));

  /* Group C: the pacing row reaches the transport's own clock, which is the
   * only clock this backend has and the one the facade's wait is paced by. */
  const uint32_t before = ra8_c6_model()->delays;
  b->idle(&s_c6, (uint16_t)k_ra8_wifi_poll_gap_ms);
  TEST_ASSERT_EQ(before + 1U, ra8_c6_model()->delays);
  TEST_ASSERT_EQ(k_ra8_wifi_poll_gap_ms, ra8_c6_model()->last_delay_ms);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  TEST_END("wifi-c6 backend operations");
}

/* --- a quiet co-processor is not an absent one --------------------------- */

/**
 * @test a quiet HANDSHAKE line is what `service` reports as a timeout
 *
 * The premise of #586's fix. While the co-processor is busy on the air it arms
 * nothing, the pump clocks no transaction, and this backend's `service` reports
 * ::k_ra8_err_hw_timeout -- an entirely routine reading mid-association. The
 * facade must therefore never treat one as the end of its wait, and the
 * corresponding facade-side case is `test_connect_rides_out_a_quiet_radio` in
 * test_ra8_wifi.c.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_service_reports_a_quiet_line(void)
{
  TEST_BEGIN("wifi-c6 quiet line");
  const ra8_wifi_backend_t* b = &k_ra8_wifi_backend_c6link;

  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_wifi_link_t link = k_ra8_wifi_link_up;

  ra8_c6_model()->handshake = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, b->service(&s_c6, &link));

  /* Re-armed, the same call is fine again: the reading above described a
   * moment, not a broken link. */
  ra8_c6_model()->handshake = true;
  TEST_ASSERT_EQ(k_ra8_ok, b->service(&s_c6, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_down, link);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));
  TEST_END("wifi-c6 quiet line");
}

/* --- the three service readings + the event kinds ------------------------ */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_service_transitions(void)
{
  TEST_BEGIN("wifi-c6 service transitions");
  TEST_ASSERT_EQ(k_ra8_ok, t_up());

  ra8_wifi_link_t link = k_ra8_wifi_link_up;

  /* Nothing announced yet -> down. Also drives a boot event through the event
   * callback's "neither station transition" arm. */
  ra8_c6_model_emit_boot();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&s_wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_down, link);

  /* A connected announcement -> up. */
  ra8_c6_model_emit_connected();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&s_wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_up, link);

  /* A later disconnect wins -> down, even though connected is still latched. */
  ra8_c6_model_emit_disconnected();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&s_wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_down, link);

  /* A transport fault while servicing surfaces as an error. */
  ra8_c6_model()->fail_transfer = true;
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_wifi_poll(&s_wifi, &link));
  ra8_c6_model()->fail_transfer = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));
  TEST_END("wifi-c6 service transitions");
}

/* --- the facade journey all the way to a bound IP ------------------------ */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_journey_to_ip(void)
{
  TEST_BEGIN("wifi-c6 journey to IP");
  TEST_ASSERT_EQ(k_ra8_ok, t_up());

  /* The AP associates; the application observes it by pumping the facade. */
  ra8_c6_model_emit_connected();
  ra8_wifi_link_t link = k_ra8_wifi_link_down;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&s_wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_up, link);

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&s_wifi, &st));
  TEST_ASSERT(st.associated);
  TEST_ASSERT(!st.ip_bound);

  /* The AP record decodes into the facade type (Req_WifiStaGetApInfo). */
  ra8_wifi_ap_t ap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_ap(&s_wifi, &ap));
  TEST_ASSERT_EQ(k_t_channel, ap.channel);
  TEST_ASSERT(ap.rssi < 0);

  /* DHCP via the canned provider -> a bound lease. */
  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_wait_ip(&s_wifi, &lease));
  TEST_ASSERT(lease.bound);
  TEST_ASSERT_EQ(k_t_ip, lease.ip);
  TEST_ASSERT_EQ(1, s_ip_calls);

  /* With an IP bound, the IP stack owns the wire: poll is refused. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wifi_poll(&s_wifi, &link));

  /* Leave and power down (Req_WifiDisconnect / WifiStop / WifiDeinit). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_disconnect(&s_wifi));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&s_wifi, &st));
  TEST_ASSERT_EQ(k_ra8_wifi_state_down, st.state);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));
  TEST_END("wifi-c6 journey to IP");
}

/* --- per-operation failures propagate ------------------------------------ */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_operation_failures(void)
{
  TEST_BEGIN("wifi-c6 operation failures");
  const ra8_wifi_backend_t* b = &k_ra8_wifi_backend_c6link;

  /* radio_up: the co-processor refuses Req_WifiStart. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStart;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, b->radio_up(&s_c6));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* get_mac: Req_GetMACAddress refused. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_GetMACAddress;
  ra8_wifi_mac_t mac       = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, b->get_mac(&s_c6, &mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* join: an empty SSID is rejected by ra8_c6link_sta_cfg_set before the wire. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, b->join(&s_c6, "", "pw"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* join: the co-processor refuses Req_WifiConnect. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiConnect;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, b->join(&s_c6, "ra8-bench", "pw"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* get_ap: Req_WifiStaGetApInfo refused. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStaGetApInfo;
  ra8_wifi_ap_t ap         = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, b->get_ap(&s_c6, &ap));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  /* service: a transport fault surfaces from the pump. */
  TEST_ASSERT_EQ(k_ra8_ok, t_up());
  ra8_c6_model()->fail_transfer = true;
  ra8_wifi_link_t link          = k_ra8_wifi_link_up;
  TEST_ASSERT_EQ(k_ra8_err_spi_error, b->service(&s_c6, &link));
  ra8_c6_model()->fail_transfer = false;

  /* The IP provider can also fail; wait_ip surfaces it and clears the lease. */
  ra8_c6_model_emit_connected();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&s_wifi, &link));
  s_ip_ret               = k_ra8_err_timeout;
  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_wifi_wait_ip(&s_wifi, &lease));
  TEST_ASSERT(!lease.bound);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&s_wifi));

  TEST_END("wifi-c6 operation failures");
}

/**
 * @brief Run every ESP32-C6 backend test; return non-zero on first failure.
 * @return 0 when all tests pass.
 */
int main(void)
{
  test_setup_validation();
  test_init_open_fail();
  test_backend_direct_guards();
  test_backend_ops();
  test_service_reports_a_quiet_line();
  test_service_transitions();
  test_journey_to_ip();
  test_operation_failures();
  return 0;
}
