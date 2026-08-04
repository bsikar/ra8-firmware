/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_hcdc_ecm.h
 * @brief Native USB host-side CDC ECM (Communications Device Class -
 * @ingroup grp_hal_usb
 *        Ethernet Networking Control Model) class layer
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a CDC-ECM USB
 * Ethernet adapter attached on the EK-RA8D2's USB-HS host port. Mirrors
 * FSP's `r_usb_hcdc_ecm` host-CDC-ECM class flow but compiled as part
 * of this tree with no FSP / CherryUSB / TinyUSB binaries pulled in.
 *
 * Lifecycle:
 *
 *  1. `ra8_usb_hcdc_ecm_init(speed)` flips the controller to host mode,
 *     enables SOF generation and waits for `SYSSTS0.LNST` to report a
 *     J-state attach.
 *  2. The driver runs the chapter-9 enumeration step machine
 *     (Reset -> SET_ADDRESS -> GET_DEVICE_DESCRIPTOR ->
 *     GET_CONFIG_DESCRIPTOR -> SET_CONFIG -> SET_INTERFACE) over the
 *     DCP and walks the configuration descriptor for the CDC
 *     Communication-Interface (class=0x02 / subclass=0x06) plus the
 *     Data-Interface (class=0x0A) and the Ethernet Networking
 *     Functional descriptor (subtype=0x0F) which holds the iMACAddress
 *     string-descriptor index.
 *  3. After SET_INTERFACE the driver issues GET_STRING_DESCRIPTOR for
 *     the iMACAddress index and parses the 12 ASCII hex digits into a
 *     6-byte MAC.
 *  4. When enumeration completes, the registered attach callback fires
 *     once with the bulk-IN / bulk-OUT pipe handles, max-packet sizes,
 *     and the parsed MAC.
 *  5. `ra8_usb_hcdc_ecm_send_frame` / `_recv_frame` move Ethernet frames
 *     through the bulk pipes.
 *  6. `ra8_usb_hcdc_ecm_set_packet_filter` issues the SET_ETHERNET_PACKET_FILTER
 *     class request (bRequest=0x43, bmRequestType=0x21) so the adapter
 *     accepts the desired class of frames.
 *  7. `ra8_usb_hcdc_ecm_get_link_status` issues GET_ETHERNET_STATISTIC
 *     (bRequest=0x44).
 *  8. `ra8_usb_hcdc_ecm_close()` drops bus power and releases the device.
 *
 * The starter does not support hubs; it tracks a single attached
 * CDC-ECM adapter. Hub class enumeration is tracked as a deferred
 * follow-up.
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
 * @enum ra8_usb_hcdc_ecm_pipe_t
 * @brief PIPE numbers used by the host-CDC-ECM driver for the attached
 *        adapter's endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5 and interrupt pipes to PIPE6..PIPE9.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_pipe_bulk_in  = 1U, /**< PIPE1 -> attached EP bulk IN.  */
  k_ra8_hcdc_ecm_pipe_bulk_out = 2U, /**< PIPE2 -> attached EP bulk OUT. */
  k_ra8_hcdc_ecm_pipe_intr_in  = 6U, /**< PIPE6 -> attached EP intr IN.  */
} ra8_usb_hcdc_ecm_pipe_t;

/**
 * @enum ra8_usb_hcdc_ecm_packet_t
 * @brief Packet sizing for the attached adapter's bulk + interrupt
 *        endpoints.
 *
 * @details The Ethernet MTU plus 14-byte Ethernet header plus 4-byte
 * FCS yields the canonical 1514-byte maximum frame size mirrored from
 * FSP `USB_HCDC_ECM_MAXIMUM_ETHERNET_FRAME_SIZE`.
 */
typedef enum : uint16_t {
  k_ra8_hcdc_ecm_bulk_max_packet_fs = 64U,   /**< Bulk size at full speed. */
  k_ra8_hcdc_ecm_bulk_max_packet_hs = 512U,  /**< Bulk size at high speed. */
  k_ra8_hcdc_ecm_intr_max_packet    = 16U,   /**< Notification pipe size.  */
  k_ra8_hcdc_ecm_max_ethernet_frame = 1514U, /**< Ethernet frame ceiling.  */
} ra8_usb_hcdc_ecm_packet_t;

/**
 * @enum ra8_usb_hcdc_ecm_class_t
 * @brief Class / subclass / protocol triplets that identify a CDC ECM
 *        function within an attached USB device's descriptor walk.
 *
 * @details Numbered from the USB CDC ECM 1.20 spec (subclass 0x06) and
 * the USB-IF Class Codes registry. Protocol = 0x00 (no specific
 * protocol).
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_class_comms   = 0x02U, /**< CDC control IF class.      */
  k_ra8_hcdc_ecm_subclass_ecm  = 0x06U, /**< Ethernet Networking model. */
  k_ra8_hcdc_ecm_protocol_none = 0x00U, /**< No protocol.               */
  k_ra8_hcdc_ecm_class_data    = 0x0AU, /**< CDC data interface class.  */
  k_ra8_hcdc_ecm_subclass_zero = 0x00U, /**< Data subclass = 0.         */
  k_ra8_hcdc_ecm_protocol_zero = 0x00U, /**< Data protocol = 0.         */
} ra8_usb_hcdc_ecm_class_t;

/**
 * @enum ra8_usb_hcdc_ecm_request_t
 * @brief CDC-ECM class-specific request codes the host issues to the
 *        attached adapter (USB CDC ECM 1.20 sec 6.2 "Management
 *        Element Requests").
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_req_set_packet_filter = 0x43U, /**< SET_ETHERNET_PACKET_FILTER. */
  k_ra8_hcdc_ecm_req_get_statistic     = 0x44U, /**< GET_ETHERNET_STATISTIC.     */
} ra8_usb_hcdc_ecm_request_t;

/**
 * @enum ra8_usb_hcdc_ecm_filter_t
 * @brief Filter mask bits passed to
 *        `ra8_usb_hcdc_ecm_set_packet_filter`.
 *
 * @details Wire-level layout matches USB CDC ECM 1.20 sec 6.2.4 "Set
 * Ethernet Packet Filter" (Table 6: PACKET_TYPE bitmap).
 */
typedef enum : uint16_t {
  k_ra8_hcdc_ecm_filter_promiscuous   = 0x01U, /**< All received frames.     */
  k_ra8_hcdc_ecm_filter_all_multicast = 0x02U, /**< All multicast frames.    */
  k_ra8_hcdc_ecm_filter_directed      = 0x04U, /**< Frames addressed to MAC. */
  k_ra8_hcdc_ecm_filter_broadcast     = 0x08U, /**< Broadcast frames.        */
  k_ra8_hcdc_ecm_filter_multicast     = 0x10U, /**< Listed multicast.        */
  k_ra8_hcdc_ecm_filter_all           = 0x1FU, /**< All bits set.            */
} ra8_usb_hcdc_ecm_filter_t;

/**
 * @enum ra8_usb_hcdc_ecm_link_t
 * @brief Adapter link status reported by `_get_link_status`.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_link_down = 0U, /**< NetworkConnection notification 0. */
  k_ra8_hcdc_ecm_link_up   = 1U, /**< NetworkConnection notification 1. */
} ra8_usb_hcdc_ecm_link_t;

/**
 * @enum ra8_usb_hcdc_ecm_mac_t
 * @brief Sizes related to the 6-byte Ethernet MAC + its 12-character
 *        ASCII hex representation in the iMACAddress string descriptor.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_mac_bytes     = 6U,  /**< Ethernet MAC byte count.        */
  k_ra8_hcdc_ecm_mac_hex_chars = 12U, /**< 12 ASCII hex chars in iMACAddr. */
} ra8_usb_hcdc_ecm_mac_t;

/**
 * @struct ra8_usb_hcdc_ecm_device_t
 * @brief Snapshot of the attached CDC-ECM adapter, passed to the
 *        attach callback.
 *
 * @details Populated by the host-CDC-ECM driver once the descriptor
 * walk and iMACAddress string-descriptor fetch complete. Pipe numbers
 * are the controller-side handles wired up against the attached
 * adapter's bulk + interrupt endpoints. The attach-callback recipient
 * should hold onto this struct (or copy it) so subsequent
 * `_send_frame` / `_recv_frame` calls land on the right pipes.
 */
typedef struct {
  uint8_t  device_address;                        /**< Assigned USB address (1..127).      */
  uint8_t  bulk_in_ep;                            /**< Adapter's bulk IN EP num.           */
  uint8_t  bulk_out_ep;                           /**< Adapter's bulk OUT EP num.          */
  uint8_t  intr_in_ep;                            /**< Adapter's intr IN (notify) EP num.  */
  uint16_t bulk_in_max_packet;                    /**< Adapter bulk-IN wMaxPacketSize.     */
  uint16_t bulk_out_max_packet;                   /**< Adapter bulk-OUT wMaxPacketSize.    */
  uint16_t vendor_id;                             /**< idVendor from device descriptor.    */
  uint16_t product_id;                            /**< idProduct from device descriptor.   */
  uint8_t  i_mac_address;                         /**< iMACAddress string descr index.     */
  uint8_t  mac_address[k_ra8_hcdc_ecm_mac_bytes]; /**< 6-byte MAC parsed from iMACAddress. */
} ra8_usb_hcdc_ecm_device_t;

/**
 * @typedef ra8_usb_hcdc_ecm_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_hcdc_ecm_attach_callback`.
 * @param[in] device Snapshot of the attached CDC-ECM adapter. Pointer
 *                   remains valid only for the duration of the call;
 *                   copy out anything you need.
 *
 * @note Invoked from the dispatch site (typically ISR context) after
 * enumeration completes successfully.
 */
typedef void (*ra8_usb_hcdc_ecm_attach_fn_t)(void* ctx, const ra8_usb_hcdc_ecm_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-CDC-ECM driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in HOST mode for `speed`
 * by delegating to `ra8_usb_host_init`, programs the DCP for 64-byte
 * EP0 control transfers, leaves the bus in the "wait for attach" state
 * (UACT cleared), and arms the internal enumeration step machine.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Host-CDC-ECM ready, awaiting attach.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_host_init` failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired yet.
 *
 * @note Not thread-safe.
 * @see ra8_usb_hcdc_ecm_attach_callback
 * @see ra8_usb_hcdc_ecm_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the host-CDC-ECM driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_host_deinit` ran; SOF generation halted; bus power
 *       dropped.
 * @post Subsequent host-CDC-ECM API calls return
 *       `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_close(void);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the attach callback.
 *
 * @details
 * The supplied callback fires exactly once per attach event, after the
 * descriptor walk identifies a CDC-ECM Communication + Data interface
 * pair on the attached device and the iMACAddress string descriptor
 * has been parsed into `device->mac_address`. Pass `NULL` to detach.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre `ra8_usb_hcdc_ecm_init` has run.
 *
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_attach_callback(ra8_usb_hcdc_ecm_attach_fn_t on_attach,
                                                         void*                        ctx);

/* =============================================================================
 * Bulk frame pipe
 * =============================================================================
 */

/**
 * @brief Send an Ethernet frame OUT to the attached CDC-ECM adapter.
 *
 * @details
 * Pushes `buf` bytes on PIPE2 (bulk OUT) so the controller delivers
 * them at the next OUT token. Per USB CDC ECM 1.20 sec 5.4 "Data
 * Class Interface Definitions" each USB transfer carries exactly one
 * Ethernet frame (no length prefix). A frame whose length is an
 * integer multiple of wMaxPacketSize is followed by a zero-length
 * packet to terminate the transfer.
 *
 * @param[in] buf Frame buffer. NULL allowed iff `len == 0`.
 * @param[in] len Byte count, 0..k_ra8_hcdc_ecm_max_ethernet_frame.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no adapter
 *         attached.
 * @retval k_ra8_err_invalid_arg Bad `buf` / `len`.
 *
 * @pre `ra8_usb_hcdc_ecm_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post Frame will be delivered on the next bulk OUT token.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_send_frame(const uint8_t* buf, uint16_t len);

/**
 * @brief Drain an Ethernet frame IN from the attached CDC-ECM adapter.
 *
 * @details
 * Polling / non-blocking. Pulls bytes from PIPE1 (bulk IN) into `buf`
 * and stores the actual byte count in `*got_len`. Per USB CDC ECM 1.20
 * sec 5.4 "Data Class Interface Definitions" one bulk IN transfer
 * yields one Ethernet frame.
 *
 * @param[out] buf Destination buffer.
 * @param[in] max_len Capacity of `buf`, > 0.
 * @param[out] got_len Receives the number of bytes actually placed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Frame drained; `*got_len` reflects the count.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no adapter
 *         attached.
 * @retval k_ra8_err_null_ptr `buf` or `got_len` was NULL.
 * @retval k_ra8_err_invalid_arg `max_len == 0`.
 *
 * @pre `ra8_usb_hcdc_ecm_init` succeeded.
 * @pre Attach callback already fired.
 * @pre `buf`, `got_len` non-NULL, `max_len > 0`.
 *
 * @post On success `*got_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_hcdc_ecm_recv_frame(uint8_t* buf, uint16_t max_len, uint16_t* got_len);

/* =============================================================================
 * Class control transfers
 * =============================================================================
 */

/**
 * @brief Issue SET_ETHERNET_PACKET_FILTER (bRequest=0x43) to the
 *        attached CDC-ECM adapter.
 *
 * @details
 * Builds a class-interface-out SETUP packet
 * (bmRequestType=0x21, bRequest=0x43) with the requested filter mask
 * in `wValue` and `wLength=0`, and hands it to
 * `ra8_usb_host_setup_request`. See USB CDC ECM 1.20 sec 6.2.4
 * "SetEthernetPacketFilter".
 *
 * @param[in] filter_mask OR of `k_ra8_hcdc_ecm_filter_*` bits.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no adapter
 *         attached.
 * @retval k_ra8_err_invalid_arg `filter_mask` has bits outside the
 *         documented set.
 * @retval k_ra8_err_busy A control transfer is already in flight.
 *
 * @pre `ra8_usb_hcdc_ecm_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post DCP control transfer queued at the controller.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_set_packet_filter(uint16_t filter_mask);

/**
 * @brief Issue GET_ETHERNET_STATISTIC (bRequest=0x44) to read the
 *        adapter's link state and store the result in `*out_link`.
 *
 * @details
 * Builds a class-interface-in SETUP packet
 * (bmRequestType=0xA1, bRequest=0x44) with a 1-byte data stage that
 * carries the latest NetworkConnection notification value (0 = down,
 * 1 = up). See USB CDC ECM 1.20 sec 6.2.5 "GetEthernetStatistic".
 *
 * @param[out] out_link Receives the current link state.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Control transfer queued; `*out_link` populated.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no adapter
 *         attached.
 * @retval k_ra8_err_null_ptr `out_link` was NULL.
 * @retval k_ra8_err_busy A control transfer is already in flight.
 *
 * @pre `ra8_usb_hcdc_ecm_init` succeeded.
 * @pre Attach callback already fired.
 * @pre `out_link != NULL`.
 *
 * @post `*out_link` reflects the cached NetworkConnection state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_get_link_status(ra8_usb_hcdc_ecm_link_t* out_link);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Drive the enumeration step machine forward by one step.
 *
 * @details
 * Test / debug entry point. The production path drives this from the
 * `ra8_usb_dispatch` callback when the controller fires a CTRT or BRDY
 * interrupt; tests call it directly to walk the state machine
 * deterministically.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 *
 * @pre `ra8_usb_hcdc_ecm_init` succeeded.
 *
 * @post Internal enumeration step counter advances by one. When the
 *       step machine reaches the terminal state, the registered
 *       attach callback fires.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_step(void);

/**
 * @brief Parse 12 ASCII hex characters from an iMACAddress string
 *        descriptor payload into a 6-byte MAC.
 *
 * @details
 * USB string descriptors are UTF-16LE; the iMACAddress payload is a
 * 12-character ASCII-hex MAC encoded as 12 UTF-16LE code units (24
 * bytes). This helper walks `chars` two ASCII hex digits at a time and
 * writes each parsed nibble pair to `out_mac[0..5]`.
 *
 * See USB CDC ECM 1.20 sec 5.4 "iMACAddress".
 *
 * @param[in]  chars   Pointer to 12 ASCII hex characters (post-UTF-16
 *                     decoding).
 * @param[out] out_mac Receives the 6-byte MAC.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Parsed successfully.
 * @retval k_ra8_err_null_ptr `chars` or `out_mac` was NULL.
 * @retval k_ra8_err_invalid_arg One of the characters was not a valid
 *         ASCII hex digit.
 *
 * @pre `chars` points to >= 12 readable bytes.
 * @pre `out_mac` points to >= 6 writable bytes.
 *
 * @post On success, `out_mac[0..5]` carries the parsed MAC.
 * @post On failure, `out_mac` contents are unspecified.
 *
 * @note Pure function; safe to call before init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hcdc_ecm_parse_mac(const char* chars, uint8_t* out_mac);

#ifdef __cplusplus
}
#endif
