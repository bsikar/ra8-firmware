/**
 * @file examples/_unsupported/ble_peripheral/main.c
 * @brief BLE peripheral advertising the GATT Battery Service smoke test
 *        (Bluetooth Core 5.3 -- ``EK-RA8D2`` device, 0x180F service)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()``, opens SCI8 for log output,
 * brings up the BLE host stack via ``ra_ble_host_init`` in the
 * peripheral GAP role (Bluetooth Core 5.3 Vol 3 Part C 2.2.2), registers
 * a Battery Service (UUID 0x180F, Bluetooth Assigned Numbers 3.4) with
 * a single Battery Level characteristic (UUID 0x2A19, Read | Notify --
 * Bluetooth Core 5.3 Vol 3 Part G 3.3.1.1), and starts connectable
 * undirected advertising under the local-name ``EK-RA8D2``.
 *
 * The main loop decrements the battery level by one every ten seconds
 * (wrapping back to 100 at zero). Whenever the connected peer has
 * subscribed to notifications via the CCCD (UUID 0x2902,
 * Bluetooth Core 5.3 Vol 3 Part F 3.3.3.3), the new value is pushed
 * out via ``ra_ble_host_gatt_notify``. Connection / disconnection /
 * subscribe / write events are surfaced through the registered host
 * event callback and logged over SCI8.
 *
 * ## Verification
 *
 * Open ``nRF Connect for Mobile`` (Nordic Semiconductor) on a phone
 * and start a scan: the device should show up as ``EK-RA8D2``. Tap
 * to connect, expand the Battery Service (0x180F), tap the down-arrow
 * on the Battery Level characteristic to enable notifications, and
 * watch the value tick down once every ten seconds.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_ble.h"
#include "ra_ble_host.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/**
 * @enum ble_peripheral_config_t
 * @brief Numeric configuration constants for the demo.
 *
 * @details
 * Every literal used by the bring-up path lives here so the magic-
 * number lint never sees a bare integer. The SCI8 channel matches the
 * on-board J-Link OB CDC bridge pins (PD_02 / PD_03).
 */
typedef enum : uint32_t {
  k_ble_peripheral_baud        = 115200U, /**< J-Link OB CDC log baud.            */
  k_ble_peripheral_sci_channel = 8U,      /**< SCI8 logging channel.              */
  k_ble_peripheral_tick_ms     = 10000U,  /**< 10 s between battery-level steps.  */
  k_ble_peripheral_adv_int_ms  = 100U,    /**< 100 ms advertising interval.       */
} ble_peripheral_config_t;

/**
 * @enum ble_peripheral_battery_t
 * @brief Battery Level characteristic policy.
 *
 * @details Battery Level is a percentage in the range 0..100
 * (Bluetooth Core 5.3 Service Specifications -- Battery Service 1.0
 * sec 3.1).
 */
typedef enum : uint8_t {
  k_ble_peripheral_battery_max  = 100U, /**< Maximum percentage.                   */
  k_ble_peripheral_battery_min  = 0U,   /**< Minimum percentage (rolls to max).    */
  k_ble_peripheral_battery_step = 1U,   /**< Decrement amount per tick.            */
  k_ble_peripheral_battery_init = 50U,  /**< Initial reading at boot.              */
} ble_peripheral_battery_t;

/**
 * @enum ble_peripheral_uuid_t
 * @brief 16-bit UUID handles for the Battery Service.
 *
 * @details Bluetooth Assigned Numbers 3.4 ("Services") and 3.8
 * ("Characteristics by Name").
 */
typedef enum : uint16_t {
  k_ble_uuid_battery_service = 0x180FU, /**< Battery Service.                  */
  k_ble_uuid_battery_level   = 0x2A19U, /**< Battery Level characteristic.     */
} ble_peripheral_uuid_t;

/**
 * @enum ble_peripheral_adv_type_t
 * @brief AD-type values used in legacy advertising data.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part C 11 "Advertising and Scan
 * Response Data Format".
 */
typedef enum : uint8_t {
  k_ble_ad_flags                 = 0x01U, /**< Flags AD type.                       */
  k_ble_ad_complete_uuid16       = 0x03U, /**< Complete List of 16-bit UUIDs.       */
  k_ble_ad_complete_local_name   = 0x09U, /**< Complete Local Name.                 */
  k_ble_ad_flags_le_general_disc = 0x06U, /**< LE General Discoverable + BR/EDR not */
                                          /**< supported (bits 1 + 2).              */
} ble_peripheral_adv_type_t;

/**
 * @enum ble_peripheral_layout_t
 * @brief Byte indices and lengths used to assemble the advertising data.
 */
typedef enum : uint8_t {
  k_ble_idx_byte_low   = 0U,    /**< LSB of a 16-bit value (little-endian).         */
  k_ble_idx_byte_high  = 1U,    /**< MSB of a 16-bit value.                         */
  k_ble_uuid128_bytes  = 16U,   /**< 128-bit UUID byte length.                     */
  k_ble_shift_byte     = 8U,    /**< 8-bit shift used in 16-bit fold/unfold.        */
  k_ble_local_name_len = 8U,    /**< strlen("EK-RA8D2") -- ASCII, no NUL.            */
  k_ble_byte_mask      = 0xFFU, /**< Low-byte mask for 16-bit splits.            */
  k_ble_adv_buf_cap    = 31U,   /**< k_ra_ble_adv_data_max ceiling.                 */
  k_ble_struct_flags   = 2U,    /**< AD struct length for Flags.                    */
  k_ble_struct_uuid16  = 3U,    /**< AD struct length for one 16-bit UUID.          */
  k_ble_struct_min_cap = 16U,   /**< Minimum AD buffer size used by build_adv.      */
} ble_peripheral_layout_t;

/**
 * @brief Local-name string broadcast in adv-data and surfaced to GATT.
 *
 * @details Strict ASCII so the project's ASCII-only policy holds.
 */
static const char k_ble_peripheral_name[] = "EK-RA8D2";

/** @brief SCI8 / J-Link OB CDC pins (TXD8 / RXD8 -- PD_02 / PD_03). */
static const ra_port_pin_t k_ble_peripheral_pin_log_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_ble_peripheral_pin_log_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief Shadow value backing the Battery Level characteristic.
 *
 * @details Lives in BSS so ``ra_ble_host_gatt_register_char`` can hand
 * the host stack a writable backing buffer. The main loop updates the
 * shadow and calls ``ra_ble_host_gatt_set_value`` to keep the host's
 * cache aligned before each notification.
 */
static uint8_t s_ble_peripheral_battery_level = k_ble_peripheral_battery_init;

/**
 * @brief Cached value-attribute handle for the Battery Level char.
 *
 * @details Captured during init so the main-loop ``ra_ble_host_gatt_notify``
 * call has the right ATT handle. Value 0 is reserved per Bluetooth Core
 * 5.3 Vol 3 Part F 3.2.2 ("Attribute Handle"), so any real handle is
 * non-zero.
 */
static uint16_t s_ble_peripheral_level_handle = 0U;

/**
 * @brief Tag used in SCI8 / ra_log output to identify this app.
 */
static const char* s_ble_peripheral_tag = "ble";

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void ble_peripheral_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Send a NUL-terminated ASCII line over SCI8 (best-effort).
 *
 * @param[in] s ASCII string (NUL-terminated). May be ``nullptr``.
 *
 * @pre ra_sci_init() succeeded for the SCI8 channel.
 * @post Bytes have been polled out of TXD8 (or silently discarded on
 *       backpressure -- this is logging only).
 *
 * @since 0.1.0
 */
static void ble_peripheral_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra_sci_write_polling((uint8_t)k_ble_peripheral_sci_channel, (const uint8_t*)s, len);
}

/**
 * @brief Build a 128-bit UUID buffer from a 16-bit Bluetooth SIG UUID.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part B 2.5.1 "UUID" defines the 16-bit ->
 * 128-bit promotion: ``0000xxxx-0000-1000-8000-00805F9B34FB`` little-
 * endian. ``ra_ble_host_gatt_register_*`` wants the 16-byte form
 * regardless of whether the underlying value is 16-bit assigned.
 *
 * @param[out] out      16-byte destination buffer.
 * @param[in]  uuid_16  16-bit Bluetooth SIG UUID.
 *
 * @pre out points to at least 16 writable bytes.
 * @post out holds the little-endian 128-bit UUID.
 *
 * @since 0.1.0
 */
static void ble_peripheral_uuid16_to_128(uint8_t* out, uint16_t uuid_16)
{
  /* Bluetooth Base UUID (LE byte order):
   * FB-34-9B-5F-80-00-00-80-00-10-00-00-XX-XX-00-00. */
  enum : uint8_t {
    k_idx_uuid_lo = 12U,
    k_idx_uuid_hi = 13U,
  };
  static const uint8_t base[k_ble_uuid128_bytes] = {
    0xFBU,
    0x34U,
    0x9BU,
    0x5FU,
    0x80U,
    0x00U,
    0x00U,
    0x80U,
    0x00U,
    0x10U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
  };
  for (uint8_t i = 0U; i < k_ble_uuid128_bytes; i++) {
    out[i] = base[i];
  }
  out[k_idx_uuid_lo] = (uint8_t)(uuid_16 & k_ble_byte_mask);
  out[k_idx_uuid_hi] = (uint8_t)((uuid_16 >> k_ble_shift_byte) & k_ble_byte_mask);
}

/**
 * @brief Append the Flags AD field to the advertising buffer.
 *
 * @param[out] adv_buf Destination AD buffer.
 * @param[in]  pos     Current write offset.
 *
 * @return New write offset after appending the Flags struct.
 *
 * @pre adv_buf has room for 3 more bytes at offset pos.
 * @post adv_buf[pos..pos+2] holds a valid Flags AD struct.
 *
 * @since 0.1.0
 */
static uint8_t ble_peripheral_adv_append_flags(uint8_t* adv_buf, uint8_t pos)
{
  adv_buf[pos] = k_ble_struct_flags;
  pos++;
  adv_buf[pos] = k_ble_ad_flags;
  pos++;
  adv_buf[pos] = k_ble_ad_flags_le_general_disc;
  pos++;
  return pos;
}

/**
 * @brief Append the Complete UUID16 List AD field for the Battery Service.
 *
 * @param[out] adv_buf Destination AD buffer.
 * @param[in]  pos     Current write offset.
 *
 * @return New write offset after appending the UUID16 struct.
 *
 * @pre adv_buf has room for 4 more bytes at offset pos.
 * @post adv_buf[pos..pos+3] holds a Complete UUID16 List AD struct
 *       advertising the Battery Service (0x180F).
 *
 * @since 0.1.0
 */
static uint8_t ble_peripheral_adv_append_uuid16(uint8_t* adv_buf, uint8_t pos)
{
  const uint16_t uuid = k_ble_uuid_battery_service;
  adv_buf[pos]        = k_ble_struct_uuid16;
  pos++;
  adv_buf[pos] = k_ble_ad_complete_uuid16;
  pos++;
  adv_buf[pos] = (uint8_t)(uuid & k_ble_byte_mask);
  pos++;
  adv_buf[pos] = (uint8_t)((uuid >> k_ble_shift_byte) & k_ble_byte_mask);
  pos++;
  return pos;
}

/**
 * @brief Append the Complete Local Name AD field with the demo name.
 *
 * @param[out] adv_buf Destination AD buffer.
 * @param[in]  pos     Current write offset.
 *
 * @return New write offset after appending the Local Name struct.
 *
 * @pre adv_buf has room for 2 + k_ble_local_name_len bytes at offset pos.
 * @post adv_buf[pos..pos+9] holds a Complete Local Name AD struct.
 *
 * @since 0.1.0
 */
static uint8_t ble_peripheral_adv_append_name(uint8_t* adv_buf, uint8_t pos)
{
  adv_buf[pos] = (uint8_t)(k_ble_local_name_len + 1U);
  pos++;
  adv_buf[pos] = k_ble_ad_complete_local_name;
  pos++;
  for (uint8_t i = 0U; i < k_ble_local_name_len; i++) {
    adv_buf[pos] = (uint8_t)k_ble_peripheral_name[i];
    pos++;
  }
  return pos;
}

/**
 * @brief Assemble the legacy 31-byte advertising payload.
 *
 * @details
 * Layout (Bluetooth Core 5.3 Vol 3 Part C 11 "AD Format"):
 *
 *   - <len=2><type=Flags><LE General Discoverable + BR/EDR not supported>
 *   - <len=3><type=Complete UUID16 list><0x180F (Battery Service)>
 *   - <len=N+1><type=Complete Local Name><"EK-RA8D2">
 *
 * @param[out] adv_buf   Destination buffer.
 * @param[in]  cap_bytes Capacity of ``adv_buf`` in bytes.
 *
 * @return Number of bytes written into ``adv_buf``.
 *
 * @pre adv_buf != nullptr.
 * @pre cap_bytes >= 16.
 * @post adv_buf holds a valid AD-format advertising payload.
 *
 * @since 0.1.0
 */
static uint8_t ble_peripheral_build_adv(uint8_t* adv_buf, uint8_t cap_bytes)
{
  if (adv_buf == nullptr || cap_bytes < k_ble_struct_min_cap) {
    return 0U;
  }
  uint8_t pos = 0U;
  pos         = ble_peripheral_adv_append_flags(adv_buf, pos);
  pos         = ble_peripheral_adv_append_uuid16(adv_buf, pos);
  pos         = ble_peripheral_adv_append_name(adv_buf, pos);
  return pos;
}

/**
 * @brief Application event handler -- log connect/disconnect/subscribe.
 *
 * @param[in] ctx Unused context (registered as ``nullptr``).
 * @param[in] evt Event payload from the BLE host stack.
 *
 * @pre evt != nullptr.
 * @post One log line emitted per event kind.
 *
 * @note Invoked from the host stack's HCI/L2CAP dispatch path.
 *
 * @since 0.1.0
 */
static void ble_peripheral_event_cb(void* ctx, const ra_ble_host_event_t* evt)
{
  (void)ctx;
  if (evt == nullptr) {
    return;
  }
  switch (evt->kind) {
    case k_ra_ble_host_event_connected:
      ble_peripheral_log("ble: connected\r\n");
      break;
    case k_ra_ble_host_event_disconnected:
      ble_peripheral_log("ble: disconnected\r\n");
      break;
    case k_ra_ble_host_event_subscribe:
      ble_peripheral_log("ble: subscribed\r\n");
      break;
    case k_ra_ble_host_event_write:
      ble_peripheral_log("ble: write\r\n");
      break;
    default:
      /* Unknown event kind -- drop. */
      break;
  }
}

/**
 * @brief Register the Battery Service + Battery Level characteristic.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part G 3.3.1.1 -- Read | Notify properties
 * mean the ``ra_ble_host`` stack auto-creates the Client Characteristic
 * Configuration Descriptor (CCCD, UUID 0x2902).
 *
 * @return ``ra_err_t`` Error code from the underlying registration calls.
 *
 * @retval k_ra_ok                  Service + characteristic installed.
 * @retval k_ra_err_no_mem          Attribute table full.
 * @retval k_ra_err_invalid_arg     Properties bitmask invalid.
 *
 * @pre ``ra_ble_host_init`` returned k_ra_ok.
 * @post ``s_ble_peripheral_level_handle`` is set to a non-zero ATT
 *       handle on success.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ble_peripheral_register_gatt(void)
{
  uint8_t svc_uuid[k_ble_uuid128_bytes] = {};
  uint8_t chr_uuid[k_ble_uuid128_bytes] = {};
  ble_peripheral_uuid16_to_128(svc_uuid, k_ble_uuid_battery_service);
  ble_peripheral_uuid16_to_128(chr_uuid, k_ble_uuid_battery_level);

  uint16_t       svc_handle = 0U;
  const ra_err_t err        = ra_ble_host_gatt_register_service(svc_uuid, &svc_handle);
  if (err != k_ra_ok) {
    return err;
  }

  const uint8_t props = (uint8_t)(k_ra_ble_host_char_prop_read | k_ra_ble_host_char_prop_notify);
  return ra_ble_host_gatt_register_char(svc_handle,
                                        chr_uuid,
                                        props,
                                        &s_ble_peripheral_battery_level,
                                        (uint16_t)sizeof(s_ble_peripheral_battery_level),
                                        &s_ble_peripheral_level_handle);
}

/**
 * @brief Bring up CGC + SysTick + GPIO basics. Panic-halts on fail.
 *
 * @param[out] cpuclk0_hz Receives the CPUCLK0 rate.
 * @param[out] pclka_hz   Receives the PCLKA rate.
 *
 * @pre cpuclk0_hz / pclka_hz non-null.
 * @post Clocks brought up; LED1 ready.
 *
 * @since 0.1.0
 */
static void ble_peripheral_clocks_or_halt(uint32_t* cpuclk0_hz, uint32_t* pclka_hz)
{
  if (ra_cgc_init() != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, cpuclk0_hz) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, pclka_hz) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_time_init(*cpuclk0_hz) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
}

/**
 * @brief Bring SCI8 up for log output.
 *
 * @param[in] pclka_hz PCLKA rate from ra_cgc_get_clock_hz.
 *
 * @pre pclka_hz > 0.
 * @post SCI8 ready at 115200 8N1 on PD_02 / PD_03.
 *
 * @since 0.1.0
 */
static void ble_peripheral_sci_or_halt(uint32_t pclka_hz)
{
  if (ra_pfs_route_peripheral(k_ble_peripheral_pin_log_tx,
                              k_ra_psel_sci_async,
                              "ble_peripheral.log_tx") != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ble_peripheral_pin_log_rx,
                              k_ra_psel_sci_async,
                              "ble_peripheral.log_rx") != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_ble_peripheral_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_ble_peripheral_sci_channel, &sci_cfg) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
}

/**
 * @brief Bring the BLE host stack + GATT table + advertising up.
 *
 * @pre Clocks + SCI8 are already initialised.
 * @post Stack is in peripheral role and connectable advertising on the
 *       primary channels.
 *
 * @since 0.1.0
 */
static void ble_peripheral_stack_or_halt(void)
{
  const ra_ble_host_config_t ble_cfg = {
    .role       = k_ra_ble_host_role_peripheral,
    .name       = k_ble_peripheral_name,
    .appearance = 0U,
  };
  if (ra_ble_host_init(&ble_cfg) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ra_ble_host_attach_event_handler(ble_peripheral_event_cb, nullptr) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  if (ble_peripheral_register_gatt() != k_ra_ok) {
    ble_peripheral_panic_halt();
  }

  uint8_t       adv_buf[k_ble_adv_buf_cap] = {};
  const uint8_t adv_len                    = ble_peripheral_build_adv(adv_buf, k_ble_adv_buf_cap);
  if (ra_ble_host_advertise_start(adv_buf,
                                  adv_len,
                                  nullptr,
                                  0U,
                                  (uint16_t)k_ble_peripheral_adv_int_ms) != k_ra_ok) {
    ble_peripheral_panic_halt();
  }
  ra_log_info(s_ble_peripheral_tag, "advertising");
  ble_peripheral_log("ble: advertising\r\n");
}

/**
 * @brief Bring CGC + SysTick + SCI8 + BLE host up. Panic-halts on fail.
 *
 * @details
 * Mirrors the other examples: clocks first, then logging, then the
 * BLE host stack with role peripheral and the local name. After
 * registering the Battery Service the function builds a legacy AD
 * payload and starts connectable undirected advertising at the 100 ms
 * default interval (Bluetooth Core 5.3 Vol 4 Part E 7.8.5).
 *
 * @since 0.1.0
 */
static void ble_peripheral_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  ble_peripheral_clocks_or_halt(&cpuclk0_hz, &pclka_hz);
  ble_peripheral_sci_or_halt(pclka_hz);
  ble_peripheral_stack_or_halt();
}

/**
 * @brief Run one tick of the battery-level state machine.
 *
 * @details Decrements the shadow by one (wrapping back to 100 at
 * zero), pushes it into the host stack's value cache, and notifies
 * any subscribers.
 *
 * @pre ``s_ble_peripheral_level_handle`` is non-zero.
 * @post ``s_ble_peripheral_battery_level`` decremented (with wrap).
 * @post On subscribed peers a Handle Value Notification is sent.
 *
 * @since 0.1.0
 */
static void ble_peripheral_tick_battery(void)
{
  if (s_ble_peripheral_battery_level == k_ble_peripheral_battery_min) {
    s_ble_peripheral_battery_level = k_ble_peripheral_battery_max;
  } else {
    s_ble_peripheral_battery_level =
      (uint8_t)(s_ble_peripheral_battery_level - k_ble_peripheral_battery_step);
  }

  (void)ra_ble_host_gatt_set_value(s_ble_peripheral_level_handle,
                                   &s_ble_peripheral_battery_level,
                                   (uint16_t)sizeof(s_ble_peripheral_battery_level));
  (void)ra_ble_host_gatt_notify(s_ble_peripheral_level_handle);
  (void)ra_board_led_toggle(k_ra_board_led1);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + BLE host, registers the
 *        Battery Service, starts advertising, then ticks the level
 *        every ten seconds forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the battery-tick loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  ble_peripheral_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    ra_delay_ms(k_ble_peripheral_tick_ms);
    ble_peripheral_tick_battery();
  }

  ble_peripheral_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
