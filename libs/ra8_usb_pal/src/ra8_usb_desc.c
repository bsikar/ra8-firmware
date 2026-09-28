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
  k_internal_type_device   = 0x01U, /**< DEVICE.                */
  k_internal_type_config   = 0x02U, /**< CONFIGURATION.         */
  k_internal_type_iface    = 0x04U, /**< INTERFACE.             */
  k_internal_type_endpoint = 0x05U, /**< ENDPOINT.              */
  k_internal_type_iad      = 0x0BU, /**< INTERFACE ASSOCIATION. */
  k_internal_type_cs_iface = 0x24U, /**< CS_INTERFACE (CDC).    */
} internal_desc_type_t;

/**
 * @enum internal_wire_t
 * @brief Fixed field values the CDC-ACM layout does not parameterise.
 */
typedef enum : uint16_t {
  k_internal_bcd_usb_200     = 0x0200U, /**< bcdUSB. IAD needs 2.00 or later.   */
  k_internal_bcd_cdc_120     = 0x0120U, /**< bcdCDC 1.20.                       */
  k_internal_bcd_device_dflt = 0x0100U, /**< bcdDevice when the caller says 0.  */
  k_internal_ep0_max_packet  = 64U,     /**< bMaxPacketSize0.                   */
  k_internal_class_misc      = 0xEFU,   /**< Miscellaneous device class.        */
  k_internal_subclass_common = 0x02U,   /**< Common class.                      */
  k_internal_protocol_iad    = 0x01U,   /**< Interface association protocol.    */
  k_internal_class_cdc       = 0x02U,   /**< Communications interface class.    */
  k_internal_subclass_acm    = 0x02U,   /**< Abstract control model.            */
  k_internal_protocol_at     = 0x01U,   /**< AT command protocol (V.250).       */
  k_internal_class_cdc_data  = 0x0AU,   /**< CDC data interface class.          */
  k_internal_ep_attr_bulk    = 0x02U,   /**< bmAttributes, bulk.                */
  k_internal_ep_attr_intr    = 0x03U,   /**< bmAttributes, interrupt.           */
  k_internal_cfg_attr_base   = 0x80U,   /**< bmAttributes bit 7, reserved-one.  */
  k_internal_cfg_attr_self   = 0x40U,   /**< bmAttributes bit 6, self-powered.  */
  k_internal_cfg_attr_wakeup = 0x20U,   /**< bmAttributes bit 5, remote wakeup. */
  k_internal_power_ma_max    = 500U,    /**< Largest draw bMaxPower can encode. */
  k_internal_ep_dir_in       = 0x80U,   /**< Direction bit of an IN endpoint.   */
  k_internal_cfg_value       = 1U,      /**< bConfigurationValue.               */
  k_internal_num_configs     = 1U,      /**< bNumConfigurations.                */
  k_internal_cdc_ifaces      = 2U,      /**< Control plus data interface.       */
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
  out[cfg_at + 2U]     = (uint8_t)(total & 0xFFU);
  out[cfg_at + 3U]     = (uint8_t)((total >> 8U) & 0xFFU);

  *out_len = cur.len;
  return k_ra8_ok;
}
