/**
 * @file sim_memmap.c
 * @brief Emulated memory map implementation (see sim_memmap.h)
 *
 * @details
 * The region table, the host-backed TrustZone alias buffers, the TSN
 * factory-trim seed, and the region + MMIO-window mapping -- moved verbatim
 * out of the board_sim main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_memmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_console.h"
#include "sim_mmio.h"

/* RA8D2 memory map (EK board) -- from the linker script / HUM R01UH1065EJ. */
static const mem_region_t k_regions[] = {
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
                                             * (src/app) run their NS world in board_sim. */
  /* No DATA_FLASH region. 0x27000000 was carried in every app linker script as a
   * 16 KiB "data flash (EEPROM emulation)" MEMORY declaration inherited from
   * other RA parts, and board_sim used to map it as plain RAM -- so writes there
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
                                             * host buffer makes them visible to
                                             * the Secure GLCDC read at
                                             * 0x68000000. Must follow SDRAM in
                                             * this table so the shared host
                                             * buffer is allocated first. */
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
                                             * instructions from this alias. Must
                                             * follow OSPI in this table so the
                                             * shared host buffer is allocated
                                             * before the alias maps onto it. */
  {"PPB", 0xE0000000UL, 0x00100000UL},      /* ARM private peripheral bus */
};

/* Octo-SPI (XSPI) execute-in-place flash window. The XSPI controller's register
 * command-engine is modelled in peripheral space (board_periph_xspi.c, base
 * 0x40268000); this is the SEPARATE memory-mapped, executable view of the 64 MiB
 * flash array the controller exposes once the firmware enters XIP mode. The
 * Secure physical window sits at 0x80000000; its TrustZone Non-secure alias
 * (IDAU bit[28]=1) sits at 0x90000000. Both views address the same flash array,
 * so board_sim backs them with a single host buffer (mirrors the SRAM/NS_SRAM2
 * alias pair). A Non-secure reader image linked for XIP places .text/.rodata at
 * 0x90000000 and the CPU fetches Non-secure instructions from there. */
typedef enum : uint64_t {
  k_ospi_xip_base = 0x80000000UL, /**< OSPI XIP window: Secure physical base.      */
  k_ospi_ns_base  = 0x90000000UL, /**< OSPI XIP window: NS alias (IDAU bit[28]=1). */
  k_ospi_xip_size = 0x04000000UL, /**< 64 MiB execute-in-place flash array.        */
} ospi_xip_map_t;

/* On-chip temperature-sensor factory calibration. The TSN two-point trim words
 * TSCDR (code at the high reference) and TSCDR2 (code at the low reference) live
 * in the MRAM factory-trim region at 0x02C1EDA0 (HUM Ch 55.2.2 p 3498-3499).
 * Real silicon ships them factory-programmed; board_sim's blank map left the
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

/** @brief Host-backed on-chip SRAM (shared with the cpu1 engine). */
static uint8_t* s_sram_buf;

/** @brief Host-backed OSPI XIP flash (shared with its NS alias). */
static uint8_t* s_ospi_buf;

/** @brief Host-backed external SDRAM (shared with its NS alias). */
static uint8_t* s_sdram_buf;

/**
 * @brief Seed the on-chip temperature-sensor factory calibration into memory.
 *
 * @details
 * Writes the two-point trim words TSCDR / TSCDR2 (see ::tsn_cal_seed_t) into the
 * mapped MRAM factory-trim page at ::k_tsn_cal_addr. On silicon these cells are
 * factory-programmed; the emulator's map is blank, so without this seed
 * ra8_tsn_convert_to_milli_c reads an unmapped address and the run bus-faults.
 * Must be called after the memory regions (including "TRIM") are mapped.
 *
 * @param[in,out] uc Unicorn engine whose "TRIM" page receives the seed.
 * @return Nothing.
 * @pre The TRIM region covering ::k_tsn_cal_addr has been mem-mapped on @p uc.
 * @post @p uc holds TSCDR at ::k_tsn_cal_addr and TSCDR2 at the next word.
 * @since 0.1.0
 */
static void seed_tsn_calibration(uc_engine* uc)
{
  const uint32_t tscdr  = (uint32_t)k_tsn_cal_tscdr;
  const uint32_t tscdr2 = (uint32_t)k_tsn_cal_tscdr2;
  (void)uc_mem_write(uc, (uint64_t)k_tsn_cal_addr, &tscdr, sizeof(tscdr));
  (void)uc_mem_write(uc, (uint64_t)k_tsn_cal_addr + sizeof(tscdr), &tscdr2, sizeof(tscdr2));
}

/**
 * @brief Allocate a zeroed, page-aligned host buffer for a host-backed region.
 *
 * The three physical apertures (SRAM, OSPI, SDRAM) are host-backed so their
 * Non-secure bit[28] aliases can mirror the SAME bytes.  Factored out so each
 * arm of `map_one_region()` reads as one line instead of an alloc/null/memset
 * triple.  Returns nullptr (and logs) on allocation failure.
 */
static uint8_t* alloc_backing(uint64_t size, const char* what)
{
  uint8_t* const buf = (uint8_t*)aligned_alloc((size_t)k_page_size, (size_t)size);
  if (buf == nullptr) {
    (void)fprintf(stderr, "%s host buffer alloc failed\n", what);
    return nullptr;
  }
  (void)memset(buf, 0, (size_t)size);
  return buf;
}

/**
 * @brief Map one memory region into the engine.
 *
 * Three physical apertures -- on-chip SRAM (0x22..), OSPI XIP flash (0x80..)
 * and external SDRAM (0x68..) -- are host-backed (``uc_mem_map_ptr``) rather
 * than engine-backed so their IDAU bit[28]=1 Non-secure aliases (0x32.. /
 * 0x90.. / 0x78..) can mirror the SAME host bytes, keeping the Secure and
 * Non-secure views coherent: SRAM lets cpu1 share IPC memory and BLXNS land on
 * the copied NS image; OSPI lets an execute-in-place image be fetched through
 * either view (UC_PROT_ALL includes EXEC); SDRAM lets the NS e-reader draw the
 * framebuffer the Secure GLCDC scans back.  Each physical region precedes its
 * alias in ``k_regions``, so its host buffer is allocated first and the alias
 * arm reuses it at the bit[28]-stripped offset.  Every other region is plainly
 * engine-mapped.  Transparent for cpu0 (uc_mem_map_ptr behaves like
 * uc_mem_map otherwise).
 */
static bool map_one_region(uc_engine* uc, const mem_region_t* r)
{
  uc_err mr = UC_ERR_OK;
  if (r->base == (uint64_t)k_sram_base) {
    s_sram_buf = alloc_backing(r->size, "SRAM");
    if (s_sram_buf == nullptr) {
      return false;
    }
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, s_sram_buf);
  } else if (r->base == (uint64_t)k_ns_sram2_base) {
    uint8_t* const ns_host =
      s_sram_buf + ((size_t)k_ns_sram2_base - (size_t)k_ns_alias_bit - (size_t)k_sram_base);
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, ns_host);
  } else if (r->base == (uint64_t)k_ospi_xip_base) {
    s_ospi_buf = alloc_backing(r->size, "OSPI");
    if (s_ospi_buf == nullptr) {
      return false;
    }
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, s_ospi_buf);
  } else if (r->base == (uint64_t)k_ospi_ns_base) {
    uint8_t* const ns_host =
      s_ospi_buf + ((size_t)k_ospi_ns_base - (size_t)k_ns_alias_bit - (size_t)k_ospi_xip_base);
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, ns_host);
  } else if (r->base == (uint64_t)k_sdram_base) {
    s_sdram_buf = alloc_backing(r->size, "SDRAM");
    if (s_sdram_buf == nullptr) {
      return false;
    }
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, s_sdram_buf);
  } else if (r->base == (uint64_t)k_ns_sdram_base) {
    uint8_t* const ns_host =
      s_sdram_buf + ((size_t)k_ns_sdram_base - (size_t)k_ns_alias_bit - (size_t)k_sdram_base);
    mr = uc_mem_map_ptr(uc, r->base, (size_t)r->size, UC_PROT_ALL, ns_host);
  } else {
    mr = uc_mem_map(uc, r->base, (size_t)r->size, UC_PROT_ALL);
  }
  if (mr != UC_ERR_OK) {
    (void)fprintf(stderr, "map %s @0x%08llX failed\n", r->name, (unsigned long long)r->base);
    return false;
  }
  return true;
}

/** @brief Map the Secure peripheral window and its IDAU bit[28] NS alias. */
static bool map_periph_mmio(uc_engine* uc)
{
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map failed\n");
    return false;
  }
  /* IDAU bit[28]=1 Non-secure peripheral alias (0x50000000): the SAME silicon
   * registers as 0x40000000, reached by TrustZone Non-secure code (an NS image
   * built with RA8_PERIPH_NS_ALIAS -- MSTP at 0x5020_3000, USBFS at 0x5025_0000,
   * USBHS at 0x5035_1000 -- or the NS-side IPC ping-pong at 0x5002_0000). The
   * hooks rebuild the absolute address as k_periph_base + window-relative
   * offset, so a 0x50020000 access (offset 0x20000) dispatches to 0x40020000
   * identically to a Secure one -- the exact mapping the cpu1 engine already
   * carries (see cpu1_engine_init). */
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map (NS alias) failed\n");
    return false;
  }
  return true;
}

/** @brief Implementation of `sim_memmap_init()` -- regions, TSN seed, MMIO windows. */
bool sim_memmap_init(uc_engine* uc)
{
  for (size_t i = 0U; i < (sizeof(k_regions) / sizeof(k_regions[0])); i++) {
    if (!map_one_region(uc, &k_regions[i])) {
      return false;
    }
  }
  /* Factory-programmed on silicon; seed it now the TRIM page is mapped so the
   * temperature-sensor two-point conversion reads real data (see the helper). */
  seed_tsn_calibration(uc);
  return map_periph_mmio(uc);
}

/** @brief Implementation of `sim_memmap_regions()` -- static table access. */
const mem_region_t* sim_memmap_regions(uint32_t* count)
{
  *count = (uint32_t)(sizeof(k_regions) / sizeof(k_regions[0]));
  return k_regions;
}

/** @brief Implementation of `sim_memmap_sram_buf()` -- shared backing access. */
uint8_t* sim_memmap_sram_buf(void)
{
  return s_sram_buf;
}

/** @brief Implementation of `sim_memmap_mram_base()` -- named MRAM lookup. */
uint64_t sim_memmap_mram_base(void)
{
  return k_regions[1].base;
}
