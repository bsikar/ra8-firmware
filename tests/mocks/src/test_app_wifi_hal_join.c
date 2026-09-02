/**
 * @file test_app_wifi_hal_join.c
 * @brief Host test for the wifi_hal_join example's core wiring.
 *
 * @details
 * Drives the exact ::wifi_hal_join_run the ARM example runs -- the same
 * init/connect/settle/DHCP orchestration from
 * examples/.../c6/wifi_hal_join/src/wifi_hal_core.c -- bound to a mock
 * ra8_wifi backend and a canned IP provider instead of the ESP32-C6 and NetX
 * Duo. It proves the EXAMPLE's wiring reaches a bound IP and selects its PASS
 * line; the facade itself is covered in test_ra8_wifi.c.
 *
 * A mock backend (not the co-processor model) is used deliberately: the model
 * answers only a handful of frames per reset, too few for a full connect, so a
 * mock that reports the station associated is the way to exercise the whole app
 * flow to PASS. The facade-over-real-c6 path is covered in
 * test_ra8_wifi_c6link.c.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_wifi.h"
#include "ra8_wifi_backend.h"
#include "unity_minimal.h"
#include "wifi_hal_join.h"

/**
 * @enum t_app_const_t
 * @brief Canned values this test owns.
 */
typedef enum : uint32_t {
  k_t_ip        = 0xC0A80164U, /**< 192.168.1.100 leased address.       */
  k_t_gw        = 0xC0A80101U, /**< 192.168.1.1 gateway.                */
  k_t_channel   = 6U,          /**< AP channel the mock reports.        */
  k_t_budget    = 8U,          /**< Settle poll budget the test passes. */
  k_t_mac_first = 0x02U,       /**< High octet: locally administered.   */
  k_t_mac_last  = 5U,          /**< Index of the low octet of a MAC.    */
  k_t_mac_low   = 0x2AU,       /**< Low octet the mock reports.         */
} t_app_const_t;

/** @brief Signed constants the mock reports (an enum cannot mix signedness). */
typedef enum : int8_t {
  k_t_rssi_dbm = -55, /**< RSSI the mock AP record carries. */
} t_app_signed_t;

/** @brief Whether the mock backend reports the station associated. */
static bool s_link_up;
/** @brief Result the canned IP provider returns. */
static ra8_err_t s_ip_ret;
/** @brief Lease the canned IP provider returns on success. */
static ra8_wifi_lease_t s_ip_lease;
/** @brief Optional facade the join mock invalidates before association settles. */
static ra8_wifi_t* s_join_invalidation_target;

static ra8_err_t m_ok(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

static ra8_err_t m_join(void* ctx, const char* ssid, const char* psk)
{
  (void)ctx;
  (void)ssid;
  (void)psk;
  if (s_join_invalidation_target != nullptr) {
    s_join_invalidation_target->open = false;
  }
  return k_ra8_ok;
}

static ra8_err_t m_service(void* ctx, ra8_wifi_link_t* out)
{
  (void)ctx;
  *out = s_link_up ? k_ra8_wifi_link_up : k_ra8_wifi_link_down;
  return k_ra8_ok;
}

static ra8_err_t m_get_mac(void* ctx, ra8_wifi_mac_t* out)
{
  (void)ctx;
  *out                     = (ra8_wifi_mac_t){};
  out->octet[0]            = (uint8_t)k_t_mac_first;
  out->octet[k_t_mac_last] = (uint8_t)k_t_mac_low;
  return k_ra8_ok;
}

static ra8_err_t m_get_ap(void* ctx, ra8_wifi_ap_t* out)
{
  (void)ctx;
  *out         = (ra8_wifi_ap_t){};
  out->channel = (uint8_t)k_t_channel;
  out->rssi    = (int8_t)k_t_rssi_dbm;
  return k_ra8_ok;
}

static void m_idle(void* ctx, uint16_t ms)
{
  (void)ctx;
  (void)ms;
}

/** @brief A backend that always succeeds and reports link per ::s_link_up. */
static const ra8_wifi_backend_t k_mock = {
  .open       = m_ok,
  .close      = m_ok,
  .radio_up   = m_ok,
  .radio_down = m_ok,
  .join       = m_join,
  .leave      = m_ok,
  .service    = m_service,
  .get_mac    = m_get_mac,
  .get_ap     = m_get_ap,
  .idle       = m_idle,
};

static ra8_err_t m_ip_bind(void* ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  (void)ctx;
  (void)mac;
  if (s_ip_ret != k_ra8_ok) {
    return s_ip_ret;
  }
  *out = s_ip_lease;
  return k_ra8_ok;
}

/** @brief Reset the mock and build a facade config wired to it. */
static ra8_wifi_cfg_t t_reset(void)
{
  s_link_up                  = true;
  s_ip_ret                   = k_ra8_ok;
  s_ip_lease                 = (ra8_wifi_lease_t){};
  s_ip_lease.ip              = (uint32_t)k_t_ip;
  s_ip_lease.gateway         = (uint32_t)k_t_gw;
  s_join_invalidation_target = nullptr;

  ra8_wifi_cfg_t cfg = {};
  cfg.backend        = &k_mock;
  cfg.backend_ctx    = nullptr;
  cfg.ip_bind        = m_ip_bind;
  cfg.ip_ctx         = nullptr;
  return cfg;
}

/** @brief Build a run config over @p cfg and @p wifi for @p ssid / @p psk. */
static wifi_hal_run_cfg_t
t_run_cfg(const ra8_wifi_cfg_t* cfg, ra8_wifi_t* wifi, const char* ssid, const char* psk)
{
  wifi_hal_run_cfg_t rcfg = {};
  rcfg.wifi_cfg           = cfg;
  rcfg.wifi               = wifi;
  rcfg.ssid               = ssid;
  rcfg.psk                = psk;
  rcfg.poll_budget        = (uint32_t)k_t_budget;
  return rcfg;
}

/* --- the full example flow reaches a bound IP --------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_app_reaches_ip(void)
{
  TEST_BEGIN("wifi_hal_join reaches IP");
  ra8_wifi_cfg_t     cfg  = t_reset();
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "ra8-bench", "hunter2hunter2");

  wifi_hal_result_t res = {};
  TEST_ASSERT(wifi_hal_join_run(&rcfg, &res));
  TEST_ASSERT(res.init_ok);
  TEST_ASSERT(res.associated);
  TEST_ASSERT(res.ip_bound);
  TEST_ASSERT(res.passed);
  TEST_ASSERT(res.lease.bound);
  TEST_ASSERT_EQ(k_t_ip, res.lease.ip);
  TEST_ASSERT_EQ(k_t_channel, res.ap.channel);

  /* The PASS line the example prints on this outcome is the one hil.conf keys
   * on: prove the shared constant carries exactly that text. */
  TEST_ASSERT_EQ(0,
                 strcmp(k_wifi_hal_pass_line,
                        "wifi_hal: PASS ra8_wifi joined the bench Wi-Fi and DHCP leased an "
                        "address\r\n"));
  TEST_END("wifi_hal_join reaches IP");
}

/**
 * @test A zero continuation budget still takes the required final sample.
 * @brief Verify the settle helper's final status sample.
 * @details The mock associates during the facade connect itself. A zero
 *          continuation budget therefore skips the polling loop and must still
 *          observe that associated state through the final status read.
 * @par MC/DC:
 * `wifi_hal_settle()` contains no compound boolean decisions after the status
 * error and association checks were separated. This zero-budget vector proves
 * the loop can be skipped while the required final status sample still runs.
 * @return Nothing.
 * @pre The successful mock backend and IP provider are reset.
 * @pre The run configuration owns an initialized facade object.
 * @post The join succeeds without issuing a continuation poll.
 * @post The result records association and a bound IP lease.
 * @note Not thread-safe because the test executable owns shared mock state.
 * @since 0.1.0
 */
static void test_app_zero_settle_budget(void)
{
  TEST_BEGIN("wifi_hal_join zero settle budget");
  ra8_wifi_cfg_t     cfg  = t_reset();
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "ra8-bench", "hunter2hunter2");
  rcfg.poll_budget        = 0U;
  wifi_hal_result_t res   = {};
  TEST_ASSERT(wifi_hal_join_run(&rcfg, &res));
  TEST_ASSERT(res.associated);
  TEST_ASSERT(res.lease.bound);
  TEST_END("wifi_hal_join zero settle budget");
}

/**
 * @test A facade invalidated during join fails both settle status locations.
 * @brief Verify status-read failures stop association settling.
 * @details The join mock invalidates the initialized facade after the connect
 *          request. One vector uses a positive continuation budget and reaches
 *          the loop status read; the other uses a zero budget and reaches the
 *          final status sample directly.
 * @par MC/DC:
 * Each status-error decision has one true vector here. Their false vectors are
 * exercised by the successful positive-budget and zero-budget tests.
 * @return Nothing.
 * @pre The mock backend invalidation target is reset before each vector.
 * @pre Each run configuration owns a distinct facade object.
 * @post Both join attempts fail closed before DHCP binding.
 * @post Neither result reports association or a bound IP lease.
 * @note Not thread-safe because the test executable owns shared mock state.
 * @since 0.1.0
 */
static void test_app_settle_status_failures(void)
{
  TEST_BEGIN("wifi_hal_join settle status failures");
  ra8_wifi_cfg_t     loop_cfg  = t_reset();
  ra8_wifi_t         loop_wifi = {};
  wifi_hal_run_cfg_t loop_run  = t_run_cfg(&loop_cfg, &loop_wifi, "ra8-bench", "pw");
  s_join_invalidation_target   = &loop_wifi;

  wifi_hal_result_t loop_result = {};
  TEST_ASSERT(!wifi_hal_join_run(&loop_run, &loop_result));
  TEST_ASSERT(!loop_result.associated);
  TEST_ASSERT(!loop_result.ip_bound);

  ra8_wifi_cfg_t     final_cfg  = t_reset();
  ra8_wifi_t         final_wifi = {};
  wifi_hal_run_cfg_t final_run  = t_run_cfg(&final_cfg, &final_wifi, "ra8-bench", "pw");
  final_run.poll_budget         = 0U;
  s_join_invalidation_target    = &final_wifi;

  wifi_hal_result_t final_result = {};
  TEST_ASSERT(!wifi_hal_join_run(&final_run, &final_result));
  TEST_ASSERT(!final_result.associated);
  TEST_ASSERT(!final_result.ip_bound);
  TEST_END("wifi_hal_join settle status failures");
}

/* --- failure paths the example reports --------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_app_no_credentials(void)
{
  TEST_BEGIN("wifi_hal_join no credentials");
  ra8_wifi_cfg_t     cfg  = t_reset();
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "", "");

  wifi_hal_result_t res = {};
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  TEST_ASSERT(res.init_ok); /* init ran; the empty SSID stops it after. */
  TEST_ASSERT(!res.associated);
  TEST_ASSERT(!res.passed);
  TEST_END("wifi_hal_join no credentials");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_app_association_fails(void)
{
  TEST_BEGIN("wifi_hal_join association fails");
  ra8_wifi_cfg_t cfg      = t_reset();
  s_link_up               = false; /* the mock never reports the station associated */
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "ra8-bench", "pw");

  wifi_hal_result_t res = {};
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  TEST_ASSERT(res.init_ok);
  TEST_ASSERT(!res.associated);
  TEST_ASSERT(!res.ip_bound);
  TEST_ASSERT(!res.passed);
  TEST_ASSERT_EQ(k_ra8_err_timeout, res.connect_err);
  /* wait_ip was never reached, so its slot still says so rather than lying
   * about a call that did not happen. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, res.ip_err);
  TEST_END("wifi_hal_join association fails");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_app_dhcp_fails(void)
{
  TEST_BEGIN("wifi_hal_join dhcp fails");
  ra8_wifi_cfg_t cfg      = t_reset();
  s_ip_ret                = k_ra8_err_timeout; /* the provider never binds a lease */
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "ra8-bench", "pw");

  wifi_hal_result_t res = {};
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  TEST_ASSERT(res.associated); /* got as far as association... */
  TEST_ASSERT(!res.ip_bound);  /* ...but no lease.             */
  TEST_ASSERT(!res.passed);
  /* The run names the step and the reason, so a bench failure is diagnosable
   * from the console alone rather than costing another flash to find out. */
  TEST_ASSERT_EQ(k_ra8_ok, res.init_err);
  TEST_ASSERT_EQ(k_ra8_ok, res.connect_err);
  TEST_ASSERT_EQ(k_ra8_err_timeout, res.ip_err);
  TEST_END("wifi_hal_join dhcp fails");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_app_null_guards(void)
{
  TEST_BEGIN("wifi_hal_join null guards");
  ra8_wifi_cfg_t     cfg  = t_reset();
  ra8_wifi_t         wifi = {};
  wifi_hal_run_cfg_t rcfg = t_run_cfg(&cfg, &wifi, "ra8-bench", "pw");

  wifi_hal_result_t res = {};
  TEST_ASSERT(!wifi_hal_join_run(nullptr, &res));
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, nullptr));
  rcfg.wifi = nullptr;
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  rcfg.wifi     = &wifi;
  rcfg.wifi_cfg = nullptr;
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  rcfg.wifi_cfg = &cfg;
  rcfg.ssid     = nullptr;
  TEST_ASSERT(!wifi_hal_join_run(&rcfg, &res));
  TEST_END("wifi_hal_join null guards");
}

/**
 * @brief Run every wifi_hal_join example test; return non-zero on first failure.
 * @return 0 when all tests pass.
 */
int main(void)
{
  test_app_reaches_ip();
  test_app_zero_settle_budget();
  test_app_settle_status_failures();
  test_app_no_credentials();
  test_app_association_fails();
  test_app_dhcp_fails();
  test_app_null_guards();
  return 0;
}
