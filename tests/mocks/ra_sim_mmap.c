/**
 * @file ra_sim_mmap.c
 * @brief Host-side MMIO backing store for RA8D2 HAL unit tests
 *
 * @details
 * The RA8D2 HAL driver sources compute peripheral addresses directly
 * from typed-enum base addresses (e.g. `0x40400000` for PORT0) and
 * dereference them. On the Cortex-M85 target those addresses are real
 * memory-mapped registers. On the x86_64 host the same addresses are
 * unmapped virtual memory -- any read would segfault.
 *
 * Rather than maintaining a parallel set of "mock" register header
 * files, this file maps ordinary anonymous RAM at exactly the same
 * virtual addresses the HAL expects, so the production accessor
 * functions (`ra_port()`, `ra_sci0()`, ...) work unchanged in
 * `RA_SIMULATOR_MODE`. Writes land in host RAM and reads observe
 * whatever the tests wrote, which is sufficient for driver-level
 * unit tests that only care about the sequence of register writes.
 *
 * The backing regions are installed by a constructor so they are
 * available before `main()` in every unit-test binary that links
 * against `ra_core_hal`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA_SIMULATOR_MODE

#define _GNU_SOURCE
#include "ra_sim_mmap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/**
 * @enum ra_sim_region_id_t
 * @brief Indices into ::s_ra_sim_regions.
 */
typedef enum : uint8_t {
  k_ra_sim_region_peri  = 0, /**< Peripheral bus 0x40000000.       */
  k_ra_sim_region_core  = 1, /**< ARM SCB / MPU 0xE0000000.        */
  k_ra_sim_region_mram  = 2, /**< MRAM calibration 0x02C00000.     */
  k_ra_sim_region_sram  = 3, /**< SRAM 0x22000000.                 */
  k_ra_sim_region_sdram = 4, /**< SDRAM framebuffers 0x68000000.  */
  k_ra_sim_region_count = 5,
} ra_sim_region_id_t;

/**
 * @struct ra_sim_region_t
 * @brief Backing memory region descriptor.
 */
typedef struct {
  uintptr_t base; /**< Virtual base address to map.         */
  size_t    size; /**< Bytes to map.                        */
} ra_sim_region_t;

/**
 * @brief All memory windows that have to exist on the host.
 *
 * Sizes are rounded up to whole megabytes so we cover every enum
 * base plus any future driver without revisiting this list.
 */
enum : size_t {
  k_ra_sim_peri_size  = 0x00800000U, /**< 8 MiB over the peri bus. */
  k_ra_sim_core_size  = 0x00100000U, /**< 1 MiB SCB/MPU window.    */
  k_ra_sim_mram_size  = 0x00100000U, /**< 1 MiB MRAM span.         */
  k_ra_sim_sram_size  = 0x00200000U, /**< 2 MiB SRAM span.         */
  k_ra_sim_sdram_size = 0x00100000U, /**< 1 MiB SDRAM head.        */
};

enum : uintptr_t {
  k_ra_sim_peri_base  = 0x40000000UL,
  k_ra_sim_core_base  = 0xE0000000UL,
  k_ra_sim_mram_base  = 0x02C00000UL,
  k_ra_sim_sram_base  = 0x22000000UL,
  k_ra_sim_sdram_base = 0x68000000UL,
};

static const ra_sim_region_t s_ra_sim_regions[k_ra_sim_region_count] = {
  {.base = (uintptr_t)k_ra_sim_peri_base, .size = (size_t)k_ra_sim_peri_size},
  {.base = (uintptr_t)k_ra_sim_core_base, .size = (size_t)k_ra_sim_core_size},
  {.base = (uintptr_t)k_ra_sim_mram_base, .size = (size_t)k_ra_sim_mram_size},
  {.base = (uintptr_t)k_ra_sim_sram_base, .size = (size_t)k_ra_sim_sram_size},
  {.base = (uintptr_t)k_ra_sim_sdram_base, .size = (size_t)k_ra_sim_sdram_size},
};

static uint8_t s_ra_sim_mapped = 0U;

/**
 * @brief Install RAM backings for every hardware window.
 *
 * @details
 * Runs via ``__attribute__((constructor))`` before ``main()`` in every
 * test binary. Failures are fatal -- if MAP_FIXED cannot place a
 * region, tests would segfault on the first HAL register access, so
 * we print a diagnostic and abort instead.
 */
__attribute__((constructor)) static void ra_sim_mmap_install(void)
{
  if (s_ra_sim_mapped != 0U) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sim_region_count; ++i) {
    const ra_sim_region_t region = s_ra_sim_regions[i];
    void*                 p      = mmap((void*)region.base,
                                        region.size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                                        -1,
                                        0);
    if (p == MAP_FAILED || (uintptr_t)p != region.base) {
      (void)fprintf(stderr,
                    "ra_sim_mmap: failed to map 0x%llx..0x%llx\n",
                    (unsigned long long)region.base,
                    (unsigned long long)(region.base + region.size));
      abort();
    }
    (void)memset(p, 0, region.size);
  }
  s_ra_sim_mapped = 1U;
}

void ra_sim_mmap_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sim_region_count; ++i) {
    const ra_sim_region_t region = s_ra_sim_regions[i];
    (void)memset((void*)region.base, 0, region.size);
  }
}

#else
/* Non-simulator build: this TU contributes nothing. */
typedef int ra_sim_mmap_placeholder_t;
#endif /* RA_SIMULATOR_MODE */
