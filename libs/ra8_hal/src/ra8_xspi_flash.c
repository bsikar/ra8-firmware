/**
 * @file ra8_xspi_flash.c
 * @brief OSPI / xSPI manual-command engine + JEDEC NOR-flash operations
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Sibling of the Zig XSPI units (``xspi_*_abi.zig``) (split out for file size).
 * Owns the JEDEC SPI NOR-flash operations layered on the xSPI
 * manual-command engine:
 *
 * - ``ra8_xspi_flash_program()``      -- 0x06 WREN, 0x02 PP, 0x05 WIP poll.
 * - ``ra8_xspi_flash_erase_sector()`` -- 0x06 WREN, 0x20 SE, 0x05 WIP poll.
 *
 * The engine itself (``priv_ra8_xspi_make_cdt()``,
 * ``priv_ra8_xspi_kick_command()``, ``priv_ra8_xspi_issue_simple_opcode()``)
 * and ``ra8_xspi_flash_read_status()`` / ``ra8_xspi_flash_read_id()`` are
 * Zig (``xspi_cmd_abi.zig``, RA8FW-869), declared in ``ra8_xspi_internal.h``.
 * ``ra8_xspi_flash_read()``, the 3-byte range check and the chunk header
 * are Zig too (``xspi_read_abi.zig``, RA8FW-870).
 *
 * Every build runs the identical register sequence. On the host the
 * CMDCMP poll consults the ``ra8_fake_mmio`` seam
 * (``tests/mocks/src/ra8_fake_mmio.c``) so a unit test can model the
 * peripheral: the register-level NOR-flash model in
 * ``tests/mocks/src/ra8_fake_xspi_flash.c`` services each ``TRREQ`` kick on
 * the driver's own poll thread, and fault tests arm the seam to drive
 * the timeout legs. Every register access carries a
 * ``HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986``
 * citation comment for the cite checker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi.h"
#include "ra8_xspi_internal.h"

/** @brief Logging tag for this driver. */
static const char* const s_tag = "XSPI";

/**
 * @enum ra8_spi_flash_op_t
 * @brief Standard JEDEC NOR-flash command opcodes used by this driver.
 */
typedef enum : uint8_t {
  k_ra8_spi_flash_op_write_enable = 0x06U, /**< 0x06 WREN.            */
  k_ra8_spi_flash_op_page_program = 0x02U, /**< 0x02 page program.    */
  k_ra8_spi_flash_op_erase_sector = 0x20U, /**< 0x20 sector erase.    */
} ra8_spi_flash_op_t;

/**
 * @enum ra8_flash_status_bit_t
 * @brief Bit positions in the SPI flash Status Register.
 */
typedef enum : uint8_t {
  k_ra8_flash_status_bit_wip = 0U, /**< Write-In-Progress (busy). */
  k_ra8_flash_status_bit_wel = 1U, /**< Write Enable Latch.       */
} ra8_flash_status_bit_t;

/**
 * @enum ra8_xspi_cdt_limits_t
 * @brief Per-transaction byte-size limits encodable in ``CDT``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_max_data_bytes = 8U, /**< CDD0 + CDD1 = 8 bytes per slot. */
} ra8_xspi_cdt_limits_t;

/**
 * @brief Stage WREN + page-program header (CDT + CDA only) without kicking.
 *
 * @details
 * Splits the previous "WREN -> build PP header -> KICK" helper so the
 * caller can load the outgoing payload into ``CDBUF[CDD0]`` /
 * ``CDBUF[CDD1]`` BEFORE TRREQ is asserted. The earlier code asserted
 * TRREQ here and only loaded CDD0/CDD1 afterwards, which clocked the
 * stale CDBUF contents (zeros, or whatever the previous read response
 * left behind) onto the bus instead of the caller's data. That bug
 * surfaced as: (a) flash_journal's first round-trip "passing" purely
 * because CDD0/CDD1 happened to be zero (matching counter=0) while
 * every subsequent counter mismatched, (b) the threadx_levelx_demo
 * panicking inside ``lx_nor_flash_format`` because LevelX's free-bit
 * metadata reads back as the previous transfer's status byte instead
 * of the bit-pattern it just wrote, and (c) the threadx_fs_levelx_demo
 * surfacing ``lx_nor_flash_format failed`` for the same reason -- every
 * LevelX sector-header write to the on-board MX25xxx flash dropped its
 * payload, so LevelX saw zero usable sectors.
 *
 * HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 documents
 * the manual-command flow as "fill CDBUF, then set CDCTL0.TRREQ"; FSP
 * ``r_ospi_b_direct_transfer`` (``r_ospi_b.c`` line ~1311) writes the
 * data words into CDBUF before TRREQ on every PP transfer.
 *
 * @param[in] reg        xSPI register block (already gated open).
 * @param[in] flash_addr Destination flash byte address.
 * @param[in] len        Bytes to program in this chunk (1..8).
 *
 * @return ::ra8_err_t outcome of the WREN sub-command.
 * @retval k_ra8_ok       WREN dispatched and CMDCMP cleared.
 * @retval other         Underlying CMDCMP timeout from WREN.
 *
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre ``len`` has been clamped by the caller to ``[1..8]``.
 * @post On success the controller has accepted WREN and the PP CDT/CDA
 *       words are staged in CDBUF slot 0 awaiting TRREQ.
 * @post CDBUF[CDD0]/CDBUF[CDD1] are intentionally NOT touched here so
 *       the caller can load the payload before kicking.
 *
 * @note Not thread-safe; caller serialises bus access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_flash_stage_program(volatile r_xspi_regs_t* reg, uint32_t flash_addr, uint32_t len)
{
  const ra8_err_t wren = priv_ra8_xspi_issue_simple_opcode(reg, k_ra8_spi_flash_op_write_enable);
  if (wren != k_ra8_ok) {
    return wren;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x02 page-program with 3-byte address. The
   * outgoing byte count is encoded in CDT.DATASIZE (FSP semantics);
   * leaving the periodic-mode CDCTL1/CDCTL2 PEREXP/PERMSK fields
   * untouched. The kick (TRREQ) is deferred to the caller so that
   * CDBUF[CDD0]/CDBUF[CDD1] can be loaded with the payload first --
   * see this helper's @details for the rationale. */
  const uint8_t chunk =
    (len > (uint32_t)k_ra8_xspi_cdt_max_data_bytes) ? k_ra8_xspi_cdt_max_data_bytes : (uint8_t)len;
  priv_ra8_xspi_build_chunk_header(reg,
                                   k_ra8_spi_flash_op_page_program,
                              flash_addr,
                              chunk,
                              k_ra8_xspi_cdt_trtype_write);
  return k_ra8_ok;
}

/**
 * @brief Poll the SPI-flash Status Register until Write-In-Progress clears.
 *
 * @details
 * Issues ``RDSR`` (via ::ra8_xspi_flash_read_status) in a statically-bounded loop
 * until the ``WIP`` bit reads 0 or the program-timeout budget is exhausted. The
 * status byte comes from the real RDSR response in CDD0 on every build; on the
 * host the ``tests/mocks/src/ra8_fake_xspi_flash.c`` model answers it, holding WIP
 * asserted for as many polls as the test configured so the loop's continuation
 * and timeout legs are reachable.
 *
 * @param[in] instance XSPI flash instance index passed to
 *                     ::ra8_xspi_flash_read_status.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok          ``WIP`` cleared within the timeout budget.
 * @retval k_ra8_err_timeout ``WIP`` stayed set for the full budget.
 * @retval other            ::ra8_xspi_flash_read_status reported a read fault.
 *
 * @pre ``instance`` identifies an opened XSPI flash.
 * @pre A write or erase command was just issued (``WIP`` may be set).
 * @post On ``k_ra8_ok`` the device is idle (``WIP == 0``).
 * @post No register is written beyond the RDSR commands themselves.
 *
 * @note Not thread-safe; the XSPI program path is single-owner.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_poll_wip_clear(uint8_t instance)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_flash_program_timeout_us; i++) {
    uint8_t         status = 0U;
    const ra8_err_t e      = ra8_xspi_flash_read_status(instance, &status);
    if (e != k_ra8_ok) {
      return e;
    }
    const bool wip_clear = ((status & (uint8_t)(1U << (uint8_t)k_ra8_flash_status_bit_wip)) == 0U);
    if (wip_clear) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_timeout;
}

/**
 * @brief Stage the page-program payload into CDBUF[CDD0] / CDBUF[CDD1].
 *
 * @details
 * HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 + FSP
 * ``r_ospi_b_direct_transfer`` document the manual-command flow as
 * "fill CDBUF, then set CDCTL0.TRREQ". The prior implementation
 * issued the WREN + 0x02 page-program header and asserted TRREQ
 * BEFORE staging the caller's bytes; the controller therefore
 * clocked out whatever stale words were left in CDD0/CDD1 from the
 * previous transfer. Splitting the staging step into this helper
 * lets ``ra8_xspi_flash_program`` write the real payload first and
 * only then kick the transaction.
 *
 * @param[in] reg  xSPI register block (already gated open by the
 *                 caller).
 * @param[in] data Caller bytes. Must be non-NULL; ``len`` bytes are
 *                 read.
 * @param[in] len  Number of bytes to stage, in ``[1..8]`` (manual-
 *                 command CDBUF capacity).
 *
 * @pre ``reg != nullptr`` and the xSPI MSTPCR gate has been opened.
 * @pre ``data != nullptr`` and ``len`` is clamped to ``[1..8]``.
 * @post CDBUF[CDD0] holds bytes ``data[0..min(3,len)]`` packed
 *       little-endian.
 * @post CDBUF[CDD1] holds bytes ``data[4..len-1]`` (or zero if
 *       ``len <= 4``).
 *
 * @note Not thread-safe; caller serialises bus access.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_xspi_stage_payload(volatile r_xspi_regs_t* reg, const uint8_t* data, uint32_t len)
{
  uint32_t data_lo = 0U;
  uint32_t data_hi = 0U;
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t shift = (uint32_t)((i % 4U) * 8U);
    if (i < 4U) {
      data_lo |= ((uint32_t)data[i]) << shift;
    } else {
      data_hi |= ((uint32_t)data[i]) << shift;
    }
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[(uint8_t)k_ra8_xspi_cdbuf_idx_data0] = data_lo;
  reg->CDBUF[(uint8_t)k_ra8_xspi_cdbuf_idx_data1] = data_hi;
}

/**
 * @brief Program one page-program slot (<= 8 bytes) at ``flash_addr``.
 *
 * @details
 * Runs the full WREN -> 0x02 page-program -> WIP-poll sequence for a
 * single manual-command slot of ``chunk`` bytes:
 *
 *   1. ``internal_flash_stage_program`` writes WREN, builds the PP
 *      command header in CDT/CDA, and returns without asserting
 *      TRREQ.
 *   2. ``internal_xspi_stage_payload`` packs ``data[]`` into
 *      CDBUF[CDD0/CDD1].
 *   3. ``priv_ra8_xspi_kick_command`` asserts CDCTL0.TRREQ and polls
 *      CMDCMP.
 *   4. ``internal_poll_wip_clear`` issues 0x05 RDSR until WIP=0.
 *
 * ``chunk`` must be <= ``k_ra8_xspi_cdt_max_data_bytes`` (8) and must not
 * cross a ``k_ra8_xspi_page_len`` (256-byte) NOR page boundary; the
 * ``ra8_xspi_flash_program`` loop enforces both before calling.
 *
 * @param[in] reg        xSPI register block (already gated open).
 * @param[in] instance   xSPI instance index (for the RDSR WIP poll).
 * @param[in] flash_addr Destination flash byte address for this chunk.
 * @param[in] data       Source bytes (``chunk`` of them).
 * @param[in] chunk      Byte count for this transfer (1..8).
 *
 * @return ::ra8_err_t outcome of the chunk program.
 * @retval k_ra8_ok             Chunk programmed and WIP cleared.
 * @retval k_ra8_err_hw_timeout WREN or PP never retired (CMDCMP timeout).
 * @retval k_ra8_err_timeout    WIP never cleared after the program.
 *
 * @pre ``reg != nullptr`` and ``data != nullptr``.
 * @pre ``chunk`` is in ``[1..8]`` and stays within one 256-byte page.
 * @post On success ``chunk`` bytes are persisted at ``flash_addr``.
 * @post On success the flash WIP bit is clear (controller idle).
 *
 * @note Not thread-safe; caller serialises bus access.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flash_program_chunk(volatile r_xspi_regs_t* reg,
                                              uint8_t                 instance,
                                              uint32_t                flash_addr,
                                              const uint8_t*          data,
                                              uint32_t                chunk)
{
  const ra8_err_t p = internal_flash_stage_program(reg, flash_addr, chunk);
  if (p != k_ra8_ok) {
    return p;
  }
  /* Stage CDD0/CDD1 BEFORE TRREQ (cf. internal_xspi_stage_payload). */
  internal_xspi_stage_payload(reg, data, chunk);
  const ra8_err_t kick = priv_ra8_xspi_kick_command(reg);
  if (kick != k_ra8_ok) {
    return kick;
  }
  return internal_poll_wip_clear(instance);
}

ra8_err_t
ra8_xspi_flash_program(uint8_t instance, uint32_t flash_addr, const uint8_t* data, uint32_t len)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  if ((len == 0U) || (len > k_ra8_xspi_max_xfer)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  const ra8_err_t rng = priv_ra8_xspi_flash_range_check(flash_addr, len);
  if (rng != k_ra8_ok) {
    return rng;
  }

  /* Each manual-command page-program carries <= 8 data bytes and a
   * single PP must not cross a 256-byte NOR page boundary, so walk the
   * payload in chunks clamped to both limits (HUM Ch 44 p 2986).  This
   * is what makes a 512-byte LevelX sector write round-trip instead of
   * persisting only the first 8 bytes. */
  uint32_t off = 0U;
  while (off < len) {
    const uint32_t addr = flash_addr + off;
    const uint32_t page_left =
      (uint32_t)k_ra8_xspi_page_len - (addr & ((uint32_t)k_ra8_xspi_page_len - 1U));
    uint32_t chunk = len - off;
    if (chunk > (uint32_t)k_ra8_xspi_cdt_max_data_bytes) {
      chunk = (uint32_t)k_ra8_xspi_cdt_max_data_bytes;
    }
    if (chunk > page_left) {
      chunk = page_left;
    }
    const ra8_err_t e = internal_flash_program_chunk(reg, instance, addr, &data[off], chunk);
    if (e != k_ra8_ok) {
      return e;
    }
    off += chunk;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_flash_erase_sector(uint8_t instance, uint32_t flash_addr)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  const ra8_err_t rng = priv_ra8_xspi_flash_range_check(flash_addr, 0U);
  if (rng != k_ra8_ok) {
    return rng;
  }

  const ra8_err_t wren = priv_ra8_xspi_issue_simple_opcode(reg, k_ra8_spi_flash_op_write_enable);
  if (wren != k_ra8_ok) {
    return wren;
  }

  /* Programme a JEDEC 0x20 sector-erase with 3-byte address.
   * Erase has no payload, so DATASIZE=0 and TRTYPE=write. */
  priv_ra8_xspi_build_chunk_header(reg,
                                   k_ra8_spi_flash_op_erase_sector,
                              flash_addr,
                              0U,
                              k_ra8_xspi_cdt_trtype_write);

  const ra8_err_t wait = priv_ra8_xspi_kick_command(reg);
  if (wait != k_ra8_ok) {
    return wait;
  }
  return internal_poll_wip_clear(instance);
}
