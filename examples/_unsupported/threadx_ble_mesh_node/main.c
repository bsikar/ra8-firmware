/**
 * @file examples/_unsupported/threadx_ble_mesh_node/main.c
 * @brief NimBLE Bluetooth Mesh node demo on ThreadX (RA8D2).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up an unprovisioned Bluetooth Mesh node hosting a single
 * element with a Configuration Server model and a Generic OnOff
 * Server. The flow is:
 *
 *   1. Boot the EK-RA8D2 (CGC + SCI8 + SysTick).
 *   2. Power up ra_ble + NimBLE via the port/nimble adapter.
 *   3. Initialise ra_ble_security (LE Secure Connections + bonding).
 *   4. Hand a 1-element composition data struct to ra_ble_mesh_init.
 *   5. Enable provisioning beaconing (PB-ADV + PB-GATT).
 *   6. Wait forever -- the application callback logs every mesh
 *      lifecycle event.
 *
 * @par Verification
 * Use the nRF Mesh app (Nordic Semiconductor) to provision the node
 * and exchange Generic OnOff messages.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_ble_mesh.h"
#include "ra_ble_security.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

#ifndef RA_SIMULATOR_MODE
#include "ble_hci_ra_ble.h"
#include "nimble_npl_threadx.h"
#include "ra_ble.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_mesh_cfg_t
 * @brief Numeric configuration constants.
 */
/** @brief Bluetooth SIG company identifier. */
typedef enum : uint16_t {
  k_mesh_company_id_lf = 0x05F1U, /**< Linux Foundation CID. */
} mesh_company_id_t;

typedef enum : uint32_t {
  k_demo_baud         = 115200U,
  k_demo_sci_channel  = 8U,
  k_demo_thread_stack = 8192U,
  k_demo_thread_prio  = 8U,
  k_demo_idle_ms      = 5000U,
} demo_mesh_cfg_t;

/**
 * @enum demo_mesh_models_t
 * @brief SIG model identifiers used in the composition.
 */
typedef enum : uint16_t {
  k_demo_model_config_server  = 0x0000U,
  k_demo_model_generic_on_off = 0x1000U,
} demo_mesh_models_t;

/** @brief Mesh device UUID literal. */
static const uint8_t k_demo_dev_uuid[k_ra_ble_mesh_uuid_bytes] = {
  0xEAU,
  0x8DU,
  0x21U,
  0x00U,
  0xCAU,
  0xFEU,
  0xBAU,
  0xBEU,
  0x00U,
  0x01U,
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
};

/** @brief Tag for log lines. */
static const char* s_demo_tag = "ble_mesh_node";

static const ra_port_pin_t k_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

#ifndef RA_SIMULATOR_MODE
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif

/**
 * @brief Park the CPU forever in WFI on a fatal init failure.
 *
 * @pre Unrecoverable boot path.
 * @post CPU is parked; debugger / external reset only wakes it.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Best-effort SCI8 log helper.
 *
 * @param[in] s NUL-terminated ASCII string. May be NULL.
 *
 * @since 0.1.0
 */
static void demo_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = (uint32_t)strlen(s);
  (void)ra_sci_write_polling((uint8_t)k_demo_sci_channel, (const uint8_t*)s, len);
}

/**
 * @brief Bring CGC + SysTick + SCI8 up. Halts on failure.
 *
 * @pre Reset_Handler complete.
 * @post On success SCI8 is sending at 115200 8N1.
 *
 * @since 0.1.0
 */
static void demo_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_demo_pin_txd, k_ra_psel_sci_async, "mesh.tx") != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_demo_pin_rxd, k_ra_psel_sci_async, "mesh.rx") != k_ra_ok) {
    demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Mesh lifecycle event callback -- log each transition.
 *
 * @param[in] ctx Unused.
 * @param[in] evt Event payload.
 *
 * @since 0.1.0
 */
static void demo_mesh_evt(void* ctx, const ra_ble_mesh_event_t* evt)
{
  (void)ctx;
  switch (evt->kind) {
    case k_ra_ble_mesh_evt_provisioned:
      demo_log("[mesh] provisioned\r\n");
      break;
    case k_ra_ble_mesh_evt_unprovisioned:
      demo_log("[mesh] unprovisioned\r\n");
      break;
    case k_ra_ble_mesh_evt_key_refresh:
      demo_log("[mesh] key-refresh\r\n");
      break;
    case k_ra_ble_mesh_evt_iv_update:
      demo_log("[mesh] iv-update\r\n");
      break;
    default:
      break;
  }
}

#ifndef RA_SIMULATOR_MODE
/**
 * @brief Bring NimBLE host + security + mesh stack up.
 *
 * @pre Clocks + SCI8 ready.
 * @post Mesh stack is provisioning-beaconing.
 *
 * @since 0.1.0
 */
static void demo_ble_or_halt(void)
{
  const ra_ble_config_t ble_cfg = {
    .use_external_osc  = 1U,
    .deep_sleep_enable = 0U,
  };
  if (ra_ble_open(&ble_cfg) != k_ra_ok) {
    demo_log("[mesh] ra_ble_open failed\r\n");
    demo_panic_halt();
  }
  if (ble_hci_ra_ble_init() != k_ra_ok) {
    demo_log("[mesh] ble_hci_ra_ble_init failed\r\n");
    demo_panic_halt();
  }
  nimble_port_init();

  const ra_ble_security_config_t sec_cfg = {
    .io_cap           = k_ra_ble_io_cap_no_input_no_out,
    .bonding_enable   = 1U,
    .mitm_required    = 0U,
    .sc_only          = 0U,
    .use_rsip_offload = 1U,
  };
  if (ra_ble_security_init(&sec_cfg) != k_ra_ok) {
    demo_log("[mesh] ra_ble_security_init failed\r\n");
    demo_panic_halt();
  }

  ra_ble_mesh_config_t mesh_cfg = {};
  mesh_cfg.cid                  = k_mesh_company_id_lf; /* Linux Foundation CID. */
  mesh_cfg.pid                  = 0x0001U;
  mesh_cfg.vid                  = 0x0001U;
  memcpy(mesh_cfg.uuid, k_demo_dev_uuid, sizeof(mesh_cfg.uuid));
  mesh_cfg.element_count            = 1U;
  mesh_cfg.elements[0].model_count  = 2U;
  mesh_cfg.elements[0].models[0].id = (uint16_t)k_demo_model_config_server;
  mesh_cfg.elements[0].models[1].id = (uint16_t)k_demo_model_generic_on_off;

  if (ra_ble_mesh_init(&mesh_cfg) != k_ra_ok) {
    demo_log("[mesh] ra_ble_mesh_init failed\r\n");
    demo_panic_halt();
  }
  (void)ra_ble_mesh_attach_event_handler(demo_mesh_evt, nullptr);
  if (ra_ble_mesh_prov_enable() != k_ra_ok) {
    demo_log("[mesh] prov_enable failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Worker thread.
 *
 * @param[in] arg Unused.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG arg)
{
  (void)arg;
  demo_log("[mesh] starting NimBLE mesh node\r\n");
  demo_ble_or_halt();
  demo_log("[mesh] beaconing for provisioner\r\n");
  while (1) {
    (void)ra_ble_dispatch();
    (void)tx_thread_sleep((ULONG)k_demo_idle_ms);
  }
}

/**
 * @brief ThreadX system-define hook.
 *
 * @param[in] first_unused_memory Unused.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         (CHAR*)"ble_mesh",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         (UINT)k_demo_thread_prio,
                         (UINT)k_demo_thread_prio,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler complete.
 * @post Hands off to ThreadX or panic-halts.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  demo_clocks_or_halt();
  ra_isr_globals_enable();
  demo_log("[mesh] booting on ");
  demo_log(s_demo_tag);
  demo_log("\r\n");
#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#else
  (void)demo_mesh_evt;
#endif
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
