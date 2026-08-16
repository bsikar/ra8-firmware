/**
 * @file emu_memmap.c
 * @brief Emulated memory map implementation (see emu_memmap.h)
 *
 * @details Keeps SRAM, SDRAM and OSPI bytes in three workspace-owned host
 * mappings. Every attached engine binds the Secure aperture and its IDAU
 * bit[28] Non-secure alias onto the SAME host pages with uc_mem_map_ptr, so
 * Secure/Non-secure coherence and cpu0/cpu1 coherence are structural: a guest
 * store stays on the translator's fast path and costs nothing beyond the store
 * itself. All other ordinary regions remain private Unicorn mappings.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_memmap.h"

#include <errno.h>
#include <sys/mman.h>

#include "emu_console.h"
#include "emu_host_io_internal.h"
#include "emu_mmio.h"

#ifndef MAP_ANONYMOUS
/**
 * @def MAP_ANONYMOUS
 * @brief Portable spelling of the anonymous-mapping flag.
 * @details Build-configuration alias only: macOS SDKs that predate the
 * MAP_ANONYMOUS spelling supply the same flag as MAP_ANON. Defined only when
 * the platform header did not, so a host that has the standard name keeps it.
 * @note Not a numeric constant of this module's own; it names a platform flag,
 * which is why it is a macro rather than a typed enum.
 * @since 0.1.0
 */
#define MAP_ANONYMOUS MAP_ANON
#endif

/* RA8D2 memory map (EK board) -- from the linker script / HUM R01UH1065EJ. */
static const mem_region_t s_regions[] = {
  {"ITCM", 0x00000000UL, 0x00010000UL},     /* 64 KiB tightly-coupled code */
  {"MRAM", 0x02000000UL, 0x00100000UL},     /* 1 MiB code flash + vectors  */
  {"TRIM", 0x02C1E000UL, 0x00001000UL},     /* MRAM factory-trim page: the
                                             * on-chip temperature-sensor
                                             * calibration cells (TSCDR/TSCDR2 at
                                             * 0x02C1EDA0) live here. Mapped +
                                             * seeded (see k_tsn_cal_*) so
                                             * ra8_tsn reads a real two-point pair
                                             * instead of bus-faulting on the
                                             * previously-unmapped read. */
  {"OFS_CFG", 0x02C9F000UL, 0x00001000UL},  /* Option-setting configuration words
                                             * (OFS0/1/2/3, SAS, BPS, selectors) --
                                             * HUM Ch 7 Figure 7.1 p 279, secure alias
                                             * 0x02C9_F0xx. Replaces the phantom
                                             * 0x0300A000 OFS block (#391). */
  {"OFS_OTP", 0x02E07000UL, 0x00011000UL},  /* Option-setting OTP area + the extra-MRAM
                                             * Program window: FSBL, code-cert, GPOTP,
                                             * PBPS, POFSPS, REVOKE, ZHUK, anti-rollback
                                             * (HUM Ch 59.7.4.5 Table 59.15 p 3592,
                                             * 0x02E0_7600..0x02E1_79F0). Readable on
                                             * silicon, so mapped; the MACI model
                                             * (board_periph_mram.c) lands accepted
                                             * Program payloads here. */
  {"DTCM", 0x20000000UL, 0x00010000UL},     /* 64 KiB tightly-coupled data */
  {"SRAM", 0x22000000UL, 0x001D4000UL},     /* On-chip SRAM: CPU0 bank + shared mailbox +
                                             * CPU1 bank. The extent is BENCH-MEASURED, not
                                             * assumed: over J-Link on an EK-RA8D2, words at
                                             * 0x221D0000, 0x221D2000 and 0x221D3FFC read and
                                             * write normally, while 0x221D4000 answers
                                             * "Could not read memory" and 0x221E0000 /
                                             * 0x22200000 answer "Failed to write memory".
                                             * Distinct values written at 0x22030000 and
                                             * 0x221B0000 survive independently, so the top
                                             * of the window is real memory and not an alias
                                             * of the bottom.
                                             *
                                             * This window used to be a deliberately wide
                                             * 4 MiB "so every dual-core example maps
                                             * cleanly", which made 2.2 MB of NONEXISTENT
                                             * address space silently writable here. That is
                                             * the exact defect shape that let
                                             * ota_ab_orchestration hold hw_validated while
                                             * its A/B slots went to an address the silicon
                                             * does not decode (#397): a model permissive
                                             * where the hardware is not cannot fail, so it
                                             * validates nothing. */
  {"NS_SRAM2", 0x32100000UL, 0x00080000UL}, /* SRAM2 Non-secure alias (bit[28]=1): the
                                             * TrustZone NS image run region. The Secure
                                             * boot copies the NS image here then BLXNS-es
                                             * to it; mapping it lets two-image TZ apps
                                             * (src/app) run their NS world in ra8_emulator. */
  /* No DATA_FLASH region. 0x27000000 was carried in every app linker script as a
   * 16 KiB "data flash (EEPROM emulation)" MEMORY declaration inherited from
   * other RA parts, and ra8_emulator used to map it as plain RAM -- so writes there
   * succeeded in the emulator. They do not on this silicon: a J-Link
   * `w4 0x27000000` on an EK-RA8D2 answers "Failed to write memory". That gap
   * is what #397 found under ra8_io_mram_demo. #397 removed the phantom region
   * from the linker scripts and repointed the extra-MRAM constant at the real
   * option-setting window (OFS_OTP above, 0x02E0_7600); 0x27000000 stays
   * unmapped so any straggler write still faults exactly where the bench does. */
  {"SDRAM", 0x68000000UL, 0x04000000UL},    /* 64 MiB external SDRAM (Secure
                                             * physical view). Host-backed so the
                                             * NS alias below mirrors the bytes:
                                             * the GLCDC model scans the
                                             * framebuffer from here while the NS
                                             * world draws it through 0x78000000. */
  {"NS_SDRAM", 0x78000000UL, 0x04000000UL}, /* External SDRAM Non-secure alias
                                             * (IDAU bit[28]=1): the SAME 64 MiB
                                             * array as SDRAM. The Non-secure
                                             * e-reader writes framebuffer pixels
                                             * through this alias; sharing one
                                             * host mapping makes them visible to
                                             * the Secure GLCDC read at
                                             * 0x68000000. */
  {"OSPI", 0x80000000UL, 0x04000000UL},     /* 64 MiB Octo-SPI XIP flash (Secure
                                             * physical view). Memory-mapped and
                                             * executable: an NS reader image runs
                                             * execute-in-place from here. Host-
                                             * backed and shared with NS_OSPI so
                                             * the alias below mirrors the bytes. */
  {"NS_OSPI", 0x90000000UL, 0x04000000UL},  /* OSPI XIP Non-secure alias
                                             * (IDAU bit[28]=1): the SAME 64 MiB
                                             * flash array as OSPI. The XIP-linked
                                             * NS image's .text/.rodata live at
                                             * 0x90000000; the M85 fetches NS
                                             * instructions from this alias. */
  {"PPB", 0xE0000000UL, 0x00100000UL},      /* ARM private peripheral bus */
};

/* Octo-SPI (XSPI) execute-in-place flash window. The XSPI controller's register
 * command-engine is modelled in peripheral space (board_periph_xspi.c, base
 * 0x40268000); this is the SEPARATE memory-mapped, executable view of the 64 MiB
 * flash array the controller exposes once the firmware enters XIP mode. The
 * Secure physical window sits at 0x80000000; its TrustZone Non-secure alias
 * (IDAU bit[28]=1) sits at 0x90000000. Both views address the same flash array,
 * so ra8_emulator binds them to a single host mapping (mirrors the SRAM/NS_SRAM2
 * alias pair). A Non-secure reader image linked for XIP places .text/.rodata at
 * 0x90000000 and the CPU fetches Non-secure instructions from there. */
typedef enum : uint64_t {
  k_ospi_xip_base = 0x80000000UL, /**< OSPI XIP window: Secure physical base.      */
  k_ospi_ns_base  = 0x90000000UL, /**< OSPI XIP window: NS alias (IDAU bit[28]=1). */
} ospi_xip_map_t;

/* On-chip temperature-sensor factory calibration. The TSN two-point trim words
 * TSCDR (code at the high reference) and TSCDR2 (code at the low reference) live
 * in the MRAM factory-trim region at 0x02C1EDA0 (HUM Ch 55.2.2 p 3498-3499).
 * Real silicon ships them factory-programmed; ra8_emulator's blank map left the
 * region unreadable, so ra8_tsn_convert_to_milli_c bus-faulted (UC_ERR_READ_-
 * UNMAPPED) reading them and adc_diag_tsn_demo aborted. Seed a deterministic,
 * plausible positive-slope pair (TSCDR @ +125C > TSCDR2 @ -40C) into the mapped
 * TRIM page after mem-map. Paired with the ADC temperature code the ADC model
 * reports (k_adc_temp_code = 1800 in board_periph_adc.c), the two-point math
 * yields ~26 degC. This is factory-constant data, not a masked poll. */
typedef enum : uint32_t {
  k_tsn_cal_addr   = 0x02C1EDA0U, /**< TSCDR (+0x00), TSCDR2 (+0x04).        */
  k_tsn_cal_tscdr  = 3000U,       /**< 12-bit calibration code at +125 degC. */
  k_tsn_cal_tscdr2 = 1000U,       /**< 12-bit calibration code at -40 degC.  */
} tsn_cal_seed_t;

/** @brief Identity of each shared aperture within a workspace. */
typedef enum : size_t {
  k_backing_sram  = 0U, /**< On-chip SRAM aperture index.   */
  k_backing_sdram = 1U, /**< External SDRAM aperture index. */
  k_backing_ospi  = 2U, /**< OSPI XIP aperture index.       */
} backing_index_t;

/** @brief Exact logical extents of the three shared apertures. */
typedef enum : uint64_t {
  k_sram_size       = 0x001D4000UL, /**< Exact on-chip SRAM extent.             */
  k_sdram_size      = 0x04000000UL, /**< Exact external SDRAM extent.           */
  k_ospi_size       = 0x04000000UL, /**< Exact OSPI XIP extent.                 */
  k_ns_sram2_offset = 0x00100000UL, /**< SRAM offset the SRAM2 alias starts at. */
  k_ns_sram2_size   = 0x00080000UL, /**< SRAM2 alias extent.                    */
  /** @brief Total logical bytes the three shared apertures span. */
  k_logical_bytes = k_sram_size + k_sdram_size + k_ospi_size,
} backing_geometry_t;

/** @brief Fixed arguments for an anonymous host aperture mapping. */
typedef enum : int {
  k_mmap_no_fd     = -1, /**< Anonymous mappings are backed by no descriptor. */
  k_mmap_no_offset = 0,  /**< Anonymous mappings start at offset zero.        */
} mmap_argument_t;

static_assert((k_ns_sram2_offset + k_ns_sram2_size) <= k_sram_size,
              "the SRAM2 Non-secure alias must lie inside the on-chip SRAM aperture");
static_assert(k_logical_bytes == (k_sram_size + k_sdram_size + k_ospi_size),
              "the reported logical geometry must be the sum of the three apertures");

/**
 * @brief One guest window that resolves onto a shared host aperture.
 * @details Both the Secure physical view and its IDAU bit[28] Non-secure alias
 * appear here, pointing at the same aperture, which is what makes the two views
 * one state once the engine binds them with uc_mem_map_ptr.
 */
typedef struct {
  uint64_t base;    /**< Guest base address of this window.         */
  size_t   backing; /**< Aperture the window resolves onto.         */
  uint64_t offset;  /**< Byte offset of the window in the aperture. */
} aliased_window_t;

/**
 * @brief Construct one exact lifecycle result.
 * @details Construct one exact lifecycle result; every lifecycle entry point
 * reports the same immutable aperture geometry alongside its status so a caller
 * never has to correlate two calls.
 * @param[in] status Status value published by the operation.
 * @param[in] os_error Host error code captured on failure, or zero.
 * @return The result produced by the emu memmap model.
 * @retval value The operation-specific result value.
 * @pre Arguments satisfy the ranges documented for result.
 * @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the returned value.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_memmap_result_t internal_result(emu_memmap_status_t status, int os_error)
{
  return (emu_memmap_result_t){.status                = status,
                               .logical_backing_bytes = k_logical_bytes,
                               .os_error              = os_error};
}

/**
 * @brief Resolve a guest region base to its shared host aperture address.
 * @details Looks the region up in the aliased-window table and returns the host
 * address the window starts at, after proving the whole window fits inside the
 * aperture. Any region that is not an aliased window returns nullptr and is
 * mapped as ordinary private Unicorn memory.
 * @param[in] workspace Open workspace owning the aperture mappings.
 * @param[in] base Guest base address of the region being mapped.
 * @param[in] size Guest byte length of the region being mapped.
 * @return The host address backing the window.
 * @retval nullptr The region is not shared, or it would overrun its aperture.
 * @pre @p workspace is open and every aperture mapping is acquired.
 * @pre The call executes on the emulator's single owning thread.
 * @post No workspace, mapping, or engine state changes.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t*
internal_window_host(const emu_memmap_workspace_t* workspace, uint64_t base, uint64_t size)
{
  /** @brief Every guest window bound to shared host pages, Secure and NS alike. */
  static const aliased_window_t k_aliased_windows[] = {
    {(uint64_t)k_sram_base, k_backing_sram, 0U},
    {(uint64_t)k_ns_sram2_base, k_backing_sram, k_ns_sram2_offset},
    {(uint64_t)k_sdram_base, k_backing_sdram, 0U},
    {(uint64_t)k_ns_sdram_base, k_backing_sdram, 0U},
    {(uint64_t)k_ospi_xip_base, k_backing_ospi, 0U},
    {(uint64_t)k_ospi_ns_base, k_backing_ospi, 0U},
  };
  for (size_t index = 0U; index < (sizeof(k_aliased_windows) / sizeof(k_aliased_windows[0]));
       index++) {
    const aliased_window_t* const window = &k_aliased_windows[index];
    if (window->base != base) {
      continue;
    }
    const emu_memmap_backing_t* const backing = &workspace->backings[window->backing];
    if ((backing->host == nullptr) || (size > backing->size) ||
        (window->offset > (backing->size - size))) {
      return nullptr;
    }
    return &backing->host[window->offset];
  }
  return nullptr;
}

/**
 * @brief Acquire one zero-filled, lazily-committed host aperture mapping.
 * @details Uses an anonymous mapping rather than an allocator: the pages are
 * page-aligned (which uc_mem_map_ptr requires), start zeroed, and cost resident
 * memory only once the firmware touches them, so mapping 130 MiB of guest
 * memory does not cost 130 MiB of host memory.
 * @param[in] size Exact aperture byte length.
 * @param[out] backing Receives the acquired mapping.
 * @return Whether the aperture mapping was acquired.
 * @retval true @p backing holds a live page-aligned host mapping.
 * @retval false The host refused the mapping; errno describes why.
 * @pre @p backing is non-null.
 * @pre @p size is a non-zero multiple of ::k_page_size.
 * @post Success leaves every aperture byte zero and no page resident.
 * @post Failure leaves @p backing untouched.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_backing_open(uint64_t size, emu_memmap_backing_t* backing)
{
  void* const host = mmap(nullptr,
                          (size_t)size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          (int)k_mmap_no_fd,
                          (off_t)k_mmap_no_offset);
  if (host == MAP_FAILED) {
    return false;
  }
  *backing = (emu_memmap_backing_t){.host = (uint8_t*)host, .size = size};
  return true;
}

/**
 * @brief Release every acquired aperture mapping in a workspace.
 * @details Unmaps each live aperture and clears its record, so a partially
 * acquired workspace and a fully closed one are released by the same code.
 * @param[in,out] workspace Caller-owned workspace whose apertures are released.
 * @pre @p workspace is non-null.
 * @pre No engine binding still references these host pages.
 * @post Every backing pointer is nullptr and every size is zero.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_backings_close(emu_memmap_workspace_t* workspace)
{
  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    emu_memmap_backing_t* const backing = &workspace->backings[index];
    if (backing->host != nullptr) {
      (void)munmap(backing->host, (size_t)backing->size);
    }
    *backing = (emu_memmap_backing_t){};
  }
}

/**
 * @brief Seed deterministic factory TSN calibration words.
 * @details Writes the two-point trim pair into the engine's TRIM page (see
 * ::tsn_cal_seed_t). The page is ordinary private Unicorn memory, so the seed
 * is applied per engine at attach time.
 * @param[in,out] uc Unicorn engine whose TRIM page receives the seed.
 * @pre The TRIM region covering ::k_tsn_cal_addr is mapped on @p uc.
 * @pre The call executes on the emulator's single owning thread.
 * @post @p uc holds TSCDR at ::k_tsn_cal_addr and TSCDR2 at the next word.
 * @post No other engine or workspace state changes.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seed_tsn(uc_engine* uc)
{
  const uint32_t tscdr  = (uint32_t)k_tsn_cal_tscdr;
  const uint32_t tscdr2 = (uint32_t)k_tsn_cal_tscdr2;
  (void)emu_mem_write(uc, (uint64_t)k_tsn_cal_addr, &tscdr, sizeof(tscdr));
  (void)emu_mem_write(uc, (uint64_t)k_tsn_cal_addr + sizeof(tscdr), &tscdr2, sizeof(tscdr2));
}

/**
 * @brief Map the Secure peripheral window and its IDAU bit[28] NS alias.
 * @details Installs the modelled peripheral callbacks over 0x40000000 and over
 * the Non-secure alias at 0x50000000, so an NS image reaches the same models.
 * @param[in,out] uc Unicorn engine receiving the MMIO windows.
 * @return Whether both windows were installed.
 * @retval true Both the Secure and Non-secure peripheral windows are live.
 * @retval false Unicorn refused a window; a diagnostic was written to stderr.
 * @pre @p uc is a fresh engine with no peripheral window installed.
 * @pre The call executes on the emulator's single owning thread.
 * @post Success routes every peripheral access through the board models.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_map_periph_mmio(uc_engine* uc)
{
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)priv_emu_io_errf("mmio_map failed\n");
    return false;
  }
  /* IDAU bit[28]=1 Non-secure peripheral alias (0x50000000): the SAME silicon
   * registers as 0x40000000, reached by TrustZone Non-secure code (an NS image
   * built with RA8_PERIPH_NS_ALIAS -- MSTP at 0x5020_3000, USBFS at
   * 0x5025_0000, USBHS at 0x5035_1000 -- or the NS-side IPC ping-pong at
   * 0x5002_0000). The hooks rebuild the absolute address as k_periph_base +
   * window-relative offset, so a 0x50020000 access (offset 0x20000) dispatches
   * to 0x40020000 identically to a Secure one -- the exact mapping the cpu1
   * engine already carries (see cpu1_engine_init). */
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)priv_emu_io_errf("mmio_map (NS alias) failed\n");
    return false;
  }
  return true;
}

/**
 * @brief Map every guest region into one engine, sharing the apertures.
 * @details Each region either resolves to a shared host aperture -- and is
 * bound with uc_mem_map_ptr so it and its alias are one state -- or becomes an
 * ordinary private Unicorn mapping. Because the shared pages already hold the
 * workspace's bytes, a freshly attached engine observes everything written
 * before it existed with no replay step.
 * @param[in] workspace Open workspace owning the aperture mappings.
 * @param[in,out] uc Unicorn engine receiving the regions.
 * @return Whether every region was mapped.
 * @retval true The engine carries the complete RA8D2 region map.
 * @retval false A region map failed; a diagnostic was written to stderr.
 * @pre @p workspace is open with every aperture acquired.
 * @pre @p uc has no guest memory mappings.
 * @post Success makes the six aliased windows share the workspace pages.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_map_regions(const emu_memmap_workspace_t* workspace,
                                              uc_engine*                    uc)
{
  for (size_t index = 0U; index < (sizeof(s_regions) / sizeof(s_regions[0])); index++) {
    const mem_region_t* const region = &s_regions[index];
    uint8_t* const            host   = internal_window_host(workspace, region->base, region->size);
    const uc_err              mapped =
      (host != nullptr) ? uc_mem_map_ptr(uc, region->base, (size_t)region->size, UC_PROT_ALL, host)
                        : uc_mem_map(uc, region->base, (size_t)region->size, UC_PROT_ALL);
    if (mapped != UC_ERR_OK) {
      (void)priv_emu_io_errf("map %s @0x%08llX failed\n",
                             region->name,
                             (unsigned long long)region->base);
      return false;
    }
  }
  return true;
}

emu_memmap_result_t emu_memmap_requirements(void)
{
  return internal_result(k_emu_memmap_ok, 0);
}

emu_memmap_result_t emu_memmap_open(emu_memmap_workspace_t* workspace)
{
  if ((workspace == nullptr) || workspace->open) {
    return internal_result(k_emu_memmap_invalid, 0);
  }
  emu_memmap_workspace_t candidate                         = {};
  const uint64_t         sizes[k_emu_memmap_backing_count] = {
    [k_backing_sram]  = k_sram_size,
    [k_backing_sdram] = k_sdram_size,
    [k_backing_ospi]  = k_ospi_size,
  };

  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    if (!internal_backing_open(sizes[index], &candidate.backings[index])) {
      const int failure = errno;
      internal_backings_close(&candidate);
      return internal_result(k_emu_memmap_backing, failure);
    }
  }
  candidate.open = true;
  *workspace     = candidate;
  return internal_result(k_emu_memmap_ok, 0);
}

emu_memmap_result_t emu_memmap_attach(emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  if ((workspace == nullptr) || !workspace->open || (uc == nullptr)) {
    return internal_result(k_emu_memmap_invalid, 0);
  }
  emu_memmap_binding_t* binding = nullptr;
  for (size_t index = 0U; index < k_emu_memmap_binding_count; index++) {
    if (!workspace->bindings[index].active) {
      binding = &workspace->bindings[index];
      break;
    }
  }
  if (binding == nullptr) {
    return internal_result(k_emu_memmap_invalid, 0);
  }
  if (!internal_map_regions(workspace, uc) || !internal_map_periph_mmio(uc)) {
    return internal_result(k_emu_memmap_unicorn, 0);
  }
  *binding = (emu_memmap_binding_t){.uc = uc, .active = true};
  internal_seed_tsn(uc);
  return internal_result(k_emu_memmap_ok, 0);
}

bool emu_memmap_detach(emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  if ((workspace == nullptr) || (uc == nullptr)) {
    return false;
  }
  for (size_t index = 0U; index < k_emu_memmap_binding_count; index++) {
    if (workspace->bindings[index].active && (workspace->bindings[index].uc == uc)) {
      workspace->bindings[index] = (emu_memmap_binding_t){};
      return true;
    }
  }
  return false;
}

bool emu_memmap_close(emu_memmap_workspace_t* workspace)
{
  if ((workspace == nullptr) || !workspace->open) {
    return true;
  }
  for (size_t index = 0U; index < k_emu_memmap_binding_count; index++) {
    if (workspace->bindings[index].active) {
      return false;
    }
  }
  internal_backings_close(workspace);
  *workspace = (emu_memmap_workspace_t){};
  return true;
}

/** @brief Implementation of `emu_memmap_regions()` -- static table access. */
const mem_region_t* emu_memmap_regions(uint32_t* count)
{
  *count = (uint32_t)(sizeof(s_regions) / sizeof(s_regions[0]));
  return s_regions;
}

/** @brief Implementation of `emu_memmap_mram_base()` -- named MRAM lookup. */
uint64_t emu_memmap_mram_base(void)
{
  return s_regions[1].base;
}
