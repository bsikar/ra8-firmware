/**
 * @file ra8_nsc_xspi.c
 * @brief NSC veneer: external XSPI flash read + status
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * veneer over the secure-side ra8_xspi driver. Runs as
 * ordinary code in the single-world build; the
 * ``RA8_NSC_VENEER`` attribute expands to
 * ``__attribute__((cmse_nonsecure_entry))`` when the TrustZone
 * build is turned on, and the ``RA8_NSC_CHECK_NS_RANGE_RW``
 * macro becomes a real ``cmse_check_address_range`` at that
 * point too.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"
#include "ra8_xspi.h"

static const char* s_tag = "NSCXSPI";

typedef enum : uint8_t {
  k_ra8_nsc_xspi_instance = 0U, /**< Only one xspi instance today. */
} ra8_nsc_xspi_instance_t;

/**
 * @brief NSC veneer: read ``len`` bytes from external XSPI flash.
 *
 * @details Validates ``len`` is in (0, k_ra8_nsc_xspi_max_read] and that
 *   the NS destination buffer is in writable NS memory, then forwards
 *   to ``ra8_xspi_flash_read`` for the single configured instance.
 *
 * @param[in]  flash_off Byte offset into XSPI flash.
 * @param[out] ns_dst    Non-Secure destination buffer.
 * @param[in]  len       Bytes to read; 1..k_ra8_nsc_xspi_max_read.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Bytes copied to ``ns_dst``.
 * @retval k_ra8_err_null_ptr     ``ns_dst`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``len`` zero / too large or range outside NS.
 *
 * @pre TrustZone substrate up.
 * @pre ``[ns_dst, ns_dst+len)`` lies in NS data region.
 * @post On success ``ns_dst`` holds ``len`` flash bytes from ``flash_off``.
 * @post On failure ``ns_dst`` content is undefined.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. The destination buffer
 *   is ``cmse_check_address_range``-validated so the secure XSPI driver
 *   cannot be tricked into writing into secure memory.
 *
 * @note Thread-safe: serialised by the secure XSPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_xspi_read(uint32_t flash_off, uint8_t* ns_dst, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ns_dst, s_tag, "xspi_read: ns_dst");
  if ((len == 0U) || (len > k_ra8_nsc_xspi_max_read)) {
    return k_ra8_err_invalid_arg;
  }
  RA8_NSC_CHECK_NS_RANGE_RW(ns_dst, len);
  return ra8_xspi_flash_read(k_ra8_nsc_xspi_instance, flash_off, ns_dst, len);
}

/**
 * @brief NSC veneer: query secure-side XSPI status.
 *
 * @details Range-checks ``out_mask`` in NS memory and forwards to
 *   ``ra8_xspi_get_status``.
 *
 * @param[in]  instance XSPI instance index.
 * @param[out] out_mask NS destination for the status mask.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Status copied to ``*out_mask``.
 * @retval k_ra8_err_null_ptr     ``out_mask`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region or bad instance.
 *
 * @pre TrustZone substrate up.
 * @pre ``out_mask`` lies in NS data region.
 * @post On success ``*out_mask`` reflects the live status bits.
 * @post On failure ``*out_mask`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``out_mask`` is
 *   range-checked.
 *
 * @note Thread-safe: serialised by the secure XSPI driver lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_xspi_status(uint8_t instance, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "xspi_status: out_mask");
  RA8_NSC_CHECK_NS_RANGE_RW(out_mask, sizeof(*out_mask));
  return ra8_xspi_get_status(instance, out_mask);
}
