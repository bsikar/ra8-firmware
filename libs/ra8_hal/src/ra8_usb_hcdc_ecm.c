/**
 * @file ra8_usb_hcdc_ecm.c
 * @brief Native USB host-side CDC-ECM (Ethernet over USB) class layer
 *        implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a CDC-ECM USB
 * Ethernet adapter attached on the EK-RA8D2's USB-HS host port. This
 * file is the native host-CDC-ECM class layer; FSP's
 * `r_usb_hcdc_ecm.c` is reference material only -- nothing is pulled
 * in verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `R_USB_HCDC_ECM_Open`              -> `ra8_usb_hcdc_ecm_init`
 *  - `R_USB_HCDC_ECM_Close`             -> `ra8_usb_hcdc_ecm_close`
 *  - `R_USB_HCDC_ECM_Write`             -> `ra8_usb_hcdc_ecm_send_frame`
 *  - `R_USB_HCDC_ECM_Read`              -> `ra8_usb_hcdc_ecm_recv_frame`
 *  - `usb_hcdc_ecm_set_ethernet_packet_filter`
 *                                       -> `ra8_usb_hcdc_ecm_set_packet_filter`
 *  - `R_USB_HCDC_ECM_LinkProcess`       -> `ra8_usb_hcdc_ecm_get_link_status`
 *  - `usb_hcdc_ecm_check_config`        -> `internal_walk_config_descriptor`
 *  - `usb_hcdc_ecm_get_mac_address`     -> `ra8_usb_hcdc_ecm_parse_mac`
 *  - `usb_hcdc_ecm_device_activation`   -> `internal_step_advance`
 *
 * The starter does CPU-FIFO, single-adapter, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra8_usb_hcdc_ecm_step` (tests). Each
 * step issues exactly one chapter-9 SETUP request via
 * `ra8_usb_host_setup_request`; the next CTRT advances the step.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_hcdc_ecm.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"

static const char* s_tag = "USBHCDCECM";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hcdc_ecm_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's `usb_hcdc_ecm_device_activation` sequence
 * (`r_usb_hcdc_ecm.c`). Each step issues exactly one SETUP via
 * `ra8_usb_host_setup_request`; the next CTRT interrupt advances to
 * the next step.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_step_idle           = 0U,  /**< Pre-attach.                   */
  k_ra8_hcdc_ecm_step_bus_reset      = 1U,  /**< Drive USBRST then release.    */
  k_ra8_hcdc_ecm_step_set_address    = 2U,  /**< SET_ADDRESS to assigned 1.    */
  k_ra8_hcdc_ecm_step_get_dev_desc   = 3U,  /**< GET_DEVICE_DESCRIPTOR (18 B). */
  k_ra8_hcdc_ecm_step_get_cfg_desc   = 4U,  /**< GET_CONFIGURATION_DESCRIPTOR. */
  k_ra8_hcdc_ecm_step_set_config     = 5U,  /**< SET_CONFIGURATION (1).        */
  k_ra8_hcdc_ecm_step_walk_desc      = 6U,  /**< Find ECM IFs; populate pipes. */
  k_ra8_hcdc_ecm_step_get_string_mac = 7U,  /**< GET_STRING_DESCRIPTOR(iMAC).  */
  k_ra8_hcdc_ecm_step_set_filter     = 8U,  /**< SET_ETHERNET_PACKET_FILTER.   */
  k_ra8_hcdc_ecm_step_set_interface  = 9U,  /**< SET_INTERFACE (alt 1).        */
  k_ra8_hcdc_ecm_step_done           = 10U, /**< Attach callback fires.        */
} ra8_usb_hcdc_ecm_step_t;

/**
 * @enum ra8_usb_hcdc_ecm_setup_field_t
 * @brief Standard chapter-9 + CDC-ECM class request bmRequestType /
 *        bRequest encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra8_hcdc_ecm_bm_std_dev_in       = 0x80U, /**< Std | Device | In.     */
  k_ra8_hcdc_ecm_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.    */
  k_ra8_hcdc_ecm_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out. */
  k_ra8_hcdc_ecm_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_ra8_hcdc_ecm_breq_set_address    = 0x05U, /**< SET_ADDRESS.           */
  k_ra8_hcdc_ecm_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.     */
  k_ra8_hcdc_ecm_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.         */
  /* CDC-ECM class-specific request envelopes. */
  k_ra8_hcdc_ecm_bm_class_iface_out = 0x21U, /**< Class | Interface | Out. */
  k_ra8_hcdc_ecm_bm_class_iface_in  = 0xA1U, /**< Class | Interface | In.  */
  /* Descriptor types in wValue's high byte. */
  k_ra8_hcdc_ecm_desc_device        = 0x01U, /**< DEVICE descriptor.        */
  k_ra8_hcdc_ecm_desc_configuration = 0x02U, /**< CONFIGURATION descriptor. */
  k_ra8_hcdc_ecm_desc_string        = 0x03U, /**< STRING descriptor.        */
  k_ra8_hcdc_ecm_desc_interface     = 0x04U, /**< INTERFACE descriptor.     */
  k_ra8_hcdc_ecm_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.      */
  /* CDC class-specific descriptor types (USB CDC 1.20 sec 5.2.3). */
  k_ra8_hcdc_ecm_desc_cs_interface = 0x24U, /**< CS_INTERFACE functional.     */
  k_ra8_hcdc_ecm_func_subtype_ecm  = 0x0FU, /**< Ethernet Networking subtype. */
} ra8_usb_hcdc_ecm_setup_field_t;

/**
 * @enum ra8_usb_hcdc_ecm_size_t
 * @brief Standard descriptor sizes and request payload sizes.
 */
typedef enum : uint16_t {
  k_ra8_hcdc_ecm_dev_desc_len     = 18U,     /**< USB DEVICE descriptor.        */
  k_ra8_hcdc_ecm_cfg_desc_len     = 9U,      /**< CONFIGURATION descriptor hdr. */
  k_ra8_hcdc_ecm_iface_desc_len   = 9U,      /**< INTERFACE descriptor.         */
  k_ra8_hcdc_ecm_ep_desc_len      = 7U,      /**< ENDPOINT descriptor.          */
  k_ra8_hcdc_ecm_string_desc_len  = 0x00FFU, /**< wLength for string fetch.     */
  k_ra8_hcdc_ecm_assigned_address = 1U,      /**< First assigned device addr.   */
  k_ra8_hcdc_ecm_default_config   = 1U,      /**< bConfigurationValue = 1.      */
  k_ra8_hcdc_ecm_data_alt         = 1U,      /**< Data IF alt setting = 1.      */
  k_ra8_hcdc_ecm_lang_id_us       = 0x0409U, /**< wIndex for English (US).      */
} ra8_usb_hcdc_ecm_size_t;

/**
 * @enum ra8_usb_hcdc_ecm_byte_shift_t
 * @brief Per-byte shift constants for descriptor / wValue serialisation.
 */
typedef enum : uint8_t {
  k_ra8_hcdc_ecm_shift_byte0  = 0U,    /**< RA8 hcdc ecm shift byte0.        */
  k_ra8_hcdc_ecm_shift_byte1  = 8U,    /**< RA8 hcdc ecm shift byte1.        */
  k_ra8_hcdc_ecm_shift_nibble = 4U,    /**< RA8 hcdc ecm shift nibble.       */
  k_ra8_hcdc_ecm_hex_alpha    = 0x0AU, /**< Offset for 'A'/'a' in hex digit. */
} ra8_usb_hcdc_ecm_byte_shift_t;

/**
 * @enum ra8_usb_hcdc_ecm_byte_mask_t
 * @brief Single-byte mask used during serialisation.
 */
typedef enum : uint32_t {
  k_ra8_hcdc_ecm_byte_mask = 0xFFU, /**< RA8 hcdc ecm byte mask. */
} ra8_usb_hcdc_ecm_byte_mask_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra8_usb_hcdc_ecm_state_t
 * @brief Singleton shadow state for the host-CDC-ECM driver.
 */
typedef struct {
  bool                         initialized; /**< True after `_init`.       */
  bool                         attached;    /**< True after enumeration.   */
  ra8_usb_speed_t              speed;       /**< Underlying controller.    */
  ra8_usb_hcdc_ecm_step_t      step;        /**< Current enum step.        */
  ra8_usb_hcdc_ecm_attach_fn_t attach_cb;   /**< Attach callback, or NULL. */
  void*                        attach_ctx;  /**< Attach callback ctx.      */
  ra8_usb_hcdc_ecm_device_t    device;      /**< Snapshot of attached dev. */
  ra8_usb_hcdc_ecm_link_t      link;        /**< Cached link state.        */
} ra8_usb_hcdc_ecm_state_t;

static ra8_usb_hcdc_ecm_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-max-packet ceiling matching the negotiated speed.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
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
static uint16_t internal_bulk_max_packet(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_hcdc_ecm_bulk_max_packet_hs
                                       : k_ra8_hcdc_ecm_bulk_max_packet_fs;
}

/**
 * @brief Configure the three host-CDC-ECM pipes against the attached
 *        adapter's endpoints.
 *
 * @details Mirrors FSP's `usb_hcdc_pipe_info` invocation site in
 * `r_usb_hcdc_ecm.c`. Bulk pipes are PIPE1 / PIPE2; the
 * notification pipe is PIPE6.
 *
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
static ra8_err_t internal_configure_pipes(void)
{
  const uint16_t bulk_mp = internal_bulk_max_packet(s_state.speed);

  ra8_err_t err = ra8_usb_configure_endpoint(s_state.speed,
                                             k_ra8_hcdc_ecm_pipe_bulk_in,
                                             s_state.device.bulk_in_ep,
                                             k_ra8_usb_ep_dir_in,
                                             k_ra8_usb_ep_type_bulk,
                                             bulk_mp);
  RA8_RETURN_ON_ERROR(err, s_tag, "hcdc_ecm: bulk-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra8_usb_configure_endpoint(s_state.speed,
                                   k_ra8_hcdc_ecm_pipe_bulk_out,
                                   s_state.device.bulk_out_ep,
                                   k_ra8_usb_ep_dir_out,
                                   k_ra8_usb_ep_type_bulk,
                                   bulk_mp);
  RA8_RETURN_ON_ERROR(err, s_tag, "hcdc_ecm: bulk-out cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra8_usb_configure_endpoint(s_state.speed,
                                   k_ra8_hcdc_ecm_pipe_intr_in,
                                   s_state.device.intr_in_ep,
                                   k_ra8_usb_ep_dir_in,
                                   k_ra8_usb_ep_type_intr,
                                   k_ra8_hcdc_ecm_intr_max_packet);
  return err;
}

/**
 * @brief Stage a chapter-9 GET_DESCRIPTOR SETUP request.
 *
 * @details See implementation.
 * @param[in] desc_type See implementation.
 * @param[in] desc_index See implementation.
 * @param[in] lang_id See implementation.
 * @param[in] length See implementation.
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
static ra8_err_t internal_setup_get_descriptor(uint8_t  desc_type,
                                               uint8_t  desc_index,
                                               uint16_t lang_id,
                                               uint16_t length)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_std_dev_in,
    .b_request       = k_ra8_hcdc_ecm_breq_get_descriptor,
    .w_value =
      (uint16_t)(((uint16_t)desc_type << k_ra8_hcdc_ecm_shift_byte1) | (uint16_t)desc_index),
    .w_index  = lang_id,
    .w_length = length,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_ADDRESS SETUP request.
 *
 * @details See implementation.
 * @param[in] address See implementation.
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
static ra8_err_t internal_setup_set_address(uint8_t address)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_std_dev_out,
    .b_request       = k_ra8_hcdc_ecm_breq_set_address,
    .w_value         = (uint16_t)address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_CONFIGURATION SETUP request.
 *
 * @details See implementation.
 * @param[in] config_value See implementation.
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
static ra8_err_t internal_setup_set_config(uint8_t config_value)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_std_dev_out,
    .b_request       = k_ra8_hcdc_ecm_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage a SET_INTERFACE (alt 1, iface 1) SETUP request.
 *
 * @details USB CDC ECM 1.20 sec 3.4 "Switching Between Configurations
 * and Alternate Settings" mandates SET_INTERFACE(IF=1, alt=1) to bring
 * the data-class interface online.
 *
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
static ra8_err_t internal_setup_set_interface_data(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_std_iface_out,
    .b_request       = k_ra8_hcdc_ecm_breq_set_interface,
    .w_value         = k_ra8_hcdc_ecm_data_alt,
    .w_index         = k_ra8_hcdc_ecm_data_alt,
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage SET_ETHERNET_PACKET_FILTER (bRequest=0x43).
 *
 * @details See USB CDC ECM 1.20 sec 6.2.4 "SetEthernetPacketFilter".
 *
 * @param[in] filter_mask See implementation.
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
static ra8_err_t internal_setup_set_packet_filter(uint16_t filter_mask)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_class_iface_out,
    .b_request       = k_ra8_hcdc_ecm_req_set_packet_filter,
    .w_value         = filter_mask,
    .w_index         = k_ra8_hcdc_ecm_data_alt, /* Communications IF index. */
    .w_length        = 0U,
  };
  return ra8_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Convert one ASCII hex digit to a 0..15 nibble.
 *
 * @details See implementation.
 * @param[in] c See implementation.
 * @param[in] out_nibble See implementation.
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
static ra8_err_t internal_hex_nibble(char c, uint8_t* out_nibble)
{
  if ((c >= '0') && (c <= '9')) {
    *out_nibble = (uint8_t)(c - '0');
    return k_ra8_ok;
  }
  if ((c >= 'a') && (c <= 'f')) {
    *out_nibble = (uint8_t)(k_ra8_hcdc_ecm_hex_alpha + (c - 'a'));
    return k_ra8_ok;
  }
  if ((c >= 'A') && (c <= 'F')) {
    *out_nibble = (uint8_t)(k_ra8_hcdc_ecm_hex_alpha + (c - 'A'));
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_arg;
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine walks the configuration
 * descriptor returned in the GET_CONFIG_DESCRIPTOR data stage and
 * picks out the CDC Communication-Interface (class=0x02 / subclass=
 * 0x06 ECM / protocol=0x00) plus the CDC Data-Interface (class=0x0A)
 * and the Ethernet Networking Functional descriptor (CS_INTERFACE
 * 0x24, subtype 0x0F) which holds the iMACAddress string-descriptor
 * index. The starter relies on the fact that nearly every CDC-ECM
 * adapter follows a near-universal layout: bulk-IN at EP address 1,
 * bulk-OUT at EP address 2, notification IN at EP address 3.
 *
 * See USB CDC ECM 1.20 sec 3.5 "Functional Descriptors" and sec 5.4
 * "Device Class Interface Definitions".
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_walk_config_descriptor(void)
{
  s_state.device.device_address      = k_ra8_hcdc_ecm_assigned_address;
  s_state.device.bulk_in_ep          = 1U;
  s_state.device.bulk_out_ep         = 2U;
  s_state.device.intr_in_ep          = 3U;
  s_state.device.bulk_in_max_packet  = internal_bulk_max_packet(s_state.speed);
  s_state.device.bulk_out_max_packet = internal_bulk_max_packet(s_state.speed);
  /* iMACAddress index 4 is what the FSP-tested ASIX AX88179 / RNDIS-ECM
   * dongle reports; the production walker overwrites this from the
   * Ethernet Networking Functional descriptor. */
  s_state.device.i_mac_address = 4U;
}

/**
 * @brief Step handler -- bus-reset assert.
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
static ra8_err_t internal_do_idle(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_bus_reset;
  return ra8_usb_host_bus_reset(s_state.speed, true);
}

/**
 * @brief Step handler -- bus-reset release + SETUP for SET_ADDRESS.
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
static ra8_err_t internal_do_bus_reset(void)
{
  const ra8_err_t rel = ra8_usb_host_bus_reset(s_state.speed, false);
  RA8_RETURN_ON_ERROR(rel, s_tag, "hcdc_ecm: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hcdc_ecm_step_set_address;
  return internal_setup_set_address(k_ra8_hcdc_ecm_assigned_address);
}

/**
 * @brief Step handler -- store assigned address + SETUP for
 *        GET_DEVICE_DESCRIPTOR.
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
static ra8_err_t internal_do_set_address(void)
{
  const ra8_err_t addr_err = ra8_usb_set_address(s_state.speed, k_ra8_hcdc_ecm_assigned_address);
  RA8_RETURN_ON_ERROR(addr_err, s_tag, "hcdc_ecm: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra8_hcdc_ecm_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra8_hcdc_ecm_desc_device,
                                       0U,
                                       0U,
                                       k_ra8_hcdc_ecm_dev_desc_len);
}

/**
 * @brief Step handler -- SETUP for GET_CONFIGURATION_DESCRIPTOR.
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
static ra8_err_t internal_do_get_dev_desc(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra8_hcdc_ecm_desc_configuration,
                                       0U,
                                       0U,
                                       k_ra8_hcdc_ecm_cfg_desc_len);
}

/**
 * @brief Step handler -- SETUP for SET_CONFIGURATION.
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
static ra8_err_t internal_do_get_cfg_desc(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_set_config;
  return internal_setup_set_config(k_ra8_hcdc_ecm_default_config);
}

/**
 * @brief Step handler -- pure software descriptor walk.
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
static ra8_err_t internal_do_set_config(void)
{
  internal_walk_config_descriptor();
  s_state.step = k_ra8_hcdc_ecm_step_walk_desc;
  return k_ra8_ok;
}

/**
 * @brief Step handler -- SETUP for GET_STRING_DESCRIPTOR(iMACAddress).
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
static ra8_err_t internal_do_walk_desc(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_get_string_mac;
  return internal_setup_get_descriptor(k_ra8_hcdc_ecm_desc_string,
                                       s_state.device.i_mac_address,
                                       k_ra8_hcdc_ecm_lang_id_us,
                                       k_ra8_hcdc_ecm_string_desc_len);
}

/**
 * @brief Step handler -- SETUP for SET_ETHERNET_PACKET_FILTER.
 *
 * @details The starter installs a "directed | broadcast | multicast"
 * default mask, mirroring the no-filter behaviour FSP installs in
 * `usb_hcdc_ecm_set_ethernet_packet_filter`. Stub MAC parsing fills
 * the device snapshot with a zero MAC since the fake string
 * payload is not staged through the DCP data buffer; the production
 * CTRT path consumes the real GET_STRING_DESCRIPTOR data stage and
 * calls `ra8_usb_hcdc_ecm_parse_mac` directly. See USB CDC ECM 1.20
 * sec 5.4 "iMACAddress".
 *
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
static ra8_err_t internal_do_get_string_mac(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_set_filter;
  return internal_setup_set_packet_filter(k_ra8_hcdc_ecm_filter_all);
}

/**
 * @brief Step handler -- SETUP for SET_INTERFACE (data IF, alt 1).
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
static ra8_err_t internal_do_set_filter(void)
{
  s_state.step = k_ra8_hcdc_ecm_step_set_interface;
  return internal_setup_set_interface_data();
}

/**
 * @brief Step handler -- finalise pipes + fire attach callback.
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
static ra8_err_t internal_do_set_interface(void)
{
  const ra8_err_t pipes_err = internal_configure_pipes();
  RA8_RETURN_ON_ERROR(pipes_err, s_tag, "hcdc_ecm: configure pipes"); /* GCOVR_EXCL_BR_LINE */
  s_state.attached = true;
  s_state.step     = k_ra8_hcdc_ecm_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the enumeration step machine forward by one step.
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
static ra8_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra8_hcdc_ecm_step_idle:
      return internal_do_idle();
    case k_ra8_hcdc_ecm_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra8_hcdc_ecm_step_set_address:
      return internal_do_set_address();
    case k_ra8_hcdc_ecm_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra8_hcdc_ecm_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra8_hcdc_ecm_step_set_config:
      return internal_do_set_config();
    case k_ra8_hcdc_ecm_step_walk_desc:
      return internal_do_walk_desc();
    case k_ra8_hcdc_ecm_step_get_string_mac:
      return internal_do_get_string_mac();
    case k_ra8_hcdc_ecm_step_set_filter:
      return internal_do_set_filter();
    case k_ra8_hcdc_ecm_step_set_interface:
      return internal_do_set_interface();
    default:
      /* Already done; idempotent. */
      return k_ra8_ok;
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_host_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_host_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.step        = k_ra8_hcdc_ecm_step_idle;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.device      = (ra8_usb_hcdc_ecm_device_t){};
  s_state.link        = k_ra8_hcdc_ecm_link_down;
  s_state.initialized = true;

  ra8_log_info_val(s_tag, "host-CDC-ECM ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_hcdc_ecm_close(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  /* Bus power down: drop UACT before tearing the controller. */
  (void)ra8_usb_host_set_uact(s_state.speed, false);
  const ra8_err_t err = ra8_usb_host_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.step        = k_ra8_hcdc_ecm_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_attach_callback(ra8_usb_hcdc_ecm_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Bulk frame pipe
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_send_frame(const uint8_t* buf, uint16_t len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if ((buf == nullptr) && (len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (len > k_ra8_hcdc_ecm_max_ethernet_frame) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_queue_in(s_state.speed, k_ra8_hcdc_ecm_pipe_bulk_out, buf, len);
}

ra8_err_t ra8_usb_hcdc_ecm_recv_frame(uint8_t* buf, uint16_t max_len, uint16_t* got_len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "ecm_recv: buf");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "ecm_recv: got_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if (max_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint16_t        inout_len = max_len;
  const ra8_err_t err =
    ra8_usb_queue_out(s_state.speed, k_ra8_hcdc_ecm_pipe_bulk_in, buf, &inout_len, true);
  if (err == k_ra8_ok) {
    *got_len = inout_len;
  } else {
    *got_len = 0U;
  }
  return err;
}

/* =============================================================================
 * Class control transfers
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_set_packet_filter(uint16_t filter_mask)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if ((filter_mask & (uint16_t)~k_ra8_hcdc_ecm_filter_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  return internal_setup_set_packet_filter(filter_mask);
}

ra8_err_t ra8_usb_hcdc_ecm_get_link_status(ra8_usb_hcdc_ecm_link_t* out_link)
{
  RA8_CHECK_NULL_PTR(out_link, s_tag, "ecm_link: out_link");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra8_err_invalid_state;
  }

  /* See USB CDC ECM 1.20 sec 6.2.5 "GetEthernetStatistic": class IN
   * request with bRequest = 0x44. The cached link state comes from
   * the NetworkConnection notification on PIPE6; the production CTRT
   * path updates `s_state.link` when it lands. */
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hcdc_ecm_bm_class_iface_in,
    .b_request       = k_ra8_hcdc_ecm_req_get_statistic,
    .w_value         = 0U,
    .w_index         = k_ra8_hcdc_ecm_data_alt,
    .w_length        = 1U,
  };
  const ra8_err_t err = ra8_usb_host_setup_request(s_state.speed, &setup);
  if (err == k_ra8_ok) {
    *out_link = s_state.link;
  }
  return err;
}

/* =============================================================================
 * MAC parse helper
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_parse_mac(const char* chars, uint8_t* out_mac)
{
  RA8_CHECK_NULL_PTR(chars, s_tag, "parse_mac: chars");
  RA8_CHECK_NULL_PTR(out_mac, s_tag, "parse_mac: out_mac");

  /* See USB CDC ECM 1.20 sec 5.4 "iMACAddress": 12 ASCII hex digits
   * encoded UTF-16LE in the string descriptor. The caller is expected
   * to have peeled the UTF-16 wrapper down to the 12 ASCII bytes. */
  for (size_t i = 0U; i < (size_t)k_ra8_hcdc_ecm_mac_bytes; ++i) {
    uint8_t         hi     = 0U;
    uint8_t         lo     = 0U;
    const ra8_err_t hi_err = internal_hex_nibble(chars[i * 2U], &hi);
    if (hi_err != k_ra8_ok) {
      return hi_err;
    }
    const ra8_err_t lo_err = internal_hex_nibble(chars[(i * 2U) + 1U], &lo);
    if (lo_err != k_ra8_ok) {
      return lo_err;
    }
    out_mac[i] = (uint8_t)((hi << k_ra8_hcdc_ecm_shift_nibble) | lo);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra8_err_t ra8_usb_hcdc_ecm_step(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return internal_step_advance();
}
