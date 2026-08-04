/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_etha_internal.h
 * @brief Cross-TU shared surface for the ra8_etha driver split.
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The ETHA driver is split across two translation units to stay under
 * the per-file line-count cap: ra8_etha.c holds the lifecycle, IRQ, and
 * traffic-shaping path, while ra8_etha_stats.c holds the per-port
 * statistics, descriptor-ring sizing, and PHY-open path. This src/-local
 * header carries the handful of symbols both TUs share: the logger tag,
 * the per-port runtime slot table and its element type, and the shared
 * port range-check predicate. It is NOT part of the public ABI.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_etha.h"

/**
 * @struct ra8_etha_slot_t
 * @brief Per-port runtime state.
 */
typedef struct {
  ra8_etha_event_fn_t   cb;    /**< Attached callback (nullptr if none). */
  void*                 ctx;   /**< Opaque cookie passed back to cb.     */
  ra8_etha_port_stats_t stats; /**< Per-port traffic counters.           */
} ra8_etha_slot_t;

/**
 * @var s_etha_slots
 * @brief Per-port handler table; index = ::ra8_etha_port_t.
 *
 * @details Backing store for the per-port callback, cookie, and counter
 * state. Defined once in ra8_etha.c and referenced from the split-out
 * statistics TU via this declaration.
 * @warning Not safe to mutate outside the ETHA driver path.
 * @since 0.1.0
 */
extern ra8_etha_slot_t s_etha_slots[k_ra8_etha_port_count];

/**
 * @brief Range-check a port argument.
 *
 * @details Shared predicate used by both ETHA translation units to
 * bounds-check a ::ra8_etha_port_t before it indexes ::s_etha_slots or
 * selects an MMIO register window. Pure; no side effects.
 *
 * @param[in] port Port to validate.
 * @return true if port is one of ::ra8_etha_port_t.
 * @retval true  port in 0 .. (k_ra8_etha_port_count - 1).
 * @retval false port out of range.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on @p port.
 * @note Thread safety: pure function, safe from any context.
 * @since 0.1.0
 */
static inline bool internal_port_ok(ra8_etha_port_t port)
{
  return (uint8_t)port < k_ra8_etha_port_count;
}

#ifdef __cplusplus
}
#endif
