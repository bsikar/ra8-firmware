/**
 * @file board_periph_xspi.c
 * @brief Octal-SPI (xSPI) manual-command + NOR-flash peripheral model for ra8_emulator
 *
 * @details
 * Models the RA8D2 XSPI0 manual-command engine (ra8_ospi_regs.h, ra8_xspi.c)
 * at @c 0x40268000 plus a backing NOR-flash array, so the firmware's real
 * "fill CDBUF, set CDCTL0.TRREQ, poll INTS.CMDCMP, read CDBUF" sequence
 * actually round-trips data. Without this block the xSPI MMIO was unmodeled:
 * flash_journal reached its run budget only because it tolerates the failed
 * ops, while threadx_levelx_demo / threadx_fs_levelx_demo panic-halted
 * inside lx_nor_flash_format (LevelX needs the flash to truly read back what
 * it wrote).
 *
 * The model decodes the slot-0 command descriptor (CDBUF[0] = CDT, [1] = CDA,
 * [2] = CDD0, [3] = CDD1) on every TRREQ kick:
 *   - 0x9F RDID  -> CDD0 = {0x9D, 0x5A, 0x1A} (ISSI IS25LX512M JEDEC triplet)
 *   - 0x05 RDSR  -> CDD0 = WEL=1, WIP=0 (flash idle)
 *   - 0x06 WREN  -> no-op (latch is implicit; status always reports WEL=1)
 *   - 0x03 READ  -> copy DATASIZE bytes from the flash array into CDD0/CDD1
 *   - 0x02 PP    -> AND DATASIZE bytes of CDD0/CDD1 into the flash array
 *   - 0x20 SE    -> fill the 4 KiB sector at CDA with 0xFF
 *   - anything else (8D/1S software-reset opcodes, mode switches) -> no-op
 * then sets INTS.CMDCMP and clears CDCTL0.TRREQ. NOR semantics (program only
 * clears bits; erase restores 0xFF) match the real part and the host-test
 * register-level model in tests/mocks/ra8_fake_xspi_flash.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief Console-tap line buffer capacity for an xSPI command summary. */
typedef enum : uint32_t {
  k_xspi_console_line_cap = 48U, /**< Max chars in an "OSPI .. @0x.." line. */
} xspi_console_t;

/** @brief XSPI0 register-window geometry (ra8_ospi_regs.h). */
typedef enum : uint64_t {
  k_xspi_base       = 0x40268000UL, /**< XSPI0 register window base.         */
  k_xspi_span       = 0x200UL,      /**< Window covering WRAPCFG..INTE.      */
  k_xspi_off_cdctl0 = 0x070UL,      /**< CDCTL0 (TRREQ bit 0 / CSSEL bit 3). */
  k_xspi_off_cdbuf  = 0x080UL,      /**< CDBUF[0..3] for manual slot 0.      */
  k_xspi_off_ints   = 0x190UL,      /**< INTS (CMDCMP bit 0).                */
  k_xspi_off_intc   = 0x194UL,      /**< INTC (write-1-to-clear).            */
} xspi_map_t;

/** @brief Command-descriptor (CDT) field positions / masks (ra8_ospi_regs.h). */
typedef enum : uint32_t {
  k_xspi_cdt_pos_cmdsize   = 0U,      /**< CMDSIZE[1:0] command bytes. */
  k_xspi_cdt_pos_datasize  = 5U,      /**< DATASIZE[8:5] data bytes.   */
  k_xspi_cdt_pos_cmd       = 16U,     /**< CMD[31:16] opcode field.    */
  k_xspi_cdt_mask_cmdsize  = 0x3U,    /**< CMDSIZE width.              */
  k_xspi_cdt_mask_datasize = 0xFU,    /**< DATASIZE width.             */
  k_xspi_cdt_mask_cmd      = 0xFFFFU, /**< CMD field width.            */
  k_xspi_cdt_byte          = 0xFFU,   /**< One opcode/data byte.       */
} xspi_cdt_field_t;

/** @brief CDCTL0 / INTS bits + CDBUF slot-0 word indices. */
typedef enum : uint32_t {
  k_xspi_cdctl0_trreq = 1UL << 0U, /**< TRREQ: kick the manual transfer. */
  k_xspi_ints_cmdcmp  = 1UL << 0U, /**< CMDCMP: command complete.        */
  k_xspi_cdbuf_cdt    = 0U,        /**< CDBUF[0] command descriptor.     */
  k_xspi_cdbuf_addr   = 1U,        /**< CDBUF[1] flash address.          */
  k_xspi_cdbuf_data0  = 2U,        /**< CDBUF[2] data bytes 0..3.        */
  k_xspi_cdbuf_data1  = 3U,        /**< CDBUF[3] data bytes 4..7.        */
} xspi_ctl_field_t;

/** @brief JEDEC opcodes the engine decodes (ra8_xspi.c). */
typedef enum : uint32_t {
  k_xspi_op_wren  = 0x06U, /**< Write enable.       */
  k_xspi_op_pp    = 0x02U, /**< Page program.       */
  k_xspi_op_rdsr  = 0x05U, /**< Read status.        */
  k_xspi_op_rdid  = 0x9FU, /**< Read JEDEC ID.      */
  k_xspi_op_read  = 0x03U, /**< Normal read.        */
  k_xspi_op_erase = 0x20U, /**< 4 KiB sector erase. */
} xspi_op_t;

/** @brief Backing-flash + register-window sizing and constants. */
typedef enum : uint32_t {
  k_xspi_flash_size = 0x4000000UL,  /**< 64 MiB: the full IS25LX512M part
                                         (512 Mbit; usb_selftest_ospi_rw's
                                         scratch window sits at +2 MiB).     */
  k_xspi_sector_len = 0x1000UL,     /**< 4 KiB sector (opcode 0x20).        */
  k_xspi_words      = 128U,         /**< 0x200-byte window as 32-bit words. */
  k_xspi_erased     = 0xFFU,        /**< NOR erased byte value.             */
  k_xspi_status_wel = 0x02U,        /**< RDSR result: WEL=1, WIP=0.         */
  k_xspi_jedec_cdd0 = 0x001A5A9DUL, /**< {0x9D,0x5A,0x1A} packed for CDD0.  */
  k_xspi_byte_bits  = 8U,           /**< Bits per byte.                     */
} xspi_lit_t;

/** @brief Per-tick / report order slot (after the SD block, before SPI). */
typedef enum : uint32_t {
  k_xspi_block_order = 95U, /**< Report ordering only. */
} xspi_order_t;

/** @brief xSPI model state: register-window shadow + op counters. */
typedef struct {
  uint32_t regs[k_xspi_words]; /**< 0x200-byte window shadow. */
  uint32_t reads;              /**< READ commands executed.   */
  uint32_t writes;             /**< Page-program commands.    */
  uint32_t erases;             /**< Sector erases.            */
} xspi_state_t;

static xspi_state_t s_xspi;

/**
 * @var s_xspi_flash_neg
 * @brief Backing NOR array for the full 64 MiB part, stored INVERTED.
 *
 * @details Each byte holds the bitwise complement of the flash content, so the
 * all-zero BSS image IS the erased (0xFF) part: a fresh ra8_emulator process pays
 * no resident memory for the 64 MiB until a sector is actually programmed or
 * erased (the zero page is never dirtied by reads). NOR semantics translate
 * directly: page-program clears flash bits = SETS stored bits (OR), sector
 * erase restores 0xFF = clears stored bytes to 0.
 *
 * @note Access only through ::xspi_flash_byte / ::xspi_do_program /
 *       ::xspi_do_erase so the inversion cannot leak.
 * @warning Do not memset this to 0xFF -- that would mean "all bits programmed
 *          to 0" AND commit 64 MiB of resident pages per emulator process.
 * @since 0.1.0
 */
static uint8_t s_xspi_flash_neg[k_xspi_flash_size];

/**
 * @var s_xspi_dirty_hi
 * @brief Exclusive high-water mark of bytes ever programmed/erased this run.
 *
 * @details Lets ::xspi_reset restore only the touched prefix of the 64 MiB
 * array instead of sweeping (and thereby committing) all of it on every
 * warm reboot of the emulator.
 *
 * @note Updated by ::xspi_do_program and ::xspi_do_erase only.
 * @since 0.1.0
 */
static uint32_t s_xspi_dirty_hi;

/** @brief Current flash content byte at @p addr (0xFF beyond the part). */
static uint8_t xspi_flash_byte(uint32_t addr)
{
  if (addr >= (uint32_t)k_xspi_flash_size) {
    return (uint8_t)k_xspi_erased;
  }
  return (uint8_t)~s_xspi_flash_neg[addr];
}

/** @brief Raise the dirty high-water mark to cover [0, @p end_excl). */
static void xspi_mark_dirty(uint32_t end_excl)
{
  if (end_excl > s_xspi_dirty_hi) {
    s_xspi_dirty_hi = end_excl;
  }
}

/** @brief Word index of CDBUF slot-0 entry @p w inside the window shadow. */
static uint32_t xspi_cdbuf_word(uint32_t w)
{
  return (uint32_t)((k_xspi_off_cdbuf / 4U) + w);
}

/** @brief Extract the 1/2-byte opcode from a CDT descriptor word. */
static uint32_t xspi_opcode(uint32_t cdt)
{
  const uint32_t cmdsize =
    (cdt >> (uint32_t)k_xspi_cdt_pos_cmdsize) & (uint32_t)k_xspi_cdt_mask_cmdsize;
  const uint32_t cmdfield = (cdt >> (uint32_t)k_xspi_cdt_pos_cmd) & (uint32_t)k_xspi_cdt_mask_cmd;
  /* internal_make_cdt left-justifies the opcode in the 16-bit CMD field:
   * shift = 8 * (2 - cmdsize). Recover the low opcode byte. */
  const uint32_t shift =
    (uint32_t)k_xspi_byte_bits * (2U - (cmdsize & (uint32_t)k_xspi_cdt_mask_cmdsize));
  return (cmdfield >> shift) & (uint32_t)k_xspi_cdt_byte;
}

/** @brief Read @p n bytes (<= 8) from flash@addr into CDD0/CDD1. */
static void xspi_do_read(uint32_t addr, uint32_t n)
{
  uint32_t d0 = 0U;
  uint32_t d1 = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    const uint8_t b = xspi_flash_byte(addr + i);
    if (i < 4U) {
      d0 |= (uint32_t)b << (i * (uint32_t)k_xspi_byte_bits);
    } else {
      d1 |= (uint32_t)b << ((i - 4U) * (uint32_t)k_xspi_byte_bits);
    }
  }
  s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data0)] = d0;
  s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data1)] = d1;
  s_xspi.reads++;
  /* Console OSPI tab: one line per manual read command. */
  char ln[k_xspi_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "OSPI rd @0x%06X %uB", (unsigned)addr, (unsigned)n);
  board_console_push(k_board_console_ch_ospi, ln);
}

/** @brief Page-program @p n bytes (<= 8) of CDD0/CDD1 into flash@addr (NOR AND). */
static void xspi_do_program(uint32_t addr, uint32_t n)
{
  const uint32_t d0 = s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data0)];
  const uint32_t d1 = s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data1)];
  for (uint32_t i = 0U; i < n; i++) {
    if ((addr + i) >= (uint32_t)k_xspi_flash_size) {
      continue;
    }
    const uint32_t w = (i < 4U) ? d0 : d1;
    const uint32_t s = (i < 4U) ? i : (i - 4U);
    const uint8_t  b =
      (uint8_t)((w >> (s * (uint32_t)k_xspi_byte_bits)) & (uint32_t)k_xspi_cdt_byte);
    /* NOR program clears flash bits only: flash &= b. In the inverted store
     * that is neg = ~(~neg & b) = neg | ~b. */
    s_xspi_flash_neg[addr + i] |= (uint8_t)~b;
    xspi_mark_dirty(addr + i + 1U);
  }
  s_xspi.writes++;
  /* Console OSPI tab: one line per page-program command. */
  char ln[k_xspi_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "OSPI pp @0x%06X %uB", (unsigned)addr, (unsigned)n);
  board_console_push(k_board_console_ch_ospi, ln);
}

/** @brief Erase the 4 KiB sector containing @p addr (restore 0xFF). */
static void xspi_do_erase(uint32_t addr)
{
  const uint32_t base = addr & ~((uint32_t)k_xspi_sector_len - 1U);
  if (base >= (uint32_t)k_xspi_flash_size) {
    return;
  }
  /* Erased flash = 0xFF = all-zero bytes in the inverted store. */
  (void)memset(&s_xspi_flash_neg[base], 0, (size_t)k_xspi_sector_len);
  xspi_mark_dirty(base + (uint32_t)k_xspi_sector_len);
  s_xspi.erases++;
}

/** @brief Execute the slot-0 manual command, then raise INTS.CMDCMP. */
static void xspi_exec_command(void)
{
  const uint32_t cdt  = s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_cdt)];
  const uint32_t addr = s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_addr)];
  const uint32_t n =
    (cdt >> (uint32_t)k_xspi_cdt_pos_datasize) & (uint32_t)k_xspi_cdt_mask_datasize;
  switch (xspi_opcode(cdt)) {
    case (uint32_t)k_xspi_op_rdid:
      s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data0)] = (uint32_t)k_xspi_jedec_cdd0;
      break;
    case (uint32_t)k_xspi_op_rdsr:
      s_xspi.regs[xspi_cdbuf_word((uint32_t)k_xspi_cdbuf_data0)] = (uint32_t)k_xspi_status_wel;
      break;
    case (uint32_t)k_xspi_op_read:
      xspi_do_read(addr, n);
      break;
    case (uint32_t)k_xspi_op_pp:
      xspi_do_program(addr, n);
      break;
    case (uint32_t)k_xspi_op_erase:
      xspi_do_erase(addr);
      break;
    default:
      break; /* WREN, 8D/1S software reset, mode switches: no flash effect. */
  }
  s_xspi.regs[(uint32_t)(k_xspi_off_ints / 4U)] |= (uint32_t)k_xspi_ints_cmdcmp;
}

/** @brief Reset the xSPI model: zero the window, erase the touched flash prefix. */
static void xspi_reset(void)
{
  (void)memset(s_xspi.regs, 0, sizeof(s_xspi.regs));
  /* Only the dirty prefix ever left the erased state; sweeping the whole
   * 64 MiB would needlessly commit resident pages in every emulator run. */
  (void)memset(s_xspi_flash_neg, 0, (size_t)s_xspi_dirty_hi);
  s_xspi_dirty_hi = 0U;
  s_xspi.reads    = 0U;
  s_xspi.writes   = 0U;
  s_xspi.erases   = 0U;
}

/** @brief MMIO read inside the xSPI window. */
static uint64_t xspi_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_xspi_base;
  if ((off / 4U) >= (uint64_t)k_xspi_words) {
    return 0U;
  }
  return s_xspi.regs[off / 4U];
}

/** @brief MMIO write inside the xSPI window (TRREQ kicks; INTC is W1C). */
static void xspi_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_xspi_base;
  if ((off / 4U) >= (uint64_t)k_xspi_words) {
    return;
  }
  if (off == (uint64_t)k_xspi_off_intc) {
    /* Write-1-to-clear against INTS. */
    s_xspi.regs[(uint32_t)(k_xspi_off_ints / 4U)] &= ~(uint32_t)value;
    return;
  }
  s_xspi.regs[off / 4U] = (uint32_t)value;
  if ((off == (uint64_t)k_xspi_off_cdctl0) &&
      (((uint32_t)value & (uint32_t)k_xspi_cdctl0_trreq) != 0U)) {
    xspi_exec_command();
    /* TRREQ self-clears on completion. */
    s_xspi.regs[off / 4U] &= ~(uint32_t)k_xspi_cdctl0_trreq;
  }
}

/** @brief End-of-run xSPI section: op counts (only if the bus was used). */
static void xspi_report(void)
{
  if ((s_xspi.reads == 0U) && (s_xspi.writes == 0U) && (s_xspi.erases == 0U)) {
    return;
  }
  (void)fprintf(stderr,
                "  XSPI flash    : %u reads  %u programs  %u erases\n",
                s_xspi.reads,
                s_xspi.writes,
                s_xspi.erases);
}

/** @brief xSPI block descriptor (self-registered with the core). */
static const board_periph_block_t k_xspi_block = {
  .base   = (uint32_t)k_xspi_base,
  .span   = (uint32_t)k_xspi_span,
  .order  = (uint32_t)k_xspi_block_order,
  .read   = xspi_read,
  .write  = xspi_write,
  .tick   = nullptr,
  .reset  = xspi_reset,
  .report = xspi_report,
  .name   = "XSPI",
};

/** @brief Register the xSPI block before main (host constructor). */
[[gnu::constructor]] static void xspi_block_register(void)
{
  board_periph_register_block(&k_xspi_block);
}
