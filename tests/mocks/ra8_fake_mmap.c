/**
 * @file ra8_fake_mmap.c
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
 * `RA8_OFF_TARGET`. Writes land in host RAM and reads observe
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

#ifdef RA8_OFF_TARGET

/** @brief GNU SOURCE. */
/* glibc fixes the spelling of its feature-test macros, so the
 * reserved-identifier and UPPER_CASE-macro rules cannot apply: personality() is
 * only declared when _GNU_SOURCE is defined before the first system header. */
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
#define _GNU_SOURCE
#include "ra8_fake_mmap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../support/ra8_test_output.h"
#include "ra8_attributes.h"

/** @brief Address-rendering dimensions for fake-MMIO diagnostics. */
typedef enum : uint8_t {
  k_fake_mmap_hex_min_digits = 1U, /**< Unpadded hexadecimal minimum width. */
} fake_mmap_output_limit_t;

/**
 * @brief Write one unpadded lowercase address to an initialized sink.
 * @details Converts the host-width address through the shared bounded
 * hexadecimal renderer without adding the diagnostic's separate `0x` prefix.
 * @param[in,out] output Initialized diagnostic sink.
 * @param[in] address Host virtual address to render.
 * @pre @p output is initialized and its callback context remains alive.
 * @pre `uintptr_t` converts losslessly to `uint64_t` on supported hosts.
 * @post The significant lowercase hexadecimal digits are attempted once.
 * @post No prefix, padding, newline, or terminator is appended.
 * @note Destination errors remain latched in @p output for the caller.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_address(ra8_test_output_t* output, uintptr_t address)
{
  (void)internal_test_output_hex64(output,
                                   (uint64_t)address,
                                   (uint8_t)k_fake_mmap_hex_min_digits,
                                   false);
}

/**
 * @brief Report one backing-window overlap before aborting the test process.
 * @details Composes both half-open address ranges and the fixed relink hint
 * through caller-local raw-descriptor state.
 * @param[in] base First address of the requested fake-MMIO window.
 * @param[in] size Requested window size in bytes.
 * @param[in] image_lo First address occupied by the loaded image.
 * @param[in] image_hi Address immediately after the loaded image and heap.
 * @pre @p size is the validated nonzero region size.
 * @pre Both `base + size` and the image range were checked for host overflow.
 * @post One complete overlap diagnostic has been attempted on descriptor 2.
 * @post No descriptor is closed and no process-global sink is installed.
 * @note The caller performs the subsequent process abort.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_report_overlap(uintptr_t base, size_t size, uintptr_t image_lo, uintptr_t image_hi)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "ra8_fake_mmap: window 0x");
  internal_put_address(&output, base);
  (void)internal_test_output_text(&output, "..0x");
  internal_put_address(&output, base + size);
  (void)internal_test_output_text(&output, " overlaps the loaded image/heap 0x");
  internal_put_address(&output, image_lo);
  (void)internal_test_output_text(&output, "..0x");
  internal_put_address(&output, image_hi);
  (void)internal_test_output_text(&output,
                                  " -- relink the test binary clear of the backing windows "
                                  "(tests/CMakeLists.txt -Ttext-segment pin)\n");
}

/**
 * @brief Report one raw virtual-memory mapping failure before aborting.
 * @details Writes the failed half-open host range through a bounded
 * caller-local raw-descriptor adapter.
 * @param[in] base First address of the requested fake-MMIO window.
 * @param[in] size Requested window size in bytes.
 * @pre @p size is the validated nonzero region size.
 * @pre `base + size` was checked for host address overflow.
 * @post One complete mapping-failure diagnostic has been attempted.
 * @post No descriptor is closed and no process-global sink is installed.
 * @note The caller performs the subsequent process abort.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_map_failure(uintptr_t base, size_t size)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "ra8_fake_mmap: failed to map 0x");
  internal_put_address(&output, base);
  (void)internal_test_output_text(&output, "..0x");
  internal_put_address(&output, base + size);
  (void)internal_test_output_text(&output, "\n");
}

/**
 * @var __executable_start
 * @brief Linker-provided first byte of the loaded ELF image.
 *
 * @details
 * GNU ld (and lld) define this symbol at the base of the text segment.
 * Together with `sbrk(0)` (the current program break, i.e. the end of
 * .bss plus the initial heap) it bounds everything the loader placed at
 * fixed addresses, which ::internal_install checks against the
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
 * @def RA8_FAKE_UNDER_ASAN
 * @brief Defined to 1 when this translation unit is compiled under
 *        AddressSanitizer; left undefined otherwise.
 *
 * @details
 * Only the libFuzzer harnesses (see tests/fuzz/, issue #193) compile this
 * mock with `-fsanitize=address`; the ordinary host unit-test build does
 * not. clang exposes ASan through `__has_feature(address_sanitizer)`,
 * while gcc and recent clang additionally predefine `__SANITIZE_ADDRESS__`.
 * The nested form below avoids referencing `__has_feature` on toolchains
 * that do not define it. When set, ::internal_region_mappable skips the one
 * backing window (the 0xE0000000 SCB/MPU region) that lands in ASan's
 * shadow gap and therefore cannot be `MAP_FIXED`. Every non-ASan build maps
 * all regions unconditionally, so `make test` is unaffected.
 *
 * @note Build-configuration guard only; never referenced by production
 *       firmware, which does not compile this file.
 * @since 0.1.0
 */
#ifdef __SANITIZE_ADDRESS__
/** @brief RA8 FAKE UNDER ASAN. */
#define RA8_FAKE_UNDER_ASAN (1)
#elifdef __has_feature
#if __has_feature(address_sanitizer)
/** @brief RA8 FAKE UNDER ASAN. */
#define RA8_FAKE_UNDER_ASAN (1)
#endif
#endif

/**
 * @enum ra8_fake_region_id_t
 * @brief Indices into ::s_ra8_fake_regions.
 */
typedef enum : uint8_t {
  k_ra8_fake_region_peri       = 0, /**< Peripheral bus 0x40000000.             */
  k_ra8_fake_region_core       = 1, /**< ARM SCB / MPU 0xE0000000.              */
  k_ra8_fake_region_mram       = 2, /**< MRAM calibration 0x02C00000.           */
  k_ra8_fake_region_sram       = 3, /**< SRAM 0x22000000.                       */
  k_ra8_fake_region_sdram      = 4, /**< SDRAM framebuffers 0x68000000.         */
  k_ra8_fake_region_code_mram  = 5, /**< Code MRAM 0x02000000 (1 MiB).          */
  k_ra8_fake_region_extra_mram = 6, /**< Extra-MRAM OTP window 0x02E07000 head. */
  k_ra8_fake_region_peri_ns    = 7, /**< NS alias of peripheral bus 0x50000000. */
  k_ra8_fake_region_count      = 8, /**< RA8 fake region count.                 */
} ra8_fake_region_id_t;

/**
 * @struct ra8_fake_region_t
 * @brief Backing memory region descriptor.
 */
typedef struct {
  uintptr_t base;            /**< Virtual base address to map.                    */
  size_t    size;            /**< Bytes to map.                                   */
  bool      asan_shadow_gap; /**< True if base lands in ASan's shadow gap (#193). */
} ra8_fake_region_t;

/**
 * @brief All memory windows that have to exist on the host.
 *
 * Sizes are rounded up to whole megabytes so we cover every enum
 * base plus any future driver without revisiting this list.
 */
enum : size_t {
  k_ra8_fake_peri_size       = 0x00800000U, /**< 8 MiB over the peri bus.                  */
  k_ra8_fake_core_size       = 0x00100000U, /**< 1 MiB SCB/MPU window.                     */
  k_ra8_fake_mram_size       = 0x00100000U, /**< 1 MiB MRAM span.                          */
  k_ra8_fake_sram_size       = 0x00200000U, /**< 2 MiB SRAM span.                          */
  k_ra8_fake_sdram_size      = 0x00100000U, /**< 1 MiB SDRAM head.                         */
  k_ra8_fake_code_mram_size  = 0x00100000U, /**< 1 MiB code MRAM (HUM 59.1 p 3543).        */
  k_ra8_fake_extra_mram_size = 0x00011000U, /**< Covers the OTP window through 0x02E18000. */
};

enum : uintptr_t {
  k_ra8_fake_peri_base       = 0x40000000UL, /**< RA8 fake peri base.                         */
  k_ra8_fake_core_base       = 0xE0000000UL, /**< RA8 fake core base.                         */
  k_ra8_fake_mram_base       = 0x02C00000UL, /**< RA8 fake MRAM base.                         */
  k_ra8_fake_sram_base       = 0x22000000UL, /**< RA8 fake SRAM base.                         */
  k_ra8_fake_sdram_base      = 0x68000000UL, /**< RA8 fake SDRAM base.                        */
  k_ra8_fake_code_mram_base  = 0x02000000UL, /**< Code MRAM (matches k_ra8_flash_code_start). */
  k_ra8_fake_extra_mram_base = 0x02E07000UL, /**< Page-aligned head of the extra-MRAM OTP
                     * window (covers k_ra8_flash_extra_start
                     * 0x02E07600, HUM Ch 59.7.4.5 Table 59.15 p 3592). */
  k_ra8_fake_peri_ns_base    = 0x50000000UL, /**< Non-Secure alias of peri bus. */
};

static const ra8_fake_region_t s_ra8_fake_regions[k_ra8_fake_region_count] = {
  {.base = (uintptr_t)k_ra8_fake_peri_base, .size = (size_t)k_ra8_fake_peri_size},
  /* The SCB/MPU window at 0xE0000000 falls inside AddressSanitizer's
     * shadow gap; it is the only region that cannot be MAP_FIXED under ASan. */
  {.base            = (uintptr_t)k_ra8_fake_core_base,
   .size            = (size_t)k_ra8_fake_core_size,
   .asan_shadow_gap = true},
  {.base = (uintptr_t)k_ra8_fake_mram_base, .size = (size_t)k_ra8_fake_mram_size},
  {.base = (uintptr_t)k_ra8_fake_sram_base, .size = (size_t)k_ra8_fake_sram_size},
  {.base = (uintptr_t)k_ra8_fake_sdram_base, .size = (size_t)k_ra8_fake_sdram_size},
  {.base = (uintptr_t)k_ra8_fake_code_mram_base, .size = (size_t)k_ra8_fake_code_mram_size},
  {.base = (uintptr_t)k_ra8_fake_extra_mram_base, .size = (size_t)k_ra8_fake_extra_mram_size},
  {.base = (uintptr_t)k_ra8_fake_peri_ns_base, .size = (size_t)k_ra8_fake_peri_size},
};

static uint8_t s_ra8_fake_mapped = 0U;

/**
 * @brief Decide whether a backing region may be mapped in this build.
 *
 * @details
 * All regions are mappable in the ordinary host unit-test build. Under
 * AddressSanitizer (::RA8_FAKE_UNDER_ASAN), the single window flagged
 * ``asan_shadow_gap`` -- the 0xE0000000 SCB/MPU region -- lands in ASan's
 * reserved shadow gap, where a ``MAP_FIXED`` (and the subsequent memset,
 * intercepted by ASan) aborts the process before ``main()`` (issue #193).
 * Only the peripheral-bus fuzz harnesses that link this mock exercise the
 * host MMIO store, and none of them touch SCB/MPU, so skipping exactly that
 * region under ASan is safe.
 *
 * @param[in] asan_shadow_gap ::ra8_fake_region_t::asan_shadow_gap of the
 * region.
 *
 * @return Whether the region should be mapped / cleared.
 * @retval true  Region is mappable (always, unless ASan + shadow-gap region).
 * @retval false Region is the shadow-gap window and this is an ASan build.
 *
 * @pre The caller iterates ::s_ra8_fake_regions in index order.
 * @pre The build's ASan configuration remains fixed for the process.
 * @post The return value is stable for a given build configuration.
 * @post No mapping or fake-region state is changed.
 *
 * @note Thread-safe: reads no shared mutable state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_region_mappable(bool asan_shadow_gap)
{
#ifdef RA8_FAKE_UNDER_ASAN
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
 * @pre Every configured region has a non-overflowing half-open range.
 * @post Returns only if no mappable window overlaps [image_lo, image_hi).
 * @post All mapping and model state remains unchanged on return.
 * @note Not thread-safe; runs once during the mmap installer.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_overlap(uintptr_t image_lo, uintptr_t image_hi)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_region_count; ++i) {
    const ra8_fake_region_t region = s_ra8_fake_regions[i];
    if (!internal_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    if ((region.base < image_hi) && (image_lo < (region.base + region.size))) {
      internal_report_overlap(region.base, region.size, image_lo, image_hi);
      abort();
    }
  }
}

/**
 * @brief MAP_FIXED-map and zero every mappable backing window.
 * @details Creates each host register or memory window at its target address,
 * verifies the exact mapping result, and clears newly mapped storage.
 * @return None.
 * @pre The backing windows do not overlap the loaded image/heap.
 * @pre ::internal_check_overlap has accepted the current process layout.
 * @post Every mappable window is mapped read/write and zero-filled, or aborts.
 * @post Shadow-gap windows skipped under ASan remain untouched.
 * @note Not thread-safe; runs once during the mmap installer.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_map_regions(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_region_count; ++i) {
    const ra8_fake_region_t region = s_ra8_fake_regions[i];
    if (!internal_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    void* p = mmap((void*)region.base,
                   region.size,
                   PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                   -1,
                   0);
    if (p == MAP_FAILED || (uintptr_t)p != region.base) {
      internal_report_map_failure(region.base, region.size);
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
 * shadow-gap region is skipped up front (see ::internal_region_mappable).
 *
 * Before mapping, the constructor disables ASLR and re-execs itself once.
 * Fixed-address windows (``MAP_FIXED``) and ASLR are fundamentally
 * incompatible: with randomization on, the kernel can place the C heap, a
 * thread stack, or an mmap arena at an address one of these windows clobbers
 * (or grow the heap into one), silently corrupting glibc's top chunk. That is
 * the root of the rare ``Subprocess aborted`` (``malloc assertion failure``)
 * flake seen when the suite runs many allocation-heavy tests. Re-execing with
 * ``ADDR_NO_RANDOMIZE`` makes every host-test process use a deterministic,
 * collision-free layout. The ``RA8_FAKE_NO_ASLR`` env sentinel bounds it to a
 * single re-exec even if ``personality()`` is refused, and a failed ``execv``
 * falls through to map best-effort rather than blocking the run.
 *
 * The ``.init_array`` calling convention passes ``(argc, argv, envp)`` to
 * constructors, which is what the re-exec needs.
 * @param[in] argc Process argument count supplied by the constructor ABI.
 * @param[in] argv Process argument vector reused for the bounded re-exec.
 * @param[in] envp Process environment vector supplied by the constructor ABI.
 * @return Nothing.
 * @pre @p argv is a valid null-terminated process argument vector.
 * @pre The configured fake windows do not wrap the host address space.
 * @post Success leaves every mappable window installed and zero-filled.
 * @post ::s_ra8_fake_mapped is set after mapping completes.
 * @note A failed `execv` falls through to best-effort mapping in this process.
 * @since 0.1.0
 */
RA8_INTERNAL [[gnu::constructor]] static void internal_install(int argc, char** argv, char** envp)
{
  (void)argc;
  (void)envp;
  if (s_ra8_fake_mapped != 0U) {
    return;
  }
  if (getenv("RA8_FAKE_NO_ASLR") == nullptr) {
    (void)personality(ADDR_NO_RANDOMIZE);
    (void)setenv("RA8_FAKE_NO_ASLR", "1", 1);
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
  internal_check_overlap(image_lo, image_hi);
  internal_map_regions();
  s_ra8_fake_mapped = 1U;
}

void ra8_fake_mmap_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_region_count; ++i) {
    const ra8_fake_region_t region = s_ra8_fake_regions[i];
    if (!internal_region_mappable(region.asan_shadow_gap)) {
      continue;
    }
    (void)memset((void*)region.base, 0, region.size);
  }
}

#else
/* On-target build: this TU contributes nothing. */
typedef int ra8_fake_mmap_placeholder_t;
#endif /* RA8_OFF_TARGET */
