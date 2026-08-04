/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_osi.c
 * @brief The esp-hosted OS-abstraction vtable and its non-RTOS slots.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core reaches every service it needs through one
 * 72-entry function-pointer table found behind the global ``g_h``. This
 * file owns that table: it defines ``g_hosted_osi_funcs`` and ``g_h``, it
 * calls the three slice binders that fill the RTOS, GPIO and SPI rows, and
 * it implements the rows that belong to no slice -- event posting, the
 * log shim, the restart and power-save hooks, and the sixteen rows that
 * belong to transports this board does not carry.
 *
 * @par Why the table is bound at run time rather than statically
 * A static initialiser would need all sixty-odd slot functions to have
 * external linkage and a declaration apiece, turning the port's internal
 * headers into a second copy of the vtable. Binding instead lets each
 * slice keep its implementations `static` and expose one function, which
 * is both a smaller surface and a testable unit: a binder that forgets a
 * row is a test failure rather than a null dereference at run time.
 *
 * @par Rows for absent transports
 * The SDIO, half-duplex SPI and UART rows are filled with functions that
 * report the transport as unavailable and name the slot that was called.
 * They are not silently-succeeding stubs: the C6 on this board is reached
 * over full-duplex SPI only -- there is no SDIO or UART connection to it
 * at all -- so "this transport is not present" is the truthful answer, and
 * saying it loudly is what a caller needs. Leaving the rows null would
 * turn the same mistake into a null-pointer fault with no diagnosis.
 *
 * @since 0.1.0
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_log.h"
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_spi.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_esp_hosted_gpio_internal.h"
#include "ra8_esp_hosted_log_internal.h"
#include "ra8_esp_hosted_osi_internal.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_esp_hosted_spi_internal.h"

DEFINE_LOG_TAG(osi);

/**
 * @enum ra8_esp_hosted_osi_const_t
 * @brief Fixed values the non-slice vtable rows report.
 *
 * @details
 * Named so the reasons behind each answer are readable at the point of
 * use rather than encoded as bare integers.
 *
 * @invariant ::k_ra8_esp_hosted_osi_wakeup_reboot matches the value the
 *            vendored power-save driver treats as "ordinary reboot".
 *
 * @par Example:
 * @code
 * return (int)k_ra8_esp_hosted_osi_wakeup_reboot;
 * @endcode
 *
 * @see ra8_esp_hosted_osi_bind_all
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** Queue depth for each of the two transmit and receive priority
      queues, restated here so the SPI configuration the port publishes
      matches what the transport actually creates. */
  k_ra8_esp_hosted_osi_queue_depth = (uint8_t)H_TRANSPORT_QUEUE_SIZE,
  /** SPI mode the co-processor image is built for. */
  k_ra8_esp_hosted_osi_spi_mode = 3U,
  /** Wake-up reason meaning the host performed an ordinary reboot. */
  k_ra8_esp_hosted_osi_wakeup_reboot = 1U,
} ra8_esp_hosted_osi_const_t;

/**
 * @var s_tag
 * @brief Tag the project logger attributes this module's lines to.
 * @details Distinct from the vendored ``TAG`` so a line from the port
 * itself is separable from a line the vendored core emitted.
 * @note Read-only.
 * @warning Keep it short; the sink does not wrap.
 * @since 0.1.0
 */
static const char* const s_tag = "C6OSI";

/**
 * @var s_event_base_wifi
 * @brief Namespace reported for events the core posts as Wi-Fi events.
 * @details Matches the ESP-IDF base name so an application that already
 * knows the co-processor's event vocabulary needs no translation table.
 * @note Read-only.
 * @warning Changing it silently changes what handlers must match on.
 * @since 0.1.0
 */
static const char* const s_event_base_wifi = "WIFI_EVENT";

/**
 * @var s_ra8_esp_hosted_event_cb
 * @brief Application callback invoked when the core posts a link event.
 * @details Null until an application registers one, in which case posted
 * events are reported as unconsumed rather than dropped silently.
 * @note Written only by ::ra8_esp_hosted_port_set_event_cb.
 * @warning Invoked from whichever thread posted the event.
 * @since 0.1.0
 */
static ra8_esp_hosted_event_cb_t s_ra8_esp_hosted_event_cb;

/**
 * @var s_ra8_esp_hosted_event_ctx
 * @brief Opaque context handed back to ::s_ra8_esp_hosted_event_cb.
 * @details Owned by the application; the port never dereferences it.
 * @note Written only by ::ra8_esp_hosted_port_set_event_cb.
 * @warning Must outlive the registration.
 * @since 0.1.0
 */
static void* s_ra8_esp_hosted_event_ctx;

/**
 * @var g_hosted_osi_funcs
 * @brief The OS-abstraction vtable the vendored core calls through.
 * @details Zero at reset and populated by ::ra8_esp_hosted_osi_bind_all,
 * which ``ra8_esp_hosted_port_init`` runs before anything can reach it.
 * @note The name and type are fixed by the vendored
 * ``esp_hosted_os_abstraction.h``.
 * @warning Calling through it before the port is initialised dereferences
 *          a null row; check ``ra8_esp_hosted_port_is_ready`` first.
 * @since 0.1.0
 */
hosted_osi_funcs_t g_hosted_osi_funcs;

/**
 * @var g_h
 * @brief The handle the vendored core dereferences to reach the vtable.
 * @details Statically bound to ::g_hosted_osi_funcs, matching the
 * upstream ``HOSTED_CONFIG_INIT_DEFAULT`` shape.
 * @note The name and type are fixed by the vendored
 * ``esp_hosted_os_abstraction.h``.
 * @warning Never reassign ``funcs``; the core caches nothing but expects
 *          the table to stay put.
 * @since 0.1.0
 */
struct hosted_config_t g_h = HOSTED_CONFIG_INIT_DEFAULT();

/** @brief Implementation of `ra8_esp_hosted_port_set_event_cb()` -- stores
 *  the pair atomically enough for a bring-up path; see the contract. */
ra8_err_t ra8_esp_hosted_port_set_event_cb(ra8_esp_hosted_event_cb_t cb, void* ctx)
{
  if ((cb == nullptr) && (ctx != nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  s_ra8_esp_hosted_event_cb  = cb;
  s_ra8_esp_hosted_event_ctx = ctx;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_esp_hosted_osi_dispatch_event()` -- the one
 *  decision behind both event rows, promoted so it is testable. */
RA8_PRIV
int ra8_esp_hosted_osi_dispatch_event(const char* base,
                                      int32_t     event_id,
                                      const void* data,
                                      size_t      data_len)
{
  if (s_ra8_esp_hosted_event_cb == nullptr) {
    return RET_FAIL;
  }
  if ((data == nullptr) && (data_len != 0U)) {
    return RET_INVALID;
  }
  s_ra8_esp_hosted_event_cb(s_ra8_esp_hosted_event_ctx,
                            (base == nullptr) ? "" : base,
                            event_id,
                            data,
                            data_len);
  return RET_OK;
}

/**
 * @brief Vtable row: post a generic esp-hosted event.
 *
 * @details
 * Hands the event to the registered application callback. There is no
 * ESP-IDF event loop on this host, and inventing one would add a thread
 * and a queue for a path the application can serve directly.
 *
 * @param[in] event_base Event namespace supplied by the core.
 * @param[in] event_id Event identifier within the namespace.
 * @param[in] event_data Event payload; may be null when the length is zero.
 * @param[in] event_data_size Payload length in bytes.
 * @param[in] ticks_to_wait Ignored; delivery is synchronous, so there is
 *                          nothing to wait for.
 *
 * @return Whether the event was consumed.
 * @retval RET_OK The callback ran.
 * @retval RET_FAIL No callback is registered.
 * @retval RET_INVALID The payload pointer and length disagree.
 *
 * @pre The port has been initialised.
 * @pre The caller is not holding a lock the callback also takes.
 * @post No port state is modified.
 * @post The callback has run exactly once, or not at all.
 *
 * @note Runs on the posting thread; the callback must not block it.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_event_post(esp_event_base_t event_base,
                               int32_t          event_id,
                               void*            event_data,
                               size_t           event_data_size,
                               uint32_t         ticks_to_wait)
{
  (void)ticks_to_wait;
  return ra8_esp_hosted_osi_dispatch_event(event_base, event_id, event_data, event_data_size);
}

/**
 * @brief Vtable row: post a Wi-Fi event.
 *
 * @details
 * Routed to the same application callback as the generic row, under a
 * fixed namespace, so an application sees one delivery path.
 *
 * @param[in] event_id Wi-Fi event identifier.
 * @param[in] event_data Event payload; may be null when the length is zero.
 * @param[in] event_data_size Payload length in bytes.
 * @param[in] ticks_to_wait Ignored; delivery is synchronous.
 *
 * @return Whether the event was consumed.
 * @retval RET_OK The callback ran.
 * @retval RET_FAIL No callback is registered.
 * @retval RET_INVALID The payload pointer and length disagree.
 *
 * @pre The port has been initialised.
 * @pre The caller is not holding a lock the callback also takes.
 * @post No port state is modified.
 * @post The callback has run exactly once, or not at all.
 *
 * @note Runs on the posting thread; the callback must not block it.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_event_wifi_post(int32_t  event_id,
                                    void*    event_data,
                                    size_t   event_data_size,
                                    uint32_t ticks_to_wait)
{
  (void)ticks_to_wait;
  return ra8_esp_hosted_osi_dispatch_event(s_event_base_wifi,
                                           event_id,
                                           event_data,
                                           event_data_size);
}

/**
 * @brief Vtable row: emit one formatted log line.
 *
 * @details
 * Forwards to the port's log bridge, which is the same path the
 * ``ESP_LOGx`` macros take, so a line emitted through the vtable and one
 * emitted directly are indistinguishable at the sink.
 *
 * @param[in] level ESP-IDF level of the line.
 * @param[in] tag Tag to attribute the line to; null is replaced.
 * @param[in] format Format string; null suppresses the line.
 *
 * @return Nothing.
 *
 * @pre Logging has been initialised, or the line is dropped.
 * @pre The variable arguments match the conversions in ``format``.
 * @post No port state is modified.
 * @post At most one line is emitted.
 *
 * @note Reentrant; formats onto a bounded stack line.
 * @since 0.1.0
 */
RA8_INTERNAL
[[gnu::format(printf, 3, 4)]] static void
internal_printf(int level, const char* tag, const char* format, ...)
{
  va_list ap;
  va_start(ap, format);
  ra8_esp_hosted_log_vwrite(level, tag, format, ap);
  va_end(ap);
}

/**
 * @brief Vtable row: run whatever the host needs before the core starts.
 *
 * @details
 * The core calls this once, from ``setup_transport``, to give the port a
 * last chance to prepare. Everything this port needs is already done by
 * ``ra8_esp_hosted_port_init``, which must run first, so the hook confirms
 * that and reports the pool state -- which is exactly the moment a
 * budgeting mistake is cheapest to see.
 *
 * @return Nothing.
 *
 * @pre ``ra8_esp_hosted_port_init`` has completed.
 * @pre The ThreadX kernel is running.
 * @post No port state is modified.
 * @post One pool-state line is emitted.
 *
 * @note Runs on the core's setup thread.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_init_hook(void)
{
  if (!ra8_esp_hosted_port_is_ready()) {
    ESP_LOGE(TAG, "core started before ra8_esp_hosted_port_init");
    return;
  }
  ra8_esp_hosted_mem_dump("hosted_init_hook");
}

/**
 * @brief Vtable row: restart the host processor.
 *
 * @details
 * The core reaches for this when the link is unrecoverable. This port
 * declines rather than resetting the board: the C6 link carries
 * connectivity only, and a reader that is displaying a page has far more
 * to lose from an unannounced reset than it gains from a fresh transport.
 * The refusal is reported so the condition is visible, and the
 * application decides what to do.
 *
 * @return Always the failure code.
 * @retval RET_FAIL The port does not reset the host on its own authority.
 *
 * @pre The caller has already decided the link is unrecoverable.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No board state is modified.
 * @post Exactly one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_restart_host(void)
{
  ESP_LOGE(TAG, "core asked for a host restart; declined by policy");
  return RET_FAIL;
}

/**
 * @brief Vtable row: configure the host's power-save wake source.
 *
 * @details
 * Host power save is disabled in this build (``H_HOST_PS_ALLOWED`` is
 * zero), so the core never reaches this row. It reports the request
 * rather than pretending to have configured a wake source that would then
 * never fire.
 *
 * @param[in] power_save_type Requested power-save mode.
 * @param[in] gpio_port Wake pin's port, in the port's encoding.
 * @param[in] gpio_num Wake pin's index within that port.
 * @param[in] level Level that should wake the host.
 *
 * @return Always the failure code.
 * @retval RET_FAIL Host power save is not enabled in this build.
 *
 * @pre ``H_HOST_PS_ALLOWED`` is zero, so this row is unreachable.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No board state is modified.
 * @post Exactly one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_config_host_power_save(uint32_t power_save_type,
                                           void*    gpio_port,
                                           uint32_t gpio_num,
                                           int      level)
{
  (void)gpio_port;
  ESP_LOGW(TAG,
           "host power save %u not enabled (pin %u level %d)",
           (unsigned int)power_save_type,
           (unsigned int)gpio_num,
           level);
  return RET_FAIL;
}

/**
 * @brief Vtable row: enter host power save.
 *
 * @details
 * Companion to ::internal_config_host_power_save and unreachable for the
 * same reason.
 *
 * @param[in] power_save_type Requested power-save mode.
 *
 * @return Always the failure code.
 * @retval RET_FAIL Host power save is not enabled in this build.
 *
 * @pre ``H_HOST_PS_ALLOWED`` is zero, so this row is unreachable.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No board state is modified.
 * @post Exactly one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_start_host_power_save(uint32_t power_save_type)
{
  ESP_LOGW(TAG, "host power save %u not enabled", (unsigned int)power_save_type);
  return RET_FAIL;
}

/**
 * @union ra8_esp_hosted_osi_view
 * @brief Row-wise view of the vtable, for the completeness scan.
 *
 * @details
 * Reading a structure of function pointers through an array of function
 * pointers is only defined through a union, so the scan uses one. The
 * accompanying static assertion is what makes the view safe: it fails the
 * build if the vendored table ever gains a member that is not a function
 * pointer, rather than letting the scan silently misread it.
 *
 * @invariant Both members are exactly the same size.
 * @invariant Every member of the table is a function pointer.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_osi_view_t view = { .table = *table };
 * @endcode
 *
 * @see ra8_esp_hosted_osi_is_complete
 * @since 0.1.0
 */
typedef union ra8_esp_hosted_osi_view {
  hosted_osi_funcs_t table; /**< The table as the vendored core sees it. */
  /** The same bytes as a flat array of rows. */
  void (*rows[sizeof(hosted_osi_funcs_t) / sizeof(void (*)(void))])(void);
} ra8_esp_hosted_osi_view_t;

static_assert(sizeof(hosted_osi_funcs_t) % sizeof(void (*)(void)) == 0U,
              "the esp-hosted vtable must be a whole number of function pointers");

/** @brief Implementation of `ra8_esp_hosted_osi_is_complete()` -- scans the
 *  table row-wise through a union, so a row added upstream is covered
 *  without editing a field list. */
RA8_PRIV
bool ra8_esp_hosted_osi_is_complete(const hosted_osi_funcs_t* table)
{
  if (table == nullptr) {
    return false;
  }

  ra8_esp_hosted_osi_view_t view = {.table = *table};
  const uint32_t            rows = (uint32_t)(sizeof(view.rows) / sizeof(view.rows[0]));

  for (uint32_t i = 0U; i < rows; i++) {
    if (view.rows[i] == nullptr) {
      return false;
    }
  }
  return true;
}

/** @brief Implementation of `ra8_esp_hosted_osi_bind_all()` -- fills every
 *  row, then proves none was missed. */
RA8_PRIV
ra8_err_t ra8_esp_hosted_osi_bind_all(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable");

  *out = (hosted_osi_funcs_t){};

  ra8_err_t err = ra8_esp_hosted_rtos_bind(out);
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_rtos_bind_pool(out);
  }
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_rtos_bind_sync(out);
  }
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_gpio_bind(out);
  }
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_spi_bind(out);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_esp_hosted_osi_bind_absent(out);

  out->_h_event_post                      = internal_event_post;
  out->_h_event_wifi_post                 = internal_event_wifi_post;
  out->_h_printf                          = internal_printf;
  out->_h_hosted_init_hook                = internal_init_hook;
  out->_h_restart_host                    = internal_restart_host;
  out->_h_config_host_power_save_hal_impl = internal_config_host_power_save;
  out->_h_start_host_power_save_hal_impl  = internal_start_host_power_save;

  if (!ra8_esp_hosted_osi_is_complete(out)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/* Every field below comes from the port headers, so the configuration the
   vendored core reads and the pins the port actually drives cannot
   disagree. Written as a plain comment rather than a Doxygen block: the
   contract lives on the declaration in the vendored
   `esp_hosted_transport_config.h`, and a Doxygen block here would attach
   itself to the struct return type rather than to the function. */
struct esp_hosted_spi_config esp_hosted_get_default_spi_config(void)
{
  struct esp_hosted_spi_config cfg = {};

  /* LEGACY-OK: pin_mosi / pin_miso are vendored esp-hosted field names. */
  cfg.pin_mosi.port = RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_copi);
  cfg.pin_mosi.pin  = RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_copi);
  /* LEGACY-OK: pin_mosi / pin_miso are vendored esp-hosted field names. */
  cfg.pin_miso.port       = RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_cipo);
  cfg.pin_miso.pin        = RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_cipo);
  cfg.pin_sclk.port       = RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_sck);
  cfg.pin_sclk.pin        = RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_sck);
  cfg.pin_cs.port         = RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_chip_select);
  cfg.pin_cs.pin          = RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_chip_select);
  cfg.pin_handshake.port  = H_GPIO_HANDSHAKE_Port;
  cfg.pin_handshake.pin   = H_GPIO_HANDSHAKE_Pin;
  cfg.pin_data_ready.port = H_GPIO_DATA_READY_Port;
  cfg.pin_data_ready.pin  = H_GPIO_DATA_READY_Pin;
  cfg.pin_reset.port      = H_GPIO_PORT_RESET;
  cfg.pin_reset.pin       = H_GPIO_PIN_RESET;

  cfg.tx_queue_size = (uint16_t)k_ra8_esp_hosted_osi_queue_depth;
  cfg.rx_queue_size = (uint16_t)k_ra8_esp_hosted_osi_queue_depth;
  cfg.mode          = (uint8_t)k_ra8_esp_hosted_osi_spi_mode;
  cfg.clk_mhz       = (uint32_t)H_SPI_FD_CLK_MHZ;

  return cfg;
}
