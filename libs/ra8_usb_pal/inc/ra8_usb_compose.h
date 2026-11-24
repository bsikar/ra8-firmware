/**
 * @file ra8_usb_compose.h
 * @brief One call that synthesises a device's whole framework set (RA8FW-317).
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * `ra8_usb_desc.h` gives an app four class encoders plus a string and a
 * language-id encoder, and every converted app has since written the same
 * three-call sequence around them: build the device framework, build the
 * strings, build the language id, carry three lengths. This header collapses
 * that into one config struct and one call.
 *
 * It is still the pure half of RA8FW-317: bytes into caller-owned buffers, no USBX
 * types, no allocation, no global state, so it is proven off-target like the
 * encoders under it. The USBX handshake (`_ux_system_initialize`,
 * `_ux_device_stack_initialize`, `_ux_device_stack_class_register`) is the
 * other half of the facade and is not here yet.
 *
 * @code
 * static uint8_t device[k_ra8_usb_desc_framework_bytes_max];
 * static uint8_t strings[k_ra8_usb_desc_strings_bytes_max];
 * static uint8_t langid[k_ra8_usb_desc_langid_bytes];
 *
 * const ra8_usb_class_t cdc = {
 *   .kind = k_ra8_usb_class_cdc_acm,
 *   .cdc_acm = { .notify_ep = 0x83U, .notify_bytes = 8U,
 *                .notify_interval_ms = 255U, .out_ep = 0x02U,
 *                .in_ep = 0x81U, .data_bytes = 64U },
 * };
 * ra8_usb_device_frameworks_t fw = {
 *   .device = device, .device_cap = sizeof(device),
 *   .strings = strings, .strings_cap = sizeof(strings),
 *   .langid = langid, .langid_cap = sizeof(langid),
 * };
 * if (ra8_usb_device_compose(&cfg, &fw) == k_ra8_ok) {
 *   // fw.device_len / fw.strings_len / fw.langid_len are the three ULONGs
 *   // _ux_device_stack_initialize wants.
 * }
 * @endcode
 *
 * ## One function per device, for now
 *
 * ::ra8_usb_device_cfg_t takes a class array because that is the shape RA8FW-317
 * asks for and the shape a composite device needs. The encoders underneath
 * model single-function devices, which is what all twenty-nine current copies
 * publish, so a `class_count` above one is refused with
 * ::k_ra8_err_not_supported rather than silently encoding the first entry.
 * Composite composition is a later slice, and the signature does not have to
 * change when it lands.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_USB_COMPOSE_H
#define RA8_USB_COMPOSE_H

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_usb_class_kind_t
 * @brief Which function a ::ra8_usb_class_t entry describes.
 *
 * @details The four the tree publishes, one per encoder in `ra8_usb_desc.h`.
 * ::k_ra8_usb_class_none is the zero value, so a partly-filled array entry
 * reads as unset rather than as a CDC port.
 */
typedef enum : uint8_t {
  k_ra8_usb_class_none    = 0U, /**< Unset; refused.          */
  k_ra8_usb_class_cdc_acm = 1U, /**< CDC-ACM serial port.     */
  k_ra8_usb_class_hid     = 2U, /**< Human-interface device.  */
  k_ra8_usb_class_msc     = 3U, /**< Bulk-only mass storage.  */
  k_ra8_usb_class_dfu     = 4U, /**< Device firmware upgrade. */
} ra8_usb_class_kind_t;

/**
 * @struct ra8_usb_class_t
 * @brief One function of a device: a kind tag and that kind's layout.
 *
 * @details A tagged union rather than four pointers, so an app writes the
 * same struct literal it already writes for the encoder and the compiler
 * keeps the tag and the payload together.
 *
 * @invariant `kind` names the union member that is set.
 */
typedef struct {
  ra8_usb_class_kind_t kind; /**< Which member of the union is live. */
  union {
    ra8_usb_desc_cdc_acm_t cdc_acm; /**< Live when kind is cdc_acm. */
    ra8_usb_desc_hid_t     hid;     /**< Live when kind is hid.     */
    ra8_usb_desc_msc_t     msc;     /**< Live when kind is msc.     */
    ra8_usb_desc_dfu_t     dfu;     /**< Live when kind is dfu.     */
  };
} ra8_usb_class_t;

/**
 * @struct ra8_usb_device_cfg_t
 * @brief Everything the synthesiser needs to describe one device.
 *
 * @details The `pool` / `pool_bytes` / `port` fields RA8FW-317 sketches belong to
 * the USBX handshake, not to descriptor synthesis, so they are not here: they
 * land with the half of the facade that owns the stack.
 *
 * @invariant `desc` and `classes` are non-NULL and `class_count` is 1.
 */
typedef struct {
  const ra8_usb_desc_device_t* desc;        /**< Identity and strings.  */
  const ra8_usb_class_t*       classes;     /**< Functions, in order.   */
  uint8_t                      class_count; /**< Entries in @p classes. */
} ra8_usb_device_cfg_t;

/**
 * @struct ra8_usb_device_frameworks_t
 * @brief The three caller-owned buffers, and what got written to them.
 *
 * @details Caps in, lengths out, in one struct, because the three lengths
 * always travel together to `_ux_device_stack_initialize`. The `_len` fields
 * are written only on success.
 */
typedef struct {
  uint8_t* device;      /**< Device framework buffer.           */
  uint32_t device_cap;  /**< Bytes available at @c device.      */
  uint32_t device_len;  /**< Bytes written to @c device.  [out] */
  uint8_t* strings;     /**< String framework buffer.           */
  uint32_t strings_cap; /**< Bytes available at @c strings.     */
  uint32_t strings_len; /**< Bytes written to @c strings. [out] */
  uint8_t* langid;      /**< Language-id framework buffer.      */
  uint32_t langid_cap;  /**< Bytes available at @c langid.      */
  uint32_t langid_len;  /**< Bytes written to @c langid.  [out] */
} ra8_usb_device_frameworks_t;

/**
 * @brief Synthesise all three frameworks for @p cfg into @p fw.
 *
 * @details Dispatches on the single class entry's kind to the matching
 * encoder in `ra8_usb_desc.h`, then encodes the strings and the language id.
 * The bytes are identical to calling those three functions by hand, which is
 * what the converted apps do today.
 *
 * @param[in]     cfg The device to describe.
 * @param[in,out] fw  Buffers and caps in, lengths out.
 *
 * @return ra8_err_t Result of the three encodes.
 * @retval k_ra8_ok                 All three frameworks written.
 * @retval k_ra8_err_invalid_arg    A NULL argument, a NULL buffer, a zero
 *                                  @c class_count, or an unset class kind.
 * @retval k_ra8_err_not_supported  @c class_count above one; composite
 *                                  composition is not modelled yet.
 * @retval k_ra8_err_invalid_size   A buffer is too small for its framework.
 *
 * @pre The three buffers are distinct and writable for their caps.
 * @post On success each `_len` counts the bytes written to its buffer; on
 *       failure the `_len` fields of encodes that did not run are untouched.
 *
 * @note Pure: no allocation, no retained pointers, no global state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_compose(const ra8_usb_device_cfg_t*  cfg,
                                               ra8_usb_device_frameworks_t* fw);

/**
 * @enum ra8_usb_device_port_t
 * @brief USB device controller port selection passed to ::ra8_usb_device_open.
 *
 * @details Binary-compatible with ``ra8_board_usb_port_t`` (0=FS, 1=HS) without
 * binding this PAL header to a board-support package header.
 */
typedef enum : uint8_t {
  k_ra8_usb_device_port_fs = 0U, /**< Full-speed USBFS controller. */
  k_ra8_usb_device_port_hs = 1U, /**< High-speed USBHS controller. */
} ra8_usb_device_port_t;

/**
 * @typedef ra8_usb_device_error_cb_t
 * @brief Stack error notification callback passed through ::ra8_usb_device_open.
 *
 * @param[in] system_level   Stack subsystem severity level.
 * @param[in] system_context Stack error origin context.
 * @param[in] error_code     Underlying stack error code.
 */
typedef void (*ra8_usb_device_error_cb_t)(uint32_t system_level,
                                          uint32_t system_context,
                                          uint32_t error_code);

/**
 * @typedef ra8_usb_device_class_entry_fn_t
 * @brief Opaque stack class entry callback signature.
 *
 * @param[in,out] command Class command block passed by the device stack.
 * @return uint32_t Zero on success, non-zero stack status on failure.
 */
typedef uint32_t (*ra8_usb_device_class_entry_fn_t)(void* command);

/**
 * @struct ra8_usb_device_class_driver_t
 * @brief Bound device class name and entry point supplied by a stack port.
 */
typedef struct {
  const char*                     name;  /**< NUL-terminated class name for registration. */
  ra8_usb_device_class_entry_fn_t entry; /**< Class stack entry function.                 */
} ra8_usb_device_class_driver_t;

/**
 * @struct ra8_usb_stack_ops_t
 * @brief Stack-agnostic seam ops implemented by the active USB device port.
 *
 * @details
 * Keeps ``libs/ra8_usb_pal`` free of middleware symbols while letting
 * ::ra8_usb_device_open own configuration checks, descriptor composition,
 * and strict bring-up ordering: system pool init -> optional error callback ->
 * device stack init -> class registration -> optional DCD controller open.
 *
 * @invariant @c system_init, @c device_stack_init, and @c class_register are non-NULL.
 * @invariant Each callback returns ::k_ra8_ok on success or a non-zero ::ra8_err_t.
 */
typedef struct {
  ra8_err_t (*system_init)(void* pool, uint32_t pool_bytes);
  ra8_err_t (*error_cb_register)(ra8_usb_device_error_cb_t error_cb);
  ra8_err_t (*device_stack_init)(uint8_t* hs_device_fw,
                                 uint32_t hs_device_fw_len,
                                 uint8_t* fs_device_fw,
                                 uint32_t fs_device_fw_len,
                                 uint8_t* strings_fw,
                                 uint32_t strings_fw_len,
                                 uint8_t* langid_fw,
                                 uint32_t langid_fw_len);
  ra8_err_t (*class_register)(const char*                     class_name,
                              ra8_usb_device_class_entry_fn_t class_entry,
                              uint8_t                         configuration,
                              uint8_t                         interface,
                              void*                           class_param);
  ra8_err_t (*dcd_open)(ra8_usb_device_port_t port);
} ra8_usb_stack_ops_t;

/**
 * @struct ra8_usb_device_open_cfg_t
 * @brief Everything ::ra8_usb_device_open needs to compose descriptors (when
 *        @c device_cfg is set), initialise the USB device stack through @c ops,
 *        and register one device class.
 *
 * @details
 * Field order keeps 4-byte fields before pointer pairs and puts the optional
 * HS length trailing so field offsets follow ``8 + N * sizeof(void*)`` across
 * both 32-bit targets and 64-bit hosts.
 *
 * @invariant @c pool, @c frameworks, @c ops, @c class_driver, and @c class_param
 *            are non-NULL.
 * @invariant When @c device_cfg is non-NULL, @c frameworks holds caller-sized
 *            output buffers and ::ra8_usb_device_open runs
 *            ::ra8_usb_device_compose before bringing up the stack. When
 *            @c device_cfg is NULL, @c frameworks already carries valid non-zero
 *            lengths.
 * @invariant `configuration >= 1`, `pool_bytes > 0`, and `port <= 1`.
 */
typedef struct {
  ra8_usb_device_port_t                port;                    /**< USB controller port.      */
  uint8_t                              configuration;           /**< USB configuration (>= 1). */
  uint8_t                              interface;               /**< USB interface index.      */
  uint32_t                             pool_bytes;              /**< Stack pool size in bytes. */
  void*                                pool;                    /**< Stack memory pool buffer. */
  const ra8_usb_device_cfg_t*          device_cfg;              /**< Optional compose input.   */
  ra8_usb_device_frameworks_t*         frameworks;              /**< FS/string/langid buffers. */
  uint8_t*                             hs_device_framework;     /**< Optional HS framework.    */
  const ra8_usb_stack_ops_t*           ops;                     /**< Stack port callbacks.     */
  const ra8_usb_device_class_driver_t* class_driver;            /**< Class driver descriptor.  */
  void*                                class_param;             /**< Class parameter block.    */
  ra8_usb_device_error_cb_t            error_cb;                /**< Optional error callback.  */
  uint32_t                             hs_device_framework_len; /**< HS framework bytes (0).   */
} ra8_usb_device_open_cfg_t;

/**
 * @brief Validate configuration, optionally compose frameworks, initialise the
 *        USB device stack through @p cfg->ops, and register its class driver.
 *
 * @details
 * When @c cfg->device_cfg is non-NULL, calls ::ra8_usb_device_compose into
 * @c cfg->frameworks first. Then dispatches through @c cfg->ops in strict order:
 * @c system_init -> optional @c error_cb_register -> @c device_stack_init ->
 * @c class_register -> optional @c dcd_open.
 *
 * @param[in] cfg Stack pool, port, framework buffers, ops seam, and class.
 *
 * @return ra8_err_t Result of validation, compose, and stack initialisation.
 * @retval k_ra8_ok                Stack and class registered.
 * @retval k_ra8_err_invalid_arg   Missing pointer, invalid port, zero pool
 *                                 or framework length, or FS port with HS FW.
 * @retval k_ra8_err_invalid_size  An undersized buffer refused by compose.
 * @retval k_ra8_err_not_supported Composite @c device_cfg refused by compose.
 * @retval k_ra8_err_invalid_state Any @c cfg->ops callback returned failure.
 *
 * @pre Caller owns @c cfg->pool backing storage for the life of the stack.
 * @post On success the stack has registered the class (and opened DCD if
 *       @c ops->dcd_open was supplied).
 *
 * @note Single-threaded startup; not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_open(const ra8_usb_device_open_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

#endif /* RA8_USB_COMPOSE_H */
