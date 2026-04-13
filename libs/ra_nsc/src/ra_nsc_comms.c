/**
 * @file ra_nsc_comms.c
 * @brief NSC veneers for the Wave 3 communications drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wave 9.3 retrofit. Each veneer validates pointer arguments
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
#include "ra_iic.h"
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
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "sci_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_sci_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_sci_putc(uint8_t channel, uint8_t byte)
{
  return ra_sci_putc_polling(channel, byte);
}

RA_NSC_VENEER ra_err_t ra_nsc_sci_getc(uint8_t  channel,
                                       uint8_t* out_byte) // NOLINT(readability-non-const-parameter)
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
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "iic_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_iic_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_iic_write(uint8_t        channel,
                                        uint8_t        target_7b,
                                        const uint8_t* data,
                                        uint32_t       len)
{
  RA_CHECK_NULL_PTR((void*)data, s_tag, "iic_write: data");
  RA_NSC_CHECK_NS_RANGE_R(data, len);
  return ra_iic_write(channel, target_7b, data, len);
}

RA_NSC_VENEER ra_err_t ra_nsc_iic_read(uint8_t  channel,
                                       uint8_t  target_7b,
                                       uint8_t* out_buf, // NOLINT(readability-non-const-parameter)
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
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "spi_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_spi_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_spi_xfer8(uint8_t  channel,
                                        uint8_t  tx,
                                        uint8_t* rx) // NOLINT(readability-non-const-parameter)
{
  /* rx is allowed to be NULL (the legacy ra_spi_xfer8 contract). */
  if (rx != nullptr) {
    RA_NSC_CHECK_NS_RANGE_RW(rx, sizeof(*rx));
  }
  return ra_spi_xfer8(channel, tx, rx);
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
