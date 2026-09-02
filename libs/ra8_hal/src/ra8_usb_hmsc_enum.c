/**
 * @file ra8_usb_hmsc_enum.c
 * @brief Native USB host-side MSC (Mass Storage Class) polled enumeration
 *        ladder
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Sibling translation unit of `ra8_usb_hmsc.c`, split out purely to satisfy
 * the per-file size cap. Holds the hardware-proven polled enumeration ladder
 * (`ra8_usb_hmsc_enumerate`) and its `internal_enum_*` helpers: wait for the
 * D+ attach, hunt the (reset, address) combination the device answers on,
 * assign address 1, read + parse the configuration descriptor set, run
 * SET_CONFIGURATION / GET_MAX_LUN, program the bulk pipes, and fire the
 * attach callback. Every chapter-9 SETUP goes through
 * `ra8_usb_host_setup_request`.
 *
 * The singleton shadow state (`g_usb_hmsc_state`) lives in `ra8_usb_hmsc.c`
 * and is shared with this TU through `ra8_hal_internal.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "ra8_usb_hmsc_internal.h"

static const char* s_tag = "USBHMSC";

/* =============================================================================
 * Polled enumeration ladder (hardware-proven; replaces the CTRT machine)
 * =============================================================================
 */

/**
 * @enum ra8_usb_hmsc_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_ra8_hmsc_vbus_settle_ms = 200U,  /**< Supply settle before probing.    */
  k_ra8_hmsc_attach_to_ms   = 2000U, /**< Wait for the D+ pull-up.         */
  k_ra8_hmsc_debounce_ms    = 500U,  /**< Post-attach debounce (>=100 ms). */
  k_ra8_hmsc_reset_hold_ms  = 50U,   /**< USB bus-reset hold (>=10 ms).    */
  k_ra8_hmsc_recovery_ms    = 20U,   /**< Post-reset recovery (TRSTRCY).   */
  k_ra8_hmsc_addr_settle_ms = 5U,    /**< Post-SET_ADDRESS recovery.       */
  k_ra8_hmsc_enum_tries     = 8U,    /**< (reset?, addr) hunt attempts.    */
  k_ra8_hmsc_no_reset_tries = 4U,    /**< Attempts before using bus reset. */
  k_ra8_hmsc_addr_alt_mask  = 0x03U, /**< Alternate addr 0..3 per attempt. */
  k_ra8_hmsc_cfg_buf_len    = 128U,  /**< Full-configuration read buffer.  */
  /** P10 iteration bound on the attach wait: the loop is primarily
   * ms-bounded via `ra8_time_ms`, but if the tick is frozen (fake
   * builds, SysTick masked) the spin cap guarantees termination. */
  k_ra8_hmsc_attach_spin_limit = 50000000UL,
} ra8_usb_hmsc_enum_tune_t;

/**
 * @enum ra8_usb_hmsc_walk_off_t
 * @brief Descriptor-walk byte offsets and identity codes.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_off_dlen        = 0U,    /**< Any descriptor: bLength.         */
  k_ra8_hmsc_off_dtype       = 1U,    /**< Any descriptor: bDescriptorType. */
  k_ra8_hmsc_off_iface_num   = 2U,    /**< Interface: bInterfaceNumber.     */
  k_ra8_hmsc_off_iface_class = 5U,    /**< Interface: bInterfaceClass.      */
  k_ra8_hmsc_off_iface_sub   = 6U,    /**< Interface: bInterfaceSubClass.   */
  k_ra8_hmsc_off_iface_proto = 7U,    /**< Interface: bInterfaceProtocol.   */
  k_ra8_hmsc_off_ep_addr     = 2U,    /**< Endpoint: bEndpointAddress.      */
  k_ra8_hmsc_off_ep_attr     = 3U,    /**< Endpoint: bmAttributes.          */
  k_ra8_hmsc_off_ep_mps      = 4U,    /**< Endpoint: wMaxPacketSize LSB.    */
  k_ra8_hmsc_off_cfg_total   = 2U,    /**< Configuration: wTotalLength LSB. */
  k_ra8_hmsc_off_cfg_value   = 5U,    /**< Configuration: bConfigValue.     */
  k_ra8_hmsc_off_dev_vid     = 8U,    /**< Device: idVendor LSB.            */
  k_ra8_hmsc_off_dev_pid     = 10U,   /**< Device: idProduct LSB.           */
  k_ra8_hmsc_ep_dir_in_bit   = 0x80U, /**< bEndpointAddress direction bit.  */
  k_ra8_hmsc_ep_num_mask     = 0x0FU, /**< bEndpointAddress number field.   */
  k_ra8_hmsc_ep_attr_mask    = 0x03U, /**< bmAttributes transfer-type mask. */
  k_ra8_hmsc_ep_attr_bulk    = 0x02U, /**< bmAttributes: bulk transfer.     */
  k_ra8_hmsc_byte_bits       = 8U,    /**< Bit width of one byte.           */
} ra8_usb_hmsc_walk_off_t;

/**
 * @brief Read the 18-byte device descriptor over the polled control engine.
 *
 * @details GET_DESCRIPTOR(DEVICE) at whatever address the DCP currently
 * targets; requires the full 18 bytes back.
 *
 * @param[out] desc Receives the descriptor (18 bytes).
 * @return Read outcome.
 * @retval k_ra8_ok           All 18 bytes arrived.
 * @retval k_ra8_err_hw_error A short descriptor came back.
 * @pre The bus is reset and UACT is on.
 * @pre @p desc holds at least 18 bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No state is modified.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_read_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hmsc_bm_std_dev_in,
    .b_request       = k_ra8_hmsc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra8_hmsc_desc_device << k_ra8_hmsc_byte_bits),
    .w_index         = 0U,
    .w_length        = k_ra8_hmsc_dev_desc_len,
  };
  uint16_t        rx  = 0U;
  const ra8_err_t err = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed,
                                                  &setup,
                                                  desc,
                                                  (uint16_t)k_ra8_hmsc_dev_desc_len,
                                                  &rx);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: dev desc");
  if (rx != (uint16_t)k_ra8_hmsc_dev_desc_len) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Wait for a device to attach, then hunt for its address.
 *
 * @details Waits for the D+ pull-up (LNST leaves SE0) plus the spec
 * debounce, then tries each (reset?, address) combination: four gentle
 * attempts at addresses 0..3 without touching the bus, then four more
 * with a full bus reset (which also returns a previously addressed
 * device to address 0). The first combination whose device-descriptor
 * read returns all 18 bytes wins.
 *
 * @param[out] desc     Receives the winning 18-byte device descriptor.
 * @param[out] out_addr Receives the address the device answered at.
 * @return Hunt outcome.
 * @retval k_ra8_ok              The device answered.
 * @retval k_ra8_err_hw_timeout  Nothing attached / nothing answered.
 * @pre ::ra8_usb_hmsc_init ran (host mode up, VBUS supplied).
 * @pre ::ra8_time_init has run (the ladder uses millisecond delays).
 * @post On success the DCP targets `*out_addr` with UACT on.
 * @post On failure the bus state is whatever the last attempt left.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_hunt(uint8_t* desc, uint8_t* out_addr)
{
  ra8_delay_ms(k_ra8_hmsc_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_ra8_hmsc_attach_spin_limit; spin++) {
    if (ra8_usb_host_line_state(g_usb_hmsc_state.speed) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_ra8_hmsc_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms(k_ra8_hmsc_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_ra8_hmsc_enum_tries; attempt++) {
    if (attempt >= (uint8_t)k_ra8_hmsc_no_reset_tries) {
      (void)ra8_usb_host_bus_reset(g_usb_hmsc_state.speed, true);
      ra8_delay_ms(k_ra8_hmsc_reset_hold_ms);
      (void)ra8_usb_host_bus_reset(g_usb_hmsc_state.speed, false);
    }
    (void)ra8_usb_host_set_uact(g_usb_hmsc_state.speed, true);
    ra8_delay_ms(k_ra8_hmsc_recovery_ms);
    const uint8_t addr = (uint8_t)(attempt & (uint8_t)k_ra8_hmsc_addr_alt_mask);
    (void)ra8_usb_host_set_target(g_usb_hmsc_state.speed, addr);
    err = internal_enum_read_dev_desc(desc);
    if (err == k_ra8_ok) {
      *out_addr = addr;
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief Move the device to address 1 when it answered at the default.
 *
 * @details SET_CONFIGURATION is only legal from the Address state (sticks
 * STALL it at the default address), so assign address 1, honour the
 * set-address recovery, and retarget the DCP. Skipped when the hunt
 * already found the device addressed.
 *
 * @param[in,out] dev_addr In: hunt result. Out: the operating address.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP targets the operating address.
 * @pre ::internal_enum_hunt succeeded.
 * @pre The bus is active (UACT on).
 * @post `*dev_addr` is non-zero on success.
 * @post Later transfers carry tokens to the new address.
 * @note Blocking (one polled control transfer + settle).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_assign_addr(uint8_t* dev_addr)
{
  if (*dev_addr != 0U) {
    return k_ra8_ok;
  }
  const ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_hmsc_bm_std_dev_out,
    .b_request       = k_ra8_hmsc_breq_set_address,
    .w_value         = k_ra8_hmsc_assigned_address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed, &setup, nullptr, 0U, nullptr);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: set address");
  ra8_delay_ms(k_ra8_hmsc_addr_settle_ms);
  err = ra8_usb_host_set_target(g_usb_hmsc_state.speed, (uint8_t)k_ra8_hmsc_assigned_address);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: set target");
  *dev_addr = (uint8_t)k_ra8_hmsc_assigned_address;
  return k_ra8_ok;
}

/**
 * @brief Record one bulk endpoint descriptor into the device snapshot.
 *
 * @details Filters for bmAttributes == bulk and slots the endpoint into
 * the IN or OUT position (first match wins) with its wMaxPacketSize.
 *
 * @param[in] d Pointer to an endpoint descriptor.
 * @pre @p d points at a descriptor with bDescriptorType ENDPOINT.
 * @pre The unfilled `g_usb_hmsc_state.device` endpoint slots are zero.
 * @post A matching bulk endpoint is recorded once.
 * @post Non-bulk endpoints leave the snapshot untouched.
 * @note Pure helper for the config-descriptor walk.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_enum_note_endpoint(const uint8_t* d)
{
  const uint8_t attr = (uint8_t)(d[k_ra8_hmsc_off_ep_attr] & (uint8_t)k_ra8_hmsc_ep_attr_mask);
  if (attr != (uint8_t)k_ra8_hmsc_ep_attr_bulk) {
    return;
  }
  const uint8_t  ea = d[k_ra8_hmsc_off_ep_addr];
  const uint16_t mps =
    (uint16_t)((uint16_t)d[k_ra8_hmsc_off_ep_mps] |
               (uint16_t)((uint16_t)d[k_ra8_hmsc_off_ep_mps + 1U] << k_ra8_hmsc_byte_bits));
  if ((ea & (uint8_t)k_ra8_hmsc_ep_dir_in_bit) != 0U) {
    if (g_usb_hmsc_state.device.bulk_in_ep == 0U) {
      g_usb_hmsc_state.device.bulk_in_ep         = (uint8_t)(ea & (uint8_t)k_ra8_hmsc_ep_num_mask);
      g_usb_hmsc_state.device.bulk_in_max_packet = mps;
    }
  } else {
    if (g_usb_hmsc_state.device.bulk_out_ep == 0U) {
      g_usb_hmsc_state.device.bulk_out_ep         = (uint8_t)(ea & (uint8_t)k_ra8_hmsc_ep_num_mask);
      g_usb_hmsc_state.device.bulk_out_max_packet = mps;
    }
  }
}

/**
 * @brief Test whether an interface descriptor is MSC SCSI Bulk-Only.
 *
 * @details Matches class 0x08 (mass storage), subclass 0x06 (SCSI
 * transparent), protocol 0x50 (Bulk-Only Transport) -- the trio every
 * consumer thumb drive reports.
 *
 * @param[in] d Interface descriptor bytes (9 valid bytes).
 * @return true when the interface is MSC SCSI BOT.
 * @retval false Any of the three class fields differs.
 * @pre @p d is non-NULL and points at an interface descriptor.
 * @pre The descriptor passed the walker's length check.
 * @post No state changes.
 * @post @p d is unmodified.
 * @note Pure helper for ::internal_enum_walk_cfg.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_enum_iface_is_msc(const uint8_t* d)
{
  if (d[k_ra8_hmsc_off_iface_class] != (uint8_t)k_ra8_hmsc_class_msc) {
    return false;
  }
  if (d[k_ra8_hmsc_off_iface_sub] != (uint8_t)k_ra8_hmsc_subclass_scsi) {
    return false;
  }
  if (d[k_ra8_hmsc_off_iface_proto] != (uint8_t)k_ra8_hmsc_protocol_bbb) {
    return false;
  }
  return true;
}

/**
 * @brief Walk a configuration blob for the MSC interface + bulk endpoints.
 *
 * @details Strides descriptor-by-descriptor; an interface descriptor with
 * class 0x08 / subclass 0x06 / protocol 0x50 opens the MSC scope, and the
 * bulk endpoints inside it populate the device snapshot.
 *
 * @param[in] cfg Configuration descriptor bytes.
 * @param[in] len Valid byte count in @p cfg.
 * @return Walk outcome.
 * @retval k_ra8_ok           Both bulk endpoints were found.
 * @retval k_ra8_err_hw_error No MSC bulk endpoint pair in the blob.
 * @pre @p cfg is non-NULL with @p len valid bytes.
 * @pre The device snapshot endpoint slots start zeroed.
 * @post On k_ra8_ok the snapshot carries eps, max packets, and iface.
 * @post @p cfg is unmodified.
 * @note Pure helper for ::internal_enum_read_config.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_walk_cfg(const uint8_t* cfg, uint16_t len)
{
  uint16_t off    = 0U;
  bool     in_msc = false;
  while (off < len) {
    const uint8_t dlen = cfg[off + (uint16_t)k_ra8_hmsc_off_dlen];
    if (dlen == 0U) {
      break;
    }
    const uint8_t dtype = cfg[off + (uint16_t)k_ra8_hmsc_off_dtype];
    if (dtype == (uint8_t)k_ra8_hmsc_desc_interface) {
      in_msc = internal_enum_iface_is_msc(&cfg[off]);
      if (in_msc) {
        g_usb_hmsc_state.device.interface_number = cfg[off + (uint16_t)k_ra8_hmsc_off_iface_num];
      }
    }
    if (dtype == (uint8_t)k_ra8_hmsc_desc_endpoint) {
      if (in_msc) {
        internal_enum_note_endpoint(&cfg[off]);
      }
    }
    off = (uint16_t)(off + dlen);
  }
  if (g_usb_hmsc_state.device.bulk_in_ep == 0U) {
    return k_ra8_err_hw_error;
  }
  if (g_usb_hmsc_state.device.bulk_out_ep == 0U) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Read + parse the configuration descriptor set.
 *
 * @details Reads the 9-byte header for wTotalLength + bConfigurationValue,
 * re-reads the full set (clamped to the local buffer), and walks it for
 * the MSC interface and bulk endpoints.
 *
 * @param[out] out_cfg_value Receives bConfigurationValue.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The device snapshot carries the MSC endpoints.
 * @pre The device is addressed and answering control reads.
 * @pre @p out_cfg_value is non-NULL.
 * @post On success the snapshot endpoints + interface are filled.
 * @post `*out_cfg_value` holds the value SET_CONFIGURATION needs.
 * @note Blocking (two polled control reads).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_read_config(uint8_t* out_cfg_value)
{
  uint8_t         cfg[k_ra8_hmsc_cfg_buf_len] = {};
  uint16_t        rx                          = 0U;
  ra8_usb_setup_t setup                       = {
    .bm_request_type = k_ra8_hmsc_bm_std_dev_in,
    .b_request       = k_ra8_hmsc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra8_hmsc_desc_configuration << k_ra8_hmsc_byte_bits),
    .w_index         = 0U,
    .w_length        = k_ra8_hmsc_cfg_desc_len,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed,
                                            &setup,
                                            cfg,
                                            (uint16_t)k_ra8_hmsc_cfg_desc_len,
                                            &rx);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: cfg header");
  uint16_t total =
    (uint16_t)((uint16_t)cfg[k_ra8_hmsc_off_cfg_total] |
               (uint16_t)((uint16_t)cfg[k_ra8_hmsc_off_cfg_total + 1U] << k_ra8_hmsc_byte_bits));
  if (total > (uint16_t)k_ra8_hmsc_cfg_buf_len) {
    total = (uint16_t)k_ra8_hmsc_cfg_buf_len;
  }
  *out_cfg_value = cfg[k_ra8_hmsc_off_cfg_value];
  setup.w_length = total;
  err            = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed, &setup, cfg, total, &rx);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: cfg full");
  return internal_enum_walk_cfg(cfg, rx);
}

/**
 * @brief SET_CONFIGURATION, best-effort GET_MAX_LUN, and pipe setup.
 *
 * @details Activates the parsed configuration (strict status), issues the
 * class GET_MAX_LUN (devices may STALL it, which legally means LUN 0, so
 * failures default to 0), then programs the bulk pipes against the
 * snapshot endpoints at @p dev_addr.
 *
 * @param[in] dev_addr  Address the device answers at.
 * @param[in] cfg_value bConfigurationValue to activate.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The device is configured and both pipes are ready.
 * @pre ::internal_enum_read_config filled the snapshot.
 * @pre The DCP targets @p dev_addr.
 * @post The bulk pipes are configured (DATA0, parked NAK).
 * @post `g_usb_hmsc_state.device.max_lun` is filled (0 on GET_MAX_LUN failure).
 * @note Blocking (polled control transfers).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_configure(uint8_t dev_addr, uint8_t cfg_value)
{
  const ra8_usb_setup_t set_cfg = {
    .bm_request_type = k_ra8_hmsc_bm_std_dev_out,
    .b_request       = k_ra8_hmsc_breq_set_config,
    .w_value         = cfg_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed, &set_cfg, nullptr, 0U, nullptr);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: set config");
  const ra8_usb_setup_t get_lun = {
    .bm_request_type = k_ra8_hmsc_bm_class_iface_in,
    .b_request       = k_ra8_hmsc_req_get_max_lun,
    .w_value         = 0U,
    .w_index         = g_usb_hmsc_state.device.interface_number,
    .w_length        = k_ra8_hmsc_get_max_lun_len,
  };
  uint8_t         lun             = 0U;
  uint16_t        lun_rx          = 0U;
  const ra8_err_t lun_err         = ra8_usb_host_control_xfer(g_usb_hmsc_state.speed,
                                                              &get_lun,
                                                              &lun,
                                                              (uint16_t)k_ra8_hmsc_get_max_lun_len,
                                                              &lun_rx);
  g_usb_hmsc_state.device.max_lun = 0U;
  if (lun_err == k_ra8_ok) {
    if (lun_rx == (uint16_t)k_ra8_hmsc_get_max_lun_len) {
      g_usb_hmsc_state.device.max_lun = lun;
    }
  }
  err = ra8_usb_host_pipe_setup(g_usb_hmsc_state.speed,
                                k_ra8_hmsc_pipe_bulk_in,
                                dev_addr,
                                g_usb_hmsc_state.device.bulk_in_ep,
                                true,
                                g_usb_hmsc_state.device.bulk_in_max_packet);
  RA8_RETURN_ON_ERROR(err, s_tag, "enum: pipe in");
  return ra8_usb_host_pipe_setup(g_usb_hmsc_state.speed,
                                 k_ra8_hmsc_pipe_bulk_out,
                                 dev_addr,
                                 g_usb_hmsc_state.device.bulk_out_ep,
                                 false,
                                 g_usb_hmsc_state.device.bulk_out_max_packet);
}

/**
 * @brief Unpack VID/PID from a device descriptor into the snapshot.
 *
 * @details Little-endian 16-bit fields at idVendor/idProduct.
 *
 * @param[in] desc Device descriptor bytes (18 valid bytes).
 * @pre @p desc is non-NULL and holds a device descriptor.
 * @pre The snapshot was reset for this enumeration pass.
 * @post `g_usb_hmsc_state.device.vendor_id` / `.product_id` are filled.
 * @post @p desc is unmodified.
 * @note Pure helper for ::ra8_usb_hmsc_enumerate.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_enum_fill_ids(const uint8_t* desc)
{
  g_usb_hmsc_state.device.vendor_id =
    (uint16_t)((uint16_t)desc[k_ra8_hmsc_off_dev_vid] |
               (uint16_t)((uint16_t)desc[k_ra8_hmsc_off_dev_vid + 1U] << k_ra8_hmsc_byte_bits));
  g_usb_hmsc_state.device.product_id =
    (uint16_t)((uint16_t)desc[k_ra8_hmsc_off_dev_pid] |
               (uint16_t)((uint16_t)desc[k_ra8_hmsc_off_dev_pid + 1U] << k_ra8_hmsc_byte_bits));
}

/**
 * @brief Publish a completed enumeration: snapshot, callback, out-copy.
 *
 * @details Stores the address, flips the attached flag, fires the
 * registered attach callback, and copies the snapshot to the caller.
 *
 * @param[in]  dev_addr   Address the device answers at.
 * @param[out] out_device Caller's snapshot copy (may be NULL).
 * @pre The bulk pipes are configured and SCSI calls may follow.
 * @pre The snapshot carries VID/PID, endpoints, and max-LUN.
 * @post `g_usb_hmsc_state.attached` is true; the callback (if any) has fired.
 * @post `*out_device` holds the snapshot when @p out_device is non-NULL.
 * @note Helper for ::ra8_usb_hmsc_enumerate.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_enum_publish(uint8_t dev_addr, ra8_usb_hmsc_device_t* out_device)
{
  g_usb_hmsc_state.device.device_address = dev_addr;
  g_usb_hmsc_state.attached              = true;
  if (g_usb_hmsc_state.attach_cb != nullptr) {
    g_usb_hmsc_state.attach_cb(g_usb_hmsc_state.attach_ctx, &g_usb_hmsc_state.device);
  }
  if (out_device != nullptr) {
    *out_device = g_usb_hmsc_state.device;
  }
}

/**
 * @brief Run the enumeration ladder: hunt, address, configure, pipes.
 *
 * @details Waits for the attach, hunts the (reset, address) combination
 * the device answers on, unpacks VID/PID, assigns address 1, parses +
 * activates the configuration, and programs the bulk pipes.
 *
 * @param[out] out_addr Receives the address the device answers at.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The device is configured and both pipes are ready.
 * @pre ::ra8_usb_hmsc_init succeeded and VBUS reaches the device.
 * @pre The snapshot was reset for this enumeration pass.
 * @post On success the snapshot carries IDs, endpoints, and max-LUN.
 * @post On failure the controller may need a fresh attach cycle.
 * @note Helper for ::ra8_usb_hmsc_enumerate (statement-count split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_enum_ladder(uint8_t* out_addr)
{
  uint8_t   desc[k_ra8_hmsc_dev_desc_len] = {};
  ra8_err_t err                           = internal_enum_hunt(desc, out_addr);
  RA8_RETURN_ON_ERROR(err, s_tag, "enumerate: hunt");
  internal_enum_fill_ids(desc);

  err = internal_enum_assign_addr(out_addr);
  RA8_RETURN_ON_ERROR(err, s_tag, "enumerate: address");
  uint8_t cfg_value = 0U;
  err               = internal_enum_read_config(&cfg_value);
  RA8_RETURN_ON_ERROR(err, s_tag, "enumerate: config");
  err = internal_enum_configure(*out_addr, cfg_value);
  RA8_RETURN_ON_ERROR(err, s_tag, "enumerate: configure");
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_hmsc_enumerate()`.
 * @details See the public header for the documented contract; runs the
 *          hardware-proven polled ladder: attach wait, (reset, address)
 *          hunt, address assignment, configuration parse + activate,
 *          GET_MAX_LUN, bulk pipe setup, then fires the attach callback.
 * @param[out] out_device See header (may be NULL).
 * @return Result code.
 * @retval k_ra8_ok Device enumerated; SCSI calls may follow.
 * @pre ::ra8_usb_hmsc_init succeeded and VBUS reaches the device.
 * @pre ::ra8_time_init has run.
 * @post On success `g_usb_hmsc_state.attached` is true and the snapshot is filled.
 * @post The registered attach callback (if any) has fired.
 * @note Blocking; bounded by the ladder timeouts.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_hmsc_enumerate(ra8_usb_hmsc_device_t* out_device)
{
  if (!g_usb_hmsc_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  g_usb_hmsc_state.attached = false;
  g_usb_hmsc_state.device   = (ra8_usb_hmsc_device_t){};

  uint8_t         dev_addr = 0U;
  const ra8_err_t err      = internal_enum_ladder(&dev_addr);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_enum_publish(dev_addr, out_device);
  return k_ra8_ok;
}
