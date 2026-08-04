/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_core.c
 * @brief The example's join+DHCP journey, hardware-free so it is host-testable.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements ::wifi_hal_join_run -- the exact orchestration ``main.c`` runs on
 * the board, with every hardware, RTOS and console dependency lifted out to the
 * caller. It drives only the ``ra8_wifi`` facade and records what happened in an
 * ::wifi_hal_result_t, so the same logic runs on silicon (bound to the C6
 * backend and a NetX DHCP provider) and in ``tests/test_app_wifi_hal_join.c``
 * (bound to a mock backend and a canned provider). No ``printf``, no ThreadX, no
 * board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wifi.h"
#include "wifi_hal_join.h"

const char k_wifi_hal_pass_line[] =
  "wifi_hal: PASS ra8_wifi joined the bench Wi-Fi and DHCP leased an address\r\n";
const char k_wifi_hal_fail_line[] =
  "wifi_hal: FAIL Wi-Fi join to a DHCP lease did not complete\r\n";

/* Poll the facade until the station is associated or the budget is spent. A
 * slow association can outlast ra8_wifi_connect's internal wait, so the run
 * finishes the job here -- the loop the facade lets a caller keep short. */
static bool wifi_hal_settle(ra8_wifi_t* wifi, uint32_t budget)
{
  ra8_wifi_status_t st = {};
  for (uint32_t i = 0U; i < budget; i++) {
    if ((ra8_wifi_status(wifi, &st) == k_ra8_ok) && st.associated) {
      return true;
    }
    ra8_wifi_link_t link = k_ra8_wifi_link_down;
    (void)ra8_wifi_poll(wifi, &link);
  }
  return (ra8_wifi_status(wifi, &st) == k_ra8_ok) && st.associated;
}

bool wifi_hal_join_run(const wifi_hal_run_cfg_t* cfg, wifi_hal_result_t* out)
{
  if (out == nullptr) {
    return false;
  }
  *out             = (wifi_hal_result_t){};
  out->init_err    = k_ra8_err_not_initialized;
  out->connect_err = k_ra8_err_not_initialized;
  out->ip_err      = k_ra8_err_not_initialized;
  if ((cfg == nullptr) || (cfg->wifi == nullptr) || (cfg->wifi_cfg == nullptr) ||
      (cfg->ssid == nullptr)) {
    return false;
  }

  out->init_err = ra8_wifi_init(cfg->wifi, cfg->wifi_cfg);
  if (out->init_err != k_ra8_ok) {
    return false;
  }
  out->init_ok = true;

  if (cfg->ssid[0] == '\0') {
    return false; /* no credentials compiled in */
  }

  out->connect_err = ra8_wifi_connect(cfg->wifi, cfg->ssid, cfg->psk);
  out->associated  = wifi_hal_settle(cfg->wifi, cfg->poll_budget);
  if (!out->associated) {
    return false;
  }

  (void)ra8_wifi_get_mac(cfg->wifi, &out->mac);
  (void)ra8_wifi_get_ap(cfg->wifi, &out->ap);

  out->ip_err = ra8_wifi_wait_ip(cfg->wifi, &out->lease);
  if ((out->ip_err != k_ra8_ok) || !out->lease.bound) {
    return false;
  }
  out->ip_bound = true;
  out->passed   = true;
  return true;
}
