/**
 * @file ra8_usb_desc.c
 * @brief The one chapter-9 encoder behind the synthesised frameworks (#766).
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A small append cursor does all the writing, so every descriptor below is a
 * list of field values rather than a hand-counted byte array, and the one
 * field a human counting by hand gets wrong, the configuration descriptor's
 * `wTotalLength`, is back-patched from the cursor after the block is emitted.
 *
 * Nothing here allocates, retains state, or names a USB stack. The encoder is
 * the half of #766 that can be proven off-target, and it is proven against the
 * arrays the example apps already carry, byte for byte.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_desc.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/* =============================================================================
 * Wire constants
 * =============================================================================
 */

/**
 * @enum internal_desc_type_t
 * @brief bDescriptorType values, USB 2.0 table 9-5 plus the CDC additions.
 */
typedef enum : uint8_t {
  k_internal_type_device   = 0x01U, /**< DEVICE.                            */
  k_internal_type_config   = 0x02U, /**< CONFIGURATION.                     */
  k_internal_type_iface    = 0x04U, /**< INTERFACE.                         */
  k_internal_type_endpoint = 0x05U, /**< ENDPOINT.                          */
  k_internal_type_iad      = 0x0BU, /**< INTERFACE ASSOCIATION.             */
  k_internal_type_cs_iface = 0x24U, /**< CS_INTERFACE (CDC).                */
  k_internal_type_hid      = 0x21U, /**< HID class descriptor.              */
  k_internal_type_report   = 0x22U, /**< HID report descriptor.             */
  k_internal_type_dfu      = 0x21U, /**< DFU functional, DFU 1.1 sec 4.1.3. */
} internal_desc_type_t;

/**
 * @enum internal_wire_t
 * @brief Fixed field values the CDC-ACM layout does not parameterise.
 */
typedef enum : uint16_t {
  k_internal_bcd_usb_200     = 0x0200U, /**< bcdUSB. IAD needs 2.00 or later.        */
  k_internal_bcd_cdc_120     = 0x0120U, /**< bcdCDC 1.20.                            */
  k_internal_bcd_device_dflt = 0x0100U, /**< bcdDevice when the caller says 0.       */
  k_internal_ep0_max_packet  = 64U,     /**< bMaxPacketSize0.                        */
  k_internal_class_misc      = 0xEFU,   /**< Miscellaneous device class.             */
  k_internal_subclass_common = 0x02U,   /**< Common class.                           */
  k_internal_protocol_iad    = 0x01U,   /**< Interface association protocol.         */
  k_internal_class_cdc       = 0x02U,   /**< Communications interface class.         */
  k_internal_subclass_acm    = 0x02U,   /**< Abstract control model.                 */
  k_internal_protocol_at     = 0x01U,   /**< AT command protocol (V.250).            */
  k_internal_class_cdc_data  = 0x0AU,   /**< CDC data interface class.               */
  k_internal_ep_attr_bulk    = 0x02U,   /**< bmAttributes, bulk.                     */
  k_internal_ep_attr_intr    = 0x03U,   /**< bmAttributes, interrupt.                */
  k_internal_cfg_attr_base   = 0x80U,   /**< bmAttributes bit 7, reserved-one.       */
  k_internal_cfg_attr_self   = 0x40U,   /**< bmAttributes bit 6, self-powered.       */
  k_internal_cfg_attr_wakeup = 0x20U,   /**< bmAttributes bit 5, remote wakeup.      */
  k_internal_power_ma_max    = 500U,    /**< Largest draw bMaxPower can encode.      */
  k_internal_ep_dir_in       = 0x80U,   /**< Direction bit of an IN endpoint.        */
  k_internal_cfg_value       = 1U,      /**< bConfigurationValue.                    */
  k_internal_num_configs     = 1U,      /**< bNumConfigurations.                     */
  k_internal_cdc_ifaces      = 2U,      /**< Control plus data interface.            */
  k_internal_class_per_iface = 0x00U,   /**< Device class deferred to the interface. */
  k_internal_class_msc       = 0x08U,   /**< Mass storage interface class.           */
  k_internal_subclass_scsi   = 0x06U,   /**< SCSI transparent command set.           */
  k_internal_protocol_bbb    = 0x50U,   /**< Bulk-only transport.                    */
  k_internal_msc_ifaces      = 1U,      /**< One mass-storage interface.             */
  k_internal_type_qualifier  = 0x06U,   /**< DEVICE QUALIFIER, USB 2.0 sec 9.6.2.    */
  k_internal_qualifier_bytes = 10U,     /**< Device qualifier wire length.           */
  k_internal_bcd_hid_111     = 0x0111U, /**< bcdHID 1.11.                            */
  k_internal_class_hid       = 0x03U,   /**< Human interface interface class.        */
  k_internal_subclass_boot   = 0x01U,   /**< Boot interface subclass, HID app. B.    */
  k_internal_hid_bytes       = 9U,      /**< HID class descriptor wire length.       */
  k_internal_hid_ifaces      = 1U,      /**< One human-interface interface.          */
  k_internal_hid_descs       = 1U,      /**< One subordinate report descriptor.      */
  k_internal_hid_endpoints   = 1U,      /**< Interrupt IN only, no OUT endpoint.     */
  k_internal_byte_mask       = 0xFFU,   /**< Low byte of a little-endian field.      */
  k_internal_class_app_spec  = 0xFEU,   /**< Application-specific interface class.   */
  k_internal_subclass_dfu    = 0x01U,   /**< Device firmware upgrade subclass.       */
  k_internal_proto_runtime   = 0x01U,   /**< DFU run-time protocol.                  */
  k_internal_proto_dfu       = 0x02U,   /**< DFU mode protocol.                      */
  k_internal_dfu_bytes       = 9U,      /**< DFU functional descriptor length.       */
  k_internal_dfu_ifaces      = 1U,      /**< One DFU interface.                      */
  k_internal_dfu_endpoints   = 0U,      /**< DFU rides EP0, no endpoints of its own. */
  k_internal_dfu_can_dnload  = 0x01U,   /**< bmAttributes bit 0.                     */
  k_internal_dfu_can_upload  = 0x02U,   /**< bmAttributes bit 1.                     */
  k_internal_dfu_manif_tol   = 0x04U,   /**< bmAttributes bit 2.                     */
  k_internal_dfu_will_detach = 0x08U,   /**< bmAttributes bit 3.                     */
} internal_wire_t;

/**
 * @enum internal_cdc_subtype_t
 * @brief bDescriptorSubtype values of the CDC functional descriptors.
 */
typedef enum : uint8_t {
  k_internal_cdc_sub_header    = 0x00U, /**< Header functional descriptor. */
  k_internal_cdc_sub_call_mgmt = 0x01U, /**< Call management.              */
  k_internal_cdc_sub_acm       = 0x02U, /**< Abstract control management.  */
  k_internal_cdc_sub_union     = 0x06U, /**< Union.                        */
} internal_cdc_subtype_t;

/* =============================================================================
 * Append cursor
 * =============================================================================
 */

/**
 * @struct internal_cursor_t
 * @brief A bounded append cursor over the caller's buffer.
 *
 * @details `overflow` latches on the first write that would not fit, so the
 * emitters below stay branch-free and one check at the end decides the result.
 */
typedef struct {
  uint8_t* buf;      /**< Destination, borrowed from the caller.       */
  uint32_t cap;      /**< Capacity of ::buf in bytes.                  */
  uint32_t len;      /**< Bytes written so far.                        */
  bool     overflow; /**< Latched on the first write that did not fit. */
} internal_cursor_t;

/**
 * @brief Append one byte, latching overflow instead of writing past the end.
 *
 * @param[in,out] cur Cursor to append through.
 * @param[in]     b   Byte to append.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post On success ::internal_cursor_t::len has grown by one.
 * @post On overflow the buffer is untouched and the latch is set.
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put(internal_cursor_t* cur, uint8_t b)
{
  if (cur->len >= cur->cap) {
    cur->overflow = true;
    return;
  }
  cur->buf[cur->len] = b;
  cur->len++;
}

/**
 * @brief Append a 16-bit field little-endian, as every USB field is.
 *
 * @param[in,out] cur Cursor to append through.
 * @param[in]     v   Value to append.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Two bytes have been appended, or the overflow latch is set.
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put16(internal_cursor_t* cur, uint16_t v)
{
  internal_put(cur, (uint8_t)(v & 0xFFU));
  internal_put(cur, (uint8_t)((v >> 8U) & 0xFFU));
}

/**
 * @brief Append one endpoint descriptor.
 *
 * @param[in,out] cur      Cursor to append through.
 * @param[in]     addr     Endpoint address with its direction bit.
 * @param[in]     attr     bmAttributes transfer type.
 * @param[in]     mps      Max packet size.
 * @param[in]     interval bInterval.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Seven bytes have been appended, or the overflow latch is set.
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_endpoint(internal_cursor_t* cur,
                                               uint8_t            addr,
                                               uint8_t            attr,
                                               uint16_t           mps,
                                               uint8_t            interval)
{
  internal_put(cur, (uint8_t)k_ra8_usb_desc_endpoint_bytes);
  internal_put(cur, (uint8_t)k_internal_type_endpoint);
  internal_put(cur, addr);
  internal_put(cur, attr);
  internal_put16(cur, mps);
  internal_put(cur, interval);
}

/**
 * @brief Append one interface descriptor.
 *
 * @param[in,out] cur       Cursor to append through.
 * @param[in]     number    bInterfaceNumber.
 * @param[in]     endpoints bNumEndpoints.
 * @param[in]     cls       bInterfaceClass.
 * @param[in]     subclass  bInterfaceSubClass.
 * @param[in]     protocol  bInterfaceProtocol.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Nine bytes have been appended, or the overflow latch is set.
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_iface(internal_cursor_t* cur,
                                            uint8_t            number,
                                            uint8_t            endpoints,
                                            uint8_t            cls,
                                            uint8_t            subclass,
                                            uint8_t            protocol)
{
  internal_put(cur, (uint8_t)k_ra8_usb_desc_iface_bytes);
  internal_put(cur, (uint8_t)k_internal_type_iface);
  internal_put(cur, number);
  internal_put(cur, 0U); /* bAlternateSetting */
  internal_put(cur, endpoints);
  internal_put(cur, cls);
  internal_put(cur, subclass);
  internal_put(cur, protocol);
  internal_put(cur, 0U); /* iInterface */
}

/**
 * @brief Length of a NUL-terminated string, capped at the header's own limit.
 *
 * @param[in]  s   String to measure, may be NULL.
 * @param[out] len Length in characters on success.
 *
 * @return ra8_err_t Result of the measurement.
 * @retval k_ra8_ok                     Measured; zero for NULL or empty.
 * @retval k_ra8_err_range_check_failed Longer than the published cap.
 *
 * @pre @p len is non-NULL.
 * @post On success @p len holds a value at or below the cap.
 * @note Internal helper; does not call into libc so the encoder stays
 *       linkable in a freestanding image.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_strlen(const char* s, uint32_t* len)
{
  uint32_t n = 0U;

  if (s == nullptr) {
    *len = 0U;
    return k_ra8_ok;
  }
  while (s[n] != '\0') {
    n++;
    if (n > (uint32_t)k_ra8_usb_desc_string_chars_max) {
      return k_ra8_err_range_check_failed;
    }
  }
  *len = n;
  return k_ra8_ok;
}

/**
 * @brief Append the device qualifier, USB 2.0 sec 9.6.2.
 *
 * @param[in,out] cur          Cursor to append through.
 * @param[in]     dev_class    bDeviceClass the device descriptor published.
 * @param[in]     dev_subclass bDeviceSubClass the device descriptor published.
 * @param[in]     dev_protocol bDeviceProtocol the device descriptor published.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Ten bytes have been appended, or the overflow latch is set.
 * @note Internal helper. The qualifier describes the same device at the other
 *       speed, so its class triple must repeat what the device descriptor
 *       published: 0/0/0 for a per-interface device, the MISC / common / IAD
 *       triple for a composite one.
 * @note A high-speed device must answer GET_DESCRIPTOR for
 *       this, describing what it would be at the other speed; a full-speed
 *       device must not publish one at all.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_qualifier(internal_cursor_t* cur,
                                                uint8_t            dev_class,
                                                uint8_t            dev_subclass,
                                                uint8_t            dev_protocol)
{
  internal_put(cur, (uint8_t)k_internal_qualifier_bytes);
  internal_put(cur, (uint8_t)k_internal_type_qualifier);
  internal_put16(cur, (uint16_t)k_internal_bcd_usb_200);
  internal_put(cur, dev_class);
  internal_put(cur, dev_subclass);
  internal_put(cur, dev_protocol);
  internal_put(cur, (uint8_t)k_internal_ep0_max_packet);
  internal_put(cur, (uint8_t)k_internal_num_configs);
  internal_put(cur, 0U); /* bReserved */
}

/**
 * @brief Append one string-framework entry.
 *
 * @param[in,out] cur    Cursor to append through.
 * @param[in]     langid Language id the entry is filed under.
 * @param[in]     index  String index this entry answers.
 * @param[in]     s      Characters to emit.
 * @param[in]     n      Character count.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post The entry has been appended, or the overflow latch is set.
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_string(internal_cursor_t* cur,
                                             uint16_t           langid,
                                             uint8_t            index,
                                             const char*        s,
                                             uint32_t           n)
{
  internal_put(cur, (uint8_t)(langid & 0xFFU));
  internal_put(cur, (uint8_t)((langid >> 8U) & 0xFFU));
  internal_put(cur, index);
  internal_put(cur, (uint8_t)n);
  for (uint32_t i = 0U; i < n; i++) {
    internal_put(cur, (uint8_t)s[i]);
  }
}

/* =============================================================================
 * Public
 * =============================================================================
 */

ra8_err_t ra8_usb_desc_build_langid(uint16_t langid, uint8_t* out, uint32_t cap, uint32_t* out_len)
{
  if ((out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (cap < (uint32_t)k_ra8_usb_desc_langid_bytes) {
    return k_ra8_err_invalid_size;
  }

  const uint16_t id = (langid == 0U) ? (uint16_t)k_ra8_usb_desc_langid_en_us : langid;

  out[0]   = (uint8_t)(id & 0xFFU);
  out[1]   = (uint8_t)((id >> 8U) & 0xFFU);
  *out_len = (uint32_t)k_ra8_usb_desc_langid_bytes;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_desc_build_strings(const ra8_usb_desc_device_t* dev,
                                     uint8_t*                     out,
                                     uint32_t                     cap,
                                     uint32_t*                    out_len)
{
  if ((dev == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  const char* const slots[k_ra8_usb_desc_string_slots] = {
    dev->manufacturer,
    dev->product,
    dev->serial,
  };
  const uint8_t indices[k_ra8_usb_desc_string_slots] = {
    (uint8_t)k_ra8_usb_desc_str_manufacturer,
    (uint8_t)k_ra8_usb_desc_str_product,
    (uint8_t)k_ra8_usb_desc_str_serial,
  };
  const uint16_t langid = (dev->langid == 0U) ? (uint16_t)k_ra8_usb_desc_langid_en_us : dev->langid;

  internal_cursor_t cur = {.buf = out, .cap = cap, .len = 0U, .overflow = false};

  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_desc_string_slots; i++) {
    uint32_t        n   = 0U;
    const ra8_err_t err = internal_strlen(slots[i], &n);
    if (err != k_ra8_ok) {
      return err;
    }
    if (n == 0U) {
      continue;
    }
    internal_put_string(&cur, langid, indices[i], slots[i], n);
  }

  if (cur.overflow) {
    return k_ra8_err_invalid_size;
  }
  *out_len = cur.len;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_desc_build_cdc_acm(const ra8_usb_desc_device_t*  dev,
                                     const ra8_usb_desc_cdc_acm_t* cdc,
                                     uint8_t*                      out,
                                     uint32_t                      cap,
                                     uint32_t*                     out_len)
{
  if ((dev == nullptr) || (cdc == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (((cdc->notify_ep & (uint8_t)k_internal_ep_dir_in) == 0U) ||
      ((cdc->in_ep & (uint8_t)k_internal_ep_dir_in) == 0U) ||
      ((cdc->out_ep & (uint8_t)k_internal_ep_dir_in) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((cdc->notify_bytes == 0U) || (cdc->data_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (dev->max_power_ma > (uint16_t)k_internal_power_ma_max) {
    return k_ra8_err_range_check_failed;
  }

  uint32_t  man_len = 0U;
  uint32_t  pro_len = 0U;
  uint32_t  ser_len = 0U;
  ra8_err_t err     = internal_strlen(dev->manufacturer, &man_len);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_strlen(dev->product, &pro_len);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_strlen(dev->serial, &ser_len);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_cursor_t cur = {.buf = out, .cap = cap, .len = 0U, .overflow = false};

  /* Device descriptor, USB 2.0 sec 9.6.1. The MISC / common / IAD triple is
   * what lets a host accept the interface association below; advertising a
   * plain CDC device class here makes macOS reject the composite layout. */
  internal_put(&cur, (uint8_t)k_ra8_usb_desc_device_bytes);
  internal_put(&cur, (uint8_t)k_internal_type_device);
  internal_put16(&cur, (uint16_t)k_internal_bcd_usb_200);
  internal_put(&cur, (uint8_t)k_internal_class_misc);
  internal_put(&cur, (uint8_t)k_internal_subclass_common);
  internal_put(&cur, (uint8_t)k_internal_protocol_iad);
  internal_put(&cur, (uint8_t)k_internal_ep0_max_packet);
  internal_put16(&cur, dev->vid);
  internal_put16(&cur, dev->pid);
  internal_put16(&cur,
                 (dev->bcd_device == 0U) ? (uint16_t)k_internal_bcd_device_dflt : dev->bcd_device);
  internal_put(&cur, (man_len == 0U) ? 0U : (uint8_t)k_ra8_usb_desc_str_manufacturer);
  internal_put(&cur, (pro_len == 0U) ? 0U : (uint8_t)k_ra8_usb_desc_str_product);
  internal_put(&cur, (ser_len == 0U) ? 0U : (uint8_t)k_ra8_usb_desc_str_serial);
  internal_put(&cur, (uint8_t)k_internal_num_configs);

  /* A high-speed device publishes what it would look like at the other speed.
   * The qualifier sits outside the configuration block, so wTotalLength below
   * is still measured from the configuration descriptor. */
  if (cdc->high_speed) {
    internal_put_qualifier(&cur,
                           (uint8_t)k_internal_class_misc,
                           (uint8_t)k_internal_subclass_common,
                           (uint8_t)k_internal_protocol_iad);
  }

  /* Configuration descriptor. wTotalLength is written as zero and patched
   * from the cursor once the whole block is emitted, which is the entire
   * reason this encoder exists. */
  const uint32_t cfg_at = cur.len;
  uint8_t        attrs  = (uint8_t)k_internal_cfg_attr_base;
  if (dev->self_powered) {
    attrs = (uint8_t)(attrs | (uint8_t)k_internal_cfg_attr_self);
  }
  if (dev->remote_wakeup) {
    attrs = (uint8_t)(attrs | (uint8_t)k_internal_cfg_attr_wakeup);
  }
  internal_put(&cur, (uint8_t)k_ra8_usb_desc_config_bytes);
  internal_put(&cur, (uint8_t)k_internal_type_config);
  internal_put16(&cur, 0U);
  internal_put(&cur, (uint8_t)k_internal_cdc_ifaces);
  internal_put(&cur, (uint8_t)k_internal_cfg_value);
  internal_put(&cur, 0U); /* iConfiguration */
  internal_put(&cur, attrs);
  internal_put(&cur, (uint8_t)((dev->max_power_ma + 1U) / 2U));

  /* Interface association: interfaces 0 and 1 are one CDC function. */
  internal_put(&cur, (uint8_t)k_ra8_usb_desc_iad_bytes);
  internal_put(&cur, (uint8_t)k_internal_type_iad);
  internal_put(&cur, 0U); /* bFirstInterface */
  internal_put(&cur, (uint8_t)k_internal_cdc_ifaces);
  internal_put(&cur, (uint8_t)k_internal_class_cdc);
  internal_put(&cur, (uint8_t)k_internal_subclass_acm);
  internal_put(&cur, (uint8_t)k_internal_protocol_at);
  internal_put(&cur, 0U); /* iFunction */

  /* Communications interface and its four functional descriptors. */
  internal_put_iface(&cur,
                     0U,
                     1U,
                     (uint8_t)k_internal_class_cdc,
                     (uint8_t)k_internal_subclass_acm,
                     (uint8_t)k_internal_protocol_at);

  internal_put(&cur, 5U);
  internal_put(&cur, (uint8_t)k_internal_type_cs_iface);
  internal_put(&cur, (uint8_t)k_internal_cdc_sub_header);
  internal_put16(&cur, (uint16_t)k_internal_bcd_cdc_120);

  internal_put(&cur, 5U);
  internal_put(&cur, (uint8_t)k_internal_type_cs_iface);
  internal_put(&cur, (uint8_t)k_internal_cdc_sub_call_mgmt);
  internal_put(&cur, 0x01U); /* bmCapabilities: device handles call management */
  internal_put(&cur, 0x01U); /* bDataInterface                                 */

  internal_put(&cur, 4U);
  internal_put(&cur, (uint8_t)k_internal_type_cs_iface);
  internal_put(&cur, (uint8_t)k_internal_cdc_sub_acm);
  internal_put(&cur, 0x02U); /* bmCapabilities: line coding + serial state */

  internal_put(&cur, 5U);
  internal_put(&cur, (uint8_t)k_internal_type_cs_iface);
  internal_put(&cur, (uint8_t)k_internal_cdc_sub_union);
  internal_put(&cur, 0U); /* bControlInterface     */
  internal_put(&cur, 1U); /* bSubordinateInterface */

  internal_put_endpoint(&cur,
                        cdc->notify_ep,
                        (uint8_t)k_internal_ep_attr_intr,
                        cdc->notify_bytes,
                        cdc->notify_interval_ms);

  /* Data interface and its two bulk endpoints. */
  internal_put_iface(&cur, 1U, 2U, (uint8_t)k_internal_class_cdc_data, 0U, 0U);
  internal_put_endpoint(&cur, cdc->out_ep, (uint8_t)k_internal_ep_attr_bulk, cdc->data_bytes, 0U);
  internal_put_endpoint(&cur, cdc->in_ep, (uint8_t)k_internal_ep_attr_bulk, cdc->data_bytes, 0U);

  if (cur.overflow) {
    return k_ra8_err_invalid_size;
  }

  const uint32_t total = cur.len - cfg_at;
  out[cfg_at + 2U]     = (uint8_t)(total & (uint32_t)k_internal_byte_mask);
  out[cfg_at + 3U]     = (uint8_t)((total >> 8U) & (uint32_t)k_internal_byte_mask);

  *out_len = cur.len;
  return k_ra8_ok;
}

/**
 * @brief Reject a mass-storage config the encoder cannot put on the wire.
 *
 * @param[in] dev Device identity.
 * @param[in] msc Endpoint layout of the mass-storage function.
 *
 * @return ra8_err_t Result of the check.
 * @retval k_ra8_ok                     The config encodes.
 * @retval k_ra8_err_invalid_arg        A direction bit is wrong, or the max
 *                                      packet size is zero.
 * @retval k_ra8_err_range_check_failed The power draw exceeds bMaxPower.
 *
 * @pre @p dev and @p msc are non-NULL.
 * @post Nothing is written; the caller decides what to emit.
 * @note Internal helper; the two bulk endpoints differ only in the direction
 *       bit, which is the field a converted app is most likely to get wrong.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_msc_check(const ra8_usb_desc_device_t* dev,
                                                 const ra8_usb_desc_msc_t*    msc)
{
  if (((msc->in_ep & (uint8_t)k_internal_ep_dir_in) == 0U) ||
      ((msc->out_ep & (uint8_t)k_internal_ep_dir_in) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (msc->data_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (dev->max_power_ma > (uint16_t)k_internal_power_ma_max) {
    return k_ra8_err_range_check_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Append the 18-byte device descriptor of a per-interface device.
 *
 * @param[in,out] cur     Cursor to append through.
 * @param[in]     dev     Device identity.
 * @param[in]     strings Which of the three string slots are published.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Eighteen bytes have been appended, or the overflow latch is set.
 * @note Internal helper. Class 0 defers the class triple to the interface,
 *       which is what a single-function device publishes; the MISC / common /
 *       IAD triple exists only to carry an interface association.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_device_per_iface(internal_cursor_t*           cur,
                                                       const ra8_usb_desc_device_t* dev,
                                                       const bool                   strings[3])
{
  internal_put(cur, (uint8_t)k_ra8_usb_desc_device_bytes);
  internal_put(cur, (uint8_t)k_internal_type_device);
  internal_put16(cur, (uint16_t)k_internal_bcd_usb_200);
  internal_put(cur, (uint8_t)k_internal_class_per_iface);
  internal_put(cur, 0U); /* bDeviceSubClass */
  internal_put(cur, 0U); /* bDeviceProtocol */
  internal_put(cur, (uint8_t)k_internal_ep0_max_packet);
  internal_put16(cur, dev->vid);
  internal_put16(cur, dev->pid);
  internal_put16(cur,
                 (dev->bcd_device == 0U) ? (uint16_t)k_internal_bcd_device_dflt : dev->bcd_device);
  internal_put(cur, strings[0] ? (uint8_t)k_ra8_usb_desc_str_manufacturer : 0U);
  internal_put(cur, strings[1] ? (uint8_t)k_ra8_usb_desc_str_product : 0U);
  internal_put(cur, strings[2] ? (uint8_t)k_ra8_usb_desc_str_serial : 0U);
  internal_put(cur, (uint8_t)k_internal_num_configs);
}

/**
 * @brief Append a configuration descriptor with wTotalLength left at zero.
 *
 * @param[in,out] cur    Cursor to append through.
 * @param[in]     dev    Device identity supplying the power and attributes.
 * @param[in]     ifaces bNumInterfaces this configuration declares.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Nine bytes have been appended, or the overflow latch is set.
 * @note Internal helper. The caller back-patches wTotalLength from the cursor
 *       once the whole configuration block is emitted.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_put_config_open(internal_cursor_t* cur, const ra8_usb_desc_device_t* dev, uint8_t ifaces)
{
  uint8_t attrs = (uint8_t)k_internal_cfg_attr_base;
  if (dev->self_powered) {
    attrs = (uint8_t)(attrs | (uint8_t)k_internal_cfg_attr_self);
  }
  if (dev->remote_wakeup) {
    attrs = (uint8_t)(attrs | (uint8_t)k_internal_cfg_attr_wakeup);
  }
  internal_put(cur, (uint8_t)k_ra8_usb_desc_config_bytes);
  internal_put(cur, (uint8_t)k_internal_type_config);
  internal_put16(cur, 0U);
  internal_put(cur, ifaces);
  internal_put(cur, (uint8_t)k_internal_cfg_value);
  internal_put(cur, 0U); /* iConfiguration */
  internal_put(cur, attrs);
  internal_put(cur, (uint8_t)((dev->max_power_ma + 1U) / 2U));
}

ra8_err_t ra8_usb_desc_build_msc(const ra8_usb_desc_device_t* dev,
                                 const ra8_usb_desc_msc_t*    msc,
                                 uint8_t*                     out,
                                 uint32_t                     cap,
                                 uint32_t*                    out_len)
{
  if ((dev == nullptr) || (msc == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_err_t err = internal_msc_check(dev, msc);
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t          lens[k_ra8_usb_desc_string_slots]  = {};
  const char* const slots[k_ra8_usb_desc_string_slots] = {
    dev->manufacturer,
    dev->product,
    dev->serial,
  };
  bool published[k_ra8_usb_desc_string_slots] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_desc_string_slots; i++) {
    err = internal_strlen(slots[i], &lens[i]);
    if (err != k_ra8_ok) {
      return err;
    }
    published[i] = (lens[i] != 0U);
  }

  internal_cursor_t cur = {.buf = out, .cap = cap, .len = 0U, .overflow = false};

  internal_put_device_per_iface(&cur, dev, published);
  if (msc->high_speed) {
    internal_put_qualifier(&cur, (uint8_t)k_internal_class_per_iface, 0U, 0U);
  }

  /* The qualifier sits outside the configuration block, so wTotalLength is
   * measured from here rather than from the start of the framework. */
  const uint32_t cfg_at = cur.len;
  internal_put_config_open(&cur, dev, (uint8_t)k_internal_msc_ifaces);

  /* The one interface, then its two bulk endpoints IN before OUT, which is the
   * order every current copy writes and the order the host walks. */
  internal_put_iface(&cur,
                     0U,
                     2U,
                     (uint8_t)k_internal_class_msc,
                     (uint8_t)k_internal_subclass_scsi,
                     (uint8_t)k_internal_protocol_bbb);
  internal_put_endpoint(&cur, msc->in_ep, (uint8_t)k_internal_ep_attr_bulk, msc->data_bytes, 0U);
  internal_put_endpoint(&cur, msc->out_ep, (uint8_t)k_internal_ep_attr_bulk, msc->data_bytes, 0U);

  if (cur.overflow) {
    return k_ra8_err_invalid_size;
  }

  const uint32_t total = cur.len - cfg_at;
  out[cfg_at + 2U]     = (uint8_t)(total & (uint32_t)k_internal_byte_mask);
  out[cfg_at + 3U]     = (uint8_t)((total >> 8U) & (uint32_t)k_internal_byte_mask);

  *out_len = cur.len;
  return k_ra8_ok;
}

/* =============================================================================
 * Human interface
 * =============================================================================
 */

/**
 * @brief Measure the three string slots and report which ones are published.
 *
 * @param[in]  dev       Device identity supplying the three strings.
 * @param[out] published One flag per slot, in manufacturer/product/serial order.
 *
 * @return ra8_err_t Result of the measurement.
 * @retval k_ra8_ok                     Every slot measured.
 * @retval k_ra8_err_range_check_failed A slot is longer than the published cap.
 *
 * @pre @p dev and @p published are non-NULL, @p published holds three flags.
 * @post On success each flag says whether that slot gets an index on the wire.
 * @note Internal helper. A NULL or empty slot is not published, so the device
 *       descriptor writes 0 for its index and the string framework skips it.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_published_slots(const ra8_usb_desc_device_t* dev,
                                                       bool                         published[3])
{
  const char* const slots[k_ra8_usb_desc_string_slots] = {
    dev->manufacturer,
    dev->product,
    dev->serial,
  };
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_desc_string_slots; i++) {
    uint32_t        len = 0U;
    const ra8_err_t err = internal_strlen(slots[i], &len);
    if (err != k_ra8_ok) {
      return err;
    }
    published[i] = (len != 0U);
  }
  return k_ra8_ok;
}

/**
 * @brief Reject a HID config the encoder cannot put on the wire.
 *
 * @param[in] dev Device identity.
 * @param[in] hid Endpoint layout of the HID function.
 *
 * @return ra8_err_t Result of the check.
 * @retval k_ra8_ok                     The config encodes.
 * @retval k_ra8_err_invalid_arg        The direction bit is wrong, a size is
 *                                      zero, or a boot protocol is declared on
 *                                      a non-boot interface.
 * @retval k_ra8_err_range_check_failed The power draw exceeds bMaxPower.
 *
 * @pre @p dev and @p hid are non-NULL.
 * @post Nothing is written; the caller decides what to emit.
 * @note Internal helper. The subclass and protocol pairing is checked because
 *       bInterfaceProtocol is reserved unless the interface is a boot one, so
 *       a boot protocol without the boot subclass is a descriptor no host is
 *       allowed to act on.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_hid_check(const ra8_usb_desc_device_t* dev,
                                                 const ra8_usb_desc_hid_t*    hid)
{
  if ((hid->in_ep & (uint8_t)k_internal_ep_dir_in) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((hid->data_bytes == 0U) || (hid->report_bytes == 0U) || (hid->poll_interval_ms == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (!hid->boot_interface && (hid->protocol != k_ra8_usb_desc_hid_protocol_none)) {
    return k_ra8_err_invalid_arg;
  }
  if (dev->max_power_ma > (uint16_t)k_internal_power_ma_max) {
    return k_ra8_err_range_check_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Append the HID class descriptor, HID 1.11 sec 6.2.1.
 *
 * @param[in,out] cur          Cursor to append through.
 * @param[in]     report_bytes Length of the subordinate report descriptor.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Nine bytes have been appended, or the overflow latch is set.
 * @note Internal helper. bCountryCode is 0, meaning not localised, which is
 *       what a device with no country-specific keycaps declares.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_hid_class(internal_cursor_t* cur, uint16_t report_bytes)
{
  internal_put(cur, (uint8_t)k_internal_hid_bytes);
  internal_put(cur, (uint8_t)k_internal_type_hid);
  internal_put16(cur, (uint16_t)k_internal_bcd_hid_111);
  internal_put(cur, 0U); /* bCountryCode, not localised */
  internal_put(cur, (uint8_t)k_internal_hid_descs);
  internal_put(cur, (uint8_t)k_internal_type_report);
  internal_put16(cur, report_bytes);
}

ra8_err_t ra8_usb_desc_build_hid(const ra8_usb_desc_device_t* dev,
                                 const ra8_usb_desc_hid_t*    hid,
                                 uint8_t*                     out,
                                 uint32_t                     cap,
                                 uint32_t*                    out_len)
{
  if ((dev == nullptr) || (hid == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_err_t err = internal_hid_check(dev, hid);
  if (err != k_ra8_ok) {
    return err;
  }

  bool published[k_ra8_usb_desc_string_slots] = {};
  err                                         = internal_published_slots(dev, published);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_cursor_t cur = {.buf = out, .cap = cap, .len = 0U, .overflow = false};

  internal_put_device_per_iface(&cur, dev, published);

  const uint32_t cfg_at = cur.len;
  internal_put_config_open(&cur, dev, (uint8_t)k_internal_hid_ifaces);

  /* Interface, then the HID class descriptor, then the endpoint. The class
   * descriptor sits between them rather than after the endpoint, because a
   * host walking the block takes it as belonging to the interface it follows. */
  internal_put_iface(&cur,
                     0U,
                     (uint8_t)k_internal_hid_endpoints,
                     (uint8_t)k_internal_class_hid,
                     hid->boot_interface ? (uint8_t)k_internal_subclass_boot : 0U,
                     (uint8_t)hid->protocol);
  internal_put_hid_class(&cur, hid->report_bytes);
  internal_put_endpoint(&cur,
                        hid->in_ep,
                        (uint8_t)k_internal_ep_attr_intr,
                        hid->data_bytes,
                        hid->poll_interval_ms);

  if (cur.overflow) {
    return k_ra8_err_invalid_size;
  }

  const uint32_t total = cur.len - cfg_at;
  out[cfg_at + 2U]     = (uint8_t)(total & (uint32_t)k_internal_byte_mask);
  out[cfg_at + 3U]     = (uint8_t)((total >> 8U) & (uint32_t)k_internal_byte_mask);

  *out_len = cur.len;
  return k_ra8_ok;
}

/**
 * @brief Reject a DFU request that could not produce a usable descriptor.
 *
 * @param[in] dev Device identity and power budget.
 * @param[in] dfu DFU capabilities and transfer geometry.
 *
 * @return ra8_err_t Result of the check.
 * @retval k_ra8_ok                 The request is encodable.
 * @retval k_ra8_err_invalid_arg    A zero transfer size or version, or neither
 *                                  capability bit set.
 * @retval k_ra8_err_range_check_failed The power budget exceeds 500 mA.
 *
 * @pre @p dev and @p dfu are non-NULL.
 * @post Nothing is written; the caller decides what to emit.
 * @note Internal helper. A function that can neither download nor upload is
 *       refused because a host enumerating it has no operation left to issue.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dfu_check(const ra8_usb_desc_device_t* dev,
                                                 const ra8_usb_desc_dfu_t*    dfu)
{
  if ((dfu->transfer_bytes == 0U) || (dfu->bcd_dfu == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (!dfu->can_download && !dfu->can_upload) {
    return k_ra8_err_invalid_arg;
  }
  if (dev->max_power_ma > (uint16_t)k_internal_power_ma_max) {
    return k_ra8_err_range_check_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Pack the four DFU capability booleans into bmAttributes.
 *
 * @param[in] dfu DFU capabilities.
 *
 * @return uint8_t The bmAttributes byte, DFU 1.1 sec 4.1.3.
 *
 * @pre @p dfu is non-NULL.
 * @post Nothing is written.
 * @note Internal helper. Bits 4 to 7 are reserved and left clear.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_dfu_attributes(const ra8_usb_desc_dfu_t* dfu)
{
  uint8_t attr = 0U;

  if (dfu->can_download) {
    attr |= (uint8_t)k_internal_dfu_can_dnload;
  }
  if (dfu->can_upload) {
    attr |= (uint8_t)k_internal_dfu_can_upload;
  }
  if (dfu->manifestation_tolerant) {
    attr |= (uint8_t)k_internal_dfu_manif_tol;
  }
  if (dfu->will_detach) {
    attr |= (uint8_t)k_internal_dfu_will_detach;
  }
  return attr;
}

/**
 * @brief Append the DFU functional descriptor, DFU 1.1 sec 4.1.3.
 *
 * @param[in,out] cur Cursor to append through.
 * @param[in]     dfu DFU capabilities and transfer geometry.
 *
 * @pre @p cur is non-NULL and owns a valid buffer.
 * @post Nine bytes have been appended, or the overflow latch is set.
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_dfu_functional(internal_cursor_t*        cur,
                                                     const ra8_usb_desc_dfu_t* dfu)
{
  internal_put(cur, (uint8_t)k_internal_dfu_bytes);
  internal_put(cur, (uint8_t)k_internal_type_dfu);
  internal_put(cur, internal_dfu_attributes(dfu));
  internal_put16(cur, dfu->detach_timeout_ms);
  internal_put16(cur, dfu->transfer_bytes);
  internal_put16(cur, dfu->bcd_dfu);
}

ra8_err_t ra8_usb_desc_build_dfu(const ra8_usb_desc_device_t* dev,
                                 const ra8_usb_desc_dfu_t*    dfu,
                                 uint8_t*                     out,
                                 uint32_t                     cap,
                                 uint32_t*                    out_len)
{
  if ((dev == nullptr) || (dfu == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_err_t err = internal_dfu_check(dev, dfu);
  if (err != k_ra8_ok) {
    return err;
  }

  bool published[k_ra8_usb_desc_string_slots] = {};
  err                                         = internal_published_slots(dev, published);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_cursor_t cur = {.buf = out, .cap = cap, .len = 0U, .overflow = false};

  internal_put_device_per_iface(&cur, dev, published);

  const uint32_t cfg_at = cur.len;
  internal_put_config_open(&cur, dev, (uint8_t)k_internal_dfu_ifaces);

  /* Zero endpoints: DFU 1.1 sec 4.1.2 puts every transfer on the default
   * control pipe, so the functional descriptor follows the interface with no
   * endpoint descriptor between them. */
  internal_put_iface(&cur,
                     0U,
                     (uint8_t)k_internal_dfu_endpoints,
                     (uint8_t)k_internal_class_app_spec,
                     (uint8_t)k_internal_subclass_dfu,
                     dfu->dfu_mode ? (uint8_t)k_internal_proto_dfu
                                   : (uint8_t)k_internal_proto_runtime);
  internal_put_dfu_functional(&cur, dfu);

  if (cur.overflow) {
    return k_ra8_err_invalid_size;
  }

  const uint32_t total = cur.len - cfg_at;
  out[cfg_at + 2U]     = (uint8_t)(total & (uint32_t)k_internal_byte_mask);
  out[cfg_at + 3U]     = (uint8_t)((total >> 8U) & (uint32_t)k_internal_byte_mask);

  *out_len = cur.len;
  return k_ra8_ok;
}
