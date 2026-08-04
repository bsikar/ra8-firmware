/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file test_ra8_wifi.c
 * @brief The ra8_wifi facade against a mock backend and a mock IP provider.
 *
 * @details
 * Drives the whole facade state machine -- init, connect, wait-for-IP, status,
 * poll, disconnect -- through a ::ra8_wifi_backend_t whose rows are host
 * functions this file controls, plus an ::ra8_wifi_ip_bind_fn it likewise
 * controls. No radio and no ra8_c6link are involved: the point of the vtable is
 * exactly that the facade can be exercised with nothing behind it, so every
 * branch of the lifecycle is reachable by choosing what the mock returns.
 *
 * The facade has no compound boolean decisions -- each guard is a single
 * condition -- so there is no MC/DC vector table here; the tests instead drive
 * both directions of every guard and every backend-failure path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_wifi.h"
#include "ra8_wifi_backend.h"
#include "unity_minimal.h"

/**
 * @enum t_wifi_const_t
 * @brief Sizes and sentinels this test owns.
 */
typedef enum : uint32_t {
  k_t_ip          = 0xC0A80164U, /**< 192.168.1.100 as a leased address.          */
  k_t_mask        = 0xFFFFFF00U, /**< 255.255.255.0.                              */
  k_t_gw          = 0xC0A80101U, /**< 192.168.1.1 gateway.                        */
  k_t_server      = 0xC0A801FEU, /**< 192.168.1.254 DHCP server.                  */
  k_t_up_after    = 3U,          /**< Service cycles before the mock joins.       */
  k_t_quiet_polls = 5U,          /**< Attempts the mock radio answers not at all. */
  k_t_ssid_cap    = 64U,         /**< Capacity of the recorded-SSID buffer.       */
  k_t_mac_last    = 5U,          /**< Index of the low octet of a MAC.            */
  k_t_mac_low     = 0x2AU,       /**< Low octet the mock reports.                 */
  k_t_mac_first   = 0x02U,       /**< High octet: locally administered.           */
} t_wifi_const_t;

/** @brief Signed constants the mock reports (an enum cannot mix signedness). */
typedef enum : int8_t {
  k_t_rssi_dbm = -56, /**< RSSI the mock AP record carries. */
} t_wifi_signed_t;

/** @brief What the mock backend did and what it should return next. */
typedef struct t_mock {
  int open_n, close_n, up_n, down_n, join_n, leave_n, service_n, mac_n, ap_n, ip_n;
  /**< Call counters, one per backend/ip operation. */
  int idle_n;
  /**< Times the facade paced itself through the `idle` row. */
  uint16_t idle_ms;
  /**< Gap in milliseconds the last `idle` was asked for. */
  ra8_err_t open_ret, close_ret, up_ret, down_ret, join_ret, leave_ret, service_ret;
  /**< Result each backend row returns next. */
  ra8_err_t mac_ret, ap_ret, ip_ret;
  /**< Result get_mac / get_ap / the ip provider return next. */
  uint16_t         service_fail_first;      /**< Fail this many services, then answer.    */
  ra8_wifi_link_t  link;                    /**< Link state the mock reports directly.    */
  uint16_t         up_after;                /**< Report up once `service_n` reaches this. */
  ra8_wifi_mac_t   mac;                     /**< Address `get_mac` returns.               */
  ra8_wifi_ap_t    ap;                      /**< AP record `get_ap` returns.              */
  ra8_wifi_lease_t lease;                   /**< Lease the IP provider returns.           */
  char             last_ssid[k_t_ssid_cap]; /**< SSID the last join saw.                  */
  bool             saw_psk_null;            /**< Last join was handed a null passphrase.  */
} t_mock_t;

/** @brief The one mock instance the backend rows share. */
static t_mock_t s_m;

static ra8_err_t t_open(void* ctx)
{
  (void)ctx;
  s_m.open_n++;
  return s_m.open_ret;
}

static ra8_err_t t_close(void* ctx)
{
  (void)ctx;
  s_m.close_n++;
  return s_m.close_ret;
}

static ra8_err_t t_radio_up(void* ctx)
{
  (void)ctx;
  s_m.up_n++;
  return s_m.up_ret;
}

static ra8_err_t t_radio_down(void* ctx)
{
  (void)ctx;
  s_m.down_n++;
  return s_m.down_ret;
}

static ra8_err_t t_join(void* ctx, const char* ssid, const char* psk)
{
  (void)ctx;
  s_m.join_n++;
  s_m.saw_psk_null = (psk == nullptr);
  if (ssid != nullptr) {
    (void)snprintf(s_m.last_ssid, sizeof(s_m.last_ssid), "%s", ssid);
  }
  return s_m.join_ret;
}

static ra8_err_t t_leave(void* ctx)
{
  (void)ctx;
  s_m.leave_n++;
  return s_m.leave_ret;
}

static ra8_err_t t_service(void* ctx, ra8_wifi_link_t* out_link)
{
  (void)ctx;
  s_m.service_n++;
  /* A radio that is busy on the air answers nothing for a while and then starts
   * answering: the silicon behaviour a permanent `service_ret` cannot express. */
  if ((uint16_t)s_m.service_n <= s_m.service_fail_first) {
    return k_ra8_err_hw_timeout;
  }
  if (s_m.service_ret != k_ra8_ok) {
    return s_m.service_ret;
  }
  if (s_m.up_after != 0U) {
    *out_link =
      ((uint16_t)s_m.service_n >= s_m.up_after) ? k_ra8_wifi_link_up : k_ra8_wifi_link_down;
  } else {
    *out_link = s_m.link;
  }
  return k_ra8_ok;
}

static ra8_err_t t_get_mac(void* ctx, ra8_wifi_mac_t* out)
{
  (void)ctx;
  s_m.mac_n++;
  if (s_m.mac_ret != k_ra8_ok) {
    return s_m.mac_ret;
  }
  *out = s_m.mac;
  return k_ra8_ok;
}

static ra8_err_t t_get_ap(void* ctx, ra8_wifi_ap_t* out)
{
  (void)ctx;
  s_m.ap_n++;
  if (s_m.ap_ret != k_ra8_ok) {
    return s_m.ap_ret;
  }
  *out = s_m.ap;
  return k_ra8_ok;
}

static ra8_err_t t_ip_bind(void* ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  (void)ctx;
  (void)mac;
  s_m.ip_n++;
  if (s_m.ip_ret != k_ra8_ok) {
    return s_m.ip_ret;
  }
  *out = s_m.lease;
  return k_ra8_ok;
}

static void t_idle(void* ctx, uint16_t ms)
{
  (void)ctx;
  s_m.idle_n++;
  s_m.idle_ms = ms;
}

/** @brief A complete, working backend table the tests copy and dent. */
static const ra8_wifi_backend_t k_t_backend = {
  .open       = t_open,
  .close      = t_close,
  .radio_up   = t_radio_up,
  .radio_down = t_radio_down,
  .join       = t_join,
  .leave      = t_leave,
  .service    = t_service,
  .get_mac    = t_get_mac,
  .get_ap     = t_get_ap,
  .idle       = t_idle,
};

/** @brief Reset the mock to "everything succeeds" defaults. */
static void t_reset(void)
{
  s_m                         = (t_mock_t){};
  s_m.link                    = k_ra8_wifi_link_up;
  s_m.mac.octet[0]            = (uint8_t)k_t_mac_first;
  s_m.mac.octet[k_t_mac_last] = (uint8_t)k_t_mac_low;
  s_m.ap.rssi                 = (int8_t)k_t_rssi_dbm;
  s_m.ap.channel              = 6U;
  s_m.ap.ssid_len             = 3U;
  s_m.ap.ssid[0]              = 'r';
  s_m.ap.ssid[1]              = 'a';
  s_m.ap.ssid[2]              = '8';
  s_m.lease.ip                = (uint32_t)k_t_ip;
  s_m.lease.mask              = (uint32_t)k_t_mask;
  s_m.lease.gateway           = (uint32_t)k_t_gw;
  s_m.lease.dhcp_server       = (uint32_t)k_t_server;
}

/** @brief A configuration wired to the mock backend and IP provider. */
static ra8_wifi_cfg_t t_cfg(void)
{
  ra8_wifi_cfg_t cfg = {};
  cfg.backend        = &k_t_backend;
  cfg.backend_ctx    = &s_m;
  cfg.ip_bind        = t_ip_bind;
  cfg.ip_ctx         = &s_m;
  return cfg;
}

/* --- init ---------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_validation(void)
{
  TEST_BEGIN("wifi init validation");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, nullptr));

  ra8_wifi_cfg_t no_ip = cfg;
  no_ip.ip_bind        = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, &no_ip));

  ra8_wifi_cfg_t no_backend = cfg;
  no_backend.backend        = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, &no_backend));

  /* A backend missing one row is rejected: dent each row in turn. */
  ra8_wifi_backend_t dented = k_t_backend;
  dented.open               = nullptr;
  ra8_wifi_cfg_t bad        = cfg;
  bad.backend               = &dented;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, &bad));
  dented        = k_t_backend;
  dented.get_ap = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, &bad));
  dented      = k_t_backend;
  dented.idle = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_init(&wifi, &bad));

  TEST_ASSERT_EQ(0, s_m.open_n); /* nothing opened while validation failed */
  TEST_END("wifi init validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_open_paths(void)
{
  TEST_BEGIN("wifi init open");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  /* Backend open failure propagates and leaves the handle closed. */
  s_m.open_ret = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_wifi_init(&wifi, &cfg));
  TEST_ASSERT_EQ(1, s_m.open_n);

  /* Success. */
  s_m.open_ret = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  TEST_ASSERT_EQ(2, s_m.open_n);

  /* Double init is refused. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wifi_init(&wifi, &cfg));

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT_EQ(k_ra8_wifi_state_down, st.state);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi init open");
}

/* --- deinit -------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("wifi deinit");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_deinit(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_deinit(&wifi));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  s_m.close_ret = k_ra8_err_spi_error; /* close result is surfaced */
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_wifi_deinit(&wifi));
  TEST_ASSERT_EQ(1, s_m.close_n);
  /* Still marked closed afterwards, so a second deinit reports not-init. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_deinit(&wifi));
  TEST_END("wifi deinit");
}

/* --- connect ------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_connect_success_and_reuse(void)
{
  TEST_BEGIN("wifi connect success");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));

  s_m.up_after = (uint16_t)k_t_up_after; /* joins after a few service cycles */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "ra8-bench", "secret"));
  TEST_ASSERT_EQ(1, s_m.up_n);
  TEST_ASSERT_EQ(1, s_m.mac_n);
  TEST_ASSERT_EQ(1, s_m.join_n);
  TEST_ASSERT((int)s_m.service_n >= (int)k_t_up_after);
  TEST_ASSERT(!s_m.saw_psk_null);
  TEST_ASSERT(strcmp(s_m.last_ssid, "ra8-bench") == 0);

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT_EQ(k_ra8_wifi_state_associated, st.state);
  TEST_ASSERT(st.associated);
  TEST_ASSERT(!st.ip_bound);

  /* A second connect finds the radio already on and does not restart it. */
  s_m.up_after = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "ra8-bench", nullptr));
  TEST_ASSERT_EQ(1, s_m.up_n); /* unchanged */
  TEST_ASSERT(s_m.saw_psk_null);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi connect success");
}

/**
 * @test connect survives a radio that is quiet while it associates
 *
 * The silicon defect behind #586: ``ra8_wifi_backend_c6link``'s `service` maps
 * onto the co-processor pump, which reports ::k_ra8_err_hw_timeout whenever the
 * co-processor arms nothing -- its normal state while an 802.11 association is
 * in flight. The wait used to return that first reading, so whether the station
 * ever joined depended on whether the radio happened to stay chatty, and the
 * bench saw the same image pass and fail alternately. The wait must ride out a
 * quiet stretch and still associate when the link finally reports up.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_connect_rides_out_a_quiet_radio(void)
{
  TEST_BEGIN("wifi connect rides out a quiet radio");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));

  /* Quiet for the first few attempts, then up on the one after. */
  s_m.service_fail_first = (uint16_t)k_t_quiet_polls;
  s_m.up_after           = (uint16_t)k_t_quiet_polls + 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "ra8-bench", "secret"));
  TEST_ASSERT_EQ(k_t_quiet_polls + 1, s_m.service_n);

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT(st.associated);

  /* Every attempt that did not associate paced itself, so the budget is spent
   * in seconds and not in however fast the backend can answer. */
  TEST_ASSERT_EQ(k_t_quiet_polls, s_m.idle_n);
  TEST_ASSERT_EQ(k_ra8_wifi_poll_gap_ms, s_m.idle_ms);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi connect rides out a quiet radio");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_connect_failure_paths(void)
{
  TEST_BEGIN("wifi connect failures");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_connect(nullptr, "x", "y"));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_connect(&wifi, "x", "y"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_connect(&wifi, nullptr, "y"));

  /* radio_up failure -- radio stays off so the next attempt retries it. */
  s_m.up_ret = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_wifi_connect(&wifi, "x", "y"));
  TEST_ASSERT_EQ(0, s_m.join_n);

  /* get_mac failure. */
  s_m.up_ret  = k_ra8_ok;
  s_m.mac_ret = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_wifi_connect(&wifi, "x", "y"));
  TEST_ASSERT_EQ(0, s_m.join_n);

  /* join request refused. */
  s_m.mac_ret  = k_ra8_ok;
  s_m.join_ret = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_wifi_connect(&wifi, "x", "y"));

  /* Service failing for the WHOLE budget is the radio being absent, so its own
   * error is reported rather than a plain timeout -- and the wait spends the
   * entire budget first, because one bad reading proves nothing. */
  s_m.join_ret    = k_ra8_ok;
  s_m.service_ret = k_ra8_err_spi_error;
  s_m.service_n   = 0;
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_wifi_connect(&wifi, "x", "y"));
  TEST_ASSERT_EQ(k_ra8_wifi_join_polls, s_m.service_n);

  /* never associates -> timeout after servicing the full poll budget. */
  s_m.service_ret = k_ra8_ok;
  s_m.up_after    = 0U;
  s_m.link        = k_ra8_wifi_link_down;
  s_m.service_n   = 0;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_wifi_connect(&wifi, "x", "y"));
  TEST_ASSERT_EQ(k_ra8_wifi_join_polls, s_m.service_n);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi connect failures");
}

/* --- wait_ip / get_ip ---------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wait_ip(void)
{
  TEST_BEGIN("wifi wait_ip");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));

  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_wait_ip(nullptr, &lease));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_wait_ip(&wifi, nullptr));

  /* Not associated yet. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wifi_wait_ip(&wifi, &lease));

  /* Associate, then a provider that fails. */
  s_m.up_after = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "x", "y"));
  s_m.ip_ret = k_ra8_err_timeout;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_wifi_wait_ip(&wifi, &lease));
  TEST_ASSERT(!lease.bound);

  /* Provider returns success but a zero address -> treated as no lease. */
  s_m.ip_ret   = k_ra8_ok;
  s_m.lease.ip = 0U;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_wifi_wait_ip(&wifi, &lease));
  TEST_ASSERT(!lease.bound);

  /* A real lease. */
  s_m.lease.ip = (uint32_t)k_t_ip;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_wait_ip(&wifi, &lease));
  TEST_ASSERT(lease.bound);
  TEST_ASSERT_EQ(k_t_ip, lease.ip);
  TEST_ASSERT_EQ(k_t_gw, lease.gateway);

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT_EQ(k_ra8_wifi_state_ip_bound, st.state);
  TEST_ASSERT(st.ip_bound);

  /* get_ip returns the cached lease without touching the provider. */
  ra8_wifi_lease_t cached = {};
  const int        before = s_m.ip_n;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_ip(&wifi, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_ip(&wifi, &cached));
  TEST_ASSERT(cached.bound);
  TEST_ASSERT_EQ(k_t_ip, cached.ip);
  TEST_ASSERT_EQ(before, s_m.ip_n);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi wait_ip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_ip_not_open(void)
{
  TEST_BEGIN("wifi get_ip guards");
  t_reset();
  ra8_wifi_t       wifi  = {};
  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_ip(nullptr, &lease));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_get_ip(&wifi, &lease));
  TEST_END("wifi get_ip guards");
}

/* --- poll ---------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_poll(void)
{
  TEST_BEGIN("wifi poll");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  ra8_wifi_link_t link = k_ra8_wifi_link_up;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_poll(nullptr, &link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_poll(&wifi, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_poll(&wifi, &link));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));

  /* service failure surfaces. */
  s_m.service_ret = k_ra8_err_spi_error;
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_wifi_poll(&wifi, &link));

  /* link up -> associated. */
  s_m.service_ret = k_ra8_ok;
  s_m.up_after    = 0U;
  s_m.link        = k_ra8_wifi_link_up;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_up, link);

  /* link down -> back to down. */
  s_m.link = k_ra8_wifi_link_down;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_poll(&wifi, &link));
  TEST_ASSERT_EQ(k_ra8_wifi_link_down, link);

  /* Once an IP is bound, poll refuses so it cannot pre-empt the IP stack. */
  s_m.link     = k_ra8_wifi_link_up;
  s_m.up_after = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "x", "y"));
  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_wait_ip(&wifi, &lease));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wifi_poll(&wifi, &link));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi poll");
}

/* --- status / get_mac / get_ap ------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_guards(void)
{
  TEST_BEGIN("wifi status guards");
  t_reset();
  ra8_wifi_t        wifi = {};
  ra8_wifi_status_t st   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_status(nullptr, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_status(&wifi, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_status(&wifi, &st));
  TEST_END("wifi status guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_mac(void)
{
  TEST_BEGIN("wifi get_mac");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();
  ra8_wifi_mac_t mac  = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_mac(nullptr, &mac));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_get_mac(&wifi, &mac));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_mac(&wifi, nullptr));

  /* Nothing has ever been read, so there is no answer to fall back on and the
   * backend's failure is the only honest reply. */
  s_m.mac_ret = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_wifi_get_mac(&wifi, &mac));

  s_m.mac_ret = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_mac(&wifi, &mac));
  TEST_ASSERT_EQ(0x02, mac.octet[0]);
  TEST_ASSERT_EQ(0x2A, mac.octet[5]);

  /* Once one HAS been read, a failed re-read serves the cached address instead
   * of handing the caller 00:00:00:00:00:00 -- a station's own address cannot
   * change under it, and the bench printed exactly that null MAC out of a run
   * that had associated (#586). */
  mac         = (ra8_wifi_mac_t){};
  s_m.mac_ret = k_ra8_err_spi_error;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_mac(&wifi, &mac));
  TEST_ASSERT_EQ(0x02, mac.octet[0]);
  TEST_ASSERT_EQ(0x2A, mac.octet[5]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi get_mac");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_ap(void)
{
  TEST_BEGIN("wifi get_ap");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();
  ra8_wifi_ap_t  ap   = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_ap(nullptr, &ap));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_get_ap(&wifi, &ap));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_get_ap(&wifi, nullptr));

  /* Not associated yet. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wifi_get_ap(&wifi, &ap));

  s_m.up_after = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "x", "y"));

  s_m.ap_ret = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_wifi_get_ap(&wifi, &ap));

  s_m.ap_ret = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_ap(&wifi, &ap));
  TEST_ASSERT_EQ((-56), ap.rssi);

  /* The rssi is now cached and shows up in status. */
  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT_EQ((-56), st.rssi);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi get_ap");
}

/* --- disconnect ---------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_disconnect(void)
{
  TEST_BEGIN("wifi disconnect");
  t_reset();
  ra8_wifi_t     wifi = {};
  ra8_wifi_cfg_t cfg  = t_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wifi_disconnect(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wifi_disconnect(&wifi));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_init(&wifi, &cfg));
  s_m.up_after = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "x", "y"));
  ra8_wifi_lease_t lease = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_wait_ip(&wifi, &lease));

  /* A leave failure is surfaced but the radio is still stopped and the lease
   * cleared -- teardown does not stop at the first error. */
  s_m.leave_ret = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_wifi_disconnect(&wifi));
  TEST_ASSERT_EQ(1, s_m.leave_n);
  TEST_ASSERT_EQ(1, s_m.down_n);

  ra8_wifi_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_status(&wifi, &st));
  TEST_ASSERT_EQ(k_ra8_wifi_state_down, st.state);
  TEST_ASSERT(!st.ip_bound);
  ra8_wifi_lease_t cleared = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_get_ip(&wifi, &cleared));
  TEST_ASSERT(!cleared.bound);

  /* After a disconnect, connecting again restarts the radio. */
  s_m.leave_ret = k_ra8_ok;
  s_m.up_after  = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_connect(&wifi, "x", "y"));
  TEST_ASSERT_EQ(2, s_m.up_n);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wifi_deinit(&wifi));
  TEST_END("wifi disconnect");
}

/**
 * @brief Run every facade test; return non-zero on the first failure.
 * @return 0 when all tests pass.
 */
int main(void)
{
  test_init_validation();
  test_init_open_paths();
  test_deinit();
  test_connect_success_and_reuse();
  test_connect_rides_out_a_quiet_radio();
  test_connect_failure_paths();
  test_wait_ip();
  test_get_ip_not_open();
  test_poll();
  test_status_guards();
  test_get_mac();
  test_get_ap();
  test_disconnect();
  return 0;
}
