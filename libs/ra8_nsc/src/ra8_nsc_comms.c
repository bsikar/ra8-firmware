/**
 * @file ra8_nsc_comms.c
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

#include "ra8_nsc_comms.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_nsc_veneer.h"
#include "ra8_sci.h"
#include "ra8_spi.h"
#include "ra8_usb.h"

static const char* s_tag = "NSCCOM";

/* =============================================================================
 * SCI
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SCI channel from Non-Secure code.
 *
 * @details Validates ``cfg`` lies inside the NS region (TZ builds) and
 *   forwards to the secure ``ra8_sci_init`` driver.
 *
 * @param[in] channel SCI channel index (0..k_ra8_sci_channel_max-1).
 * @param[in] cfg     Caller-supplied configuration in NS memory.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Channel programmed and ready.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg`` outside NS range or channel bad.
 *
 * @pre TrustZone substrate has been initialized.
 * @pre ``cfg`` lies entirely within the NS data region.
 * @post On success the SCI hardware is ready for I/O.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   Crosses the NS->S boundary via ``cmse_nonsecure_entry``. The caller
 *   pointer is checked with ``cmse_check_address_range`` so a malicious
 *   NS pointer cannot trick the secure SCI driver into reading secure
 *   memory.
 *
 * @note Thread-safe: serialised by the secure SCI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "sci_init: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra8_sci_init(channel, cfg);
}

/**
 * @brief NSC veneer: blocking single-byte SCI write.
 *
 * @details Forwards directly to ``ra8_sci_putc_polling``; no pointer
 *   crosses the boundary so no NS-range check is needed.
 *
 * @param[in] channel SCI channel index.
 * @param[in] byte    Byte to transmit.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Byte queued for TX.
 * @retval k_ra8_err_invalid_arg  Bad channel index.
 *
 * @pre ``ra8_nsc_sci_init`` succeeded for ``channel``.
 * @pre TrustZone substrate up.
 * @post On success one byte was transmitted.
 * @post On failure the bus state is unchanged.
 *
 * @par TrustZone:
 *   Crosses NS->S via ``cmse_nonsecure_entry``. Scalar-only signature;
 *   nothing leaks across the boundary.
 *
 * @note Thread-safe: serialised by the secure SCI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_putc(uint8_t channel, uint8_t byte)
{
  return ra8_sci_putc_polling(channel, byte);
}

/**
 * @brief NSC veneer: blocking single-byte SCI read.
 *
 * @details Validates ``out_byte`` is in the NS region and forwards
 *   to ``ra8_sci_getc_polling``.
 *
 * @param[in]  channel  SCI channel index.
 * @param[out] out_byte Destination byte (in NS memory).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               One byte stored at ``*out_byte``.
 * @retval k_ra8_err_null_ptr     ``out_byte`` was NULL.
 * @retval k_ra8_err_invalid_arg  Pointer not in NS region.
 *
 * @pre ``ra8_nsc_sci_init`` succeeded for ``channel``.
 * @pre ``out_byte`` lies in NS data region.
 * @post On success ``*out_byte`` reflects the received byte.
 * @post On failure ``*out_byte`` unchanged.
 *
 * @par TrustZone:
 *   Crosses NS->S via ``cmse_nonsecure_entry``. ``out_byte`` is
 *   ``cmse_check_address_range``-validated so the secure driver cannot
 *   be tricked into writing into secure memory.
 *
 * @note Thread-safe: serialised by the secure SCI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_sci_getc(uint8_t channel, uint8_t* out_byte)
{
  RA8_CHECK_NULL_PTR(out_byte, s_tag, "sci_getc: out_byte");
  RA8_NSC_CHECK_NS_RANGE_RW(out_byte, sizeof(*out_byte));
  return ra8_sci_getc_polling(channel, out_byte);
}

/* =============================================================================
 * IIC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an IIC (I2C-B) channel from NS code.
 *
 * @details Validates the NS pointer to ``cfg`` then forwards to
 *   ``ra8_i3c_init``.
 *
 * @param[in] channel IIC channel index.
 * @param[in] cfg     Caller-supplied configuration in NS memory.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Channel programmed.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg`` not in NS region or channel bad.
 *
 * @pre TrustZone substrate up.
 * @pre ``cfg`` lies entirely within NS data region.
 * @post On success the IIC hardware is ready for I/O.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cfg`` is range-checked
 *   so a hostile NS pointer cannot leak secure-region bytes.
 *
 * @note Thread-safe: serialised by the secure IIC driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_init(uint8_t channel, const ra8_i3c_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "iic_init: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  /* The IIC veneer is always an I2C-compat controller; force the mode. */
  ra8_i3c_cfg_t local = *cfg;
  local.mode          = k_ra8_i3c_mode_i2c;
  return ra8_i3c_init(channel, &local);
}

/**
 * @brief NSC veneer: blocking I2C write to a 7-bit target.
 *
 * @details Validates ``[data, data+len)`` lies in NS memory then forwards
 *   to ``ra8_i3c_write``.
 *
 * @param[in] channel   IIC channel index.
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] data      NS source buffer.
 * @param[in] len       Byte count.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bytes transmitted (or ACK-checked).
 * @retval k_ra8_err_null_ptr     ``data`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region or len bogus.
 *
 * @pre ``ra8_nsc_iic_init`` succeeded for ``channel``.
 * @pre ``[data, data+len)`` lies in NS data region.
 * @post On success the bus completed a START-WRITE-STOP transaction.
 * @post On failure the bus has been released.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cmse_check_address_range``
 *   rejects buffers that overlap secure memory before the secure driver
 *   reads them.
 *
 * @note Thread-safe: serialised by the secure IIC driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_write(uint8_t        channel,
                                           uint8_t        target_7b,
                                           const uint8_t* data,
                                           uint32_t       len)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "iic_write: data");
  RA8_NSC_CHECK_NS_RANGE_R(data, len);
  return ra8_i3c_write(channel, target_7b, data, len, false);
}

/**
 * @brief NSC veneer: blocking I2C read from a 7-bit target.
 *
 * @details Validates ``[out_buf, out_buf+len)`` lies in writable NS
 *   memory then forwards to ``ra8_i3c_read``.
 *
 * @param[in]  channel   IIC channel index.
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[out] out_buf   NS destination buffer.
 * @param[in]  len       Byte count to read.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bytes received into ``out_buf``.
 * @retval k_ra8_err_null_ptr     ``out_buf`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region.
 *
 * @pre ``ra8_nsc_iic_init`` succeeded for ``channel``.
 * @pre ``[out_buf, out_buf+len)`` lies in NS data region.
 * @post On success ``out_buf`` holds ``len`` received bytes.
 * @post On failure ``out_buf`` content is undefined.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cmse_check_address_range``
 *   guarantees the secure driver only writes to NS memory.
 *
 * @note Thread-safe: serialised by the secure IIC driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_iic_read(uint8_t  channel,
                                          uint8_t  target_7b,
                                          uint8_t* out_buf,
                                          uint32_t len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "iic_read: out_buf");
  RA8_NSC_CHECK_NS_RANGE_RW(out_buf, len);
  return ra8_i3c_read(channel, target_7b, out_buf, len, false);
}

/* =============================================================================
 * SPI
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up an SPI controller channel from NS code.
 *
 * @details Validates the NS pointer to ``cfg`` then forwards to
 *   ``ra8_spi_init``.
 *
 * @param[in] channel SPI channel index.
 * @param[in] cfg     Caller-supplied configuration in NS memory.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Channel programmed.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg`` not in NS region or channel bad.
 *
 * @pre TrustZone substrate up.
 * @pre ``cfg`` lies in NS data region.
 * @post On success the SPI hardware is ready for transfers.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cfg`` is range-checked
 *   prior to the secure-side read.
 *
 * @note Thread-safe: serialised by the secure SPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_init(uint8_t channel, const ra8_spi_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra8_spi_init(channel, cfg);
}

/**
 * @brief NSC veneer: full-duplex single-byte SPI exchange.
 *
 * @details Forwards to ``ra8_spi_xfer8``. ``rx`` may be NULL (matches
 *   the legacy ``ra8_spi_xfer8`` contract); when non-NULL it is range-
 *   checked.
 *
 * @param[in]  channel SPI channel index.
 * @param[in]  tx      Byte to transmit.
 * @param[out] rx      Optional NS destination for the received byte.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Exchange complete.
 * @retval k_ra8_err_invalid_arg  ``rx`` non-NULL but outside NS region.
 *
 * @pre ``ra8_nsc_spi_init`` succeeded for ``channel``.
 * @pre ``rx`` is NULL or in NS data region.
 * @post On success and ``rx`` non-NULL, ``*rx`` reflects the CIPO byte.
 * @post On failure the bus state is restored.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``rx`` (when non-NULL)
 *   is ``cmse_check_address_range``-validated.
 *
 * @note Thread-safe: serialised by the secure SPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  /* rx is allowed to be NULL (the legacy ra8_spi_xfer8 contract). */
  if (rx != nullptr) {
    RA8_NSC_CHECK_NS_RANGE_RW(rx, sizeof(*rx));
  }
  return ra8_spi_xfer8(channel, tx, rx);
}

/**
 * @brief Bytes-per-frame helper for the NSC SPI multi-byte veneers.
 *
 * @details Mirrors the bit-width -> byte mapping in ``ra8_spi_b.c`` so the
 *   veneer can size the NS-range check correctly. ``0`` signals an
 *   unsupported width (the underlying driver will then reject with
 *   ``k_ra8_err_invalid_arg``).
 *
 * @param[in] bit_width Frame width enum.
 *
 * @return Bytes per frame: 1, 2, or 4.
 * @retval 0 Unknown / unsupported width.
 *
 * @pre ``bit_width`` is one of the documented enum values.
 * @post No state mutated.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL
static uint8_t internal_spi_unit_bytes(ra8_spi_bit_width_t bit_width)
{
  switch (bit_width) {
    case k_ra8_spi_width_8:
      return 1U;
    case k_ra8_spi_width_16:
      return 2U;
    case k_ra8_spi_width_32:
      return 4U;
    default:
      return 0U;
  }
}

/**
 * @brief Byte span of @p len frames at @p bytes_per_unit each, in 64-bit.
 *
 * @details
 * The SPI veneers must range-check the exact byte span a hostile Non-Secure
 * caller's frame count spans before the secure driver touches it. Computing
 * ``bytes_per_unit * len`` in 32-bit overflows for large ``len`` (e.g.
 * ``len = 0x40000000`` at width 32 wraps 4 GiB to 0), which would let the
 * ``cmse_check_address_range`` argument truncate to a tiny span while the driver
 * still transfers the full ``len`` -- a hostile NS caller could shift secret
 * Secure memory out over SPI. This computes the span in 64-bit and reports
 * failure if it exceeds what the range check (a 32-bit length) can validate, so
 * the veneer rejects rather than under-validating (T5-07).
 *
 * @param[in]  bytes_per_unit Bytes per frame (1, 2, or 4); non-zero.
 * @param[in]  len            Frame count from the Non-Secure caller.
 * @param[out] out_span       Receives the byte span when the return is true.
 *                            Must be non-NULL (every caller passes a stack slot).
 *
 * @return Whether the span fits a 32-bit range-check length.
 * @retval true  Span computed and stored in ``*out_span``.
 * @retval false The span exceeds ``UINT32_MAX`` (would truncate the check).
 *
 * @pre ``out_span`` is non-NULL.
 * @pre ``bytes_per_unit`` is the validated 1/2/4 result of
 *      ::internal_spi_unit_bytes.
 * @post On false, ``*out_span`` is not written.
 * @post No hardware state is mutated.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_spi_byte_span(uint8_t bytes_per_unit, uint32_t len, uint32_t* out_span)
{
  const uint64_t span = (uint64_t)bytes_per_unit * (uint64_t)len;
  if (span > (uint64_t)UINT32_MAX) {
    return false;
  }
  *out_span = (uint32_t)span;
  return true;
}

/**
 * @brief NSC veneer: multi-frame TX-only polling SPI write.
 *
 * @details Computes ``bytes_per_frame * len``, range-checks the source
 *   buffer in NS memory, then forwards to ``ra8_spi_write``.
 *
 * @param[in] channel   SPI channel index.
 * @param[in] tx        NS source buffer (may be NULL only if ``len == 0``).
 * @param[in] len       Frame count.
 * @param[in] bit_width Frame width (8/16/32).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bytes shifted out.
 * @retval k_ra8_err_null_ptr     ``tx`` was NULL with non-zero ``len``.
 * @retval k_ra8_err_invalid_arg  Range outside NS region or bad width.
 *
 * @pre ``ra8_nsc_spi_init`` succeeded for ``channel``.
 * @pre ``[tx, tx + bytes_per_frame*len)`` lies in NS memory.
 * @post On success ``len`` frames were transmitted.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. The full byte range
 *   spanned by ``len`` frames is ``cmse_check_address_range``-validated
 *   so the secure driver only reads NS memory.
 *
 * @note Thread-safe: serialised by the secure SPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_write(uint8_t             channel,
                                           const void*         tx,
                                           uint32_t            len,
                                           ra8_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    RA8_CHECK_NULL_PTR(tx, s_tag, "spi_write: tx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra8_err_invalid_arg;
    }
    uint32_t span = 0U;
    if (!internal_spi_byte_span(bytes_per_unit, len, &span)) {
      return k_ra8_err_invalid_arg;
    }
    RA8_NSC_CHECK_NS_RANGE_R(tx, span);
  }
  return ra8_spi_write(channel, tx, len, bit_width);
}

/**
 * @brief NSC veneer: multi-frame RX-only polling SPI read.
 *
 * @details Computes the byte span, range-checks ``rx`` in NS writable
 *   memory, then forwards to ``ra8_spi_read``.
 *
 * @param[in]  channel   SPI channel index.
 * @param[out] rx        NS destination buffer (may be NULL only if ``len == 0``).
 * @param[in]  len       Frame count.
 * @param[in]  bit_width Frame width (8/16/32).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bytes received into ``rx``.
 * @retval k_ra8_err_null_ptr     ``rx`` was NULL with non-zero ``len``.
 * @retval k_ra8_err_invalid_arg  Range outside NS region or bad width.
 *
 * @pre ``ra8_nsc_spi_init`` succeeded for ``channel``.
 * @pre ``[rx, rx + bytes_per_frame*len)`` lies in NS writable memory.
 * @post On success ``rx`` holds ``len`` frames.
 * @post On failure ``rx`` content is undefined.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``rx`` is range-checked
 *   so the secure driver cannot be tricked into writing into secure RAM.
 *
 * @note Thread-safe: serialised by the secure SPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_read(uint8_t             channel,
                                          void*               rx,
                                          uint32_t            len,
                                          ra8_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    RA8_CHECK_NULL_PTR(rx, s_tag, "spi_read: rx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra8_err_invalid_arg;
    }
    uint32_t span = 0U;
    if (!internal_spi_byte_span(bytes_per_unit, len, &span)) {
      return k_ra8_err_invalid_arg;
    }
    RA8_NSC_CHECK_NS_RANGE_RW(rx, span);
  }
  return ra8_spi_read(channel, rx, len, bit_width);
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_spi_write_read(uint32_t    ch_bw,
                                                const void* tx,
                                                void*       rx,
                                                uint32_t    len)
{
  const uint8_t             channel = (uint8_t)(ch_bw & (uint32_t)k_ra8_nsc_spi_byte_msk);
  const ra8_spi_bit_width_t bit_width =
    (ra8_spi_bit_width_t)((ch_bw >> (uint32_t)k_ra8_nsc_spi_bw_shift) &
                          (uint32_t)k_ra8_nsc_spi_byte_msk);

  if (len > 0U) {
    RA8_CHECK_NULL_PTR(tx, s_tag, "spi_write_read: tx");
    RA8_CHECK_NULL_PTR(rx, s_tag, "spi_write_read: rx");
    const uint8_t bytes_per_unit = internal_spi_unit_bytes(bit_width);
    if (bytes_per_unit == 0U) {
      return k_ra8_err_invalid_arg;
    }
    uint32_t span = 0U;
    if (!internal_spi_byte_span(bytes_per_unit, len, &span)) {
      return k_ra8_err_invalid_arg;
    }
    RA8_NSC_CHECK_NS_RANGE_R(tx, span);
    RA8_NSC_CHECK_NS_RANGE_RW(rx, span);
  }
  return ra8_spi_write_read(channel, tx, rx, len, bit_width);
}

/* =============================================================================
 * USB
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the USB device controller.
 *
 * @details Forwards to ``ra8_usb_device_init``. Scalar-only signature, so
 *   no NS-range check is needed.
 *
 * @param[in] speed Negotiated USB speed enum.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              USB controller programmed.
 * @retval k_ra8_err_invalid_arg Unknown speed.
 *
 * @pre TrustZone substrate up.
 * @pre USB clock tree was enabled by ``ra8_nsc_periph_init``.
 * @post On success the device controller is in the powered, detached state.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar enum argument;
 *   nothing crosses the boundary that needs range-checking.
 *
 * @note Thread-safe: no -- USB bring-up is single-threaded.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_usb_init(ra8_usb_speed_t speed)
{
  return ra8_usb_device_init(speed);
}

/**
 * @brief NSC veneer: raise / drop the USB D+ pull-up.
 *
 * @details Forwards to ``ra8_usb_device_attach``.
 *
 * @param[in] speed    Negotiated USB speed enum.
 * @param[in] attached True to assert pull-up, false to release.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              Pull-up state set.
 * @retval k_ra8_err_invalid_arg Unknown speed.
 *
 * @pre ``ra8_nsc_usb_init`` succeeded.
 * @pre TrustZone substrate up.
 * @post On success the device is visible (or invisible) on the bus.
 * @post On failure the pull-up state is unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar arguments only.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_usb_attach(ra8_usb_speed_t speed, bool attached)
{
  return ra8_usb_device_attach(speed, attached);
}
