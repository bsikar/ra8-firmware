/**
 * @file ra_nsc_comms.c
 * @brief NSC veneers for the communications drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * retrofit. Each veneer validates pointer arguments
 * (under TrustZone) then forwards to the secure-side Ring-3
 * driver primitive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_nsc_comms.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_nsc_veneer.h"
#include "ra_sci.h"
#include "ra_spi.h"
#include "ra_usb.h"

static const char* s_tag = "NSCCOM";

/* =============================================================================
 * SCI
 * =============================================================================
 */

RA_NSC_VENEER ra_err_t ra_nsc_sci_init(uint8_t channel, const ra_sci_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "sci_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_sci_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_sci_putc(uint8_t channel, uint8_t byte)
{
  return ra_sci_putc_polling(channel, byte);
}

RA_NSC_VENEER ra_err_t ra_nsc_sci_getc(uint8_t channel, uint8_t* out_byte)
{
  RA_CHECK_NULL_PTR(out_byte, s_tag, "sci_getc: out_byte");
  RA_NSC_CHECK_NS_RANGE_RW(out_byte, sizeof(*out_byte));
  return ra_sci_getc_polling(channel, out_byte);
}

/* =============================================================================
 * IIC
 * =============================================================================
 */

RA_NSC_VENEER ra_err_t ra_nsc_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "iic_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_iic_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_iic_write(uint8_t        channel,
                                        uint8_t        target_7b,
                                        const uint8_t* data,
                                        uint32_t       len)
{
  RA_CHECK_NULL_PTR(data, s_tag, "iic_write: data");
  RA_NSC_CHECK_NS_RANGE_R(data, len);
  return ra_iic_write(channel, target_7b, data, len);
}

RA_NSC_VENEER ra_err_t ra_nsc_iic_read(uint8_t  channel,
                                       uint8_t  target_7b,
                                       uint8_t* out_buf,
                                       uint32_t len)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "iic_read: out_buf");
  RA_NSC_CHECK_NS_RANGE_RW(out_buf, len);
  return ra_iic_read(channel, target_7b, out_buf, len);
}

/* =============================================================================
 * SPI
 * =============================================================================
 */

RA_NSC_VENEER ra_err_t ra_nsc_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_spi_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  /* rx is allowed to be NULL (the legacy ra_spi_xfer8 contract). */
  if (rx != nullptr) {
    RA_NSC_CHECK_NS_RANGE_RW(rx, sizeof(*rx));
  }
  return ra_spi_xfer8(channel, tx, rx);
}

/**
 * @brief Bytes-per-frame helper for the NSC SPI multi-byte veneers.
 *
 * @details
 * Mirrors the bit-width -> byte mapping in ``ra_spi_b.c`` so the
 * veneer can size the NS-range check correctly. ``0`` signals an
 * unsupported width (the underlying driver will then reject with
 * ``k_ra_err_invalid_arg``).
 */
static uint8_t internal_spi_unit_bytes(ra_spi_bit_width_t bit_width)
{
  switch (bit_width) {
    case k_ra_spi_width_8:
      return 1U;
    case k_ra_spi_width_16:
      return 2U;
    case k_ra_spi_width_32:
      return 4U;
    default:
      return 0U;
  }
}

RA_NSC_VENEER ra_err_t ra_nsc_spi_write(uint8_t            channel,
                                        const void*        tx,
                                        uint32_t           len,
                                        ra_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    RA_CHECK_NULL_PTR(tx, s_tag, "spi_write: tx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra_err_invalid_arg;
    }
    RA_NSC_CHECK_NS_RANGE_R(tx, (uint32_t)bytes_per_unit * len);
  }
  return ra_spi_write(channel, tx, len, bit_width);
}

RA_NSC_VENEER ra_err_t ra_nsc_spi_read(uint8_t            channel,
                                       void*              rx,
                                       uint32_t           len,
                                       ra_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    RA_CHECK_NULL_PTR(rx, s_tag, "spi_read: rx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra_err_invalid_arg;
    }
    RA_NSC_CHECK_NS_RANGE_RW(rx, (uint32_t)bytes_per_unit * len);
  }
  return ra_spi_read(channel, rx, len, bit_width);
}

RA_NSC_VENEER ra_err_t ra_nsc_spi_write_read(uint8_t            channel,
                                             const void*        tx,
                                             void*              rx,
                                             uint32_t           len,
                                             ra_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    RA_CHECK_NULL_PTR(tx, s_tag, "spi_write_read: tx");
    RA_CHECK_NULL_PTR(rx, s_tag, "spi_write_read: rx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra_err_invalid_arg;
    }
    RA_NSC_CHECK_NS_RANGE_R(tx, (uint32_t)bytes_per_unit * len);
    RA_NSC_CHECK_NS_RANGE_RW(rx, (uint32_t)bytes_per_unit * len);
  }
  return ra_spi_write_read(channel, tx, rx, len, bit_width);
}

/* =============================================================================
 * USB
 * =============================================================================
 */

RA_NSC_VENEER ra_err_t ra_nsc_usb_init(ra_usb_speed_t speed)
{
  return ra_usb_device_init(speed);
}

RA_NSC_VENEER ra_err_t ra_nsc_usb_attach(ra_usb_speed_t speed, bool attached)
{
  return ra_usb_device_attach(speed, attached);
}
