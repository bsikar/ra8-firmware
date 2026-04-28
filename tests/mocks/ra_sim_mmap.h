/**
 * @file ra_sim_mmap.h
 * @brief Host-test helper to reset RA8D2 MMIO backing memory
 *
 * @details
 * Test binaries that link ``ra_sim_mmap.c`` get the full RA8D2
 * register windows wired up to ordinary host RAM via a constructor.
 * Individual tests call `ra_sim_mmap_reset()` from their `setUp()`
 * routine to scrub the backing memory between cases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zero every RA8D2 MMIO backing region.
 *
 * @details
 * Safe to call from test setup/teardown. Leaves the mappings in
 * place -- only clears their contents.
 *
 * @pre Called from a host (`RA_SIMULATOR_MODE`) test binary.
 * @post Every byte in the mapped hardware windows reads zero.
 *
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
void ra_sim_mmap_reset(void);

#ifdef __cplusplus
}
#endif
