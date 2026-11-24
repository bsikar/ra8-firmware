/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/inc/secure_boot_ns_hil.h
 * @brief Non-Secure image address contract shared by `trustzone_init.c` and `main.c`.
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Holds the fixed Non-Secure image load/run/copy-window constants for
 * `secure_boot_ns_hil`. Separating these constants from `trustzone_init.h`
 * lets the app consume `libs/ra8_board_ek_ra8d2/inc/trustzone_init.h` directly
 * without shadowing a board header.
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
 * symbols and must carry these as constants; they come in as compile
 * definitions from ra8_ns_memory_map_defines(). It is flashed
 * at the MRAM LMA and copied to the SRAM2 Non-secure alias before BLXNS.
 *
 * @invariant Matches RA8_NS_MRAM_ORIGIN / RA8_NS_SRAM_ORIGIN in
 *            libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake.
 */
typedef enum : uintptr_t {
  k_sbns_ns_load_base = RA8_NS_MRAM_BASE, /**< NS image LMA (Secure MRAM).     */
  k_sbns_ns_run_base  = RA8_NS_SRAM_BASE, /**< NS image VMA (SRAM2 NS alias).  */
  k_sbns_ns_copy_size = 0x00010000U,      /**< Bytes copied LMA->VMA (64 KiB). */
} sbns_ns_image_t;

#ifdef __cplusplus
}
#endif
