/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_composite.c
 * @brief Native USB device-side composite-class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode bring-up paths in `ra8_usb` to a class-agnostic
 * composite dispatcher so the EK-RA8D2 can present multiple USB
 * function classes (CDC + HID + MSC + ...) over a single physical
 * connection. This file is the native composite layer; FSP's
 * `r_usb_composite` `.template` reference templates are inspiration
 * only -- no FSP source is pulled in verbatim.
 *
 * Mapping vs FSP (FSP element -> our entry point):
 *
 *  - `g_apl_device` / `g_apl_configuration` (template-supplied
 *    descriptor blobs) -> caller-owned descriptors stored via
 *    `ra8_usb_composite_set_descriptors`.
 *  - FSP's per-template `usb_pcdc_*_descriptor.c` interface dispatch
 *    (an if / else ladder on `wIndex`) -> registry-walk in
 *    `internal_route_class`.
 *  - FSP's chapter-9 SETUP handler (`r_usb_pstdrequest.c`) ->
 *    `internal_handle_standard`.
 *
 * Dispatch state machine -- per SETUP:
 *
 *   IDLE -> SETUP_RX (8-byte SETUP arrived) -> classify by
 *   `bmRequestType.type`:
 *     - type == STANDARD -> STD_DISPATCH (composite layer answers).
 *     - type == CLASS    -> CLASS_DISPATCH (route on `wIndex` to the
 *                            registered class layer that owns that
 *                            interface number).
 *   ... -> DONE -> IDLE.
 *
 * Reference: USB 2.0 spec sec 9.3 "USB Device Requests" + sec 9.4
 * "Standard Device Requests"; Interface Association Descriptor ECN
 * (USB-IF, 2003-07-23) for the IAD type identifier (0x0B) used in
 * composite configuration descriptors.
 */

#include "ra8_usb_composite.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"

static const char* s_tag = "USBCOMP";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/** @brief Low-byte mask for the setup-packet wIndex. */
typedef enum : uint16_t {
  k_usbc_byte_mask = 0xFFU, /**< Usbc byte mask. */
} usbc_mask_t;

/**
 * @enum ra8_usb_composite_state_t
 * @brief Composite dispatch state-machine phases.
 *
 * @details Each SETUP packet transitions IDLE -> SETUP_RX ->
 * (STD_DISPATCH | CLASS_DISPATCH) -> DONE -> IDLE. The starter pumps
 * the machine one phase per `ra8_usb_composite_step` call.
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_state_idle           = 0U, /**< No SETUP in flight.       */
  k_ra8_usb_composite_state_setup_rx       = 1U, /**< SETUP arrived.            */
  k_ra8_usb_composite_state_std_dispatch   = 2U, /**< Standard request handler. */
  k_ra8_usb_composite_state_class_dispatch = 3U, /**< Class-routed dispatch.    */
  k_ra8_usb_composite_state_done           = 4U, /**< Reply staged.             */
} ra8_usb_composite_state_t;

/**
 * @enum ra8_usb_composite_request_type_t
 * @brief `bmRequestType` type-field values (USB 2.0 sec 9.3.1).
 *
 * @details Bits [6:5] of `bmRequestType`: 0 = STANDARD, 1 = CLASS,
 * 2 = VENDOR. The composite layer answers STANDARD itself and routes
 * CLASS / VENDOR via interface number.
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_req_type_mask     = 0x60U, /**< Mask for type field. */
  k_ra8_usb_composite_req_type_standard = 0x00U, /**< STANDARD request.    */
  k_ra8_usb_composite_req_type_class    = 0x20U, /**< CLASS request.       */
  k_ra8_usb_composite_req_type_vendor   = 0x40U, /**< VENDOR request.      */
} ra8_usb_composite_request_type_t;

/**
 * @enum ra8_usb_composite_recipient_t
 * @brief `bmRequestType` recipient-field values (USB 2.0 sec 9.3.1).
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_recipient_mask      = 0x1FU, /**< Mask for recipient.  */
  k_ra8_usb_composite_recipient_device    = 0x00U, /**< Device recipient.    */
  k_ra8_usb_composite_recipient_interface = 0x01U, /**< Interface recipient. */
  k_ra8_usb_composite_recipient_endpoint  = 0x02U, /**< Endpoint recipient.  */
} ra8_usb_composite_recipient_t;

/**
 * @enum ra8_usb_composite_std_request_t
 * @brief Standard `bRequest` codes the composite layer answers
 *        (USB 2.0 sec 9.4 Table 9-4).
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_std_get_status        = 0x00U, /**< GET_STATUS.        */
  k_ra8_usb_composite_std_clear_feature     = 0x01U, /**< CLEAR_FEATURE.     */
  k_ra8_usb_composite_std_set_feature       = 0x03U, /**< SET_FEATURE.       */
  k_ra8_usb_composite_std_set_address       = 0x05U, /**< SET_ADDRESS.       */
  k_ra8_usb_composite_std_get_descriptor    = 0x06U, /**< GET_DESCRIPTOR.    */
  k_ra8_usb_composite_std_set_descriptor    = 0x07U, /**< SET_DESCRIPTOR.    */
  k_ra8_usb_composite_std_get_configuration = 0x08U, /**< GET_CONFIGURATION. */
  k_ra8_usb_composite_std_set_configuration = 0x09U, /**< SET_CONFIGURATION. */
  k_ra8_usb_composite_std_get_interface     = 0x0AU, /**< GET_INTERFACE.     */
  k_ra8_usb_composite_std_set_interface     = 0x0BU, /**< SET_INTERFACE.     */
} ra8_usb_composite_std_request_t;

/**
 * @enum ra8_usb_composite_address_limit_t
 * @brief USB address ceiling (USB 2.0 sec 9.4.6).
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_max_address = 127U, /**< 7-bit USB address. */
} ra8_usb_composite_address_limit_t;

/**
 * @enum ra8_usb_composite_handler_sentinel_t
 * @brief Marker the dispatch helper writes when the composite layer
 *        itself answered (no class fired).
 *
 * @details Returned via `out_handler_class` when a STANDARD request is
 * dispatched. Equals `k_ra8_usb_composite_max_classes` so a direct
 * comparison with the registered-class index is unambiguous.
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_handler_self =
    (uint8_t)k_ra8_usb_composite_max_classes, /**< RA8 USB composite handler self. */
} ra8_usb_composite_handler_sentinel_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_composite_state_data_t
 * @brief Singleton shadow state for the composite-class driver.
 */
typedef struct {
  bool                      initialized; /**< True after init.              */
  ra8_usb_speed_t           speed;       /**< Underlying controller.        */
  ra8_usb_composite_state_t state;       /**< Dispatch state machine phase. */
  uint8_t                   class_count; /**< Registered classes count.     */
  /** Table. */
  ra8_usb_composite_class_t classes[k_ra8_usb_composite_max_classes];
  uint8_t                   if_owner_plus_one[k_ra8_usb_composite_max_ifs];
  /**< IF -> class index + 1 (0 = unowned). */
  const uint8_t* device_desc;        /**< Cached device descriptor.    */
  const uint8_t* config_desc;        /**< Cached config descriptor.    */
  uint8_t        last_handler_class; /**< Last dispatched class index. */
} ra8_usb_composite_state_data_t;

static ra8_usb_composite_state_data_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reset every IF-ownership slot to "unowned".
 *
 * @details A slot value of 0 means "no class owns this IF"; a slot
 * value of `class_index + 1` means class `class_index` owns it. The
 * `+1` bias keeps a single `uint8_t` array (no "magic" sentinel
 * needed) and lets `if_owner_plus_one[i] != 0` be the collision check.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_clear_if_owners(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_composite_max_ifs; ++i) {
    s_state.if_owner_plus_one[i] = 0U;
  }
}

/**
 * @brief Validate caller-supplied class-layer record.
 *
 * @details Per the public contract: all three callbacks must be
 * non-NULL, `interface_number_count >= 1`, and the requested IF range
 * must fit under `k_ra8_usb_composite_max_ifs`.
 *
 * @param[in] cl See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_class(const ra8_usb_composite_class_t* cl)
{
  if (cl == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((cl->init == nullptr) || (cl->handle_setup == nullptr) || (cl->close == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (cl->interface_number_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint16_t end = (uint16_t)cl->interface_number_first + (uint16_t)cl->interface_number_count;
  if (end > (uint16_t)k_ra8_usb_composite_max_ifs) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Detect IF-range overlap with already-registered classes.
 *
 * @details Walks `[first, first+count)` and returns
 * `k_ra8_err_exists` if any slot is already owned (i.e. nonzero in
 * `if_owner_plus_one`). USB 2.0 sec 9.6.5 requires interface numbers
 * to be unique within a configuration.
 *
 * @param[in] cl See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_collision(const ra8_usb_composite_class_t* cl)
{
  for (uint8_t i = 0U; i < cl->interface_number_count; ++i) {
    const uint8_t idx = cl->interface_number_first + i;
    if (s_state.if_owner_plus_one[idx] != 0U) {
      return k_ra8_err_exists;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Mark each IF in a class's range as owned by `class_index`.
 *
 * @details Stored as `class_index + 1` so 0 retains "unowned"
 * semantics. The class index is later recovered with `slot - 1`.
 *
 * @param[in] cl See implementation.
 * @param[in] class_index See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mark_ownership(const ra8_usb_composite_class_t* cl, uint8_t class_index)
{
  for (uint8_t i = 0U; i < cl->interface_number_count; ++i) {
    const uint8_t idx              = cl->interface_number_first + i;
    s_state.if_owner_plus_one[idx] = (uint8_t)(class_index + 1U);
  }
}

/**
 * @brief Find the registered class that owns interface `if_num`.
 *
 * @details Returns the class index in `*out_idx` and `k_ra8_ok`, or
 * `k_ra8_err_not_found` if `if_num` is not in any registered range.
 *
 * @param[in] if_num See implementation.
 * @param[in] out_idx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lookup_class_for_if(uint8_t if_num, uint8_t* out_idx)
{
  if (if_num >= (uint8_t)k_ra8_usb_composite_max_ifs) {
    return k_ra8_err_not_found;
  }
  const uint8_t slot = s_state.if_owner_plus_one[if_num];
  if (slot == 0U) {
    return k_ra8_err_not_found;
  }
  *out_idx = (uint8_t)(slot - 1U);
  return k_ra8_ok;
}

/**
 * @brief Handle a STANDARD chapter-9 request internally.
 *
 * @details The starter answers SET_ADDRESS by forwarding the new
 * address into `USBADDR` via `ra8_usb_set_address`; GET_DESCRIPTOR /
 * SET_CONFIGURATION are accepted as no-ops on the dispatch path
 * because the actual descriptor data is staged elsewhere by the
 * caller's chapter-9 stack. Other standard requests are also accepted
 * (no-op) so the dispatch state machine always advances. The starter
 * never STALLs a standard request from this layer; class layers can
 * still stall their own class-specific requests by returning a
 * non-zero error from `handle_setup`.
 *
 * @return `k_ra8_ok` on success.
 *
 * @param[in] setup See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_handle_standard(const ra8_usb_setup_t* setup)
{
  /* USB 2.0 sec 9.4 "Standard Device Requests" -- the starter
   * implements just the address-setter; the full GET_DESCRIPTOR
   * delivery path is owned by the caller's chapter-9 stack. */
  if (setup->b_request == (uint8_t)k_ra8_usb_composite_std_set_address) {
    const uint16_t addr16 = setup->w_value;
    if (addr16 > (uint16_t)k_ra8_usb_composite_max_address) {
      return k_ra8_err_invalid_arg;
    }
    return ra8_usb_set_address(s_state.speed, (uint8_t)addr16);
  }
  /* Other standard requests are accepted; descriptor staging /
   * configuration tracking are caller-owned. */
  return k_ra8_ok;
}

/**
 * @brief Route a CLASS or VENDOR SETUP request to the registered
 *        class whose IF range covers `setup->w_index`.
 *
 * @details `wIndex` carries the recipient interface number for
 * interface-recipient class requests (USB 2.0 sec 9.3.4). The
 * starter uses the low byte of `wIndex` as the IF number; the high
 * byte (for endpoint requests, sec 9.4) is ignored here because the
 * composite registry is keyed on interface number.
 *
 * @param[in] setup See implementation.
 * @param[in] out_idx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_route_class(const ra8_usb_setup_t* setup, uint8_t* out_idx)
{
  const uint8_t   if_num = (uint8_t)(setup->w_index & k_usbc_byte_mask);
  uint8_t         idx    = 0U;
  const ra8_err_t err    = internal_lookup_class_for_if(if_num, &idx);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_idx                                  = idx;
  const ra8_usb_composite_class_t* const cl = &s_state.classes[idx];
  return cl->handle_setup(cl->ctx, setup);
}

/**
 * @brief Pre-init guard helper used by every public entry point that
 *        is not `init` itself.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline ra8_err_t internal_require_init(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_composite_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_device_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_device_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }

  s_state.speed              = speed;
  s_state.state              = k_ra8_usb_composite_state_idle;
  s_state.class_count        = 0U;
  s_state.device_desc        = nullptr;
  s_state.config_desc        = nullptr;
  s_state.last_handler_class = (uint8_t)k_ra8_usb_composite_handler_self;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_composite_max_classes; ++i) {
    s_state.classes[i] = (ra8_usb_composite_class_t){};
  }
  internal_clear_if_owners();
  s_state.initialized = true;

  ra8_log_info_val(s_tag, "composite ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_composite_close(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  /* Walk in reverse-registration order so a class registered late
   * (e.g. an MSC layer that depends on CDC having brought up shared
   * state) tears down first. USB 2.0 doesn't mandate this but it
   * matches FSP's r_usb_pdriver shutdown ordering. */
  for (uint8_t i = s_state.class_count; i > 0U; --i) {
    const uint8_t                          idx = (uint8_t)(i - 1U);
    const ra8_usb_composite_class_t* const cl  = &s_state.classes[idx];
    if (cl->close != nullptr) {
      (void)cl->close(cl->ctx);
    }
  }

  const ra8_err_t deinit_err = ra8_usb_device_deinit(s_state.speed);
  s_state.initialized        = false;
  s_state.class_count        = 0U;
  s_state.device_desc        = nullptr;
  s_state.config_desc        = nullptr;
  internal_clear_if_owners();

  if (deinit_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_device_deinit failed", (uint32_t)deinit_err);
    return deinit_err;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Class registration
 * =============================================================================
 */

ra8_err_t ra8_usb_composite_register_class(const ra8_usb_composite_class_t* class_layer)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  const ra8_err_t val = internal_validate_class(class_layer);
  if (val != k_ra8_ok) {
    return val;
  }
  if (s_state.class_count >= (uint8_t)k_ra8_usb_composite_max_classes) {
    return k_ra8_err_no_mem;
  }
  const ra8_err_t coll = internal_check_collision(class_layer);
  if (coll != k_ra8_ok) {
    return coll;
  }

  const uint8_t slot    = s_state.class_count;
  s_state.classes[slot] = *class_layer;
  internal_mark_ownership(class_layer, slot);
  s_state.class_count = (uint8_t)(slot + 1U);

  /* Invoke the class's init callback last so a failure there leaves
   * the registry in a clean state via close (caller can call
   * `ra8_usb_composite_close` to tear everything down). */
  const ra8_err_t init_err = class_layer->init(class_layer->ctx);
  if (init_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "class init failed", (uint32_t)init_err);
    return init_err;
  }
  ra8_log_info_val(s_tag, "class registered", (uint32_t)slot);
  return k_ra8_ok;
}

/* =============================================================================
 * Descriptors
 * =============================================================================
 */

ra8_err_t ra8_usb_composite_set_descriptors(const uint8_t* device_desc, const uint8_t* config_desc)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  RA8_CHECK_NULL_PTR(device_desc, s_tag, "set_descriptors: device");
  RA8_CHECK_NULL_PTR(config_desc, s_tag, "set_descriptors: config");

  s_state.device_desc = device_desc;
  s_state.config_desc = config_desc;
  ra8_log_info(s_tag, "descriptors cached");
  return k_ra8_ok;
}

/* =============================================================================
 * Dispatch
 * =============================================================================
 */

ra8_err_t ra8_usb_composite_step(void)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }

  /* The starter advances by one phase per call, looping back to IDLE.
   * Production code drives the machine from EP0 / control completion
   * ISRs; here we just keep it well-behaved against repeated calls. */
  switch (s_state.state) {
    case k_ra8_usb_composite_state_idle:
      s_state.state = k_ra8_usb_composite_state_setup_rx;
      break;
    case k_ra8_usb_composite_state_setup_rx:
      s_state.state = k_ra8_usb_composite_state_std_dispatch;
      break;
    case k_ra8_usb_composite_state_std_dispatch:
      s_state.state = k_ra8_usb_composite_state_class_dispatch;
      break;
    case k_ra8_usb_composite_state_class_dispatch:
      s_state.state = k_ra8_usb_composite_state_done;
      break;
    case k_ra8_usb_composite_state_done:
    default:
      s_state.state = k_ra8_usb_composite_state_idle;
      break;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_usb_composite_dispatch_setup(const ra8_usb_setup_t* setup, uint8_t* out_handler_class)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  RA8_CHECK_NULL_PTR(setup, s_tag, "dispatch_setup: setup");
  RA8_CHECK_NULL_PTR(out_handler_class, s_tag, "dispatch_setup: out_handler");

  /* USB 2.0 sec 9.3.1: bmRequestType bits [6:5] carry the request
   * type. STANDARD goes through the composite layer; CLASS / VENDOR
   * are routed to whichever class owns wIndex. */
  const uint8_t type =
    (uint8_t)(setup->bm_request_type & (uint8_t)k_ra8_usb_composite_req_type_mask);

  if (type == (uint8_t)k_ra8_usb_composite_req_type_standard) {
    s_state.last_handler_class = (uint8_t)k_ra8_usb_composite_handler_self;
    *out_handler_class         = (uint8_t)k_ra8_usb_composite_handler_self;
    return internal_handle_standard(setup);
  }

  uint8_t         class_idx = 0U;
  const ra8_err_t route_err = internal_route_class(setup, &class_idx);
  if (route_err == k_ra8_err_not_found) {
    return k_ra8_err_not_found;
  }
  s_state.last_handler_class = class_idx;
  *out_handler_class         = class_idx;
  return route_err;
}

ra8_err_t ra8_usb_composite_get_class_count(uint8_t* out_count)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  RA8_CHECK_NULL_PTR(out_count, s_tag, "get_class_count: out");
  *out_count = s_state.class_count;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_composite_get_device_descriptor(const uint8_t** out_desc)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  RA8_CHECK_NULL_PTR(out_desc, s_tag, "get_device_descriptor: out");
  *out_desc = s_state.device_desc;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_composite_get_config_descriptor(const uint8_t** out_desc)
{
  const ra8_err_t init_check = internal_require_init();
  if (init_check != k_ra8_ok) {
    return init_check;
  }
  RA8_CHECK_NULL_PTR(out_desc, s_tag, "get_config_descriptor: out");
  *out_desc = s_state.config_desc;
  return k_ra8_ok;
}
