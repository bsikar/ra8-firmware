/**
 * @file ra8_tz_psar.h
 * @brief PRCR-gated peripheral security attribution (PSARn) for secure boot
 * @ingroup grp_system
 *
 * @details
 * `ra8_tz_partition.h` made the SAU and SRAM halves of the attribution map
 * data. The peripheral half stayed raw MMIO: three TrustZone apps each carry a
 * private `tz_usb_mark_ns()` with its own copy of the PSARB address, its own
 * USB bit literals, its own `PRCR_S.PRC4` unlock/relock scope and its own
 * bounded read-back loop. That is the same shape, and the same drift risk, the
 * SAU side already had.
 *
 * This header gives the peripheral write one home. A caller names the PSAR
 * register and the bits it wants handed to the Non-Secure world; the gate, the
 * read-back confirmation and the re-lock live here.
 *
 * Two RA8D2 rules make the read-back part of the write rather than a nicety.
 * PSARn shares the `PRCR_S.PRC4` write gate with `SRAMSABARn` (HUM Ch 13.2.1
 * "Association between PRCR bits and use of registers" p 521), so a store
 * issued with the gate locked is discarded silently: no bus fault, no status
 * flag. And HUM "Security or Privilege Bit Write Timing" p 3301 requires
 * reading the register back until it matches before relying on the new
 * attribution. A caller that skips either one gets code that looks correct and
 * leaves the peripheral Secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_tz_psar_limits_t
 * @brief Bounds the PSAR write is checked and retried against.
 *
 * @details
 * `k_ra8_tz_psar_readback_spins` bounds the confirmation loop. The register
 * settles in a handful of cycles, so a value this far out means the write did
 * not land (a locked gate, or a Non-Secure caller) rather than a slow bus, and
 * spinning forever would hang boot on exactly the case worth reporting.
 */
typedef enum : uint32_t {
  k_ra8_tz_psar_readback_spins = 1000U, /**< Bounded read-back confirm loop. */
} ra8_tz_psar_limits_t;

/**
 * @brief Hand a set of peripherals to the Non-Secure world in a PSAR register.
 *
 * @details
 * Opens `PRCR_S.PRC4`, ORs @p ns_mask into the register at @p psar_addr, spins
 * (bounded) on the read-back until the value confirms, then re-locks the gate.
 * Bits already set are left set: the mask is additive, because two independent
 * subsystems may each hand their own peripheral over during the same boot.
 *
 * @param[in]  psar_addr  PSARB..PSARE address (`R_PSCU` base + 0x04..0x10).
 * @param[in]  ns_mask    Bits to set; 1 = Non-secure. Zero is a no-op success.
 * @param[out] out_seen   Confirmed read-back value, or NULL. Written on both
 *                        the success and the timeout path so a bench halt can
 *                        see what the register actually held.
 *
 * @return k_ra8_ok when the read-back confirmed the requested bits.
 * @retval k_ra8_err_invalid_arg  @p psar_addr is zero.
 * @retval k_ra8_err_timeout        The read-back never matched; the gate is
 *                                  re-locked and @p out_seen holds the last
 *                                  value read.
 *
 * @pre Caller is in Secure state.
 * @post `PRCR_S.PRC4` is locked again on every path.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
ra8_err_t ra8_tz_psar_set_ns(uintptr_t psar_addr, uint32_t ns_mask, uint32_t* out_seen);

#ifdef __cplusplus
}
#endif
