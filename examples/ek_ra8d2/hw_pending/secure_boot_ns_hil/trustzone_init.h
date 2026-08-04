/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/trustzone_init.h
 * @brief Secure TrustZone bring-up contract for the BLXNS RoT proof (#172).
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Declares ::ra8_trustzone_init (called from ``SystemInit`` before the C runtime
 * is live) and the fixed Non-Secure image addresses shared between
 * ``trustzone_init.c`` (which copies the NS image MRAM->SRAM) and ``main.c``
 * (which authenticates it and BLXNS-es to it). The values MUST match
 * ``ns_image.ld``'s ``NS_LOAD`` / ``NS_SRAM_RUN`` origins.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum sbns_ns_image_t
 * @brief Fixed Non-Secure image load / run / copy-window addresses.
 *
 * @details
 * The NS image is a SEPARATE ELF, so the Secure side has none of its linker
 * symbols and must hard-code these (they mirror ``ns_image.ld``). It is flashed
 * at the MRAM LMA and copied to the SRAM2 Non-secure alias before BLXNS.
 *
 * @invariant Matches ORIGIN(NS_LOAD) / ORIGIN(NS_SRAM_RUN) in ns_image.ld.
 */
typedef enum : uintptr_t {
  k_sbns_ns_load_base = 0x02080000U, /**< NS image LMA (Secure MRAM).     */
  k_sbns_ns_run_base  = 0x32100000U, /**< NS image VMA (SRAM2 NS alias).  */
  k_sbns_ns_copy_size = 0x00010000U, /**< Bytes copied LMA->VMA (64 KiB). */
} sbns_ns_image_t;

/**
 * @brief Programme the SAU + SRAM NS boundary and copy the NS image (no BLXNS).
 *
 * @details
 * Carves the SRAM2 Non-secure aperture via ``SRAMSABARn``, programmes the
 * bit[28] SAU (the IDAU-NS ranges Non-secure, ALLNS = 0 default-deny), and
 * copies the NS image from its MRAM LMA to the SRAM run base. It deliberately
 * does NOT jump: ``main()`` performs the root-of-trust verify + BLXNS after the
 * C runtime (and the crypto heap) is live. On a host build (``RA8_OFF_TARGET``
 * or no ``RA8_TRUSTZONE_ENABLE``) it is a no-op.
 *
 * @return void.
 * @pre Caller is in Secure state, single-threaded, early boot (from SystemInit).
 * @pre The SAU is disabled and the IDAU is in its documented reset state.
 * @post SRAM2 is Non-secure, the SAU is enabled, and the NS image is resident
 *       at ::k_sbns_ns_run_base (or nothing happened on a host build).
 * @post No BLXNS is performed; control returns to the caller.
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
void ra8_trustzone_init(void);

#ifdef __cplusplus
}
#endif
