/**
 * @file ra8_sim_mmap.c
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
 * functions (`ra8_port()`, `ra8_sci0()`, ...) work unchanged in
 * `RA8_SIMULATOR_MODE`. Writes land in host RAM and reads observe
 * whatever the tests wrote, which is sufficient for driver-level
 * unit tests that only care about the sequence of register writes.
 *
 * The backing regions are installed by a constructor so they are
 * available before `main()` in every unit-test binary that links
 * against `ra8_core_hal`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA8_SIMULATOR_MODE

/** @brief GNU SOURCE. */
/* glibc fixes the spelling of its feature-test macros, so the reserved-identifier
 * and UPPER_CASE-macro rules cannot apply: personality() is only declared when
 * _GNU_SOURCE is defined before the first system header. */
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
#define _GNU_SOURCE
#include "ra8_sim_mmap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

/**
 * @var __executable_start
 * @brief Linker-provided first byte of the loaded ELF image.
 *
 * @details
 * GNU ld (and lld) define this symbol at the base of the text segment.
 * Together with `sbrk(0)` (the current program break, i.e. the end of
 * .bss plus the initial heap) it bounds everything the loader placed at
 * fixed addresses, which ::ra8_sim_mmap_install checks against the
 * backing windows before issuing any `MAP_FIXED` mapping.
 *
 * @note Read-only address anchor; never dereferenced as data.
 * @since 0.1.0
 */
/* The linker defines this symbol; its spelling is fixed by the toolchain,
 * so the reserved-identifier rule cannot apply. */
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern char __executable_start[];

/**
 * @def RA8_SIM_UNDER_ASAN
 * @brief Defined to 1 when this translation unit is compiled under
 *        AddressSanitizer; left undefined otherwise.
 *
 * @details
 * Only the libFuzzer harnesses (see tests/fuzz/, issue #193) compile this
 * mock with `-fsanitize=address`; the ordinary host unit-test build does
 * not. clang exposes ASan through `__has_feature(address_sanitizer)`,
 * while gcc and recent clang additionally predefine `__SANITIZE_ADDRESS__`.
 * The nested form below avoids referencing `__has_feature` on toolchains
 * that do not define it. When set, ::ra8_sim_region_mappable skips the one
 * backing window (the 0xE0000000 SCB/MPU region) that lands in ASan's
 * shadow gap and therefore cannot be `MAP_FIXED`. Every non-ASan build maps
 * all regions unconditionally, so `make test` is unaffected.
 *
 * @note Build-configuration guard only; never referenced by production
 *       firmware, which does not compile this file.
 * @since 0.1.0
 */
#ifdef __SANITIZE_ADDRESS__
/** @brief RA8 SIM UNDER ASAN. */
#define RA8_SIM_UNDER_ASAN (1)
#elifdef __has_feature
#if __has_feature(address_sanitizer)
/** @brief RA8 SIM UNDER ASAN. */
#define RA8_SIM_UNDER_ASAN (1)
#endif
#endif

/**
 * @enum ra8_sim_region_id_t
 * @brief Indices into ::s_ra8_sim_regions.
 */
typedef enum : uint8_t {
  k_ra8_sim_region_peri       = 0, /**< Peripheral bus 0x40000000.             */
  k_ra8_sim_region_core       = 1, /**< ARM SCB / MPU 0xE0000000.              */
  k_ra8_sim_region_mram       = 2, /**< MRAM calibration 0x02C00000.           */
  k_ra8_sim_region_sram       = 3, /**< SRAM 0x22000000.                       */
  k_ra8_sim_region_sdram      = 4, /**< SDRAM framebuffers 0x68000000.         */
  k_ra8_sim_region_code_mram  = 5, /**< Code MRAM 0x02000000 (1 MiB).          */
  k_ra8_sim_region_extra_mram = 6, /**< Extra MRAM 0x27000000 (64 KiB).        */
  k_ra8_sim_region_peri_ns    = 7, /**< NS alias of peripheral bus 0x50000000. */
  k_ra8_sim_region_count      = 8, /**< RA8 sim region count.                  */
} ra8_sim_region_id_t;

/**
 * @struct ra8_sim_region_t
 * @brief Backing memory region descriptor.
 */
typedef struct {
  uintptr_t base;            /**< Virtual base address to map.                    */
  size_t    size;            /**< Bytes to map.                                   */
  bool      asan_shadow_gap; /**< True if base lands in ASan's shadow gap (#193). */
} ra8_sim_region_t;

/**
 * @brief All memory windows that have to exist on the host.
 *
 * Sizes are rounded up to whole megabytes so we cover every enum
 * base plus any future driver without revisiting this list.
 */
enum : size_t {
  k_ra8_sim_peri_size       = 0x00800000U, /**< 8 MiB over the peri bus.           */
  k_ra8_sim_core_size       = 0x00100000U, /**< 1 MiB SCB/MPU window.              */
  k_ra8_sim_mram_size       = 0x00100000U, /**< 1 MiB MRAM span.                   */
  k_ra8_sim_sram_size       = 0x00200000U, /**< 2 MiB SRAM span.                   */
  k_ra8_sim_sdram_size      = 0x00100000U, /**< 1 MiB SDRAM head.                  */
  k_ra8_sim_code_mram_size  = 0x00100000U, /**< 1 MiB code MRAM (HUM 59.1 p 3543). */
  k_ra8_sim_extra_mram_size = 0x00010000U, /**< 64 KiB extra MRAM head.            */
};

enum : uintptr_t {
  k_ra8_sim_peri_base       = 0x40000000UL, /**< RA8 sim peri base.                            */
  k_ra8_sim_core_base       = 0xE0000000UL, /**< RA8 sim core base.                            */
  k_ra8_sim_mram_base       = 0x02C00000UL, /**< RA8 sim MRAM base.                            */
  k_ra8_sim_sram_base       = 0x22000000UL, /**< RA8 sim SRAM base.                            */
  k_ra8_sim_sdram_base      = 0x68000000UL, /**< RA8 sim SDRAM base.                           */
  k_ra8_sim_code_mram_base  = 0x02000000UL, /**< Code MRAM (matches k_ra8_flash_code_start).   */
  k_ra8_sim_extra_mram_base = 0x27000000UL, /**< Extra MRAM (matches k_ra8_flash_extra_start). */
  k_ra8_sim_peri_ns_base    = 0x50000000UL, /**< Non-Secure alias of peri bus.                 */
};

static const ra8_sim_region_t s_ra8_sim_regions[k_ra8_sim_region_count] = {
  {.base = (uintptr_t)k_ra8_sim_peri_base, .size = (size_t)k_ra8_sim_peri_size},
  /* The SCB/MPU window at 0xE0000000 falls inside AddressSanitizer's
   * shadow gap; it is the only region that cannot be MAP_FIXED under ASan. */
  {.base            = (uintptr_t)k_ra8_sim_core_base,
   .size            = (size_t)k_ra8_sim_core_size,
   .asan_shadow_gap = true},
  {.base = (uintptr_t)k_ra8_sim_mram_base, .size = (size_t)k_ra8_sim_mram_size},
  {.base = (uintptr_t)k_ra8_sim_sram_base, .size = (size_t)k_ra8_sim_sram_size},
  {.base = (uintptr_t)k_ra8_sim_sdram_base, .size = (size_t)k_ra8_sim_sdram_size},
  {.base = (uintptr_t)k_ra8_sim_code_mram_base, .size = (size_t)k_ra8_sim_code_mram_size},
  {.base = (uintptr_t)k_ra8_sim_extra_mram_base, .size = (size_t)k_ra8_sim_extra_mram_size},
  {.base = (uintptr_t)k_ra8_sim_peri_ns_base, .size = (size_t)k_ra8_sim_peri_size},
};

static uint8_t s_ra8_sim_mapped = 0U;

/**
 * @brief Decide whether a backing region may be mapped in this build.
 *
 * @details
 * All regions are mappable in the ordinary host unit-test build. Under
 * AddressSanitizer (::RA8_SIM_UNDER_ASAN), the single window flagged
 * ``asan_shadow_gap`` -- the 0xE0000000 SCB/MPU region -- lands in ASan's
 * reserved shadow gap, where a ``MAP_FIXED`` (and the subsequent memset,
 * intercepted by ASan) aborts the process before ``main()`` (issue #193).
 * Only the peripheral-bus fuzz harnesses that link this mock exercise the
 * host MMIO store, and none of them touch SCB/MPU, so skipping exactly that
 * region under ASan is safe.
 *
 * @param[in] asan_shadow_gap ::ra8_sim_region_t::asan_shadow_gap of the region.
 *
 * @return Whether the region should be mapped / cleared.
 * @retval true  Region is mappable (always, unless ASan + shadow-gap region).
 * @retval false Region is the shadow-gap window and this is an ASan build.
 *
 * @pre The caller iterates ::s_ra8_sim_regions in index order.
 * @post The return value is stable for a given build configuration.
 *
 * @note Thread-safe: reads no shared mutable state.
 * @since 0.1.0
 */
static bool ra8_sim_region_mappable(bool asan_shadow_gap)
{
#ifdef RA8_SIM_UNDER_ASAN
  return !asan_shadow_gap;
#else
  (void)asan_shadow_gap;
  return true;
#endif
}

/**
 * @brief Abort if any mappable backing window overlaps the loaded image/heap.
 *
 * @details A MAP_FIXED window that intersects the live image or brk heap would
 *          silently replace those pages; this pre-check turns a future layout
 *          regression into an immediate, diagnosable abort.
 *
 * @param[in] image_lo Lowest byte of the loaded image (`__executable_start`).
 * @param[in] image_hi One past the current program break.
 *
 * @return None.
 * @pre @p image_lo <= @p image_hi.
 * @post Returns only if no mappable window overlaps [image_lo, image_hi).
 * @note Not thread-safe; runs once during the mmap installer.
 * @since 0.1.0
 */
static void ra8_sim_mmap_check_overlap(uintptr_t image_lo, uintptr_t image_hi)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sim_region_count; ++i) {
    const ra8_sim_region_t region = s_ra8_sim_regions[i];
    if (!ra8_sim_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    if ((region.base < image_hi) && (image_lo < (region.base + region.size))) {
      (void)fprintf(stderr,
                    "ra8_sim_mmap: window 0x%llx..0x%llx overlaps the loaded "
                    "image/heap 0x%llx..0x%llx -- relink the test binary "
                    "clear of the backing windows (tests/CMakeLists.txt "
                    "-Ttext-segment pin)\n",
                    (unsigned long long)region.base,
                    (unsigned long long)region.base + (unsigned long long)region.size,
                    (unsigned long long)image_lo,
                    (unsigned long long)image_hi);
      abort();
    }
  }
}

/**
 * @brief MAP_FIXED-map and zero every mappable backing window.
 *
 * @return None.
 * @pre The backing windows do not overlap the loaded image/heap.
 * @post Every mappable window is mapped read/write and zero-filled, or aborts.
 * @note Not thread-safe; runs once during the mmap installer.
 * @since 0.1.0
 */
static void ra8_sim_mmap_map_regions(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sim_region_count; ++i) {
    const ra8_sim_region_t region = s_ra8_sim_regions[i];
    if (!ra8_sim_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    void* p = mmap((void*)region.base,
                   region.size,
                   PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                   -1,
                   0);
    if (p == MAP_FAILED || (uintptr_t)p != region.base) {
      (void)fprintf(stderr,
                    "ra8_sim_mmap: failed to map 0x%llx..0x%llx\n",
                    (unsigned long long)region.base,
                    (unsigned long long)region.base + (unsigned long long)region.size);
      abort();
    }
    (void)memset(p, 0, region.size);
  }
}

/**
 * @brief Install RAM backings for every hardware window.
 *
 * @details
 * Runs via ``__attribute__((constructor))`` before ``main()`` in every
 * test binary. Failures are fatal -- if MAP_FIXED cannot place a
 * region, tests would segfault on the first HAL register access, so
 * we print a diagnostic and abort instead. Under AddressSanitizer the
 * shadow-gap region is skipped up front (see ::ra8_sim_region_mappable).
 *
 * Before mapping, the constructor disables ASLR and re-execs itself once.
 * Fixed-address windows (``MAP_FIXED``) and ASLR are fundamentally
 * incompatible: with randomization on, the kernel can place the C heap, a
 * thread stack, or an mmap arena at an address one of these windows clobbers
 * (or grow the heap into one), silently corrupting glibc's top chunk. That is
 * the root of the rare ``Subprocess aborted`` (``malloc assertion failure``)
 * flake seen when the suite runs many allocation-heavy tests. Re-execing with
 * ``ADDR_NO_RANDOMIZE`` makes every host-test process use a deterministic,
 * collision-free layout. The ``RA8_SIM_NO_ASLR`` env sentinel bounds it to a
 * single re-exec even if ``personality()`` is refused, and a failed ``execv``
 * falls through to map best-effort rather than blocking the run.
 *
 * The ``.init_array`` calling convention passes ``(argc, argv, envp)`` to
 * constructors, which is what the re-exec needs.
 */
[[gnu::constructor]] static void ra8_sim_mmap_install(int argc, char** argv, char** envp)
{
  (void)argc;
  (void)envp;
  if (s_ra8_sim_mapped != 0U) {
    return;
  }
  if (getenv("RA8_SIM_NO_ASLR") == nullptr) {
    (void)personality(ADDR_NO_RANDOMIZE);
    (void)setenv("RA8_SIM_NO_ASLR", "1", 1);
    (void)execv("/proc/self/exe", argv);
    /* execv only returns on failure: fall through and map best-effort. */
  }
  /* The loaded image (text + .data + .bss) and the initial brk heap must not
   * intersect any backing window: MAP_FIXED would silently replace their live
   * pages (the classic symptom is a glibc sysmalloc assertion on the first
   * heap extension, long after the trample). A coverage build's gcov-counter
   * .bss once grew past 0x02000000 and landed inside the code-MRAM window;
   * the -Ttext-segment pin in tests/CMakeLists.txt prevents that, and this
   * check turns any future layout regression into an immediate diagnosis. */
  void* const     brk_now  = sbrk(0);
  const uintptr_t image_lo = (uintptr_t)__executable_start;
  const uintptr_t image_hi = (uintptr_t)brk_now;
  ra8_sim_mmap_check_overlap(image_lo, image_hi);
  ra8_sim_mmap_map_regions();
  s_ra8_sim_mapped = 1U;
}

void ra8_sim_mmap_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sim_region_count; ++i) {
    const ra8_sim_region_t region = s_ra8_sim_regions[i];
    if (!ra8_sim_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    (void)memset((void*)region.base, 0, region.size);
  }
}

#else
/* Non-simulator build: this TU contributes nothing. */
typedef int ra8_sim_mmap_placeholder_t;
#endif /* RA8_SIMULATOR_MODE */
