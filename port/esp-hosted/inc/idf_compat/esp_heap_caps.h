/**
 * @file port/esp-hosted/inc/idf_compat/esp_heap_caps.h
 * @brief ESP-IDF capability-flag compatibility for the esp-hosted port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * ESP-IDF splits an ESP chip's address space into regions with different
 * properties -- internal versus external, DMA-reachable or not -- and its
 * allocator takes a bitmask saying which properties the caller needs.
 * ``host/drivers/transport/transport_util.c`` passes those flags through
 * when it maps esp-hosted's ``hosted_mem_cap_t`` onto a concrete
 * allocation.
 *
 * The RA8D2 has no such split for this port's purposes. Every buffer the
 * transport allocates comes from one fixed ThreadX byte pool carved out of
 * a static array in ``.bss``, which is internal SRAM and is reachable by
 * both the DMAC and the SCI peripheral. So on this target every capability
 * request is satisfiable by the one pool, and the flags are carried for
 * source compatibility rather than steering an allocator decision.
 *
 * That is stated rather than hidden: the values below keep the upstream
 * bit positions so a flag word logged on this host means the same thing an
 * ESP-IDF reader would expect, and the port's allocator documents that it
 * ignores them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum ra8_esp_hosted_malloc_cap_t
 * @brief Allocation capability bits, at their ESP-IDF bit positions.
 *
 * @details
 * Only the bits the vendored tree actually names are defined. Keeping the
 * upstream positions matters because the flags are combined with bitwise
 * or at the call sites and are occasionally logged; a re-numbered set would
 * still compile and would still be wrong in a trace.
 *
 * The enumerator spellings are fixed by ESP-IDF, so they do not follow the
 * project's ``k_`` prefix convention.
 *
 * @invariant Each value is a single distinct bit.
 *
 * @par Example:
 * @code
 * void *p = allocate(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
typedef enum : uint32_t {
  // NOLINTBEGIN(readability-identifier-naming) -- ESP-IDF-fixed spellings.
  MALLOC_CAP_EXEC     = 0x00000001U, /**< Memory the core can fetch from. */
  MALLOC_CAP_32BIT    = 0x00000002U, /**< Word-accessible only.           */
  MALLOC_CAP_8BIT     = 0x00000004U, /**< Byte-accessible.                */
  MALLOC_CAP_DMA      = 0x00000008U, /**< Reachable by a DMA engine.      */
  MALLOC_CAP_SPIRAM   = 0x00000400U, /**< Off-chip PSRAM on an ESP part.  */
  MALLOC_CAP_INTERNAL = 0x00000800U, /**< On-chip SRAM.                   */
  MALLOC_CAP_DEFAULT  = 0x00001000U, /**< No particular requirement.      */
  // NOLINTEND(readability-identifier-naming)
} ra8_esp_hosted_malloc_cap_t;
