/**
 * @file ra8_usb_composite.h
 * @brief Native USB device-side composite-class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Mirrors FSP's `r_usb_composite` peripheral-mode composite-device
 * driver. With this layer the EK-RA8D2 can present multiple USB
 * function classes (CDC ACM serial, HID keyboard, MSC mass storage,
 * ...) over a single physical connection so the host enumerates one
 * USB device that exposes a serial port + a keyboard + a drive at the
 * same time.
 *
 * The composite layer is class-agnostic: it does not pull in
 * `ra8_usb_cdc.h`, `ra8_usb_phid.h`, or `ra8_usb_pmsc.h`. Instead the
 * application registers each class layer by passing a small
 * function-pointer struct (`ra8_usb_composite_class_t`) at boot time;
 * the composite layer walks the registered list whenever a SETUP
 * packet arrives whose `wIndex` (interface number) lands in that
 * class's owned IF range.
 *
 * Lifecycle (mirrors `ra8_usb_pmsc.h` and `ra8_usb_phid.h` shape):
 *
 *  1. `ra8_usb_composite_init(speed)` flips the controller to device
 *     mode for `speed`, clears the registered-class table, primes the
 *     dispatch state machine to IDLE, and leaves D+ pull-up off so the
 *     application can finalise its descriptor table before advertising
 *     on the bus.
 *  2. `ra8_usb_composite_register_class(class_layer)` is called once
 *     per attached class. Each call validates the requested IF range
 *     against already-registered ranges so two classes cannot claim
 *     the same interface number.
 *  3. `ra8_usb_composite_set_descriptors(device, configuration)` stores
 *     pointers to the caller-owned device + configuration descriptors.
 *     The caller is expected to have built a valid composite
 *     configuration descriptor including IADs (Interface Association
 *     Descriptors) for each multi-interface class.
 *  4. `ra8_usb_composite_step()` drives the dispatch state machine.
 *     When a SETUP packet arrives the layer walks the registered
 *     classes; the one whose `[interface_number_first,
 *     +interface_number_count)` range covers `setup->w_index` gets
 *     `handle_setup` called. Standard chapter-9 requests
 *     (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION) are handled by
 *     the composite layer itself before any class dispatch.
 *  5. `ra8_usb_composite_close()` invokes each registered class's
 *     `close` hook, drops the class table, and tears down the
 *     device-mode controller.
 *
 * Reference: USB 2.0 spec sec 9.6 "Standard USB Descriptor
 * Definitions"; Interface Association Descriptor ECN ("USB Engineering
 * Change Notice: Interface Association Descriptors", 2003-07-23).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_composite_limits_t
 * @brief Static ceilings for the composite registry.
 *
 * @details The starter caps the number of registered classes at 4
 * (CDC + HID + MSC + a vendor-specific function is the largest
 * realistic stack) and the maximum interface index at 16 to bound the
 * collision-detection bitmap. These match the FSP reference templates
 * shipped under `r_usb_composite` (`.template` files).
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_max_classes = 4U,  /**< Registered class ceiling.  */
  k_ra8_usb_composite_max_ifs     = 16U, /**< Highest IF number tracked. */
} ra8_usb_composite_limits_t;

/**
 * @enum ra8_usb_composite_iad_t
 * @brief Interface Association Descriptor (IAD) constants.
 *
 * @details Per the USB 2.0 IAD ECN sec 9.X. The composite device
 * descriptor advertises class = MISC (0xEF) / subclass = COMMON (0x02)
 * / protocol = IAD (0x01) so the host knows to look for IADs in the
 * configuration descriptor. Each IAD itself uses bDescriptorType
 * 0x0B.
 */
typedef enum : uint8_t {
  k_ra8_usb_composite_class_misc          = 0xEFU, /**< Miscellaneous class.   */
  k_ra8_usb_composite_subclass_common     = 0x02U, /**< Common subclass.       */
  k_ra8_usb_composite_protocol_iad        = 0x01U, /**< IAD protocol.          */
  k_ra8_usb_composite_descriptor_type_iad = 0x0BU, /**< bDescriptorType = IAD. */
} ra8_usb_composite_iad_t;

/* =============================================================================
 * Class-layer interface
 * =============================================================================
 */

/**
 * @typedef ra8_usb_composite_init_fn_t
 * @brief Class-layer initialise hook.
 *
 * @param[in] ctx Caller-supplied context registered with the class.
 *
 * @return `ra8_err_t` error code propagated unchanged.
 */
typedef ra8_err_t (*ra8_usb_composite_init_fn_t)(void* ctx);

/**
 * @typedef ra8_usb_composite_setup_fn_t
 * @brief Class-layer SETUP-handler hook.
 *
 * @param[in] ctx   Caller-supplied context registered with the class.
 * @param[in] setup Decoded 8-byte SETUP packet (USB 2.0 sec 9.3).
 *
 * @return `ra8_err_t` error code. Non-zero stalls the control transfer.
 */
typedef ra8_err_t (*ra8_usb_composite_setup_fn_t)(void* ctx, const ra8_usb_setup_t* setup);

/**
 * @typedef ra8_usb_composite_close_fn_t
 * @brief Class-layer teardown hook.
 *
 * @param[in] ctx Caller-supplied context registered with the class.
 *
 * @return `ra8_err_t` error code.
 */
typedef ra8_err_t (*ra8_usb_composite_close_fn_t)(void* ctx);

/**
 * @struct ra8_usb_composite_class_t
 * @brief Caller-owned class-layer registration record.
 *
 * @details All three function pointers must be non-NULL. `ctx` is
 * opaque to the composite layer and threaded back into each callback
 * unchanged. The
 * `[interface_number_first, interface_number_first + interface_number_count)`
 * range describes which IF numbers from the configuration descriptor
 * this class owns. CDC ACM consumes 2 IFs (control + data); HID and
 * MSC each consume 1.
 *
 * @invariant `interface_number_count >= 1`.
 * @invariant `interface_number_first + interface_number_count <=
 *            k_ra8_usb_composite_max_ifs`.
 *
 * @code
 * static ra8_err_t my_init(void* ctx);
 * static ra8_err_t my_setup(void* ctx, const ra8_usb_setup_t* s);
 * static ra8_err_t my_close(void* ctx);
 *
 * static const ra8_usb_composite_class_t s_cdc_layer = {
 *   .interface_number_first = 0U,
 *   .interface_number_count = 2U,
 *   .init                   = my_init,
 *   .handle_setup           = my_setup,
 *   .close                  = my_close,
 *   .ctx                    = nullptr,
 * };
 * @endcode
 */
typedef struct {
  uint8_t                      interface_number_first; /**< First IF this class owns.   */
  uint8_t                      interface_number_count; /**< IF count (CDC=2, HID=1...). */
  ra8_usb_composite_init_fn_t  init;                   /**< init callback (non-NULL).   */
  ra8_usb_composite_setup_fn_t handle_setup;           /**< setup callback (non-NULL).  */
  ra8_usb_composite_close_fn_t close;                  /**< close callback (non-NULL).  */
  void*                        ctx;                    /**< Opaque context.             */
} ra8_usb_composite_class_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the composite-class driver on a chosen USB
 *        controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, zeroes the registered-class table, primes the dispatch
 * state machine to IDLE, and clears the cached descriptor pointers.
 * D+ pull-up is left off so the application can publish the
 * configuration descriptor before the host enumerates it.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Composite ready, awaiting `register_class`.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_device_init` succeeded for `speed`.
 * @post Internal class registry empty; descriptors NULL.
 *
 * @note Not thread-safe.
 * @see ra8_usb_composite_register_class
 * @see ra8_usb_composite_set_descriptors
 * @see ra8_usb_composite_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the composite-class driver and release the
 *        controller.
 *
 * @details
 * Walks the registered class table and invokes each class's `close`
 * callback in reverse-registration order. Then drops the cached class
 * table and calls `ra8_usb_device_deinit`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; subsequent composite API calls
 *       return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_close(void);

/* =============================================================================
 * Class registration
 * =============================================================================
 */

/**
 * @brief Register a single class layer with the composite driver.
 *
 * @details
 * Validates the supplied class-layer record:
 *
 *  - All three function pointers (`init`, `handle_setup`, `close`)
 *    must be non-NULL.
 *  - `interface_number_count` must be >= 1.
 *  - `interface_number_first + interface_number_count` must not
 *    exceed `k_ra8_usb_composite_max_ifs`.
 *  - The `[first, first+count)` IF range must not overlap any
 *    previously-registered class's range (collision detection).
 *
 * On success the record is copied into internal state and the class's
 * `init` callback is invoked. Composition order is the order of
 * `register_class` calls.
 *
 * @param[in] class_layer Class-layer snapshot. Copied by value.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Class registered, init invoked.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `class_layer` was NULL or any of its
 *         callbacks was NULL.
 * @retval k_ra8_err_invalid_arg `interface_number_count` was 0 or the
 *         requested IF range overflows the IF ceiling.
 * @retval k_ra8_err_exists IF range overlaps a previously-registered
 *         class.
 * @retval k_ra8_err_no_mem Class table is full (already at
 *         `k_ra8_usb_composite_max_classes`).
 *
 * @pre `ra8_usb_composite_init` has run.
 * @pre All three callbacks non-NULL.
 *
 * @post Class slot count incremented; class's `init` callback invoked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_composite_register_class(const ra8_usb_composite_class_t* class_layer);

/* =============================================================================
 * Descriptors
 * =============================================================================
 */

/**
 * @brief Cache caller-owned device + configuration descriptor
 *        pointers.
 *
 * @details
 * The composite layer does not allocate descriptors itself; the
 * application builds them (typically as `static const uint8_t[]`) and
 * passes pointers in. The `device_desc` is the 18-byte standard device
 * descriptor (USB 2.0 sec 9.6.1); the `config_desc` is the variable-
 * length composite configuration descriptor that includes one IAD per
 * multi-interface class (USB IAD ECN sec 9.X).
 *
 * @param[in] device_desc Pointer to the device descriptor.
 * @param[in] config_desc Pointer to the configuration descriptor.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Descriptors cached.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 *
 * @pre `ra8_usb_composite_init` has run.
 * @pre Both descriptors point to caller-owned memory that lives for
 *      the device's session.
 *
 * @post `ra8_usb_composite_get_device_descriptor` /
 *       `ra8_usb_composite_get_config_descriptor` return the supplied
 *       pointers.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_set_descriptors(const uint8_t* device_desc,
                                                          const uint8_t* config_desc);

/* =============================================================================
 * Dispatch
 * =============================================================================
 */

/**
 * @brief Drive the composite dispatch state machine forward by one
 *        step.
 *
 * @details
 * Production code calls this from the EP0 / control-transfer
 * completion ISR. Each call advances the state machine by one phase:
 *
 *  - IDLE: wait for the next SETUP packet.
 *  - SETUP_RX: a SETUP arrived; classify it (standard / class /
 *    vendor) using `bm_request_type` and route accordingly.
 *  - STD_DISPATCH: standard chapter-9 requests (GET_DESCRIPTOR,
 *    SET_ADDRESS, SET_CONFIGURATION) handled here.
 *  - CLASS_DISPATCH: class / vendor requests routed to the registered
 *    class whose IF range covers `setup->w_index`.
 *  - DONE: transfer staged; next step idles back to SETUP_RX.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_composite_init` has run.
 *
 * @post Internal state machine advances by one phase.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_step(void);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Inject a SETUP packet directly into the dispatch logic.
 *
 * @details
 * Test / debug entry point. Production code receives SETUPs from the
 * controller's USBREQ / USBVAL / USBINDX / USBLENG mirrors via
 * `ra8_usb_read_setup_if_valid`; tests bypass the FIFO and feed an 8-byte
 * decoded SETUP directly. Standard chapter-9 requests are answered
 * internally; class / vendor requests are routed to the registered
 * class whose IF range covers `setup->w_index`.
 *
 * @param[in] setup Pointer to the 8-byte decoded SETUP packet.
 * @param[out] out_handler_class On success, receives the index of the
 *                               class that handled the request, or
 *                               `k_ra8_usb_composite_max_classes` if
 *                               the composite layer answered (i.e.
 *                               standard request).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP dispatched.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `setup` or `out_handler_class` was NULL.
 * @retval k_ra8_err_not_found Class request whose `wIndex` does not
 *         fall in any registered class's IF range.
 *
 * @pre Both pointers non-NULL.
 *
 * @post On `k_ra8_ok` for a class request, the matching class's
 *       `handle_setup` callback was invoked exactly once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_dispatch_setup(const ra8_usb_setup_t* setup,
                                                         uint8_t*               out_handler_class);

/**
 * @brief Read back the number of currently-registered class layers.
 *
 * @param[out] out_count Receives the registered class count.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Count returned.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_count` was NULL.
 *
 * @pre `out_count` non-NULL.
 *
 * @post No state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_get_class_count(uint8_t* out_count);

/**
 * @brief Read back the cached device descriptor pointer.
 *
 * @param[out] out_desc Receives the cached device descriptor pointer
 *                      (or NULL if `set_descriptors` never ran).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointer returned.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_desc` was NULL.
 *
 * @pre `out_desc` non-NULL.
 *
 * @post No state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_get_device_descriptor(const uint8_t** out_desc);

/**
 * @brief Read back the cached configuration descriptor pointer.
 *
 * @param[out] out_desc Receives the cached configuration descriptor
 *                      pointer (or NULL if `set_descriptors` never
 *                      ran).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pointer returned.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `out_desc` was NULL.
 *
 * @pre `out_desc` non-NULL.
 *
 * @post No state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_composite_get_config_descriptor(const uint8_t** out_desc);

#ifdef __cplusplus
}
#endif
