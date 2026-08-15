/**
 * @file emu_memmap.c
 * @brief Emulated memory map implementation (see emu_memmap.h)
 *
 * @details Keeps SRAM, SDRAM, and OSPI bytes in authoritative unlinked sparse
 * descriptors. Unicorn owns opaque guest pages; each successful publication
 * mirrors the descriptor bytes into every active Secure/NS engine view. All
 * other ordinary regions remain private Unicorn mappings.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_memmap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu_console.h"
#include "emu_host_io_internal.h"
#include "emu_mmio.h"

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
  /* No DATA_FLASH region. 0x27000000 was carried in every app linker script
     * as a 16 KiB "data flash (EEPROM emulation)" MEMORY declaration inherited
     * from other RA parts, and ra8_emulator used to map it as plain RAM -- so
     * writes there succeeded in the emulator. They do not on this silicon: a
     * J-Link `w4 0x27000000` on an EK-RA8D2 answers "Failed to write memory".
     * That gap is what #397 found under ra8_io_mram_demo. #397 removed the
     * phantom region from the linker scripts and repointed the extra-MRAM
     * constant at the real option-setting window (OFS_OTP above, 0x02E0_7600);
     * 0x27000000 stays unmapped so any straggler write still faults exactly
     * where the bench does. */
  {"SDRAM", 0x68000000UL, 0x04000000UL},    /* 64 MiB external SDRAM (Secure
                     * physical view). Descriptor-backed so the
                     * NS alias below mirrors the bytes:
                     * the GLCDC model scans the
                     * framebuffer from here while the NS
                     * world draws it through 0x78000000. */
  {"NS_SDRAM", 0x78000000UL, 0x04000000UL}, /* External SDRAM Non-secure alias
                     * (IDAU bit[28]=1): the SAME 64 MiB
                     * state as SDRAM. The Non-secure
                     * e-reader writes framebuffer pixels
                     * through this alias; shared descriptor
                     * offsets make them visible to
                     * the Secure GLCDC read at
                     * 0x68000000. Must follow SDRAM in
                     * this table so the physical view is
                     * established first. */
  {"OSPI", 0x80000000UL, 0x04000000UL},     /* 64 MiB Octo-SPI XIP flash (Secure
                     * physical view). Memory-mapped and
                     * executable: an NS reader image runs
                     * execute-in-place from here. Descriptor-
                     * backed and shared with NS_OSPI so
                     * the alias below mirrors the bytes. */
  {"NS_OSPI", 0x90000000UL, 0x04000000UL},  /* OSPI XIP Non-secure alias
                                          * (IDAU bit[28]=1): the SAME 64 MiB
                                          * flash array as OSPI. The XIP-linked
                                          * NS image's .text/.rodata live at
                                          * 0x90000000; the M85 fetches NS
                                          * instructions from this alias. Must
                                          * follow OSPI in this table so the
                                          * physical view is established before
                                          * its alias. */
  {"PPB", 0xE0000000UL, 0x00100000UL},      /* ARM private peripheral bus */
};

/* Octo-SPI (XSPI) execute-in-place flash window. The XSPI controller's register
 * command-engine is modelled in peripheral space (board_periph_xspi.c, base
 * 0x40268000); this is the SEPARATE memory-mapped, executable view of the 64
 * MiB flash array the controller exposes once the firmware enters XIP mode. The
 * Secure physical window sits at 0x80000000; its TrustZone Non-secure alias
 * (IDAU bit[28]=1) sits at 0x90000000. Both views address the same flash array,
 * so ra8_emulator resolves them to one sparse descriptor (as for SRAM2). A
 * Non-secure reader image linked for XIP places
 * .text/.rodata at 0x90000000 and the CPU fetches Non-secure instructions from
 * there. */
typedef enum : uint64_t {
  k_ospi_xip_base = 0x80000000UL, /**< OSPI XIP window: Secure physical base.      */
  k_ospi_ns_base  = 0x90000000UL, /**< OSPI XIP window: NS alias (IDAU bit[28]=1). */
  k_ospi_xip_size = 0x04000000UL, /**< 64 MiB execute-in-place flash array.        */
} ospi_xip_map_t;

/* On-chip temperature-sensor factory calibration. The TSN two-point trim words
 * TSCDR (code at the high reference) and TSCDR2 (code at the low reference)
 * live in the MRAM factory-trim region at 0x02C1EDA0 (HUM Ch 55.2.2 p
 * 3498-3499). Real silicon ships them factory-programmed; ra8_emulator's blank
 * map left the region unreadable, so ra8_tsn_convert_to_milli_c bus-faulted
 * (UC_ERR_READ_- UNMAPPED) reading them and adc_diag_tsn_demo aborted. Seed a
 * deterministic, plausible positive-slope pair (TSCDR @ +125C > TSCDR2 @ -40C)
 * into the mapped TRIM page after mem-map. Paired with the ADC temperature code
 * the ADC model reports (k_adc_temp_code = 1800 in board_periph_adc.c), the
 * two-point math yields ~26 degC. This is factory-constant data, not a masked
 * poll. */
typedef enum : uint32_t {
  k_tsn_cal_addr   = 0x02C1EDA0U, /**< TSCDR (+0x00), TSCDR2 (+0x04).        */
  k_tsn_cal_tscdr  = 3000U,       /**< 12-bit calibration code at +125 degC. */
  k_tsn_cal_tscdr2 = 1000U,       /**< 12-bit calibration code at -40 degC.  */
} tsn_cal_seed_t;

/** @brief Authoritative backing identities and exact logical sizes. */
typedef enum : uint64_t {
  k_backing_sram       = 0U,          /**< On-chip SRAM backing index.                 */
  k_backing_sdram      = 1U,          /**< External SDRAM backing index.               */
  k_backing_ospi       = 2U,          /**< OSPI XIP backing index.                     */
  k_sram_size          = 0x001D4000U, /**< Exact on-chip SRAM extent.                  */
  k_sdram_size         = 0x04000000U, /**< Exact external SDRAM extent.                */
  k_ospi_size          = 0x04000000U, /**< Exact OSPI XIP extent.                      */
  k_ns_sram_base_local = 0x32100000U, /**< SRAM2 Non-secure alias.                     */
  k_ns_sram_offset     = 0x00100000U, /**< SRAM physical offset of SRAM2.              */
  k_ns_sram_size       = 0x00080000U, /**< SRAM2 alias extent.                         */
  k_binding_address    = 0xFFFFF000U, /**< Host-only, guest-inaccessible binding page. */
  k_logical_bytes      = k_sram_size + k_sdram_size + k_ospi_size,
  /**< Total logical sparse backing bytes. */
} backing_geometry_t;

/** @brief Backing view resolved from one complete aliased access. */
typedef struct {
  size_t   index;  /**< Backing descriptor index. */
  uint64_t offset; /**< Physical byte offset.     */
} backing_view_t;

/**
 * @brief Whether two non-wrapping half-open address ranges overlap.
 * @details Whether two non-wrapping half-open address ranges overlap; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] first_base First base input used by the operation.
 * @param[in] first_size Bound for the first data.
 * @param[in] second_base Second base input used by the operation.
 * @param[in] second_size Bound for the second data.
 * @return The ranges overlap result produced by the emu memmap model.
 * @retval true The ranges overlap condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for ranges overlap. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_ranges_overlap(uint64_t first_base,
                                                 uint64_t first_size,
                                                 uint64_t second_base,
                                                 uint64_t second_size)
{
  return (first_base < (second_base + second_size)) && (second_base < (first_base + first_size));
}

/**
 * @brief Verify the protected host metadata page is outside every RA8 window.
 * @details Verify the protected host metadata page is outside every ra8 window; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @return The binding page available result produced by the emu memmap model.
 * @retval true The binding page available condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for binding page available. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_binding_page_available(void)
{
  for (size_t index = 0U; index < (sizeof(s_regions) / sizeof(s_regions[0])); index++) {
    if (internal_ranges_overlap(k_binding_address,
                                k_page_size,
                                s_regions[index].base,
                                s_regions[index].size)) {
      return false;
    }
  }
  const uint64_t ns_periph = (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit;
  return !internal_ranges_overlap(k_binding_address, k_page_size, k_periph_base, k_periph_size) &&
         !internal_ranges_overlap(k_binding_address, k_page_size, ns_periph, k_periph_size);
}

/**
 * @brief Construct one exact lifecycle result.
 * @details Construct one exact lifecycle result; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] status Status value consumed or published by the operation.
 * @param[in] supplied Supplied input used by the operation.
 * @param[in] os_error Storage receiving the host error code on failure.
 * @return The result result produced by the emu memmap model.
 * @retval value The operation-specific result value.
 * @pre Arguments satisfy the ranges documented for result. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_memmap_result_t
internal_result(emu_memmap_status_t status, size_t supplied, int os_error)
{
  return (emu_memmap_result_t){.status                 = status,
                               .required_scratch_bytes = k_emu_memmap_scratch_bytes,
                               .supplied_scratch_bytes = supplied,
                               .logical_backing_bytes  = k_logical_bytes,
                               .os_error               = os_error};
}

/**
 * @brief Resolve a complete Secure/NS aliased access to physical backing.
 * @details Resolve a complete secure/ns aliased access to physical backing; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] address Guest address involved in the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @return The resolve result produced by the emu memmap model.
 * @retval true The resolve condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for resolve. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_resolve(uint64_t address, size_t count, backing_view_t* view)
{
  if ((view == nullptr) || (count == 0U) || (address > (UINT64_MAX - count))) {
    return false;
  }
  const uint64_t end = address + count;
  if ((address >= (uint64_t)k_sram_base) && (end <= ((uint64_t)k_sram_base + k_sram_size))) {
    *view = (backing_view_t){.index = k_backing_sram, .offset = address - k_sram_base};
    return true;
  }
  if ((address >= k_ns_sram_base_local) && (end <= (k_ns_sram_base_local + k_ns_sram_size))) {
    *view = (backing_view_t){.index  = k_backing_sram,
                             .offset = k_ns_sram_offset + address - k_ns_sram_base_local};
    return true;
  }
  if ((address >= (uint64_t)k_sdram_base) && (end <= (uint64_t)k_sdram_end)) {
    *view = (backing_view_t){.index = k_backing_sdram, .offset = address - k_sdram_base};
    return true;
  }
  if ((address >= (uint64_t)k_ns_sdram_base) &&
      (end <= ((uint64_t)k_ns_sdram_base + k_sdram_size))) {
    *view = (backing_view_t){.index = k_backing_sdram, .offset = address - k_ns_sdram_base};
    return true;
  }
  if ((address >= (uint64_t)k_ospi_xip_base) &&
      (end <= ((uint64_t)k_ospi_xip_base + k_ospi_size))) {
    *view = (backing_view_t){.index = k_backing_ospi, .offset = address - k_ospi_xip_base};
    return true;
  }
  if ((address >= (uint64_t)k_ospi_ns_base) && (end <= ((uint64_t)k_ospi_ns_base + k_ospi_size))) {
    *view = (backing_view_t){.index = k_backing_ospi, .offset = address - k_ospi_ns_base};
    return true;
  }
  return false;
}

/**
 * @brief Create one unlinked sparse descriptor at an exact logical length.
 * @details Create one unlinked sparse descriptor at an exact logical length; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in,out] backing Descriptor-backed authoritative storage object.
 * @return The backing open result produced by the emu memmap model.
 * @retval true The backing open condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for backing open. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_backing_open(uint64_t size, emu_memmap_backing_t* backing)
{
  char path[] = "/tmp/ra8-emu-memory-XXXXXX";
  int  fd     = mkstemp(path);
  if (fd < 0) {
    return false;
  }
  if ((unlink(path) != 0) || (ftruncate(fd, (off_t)size) != 0)) {
    const int failure = errno;
    (void)close(fd);
    errno = failure;
    return false;
  }
  *backing = (emu_memmap_backing_t){.fd = fd, .size = size};
  return true;
}

/**
 * @brief Close every descriptor in a temporary or live workspace.
 * @details Close every descriptor in a temporary or live workspace; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] workspace Caller-owned workspace used by the operation.
 * @pre Arguments satisfy the ranges documented for backings close. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_backings_close(emu_memmap_workspace_t* workspace)
{
  for (size_t i = 0U; i < k_emu_memmap_backing_count; i++) {
    if (workspace->backings[i].fd >= 0) {
      (void)close(workspace->backings[i].fd);
      workspace->backings[i].fd = -1;
    }
  }
}

/**
 * @brief Return a committed page's global dirty-bit index.
 * @details Return a committed page's global dirty-bit index; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] backing Descriptor-backed authoritative storage object.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The dirty index result produced by the emu memmap model.
 * @retval value The operation-specific dirty index value.
 * @pre Arguments satisfy the ranges documented for dirty index. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_dirty_index(size_t backing, uint64_t offset)
{
  const size_t sram_pages  = (size_t)(k_sram_size / k_page_size);
  const size_t sdram_pages = (size_t)(k_sdram_size / k_page_size);
  size_t       base        = 0U;
  if (backing == k_backing_sdram) {
    base = sram_pages;
  } else if (backing == k_backing_ospi) {
    base = sram_pages + sdram_pages;
  }
  return base + (size_t)(offset / k_page_size);
}

/**
 * @brief Mark every page touched by one committed backing range.
 * @details Mark every page touched by one committed backing range; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] workspace Caller-owned workspace used by the operation.
 * @param[in] view Authoritative memory or presentation view accessed by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @pre Arguments satisfy the ranges documented for mark dirty. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_mark_dirty(emu_memmap_workspace_t* workspace, const backing_view_t* view, size_t count)
{
  const uint64_t last = view->offset + count - 1U;
  for (uint64_t offset = view->offset & ~((uint64_t)k_page_size - 1U); offset <= last;
       offset += k_page_size) {
    const size_t bit = internal_dirty_index(view->index, offset);
    workspace->dirty[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
  }
}

/**
 * @brief Whether one backing page has committed bytes.
 * @details Whether one backing page has committed bytes; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in] workspace Caller-owned workspace used by the operation.
 * @param[in] backing Descriptor-backed authoritative storage object.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The is dirty result produced by the emu memmap model.
 * @retval true The is dirty condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for is dirty. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_is_dirty(const emu_memmap_workspace_t* workspace, size_t backing, uint64_t offset)
{
  const size_t bit = internal_dirty_index(backing, offset);
  return (workspace->dirty[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

/**
 * @brief Write a physical backing range to every alias in one engine.
 * @details Write a physical backing range to every alias in one engine; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] binding Memory binding whose view and dirty state are processed.
 * @param[in] view Authoritative memory or presentation view accessed by the operation.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The write binding result produced by the emu memmap model.
 * @retval value The operation-specific write binding value.
 * @pre Arguments satisfy the ranges documented for write binding. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uc_err internal_write_binding(emu_memmap_binding_t* binding,
                                                  const backing_view_t* view,
                                                  const void*           bytes,
                                                  size_t                count)
{
  uint64_t primary = (uint64_t)k_sram_base + view->offset;
  uint64_t alias   = 0U;
  if (view->index == k_backing_sdram) {
    primary = (uint64_t)k_sdram_base + view->offset;
    alias   = (uint64_t)k_ns_sdram_base + view->offset;
  } else if (view->index == k_backing_ospi) {
    primary = (uint64_t)k_ospi_xip_base + view->offset;
    alias   = (uint64_t)k_ospi_ns_base + view->offset;
  } else if ((view->offset >= k_ns_sram_offset) &&
             ((view->offset + count) <= (k_ns_sram_offset + k_ns_sram_size))) {
    alias = k_ns_sram_base_local + view->offset - k_ns_sram_offset;
  }
  uc_err result = uc_mem_write(binding->uc, primary, bytes, count);
  if ((result == UC_ERR_OK) && (alias != 0U)) {
    result = uc_mem_write(binding->uc, alias, bytes, count);
  }
  return result;
}

/**
 * @brief Restore old bytes to the fd and all live engine views.
 * @details Restore old bytes to the fd and all live engine views; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] workspace Caller-owned workspace used by the operation.
 * @param[in] view Authoritative memory or presentation view accessed by the operation.
 * @param[in] old_bytes Old bytes input used by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @pre Arguments satisfy the ranges documented for rollback. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rollback(emu_memmap_workspace_t* workspace,
                                           const backing_view_t*   view,
                                           const void*             old_bytes,
                                           size_t                  count)
{
  const emu_io_result_t restored = priv_emu_io_pwrite_exact(workspace->backings[view->index].fd,
                                                            old_bytes,
                                                            count,
                                                            (off_t)view->offset);
  if (restored.status != k_emu_io_ok) {
    workspace->os_error = restored.os_error;
  }
  for (size_t i = 0U; i < k_emu_memmap_binding_count; i++) {
    if (workspace->bindings[i].active) {
      (void)internal_write_binding(&workspace->bindings[i], view, old_bytes, count);
    }
  }
}

/**
 * @brief Commit bytes to the fd then publish every engine view.
 * @details Commit bytes to the fd then publish every engine view; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] workspace Caller-owned workspace used by the operation.
 * @param[in] view Authoritative memory or presentation view accessed by the operation.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] guest Guest input used by the operation.
 * @return The publish result produced by the emu memmap model.
 * @retval value The operation-specific publish value.
 * @pre Arguments satisfy the ranges documented for publish. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uc_err internal_publish(emu_memmap_workspace_t* workspace,
                                            const backing_view_t*   view,
                                            const void*             bytes,
                                            size_t                  count,
                                            bool                    guest)
{
  if (workspace->poisoned || workspace->publishing || (count > workspace->scratch_bytes)) {
    return UC_ERR_RESOURCE;
  }
  const emu_io_result_t saved = priv_emu_io_pread_exact(workspace->backings[view->index].fd,
                                                        workspace->scratch,
                                                        count,
                                                        (off_t)view->offset);
  if (saved.status != k_emu_io_ok) {
    workspace->poisoned = true;
    workspace->os_error = saved.os_error;
    return UC_ERR_READ_UNMAPPED;
  }
  workspace->publishing       = true;
  const emu_io_result_t write = priv_emu_io_pwrite_exact(workspace->backings[view->index].fd,
                                                         bytes,
                                                         count,
                                                         (off_t)view->offset);
  if (write.status != k_emu_io_ok) {
    internal_rollback(workspace, view, workspace->scratch, count);
    workspace->publishing = false;
    workspace->poisoned   = true;
    workspace->os_error   = write.os_error;
    return UC_ERR_WRITE_UNMAPPED;
  }
  for (size_t i = 0U; i < k_emu_memmap_binding_count; i++) {
    if (workspace->bindings[i].active &&
        (internal_write_binding(&workspace->bindings[i], view, bytes, count) != UC_ERR_OK)) {
      internal_rollback(workspace, view, workspace->scratch, count);
      workspace->publishing = false;
      workspace->poisoned   = true;
      return UC_ERR_RESOURCE;
    }
  }
  internal_mark_dirty(workspace, view, count);
  workspace->publishing = false;
  if (guest) {
    workspace->guest_writes++;
  } else {
    workspace->host_writes++;
  }
  return UC_ERR_OK;
}

/**
 * @brief Recover the caller-owned binding associated with one engine.
 * @details Recover the caller-owned binding associated with one engine; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @return The binding result produced by the emu memmap model.
 * @retval nullptr No binding result is available; otherwise the returned pointer identifies it.
 * @pre Arguments satisfy the ranges documented for binding. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_memmap_binding_t* internal_binding(uc_engine* uc)
{
  uintptr_t token = 0U;
  if (uc_mem_read(uc, k_binding_address, &token, sizeof(token)) != UC_ERR_OK) {
    return nullptr;
  }
  emu_memmap_binding_t* const binding = (emu_memmap_binding_t*)token;
  return ((binding != nullptr) && binding->active && (binding->uc == uc)) ? binding : nullptr;
}

/**
 * @brief Stop a guest whose store requires boundary reconciliation.
 * @details Stop a guest whose store requires boundary reconciliation; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] binding Memory binding whose view and dirty state are processed.
 * @param[in] view Authoritative memory or presentation view accessed by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @pre Arguments satisfy the ranges documented for stop guest write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_stop_guest_write(emu_memmap_binding_t* binding, const backing_view_t* view, size_t count)
{
  binding->workspace->poisoned        = true;
  binding->workspace->restore_pending = true;
  binding->workspace->restore_backing = view->index;
  binding->workspace->restore_offset  = view->offset;
  binding->workspace->restore_count   = count;
  (void)uc_emu_stop(binding->uc);
}

/**
 * @brief Mirror each guest write before Unicorn publishes its local store.
 * @details Mirror each guest write before unicorn publishes its local store; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] type Type input used by the operation.
 * @param[in] address Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @param[in,out] user_data Hook context supplied when the callback was registered.
 * @pre Arguments satisfy the ranges documented for guest write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_guest_write(uc_engine*  uc,
                                              uc_mem_type type,
                                              uint64_t    address,
                                              int         size,
                                              int64_t     value,
                                              void*       user_data)
{
  (void)uc;
  (void)type;
  emu_memmap_binding_t* const binding = (emu_memmap_binding_t*)user_data;
  if ((binding == nullptr) || binding->workspace->publishing) {
    return;
  }
  backing_view_t view = {};
  if (!internal_resolve(address, (size > 0) ? (size_t)size : 0U, &view)) {
    return;
  }
  uint8_t bytes[sizeof(value)] = {};
  if (size > (int)sizeof(bytes)) {
    internal_stop_guest_write(binding, &view, (size_t)size);
    return;
  }
  for (int index = 0; index < size; index++) {
    bytes[index] = (uint8_t)((uint64_t)value >> ((unsigned)index * 8U));
  }
  if (internal_publish(binding->workspace, &view, bytes, (size_t)size, true) != UC_ERR_OK) {
    internal_stop_guest_write(binding, &view, (size_t)size);
  }
}

/**
 * @brief Seed deterministic factory TSN calibration words.
 * @details Seed deterministic factory tsn calibration words; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @pre Arguments satisfy the ranges documented for seed tsn. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @details Map the secure peripheral window and its idau bit[28] ns alias; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @return The map periph MMIO result produced by the emu memmap model.
 * @retval true The map periph MMIO condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for map periph MMIO. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @brief Copy committed dirty pages into one newly mapped engine.
 * @details Copy committed dirty pages into one newly mapped engine; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] binding Memory binding whose view and dirty state are processed.
 * @return The load binding result produced by the emu memmap model.
 * @retval true The load binding condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for load binding. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_load_binding(emu_memmap_binding_t* binding)
{
  emu_memmap_workspace_t* const workspace = binding->workspace;
  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    for (uint64_t offset = 0U; offset < workspace->backings[index].size; offset += k_page_size) {
      if (!internal_is_dirty(workspace, index, offset)) {
        continue;
      }
      const uint64_t remaining = workspace->backings[index].size - offset;
      const size_t   count = (remaining < k_page_size) ? (size_t)remaining : (size_t)k_page_size;
      const emu_io_result_t read = priv_emu_io_pread_exact(workspace->backings[index].fd,
                                                           workspace->scratch,
                                                           count,
                                                           (off_t)offset);
      const backing_view_t  view = {.index = index, .offset = offset};
      if ((read.status != k_emu_io_ok) ||
          (internal_write_binding(binding, &view, workspace->scratch, count) != UC_ERR_OK)) {
        workspace->poisoned = true;
        workspace->os_error = read.os_error;
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief Map every ordinary and aliased guest region into Unicorn-owned pages.
 * @details Map every ordinary and aliased guest region into unicorn-owned pages; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @return The map regions result produced by the emu memmap model.
 * @retval true The map regions condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for map regions. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_map_regions(uc_engine* uc)
{
  if (!internal_binding_page_available()) {
    (void)priv_emu_io_errf("host binding page overlaps the RA8 memory map\n");
    return false;
  }
  for (size_t i = 0U; i < (sizeof(s_regions) / sizeof(s_regions[0])); i++) {
    if (uc_mem_map(uc, s_regions[i].base, (size_t)s_regions[i].size, UC_PROT_ALL) != UC_ERR_OK) {
      (void)priv_emu_io_errf("map %s @0x%08llX failed\n",
                             s_regions[i].name,
                             (unsigned long long)s_regions[i].base);
      return false;
    }
  }
  return true;
}

emu_memmap_result_t emu_memmap_requirements(void)
{
  return internal_result(k_emu_memmap_ok, k_emu_memmap_scratch_bytes, 0);
}

emu_memmap_result_t
emu_memmap_open(void* scratch, size_t supplied_scratch_bytes, emu_memmap_workspace_t* workspace)
{
  if ((workspace == nullptr) || (scratch == nullptr)) {
    return internal_result(k_emu_memmap_invalid, supplied_scratch_bytes, 0);
  }
  if (supplied_scratch_bytes < k_emu_memmap_scratch_bytes) {
    return internal_result(k_emu_memmap_capacity, supplied_scratch_bytes, 0);
  }
  emu_memmap_workspace_t candidate = {};
  for (size_t i = 0U; i < k_emu_memmap_backing_count; i++) {
    candidate.backings[i].fd = -1;
  }
  candidate.scratch                                = (uint8_t*)scratch;
  candidate.scratch_bytes                          = k_emu_memmap_scratch_bytes;
  const uint64_t sizes[k_emu_memmap_backing_count] = {k_sram_size, k_sdram_size, k_ospi_size};
  for (size_t i = 0U; i < k_emu_memmap_backing_count; i++) {
    if (!internal_backing_open(sizes[i], &candidate.backings[i])) {
      const int failure = errno;
      internal_backings_close(&candidate);
      return internal_result(k_emu_memmap_io, supplied_scratch_bytes, failure);
    }
  }
  candidate.open = true;
  *workspace     = candidate;
  return internal_result(k_emu_memmap_ok, supplied_scratch_bytes, 0);
}

emu_memmap_result_t emu_memmap_attach(emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  if ((workspace == nullptr) || !workspace->open || workspace->poisoned || (uc == nullptr)) {
    return internal_result(k_emu_memmap_invalid,
                           (workspace != nullptr) ? workspace->scratch_bytes : 0U,
                           0);
  }
  emu_memmap_binding_t* binding = nullptr;
  for (size_t i = 0U; i < k_emu_memmap_binding_count; i++) {
    if (!workspace->bindings[i].active) {
      binding = &workspace->bindings[i];
      break;
    }
  }
  if (binding == nullptr) {
    return internal_result(k_emu_memmap_capacity, workspace->scratch_bytes, 0);
  }
  *binding              = (emu_memmap_binding_t){.workspace = workspace, .uc = uc, .active = true};
  const uintptr_t token = (uintptr_t)binding;
  if (!internal_map_regions(uc) ||
      (uc_mem_map(uc, k_binding_address, k_page_size, UC_PROT_NONE) != UC_ERR_OK) ||
      (uc_mem_write(uc, k_binding_address, &token, sizeof(token)) != UC_ERR_OK) ||
      !internal_load_binding(binding) || !internal_map_periph_mmio(uc) ||
      (uc_hook_add(uc,
                   &binding->write_hook,
                   UC_HOOK_MEM_WRITE,
                   (void*)internal_guest_write,
                   binding,
                   1U,
                   0U) != UC_ERR_OK)) {
    *binding = (emu_memmap_binding_t){};
    return internal_result(k_emu_memmap_unicorn, workspace->scratch_bytes, workspace->os_error);
  }
  internal_seed_tsn(uc);
  return internal_result(k_emu_memmap_ok, workspace->scratch_bytes, 0);
}

bool emu_memmap_detach(emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  if ((workspace == nullptr) || (uc == nullptr)) {
    return false;
  }
  for (size_t i = 0U; i < k_emu_memmap_binding_count; i++) {
    if (workspace->bindings[i].active && (workspace->bindings[i].uc == uc)) {
      (void)uc_hook_del(uc, workspace->bindings[i].write_hook);
      workspace->bindings[i] = (emu_memmap_binding_t){};
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
  for (size_t i = 0U; i < k_emu_memmap_binding_count; i++) {
    if (workspace->bindings[i].active) {
      return false;
    }
  }
  internal_backings_close(workspace);
  *workspace = (emu_memmap_workspace_t){};
  for (size_t i = 0U; i < k_emu_memmap_backing_count; i++) {
    workspace->backings[i].fd = -1;
  }
  return true;
}

uc_err emu_mem_read(uc_engine* uc, uint64_t address, void* bytes, size_t count)
{
  emu_memmap_binding_t* const binding = internal_binding(uc);
  backing_view_t              view    = {};
  if ((binding == nullptr) || (bytes == nullptr) || (count == 0U) ||
      !internal_resolve(address, count, &view)) {
    return uc_mem_read(uc, address, bytes, count);
  }
  const emu_io_result_t read = priv_emu_io_pread_exact(binding->workspace->backings[view.index].fd,
                                                       bytes,
                                                       count,
                                                       (off_t)view.offset);
  if (read.status != k_emu_io_ok) {
    binding->workspace->poisoned = true;
    binding->workspace->os_error = read.os_error;
    return UC_ERR_READ_UNMAPPED;
  }
  return UC_ERR_OK;
}

uc_err emu_mem_write(uc_engine* uc, uint64_t address, const void* bytes, size_t count)
{
  emu_memmap_binding_t* const binding = internal_binding(uc);
  backing_view_t              view    = {};
  if ((binding == nullptr) || (bytes == nullptr) || (count == 0U) ||
      !internal_resolve(address, count, &view)) {
    return uc_mem_write(uc, address, bytes, count);
  }
  return internal_publish(binding->workspace, &view, bytes, count, false);
}

bool emu_memmap_is_poisoned(const emu_memmap_workspace_t* workspace)
{
  return (workspace == nullptr) || workspace->poisoned;
}

bool emu_memmap_reconcile(emu_memmap_workspace_t* workspace)
{
  if ((workspace == nullptr) || !workspace->open) {
    return false;
  }
  if (!workspace->restore_pending) {
    return true;
  }
  const backing_view_t view = {.index  = workspace->restore_backing,
                               .offset = workspace->restore_offset};
  if ((view.index >= k_emu_memmap_backing_count) ||
      (workspace->restore_count > workspace->scratch_bytes)) {
    return false;
  }
  const emu_io_result_t read = priv_emu_io_pread_exact(workspace->backings[view.index].fd,
                                                       workspace->scratch,
                                                       workspace->restore_count,
                                                       (off_t)view.offset);
  if (read.status != k_emu_io_ok) {
    workspace->os_error = read.os_error;
    return false;
  }
  workspace->publishing = true;
  bool restored         = true;
  for (size_t index = 0U; index < k_emu_memmap_binding_count; index++) {
    if (workspace->bindings[index].active &&
        (internal_write_binding(&workspace->bindings[index],
                                &view,
                                workspace->scratch,
                                workspace->restore_count) != UC_ERR_OK)) {
      restored = false;
    }
  }
  workspace->publishing = false;
  if (restored) {
    workspace->restore_pending = false;
  }
  return restored;
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
