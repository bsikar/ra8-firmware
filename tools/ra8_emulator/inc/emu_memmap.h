/**
 * @file emu_memmap.h
 * @brief Emulated RA8D2 memory map: regions, alias backings, MMIO windows
 *
 * @details
 * Owns the emulator's memory-region table (from the linker script / HUM
 * R01UH1065EJ) and the host buffers that back the TrustZone alias pairs
 * (SRAM/NS_SRAM2, SDRAM/NS_SDRAM, OSPI/NS_OSPI share one array each so both
 * views stay coherent). emu_memmap_init() maps every region on an engine,
 * seeds the factory-trim TSN calibration cells, and installs the Renesas
 * peripheral space (Secure + bit[28] Non-secure alias) as callback MMIO
 * through the sparse model in emu_mmio. The cpu1 engine re-maps the same
 * table through the accessors here so the second core shares the exact
 * physical bytes.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct mem_region_t
 * @brief One emulated memory region: name, base address, and byte size.
 *
 * @details A row of the RA8D2 memory-map table. Alias rows share a host
 * backing with their physical row (the backing row precedes its alias in the
 * table, so the buffer exists before the alias maps onto it).
 *
 * @invariant Regions do not overlap; alias rows follow their backing row.
 * @see emu_memmap_regions()  Returns the table.
 * @since 0.1.0
 */
typedef struct {
  const char* name; /**< Human-readable region name (map-failure reporting). */
  uint64_t    base; /**< Region base address in the emulated map.            */
  uint64_t    size; /**< Region size in bytes.                               */
} mem_region_t;

/**
 * @enum ram_region_t
 * @brief Emulated RAM windows a GLCDC framebuffer may legally live in.
 */
typedef enum : uint32_t {
  k_dtcm_base     = 0x20000000U, /**< Data TCM start.                              */
  k_dtcm_end      = 0x20010000U, /**< Data TCM end.                                */
  k_sram_base     = 0x22000000U, /**< On-chip SRAM start.                          */
  k_sram_end      = 0x22100000U, /**< On-chip SRAM end.                            */
  k_sdram_base    = 0x68000000U, /**< External SDRAM start.                        */
  k_sdram_end     = 0x6C000000U, /**< External SDRAM end.                          */
  k_ns_sdram_base = 0x78000000U, /**< External SDRAM Non-secure alias (bit[28]=1). */
  k_page_size     = 0x1000U,     /**< 4 KiB host-map alignment for SRAM buffer.    */
} ram_region_t;

/**
 * @brief Map every memory region + the peripheral MMIO windows on @p uc.
 *
 * @details
 * Walks the region table: the on-chip SRAM, external SDRAM and OSPI XIP
 * windows are host-backed (so their Non-secure aliases and the cpu1 engine
 * can share the same bytes); every other region maps as plain RAM. Then
 * seeds the TSN factory calibration into the mapped TRIM page and installs
 * the Renesas peripheral space -- Secure (0x40000000) first, then the IDAU
 * bit[28] Non-secure alias (0x50000000) -- as callback MMIO through the
 * sparse model. Any failure prints the original diagnostic and returns
 * false so the caller can exit with the original status.
 *
 * @param[in,out] uc Freshly-opened Unicorn engine to prepare.
 * @return true when every region and MMIO window mapped.
 * @retval false An allocation or mapping failed (diagnostic printed).
 * @pre @p uc is open with no regions mapped yet.
 * @pre Called once during single-threaded setup.
 * @post On success every k_regions row and both MMIO windows are live.
 * @post On failure the process should exit (partial map).
 * @note Not thread-safe; call once during setup.
 * @see emu_memmap_regions()  The table this maps.
 * @since 0.1.0
 */
RA8_PRIV bool emu_memmap_init(uc_engine* uc);

/**
 * @brief The emulated memory-region table.
 *
 * @param[out] count Receives the number of rows.
 * @return Pointer to the first region row.
 * @retval NULL Never (the table is static).
 * @pre @p count is non-null.
 * @pre None otherwise.
 * @post @p count holds the row count.
 * @note Read-only table; safe to hold for the whole run.
 * @since 0.1.0
 */
RA8_PRIV const mem_region_t* emu_memmap_regions(uint32_t* count);

/**
 * @brief Host buffer backing the on-chip SRAM (shared with cpu1).
 *
 * @return The SRAM host backing, or NULL before emu_memmap_init().
 * @retval NULL emu_memmap_init() has not run (or its allocation failed).
 * @pre emu_memmap_init() succeeded.
 * @pre None otherwise.
 * @post No state is modified.
 * @note The cpu1 engine maps this same buffer so cross-core IPC over shared
 *       SRAM is coherent.
 * @since 0.1.0
 */
RA8_PRIV uint8_t* emu_memmap_sram_buf(void);

/**
 * @brief Base address of the MRAM region (the boot vector table home).
 *
 * @details The reset SP/PC live at MRAM[0]/MRAM[4] and the exception model
 * falls back to this base when VTOR reads zero; this is the named lookup for
 * what used to be the k_regions[1].base magic index.
 *
 * @return The MRAM region base address.
 * @retval 0 Never (the table is static and MRAM is always present).
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Read-only; safe at any time.
 * @since 0.1.0
 */
RA8_PRIV uint64_t emu_memmap_mram_base(void);

#ifdef __cplusplus
}
#endif
