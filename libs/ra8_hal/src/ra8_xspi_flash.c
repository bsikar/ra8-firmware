/**
 * @file ra8_xspi_flash.c
 * @brief OSPI / xSPI manual-command engine + JEDEC NOR-flash operations
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Sibling translation unit of ``ra8_xspi.c`` (split out for file size).
 * Owns the xSPI manual-command engine -- the ``CDT``/``CDBUF`` builders,
 * the ``INTS.CMDCMP`` poll, and the ``CDCTL0.TRREQ`` kick -- and the
 * JEDEC SPI NOR-flash operations layered on top of it:
 *
 * - ``ra8_xspi_flash_read()``         -- 0x03 read + CMDCMP poll.
 * - ``ra8_xspi_flash_program()``      -- 0x06 WREN, 0x02 PP, 0x05 WIP poll.
 * - ``ra8_xspi_flash_erase_sector()`` -- 0x06 WREN, 0x20 SE, 0x05 WIP poll.
 * - ``ra8_xspi_flash_read_status()``  -- 0x05.
 * - ``ra8_xspi_flash_read_id()``      -- 0x9F JEDEC ID.
 *
 * The two manual-command primitives ``priv_ra8_xspi_kick_command()`` and
 * ``priv_ra8_xspi_issue_simple_opcode()`` are exported via
 * ``ra8_xspi_internal.h`` because the lifecycle surface in ``ra8_xspi.c``
 * (suspend / resume / software-reset) reuses them.
 *
 * Every build runs the identical register sequence. On the host the
 * CMDCMP poll consults the ``ra8_fake_mmio`` seam
 * (``tests/mocks/src/ra8_fake_mmio.c``) so a unit test can model the
 * peripheral: the register-level NOR-flash model in
 * ``tests/mocks/src/ra8_fake_xspi_flash.c`` services each ``TRREQ`` kick on
 * the driver's own poll thread, and fault tests arm the seam to drive
 * the timeout legs (#238). Every register access carries a
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
static const char* s_tag = "XSPI";

/** @brief Low-byte mask for status/JEDEC-id extraction. */
typedef enum : uint32_t {
  k_xspi_byte_mask = 0xFFUL, /**< XSPI byte mask. */
} xspi_mask_t;

/**
 * @enum ra8_spi_flash_op_t
 * @brief Standard JEDEC NOR-flash command opcodes used by this driver.
 */
typedef enum : uint8_t {
  k_ra8_spi_flash_op_write_enable = 0x06U, /**< 0x06 WREN.            */
  k_ra8_spi_flash_op_page_program = 0x02U, /**< 0x02 page program.    */
  k_ra8_spi_flash_op_read_status  = 0x05U, /**< 0x05 read status reg. */
  k_ra8_spi_flash_op_read_id      = 0x9FU, /**< 0x9F JEDEC ID read.   */
  k_ra8_spi_flash_op_read         = 0x03U, /**< 0x03 normal read.     */
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
 * @enum ra8_xspi_addr_space_t
 * @brief Reachable flash address space for the 3-byte JEDEC commands.
 *
 * @details
 * Every read / program / erase this driver issues encodes its address
 * phase as ``ADDSIZE=3`` (24 bits on the wire), so a ``flash_addr`` at or
 * beyond 2^24 cannot be transmitted -- the controller would silently
 * truncate it to the low 24 bits and the command would land on the wrong
 * sector. The public operations validate the whole ``[flash_addr,
 * flash_addr + len)`` window against this bound up front and reject the
 * call instead.
 */
typedef enum : uint32_t {
  k_ra8_xspi_addr_space_3byte = 0x1000000UL, /**< 2^24: 3-byte address limit. */
} ra8_xspi_addr_space_t;

/**
 * @enum ra8_xspi_cdt_limits_t
 * @brief Per-transaction byte-size limits encodable in ``CDT``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_max_data_bytes = 8U, /**< CDD0 + CDD1 = 8 bytes per slot. */
} ra8_xspi_cdt_limits_t;

/**
 * @brief Encode a manual-command CDT word (opcode + size/type fields).
 *
 * @details
 * Mirrors FSP ``r_ospi_b_direct_transfer`` (line ~1311 of
 * ``r_ospi_b.c``): builds the ``CDBUF[0].CDT`` word from
 * CMDSIZE/ADDSIZE/DATASIZE/TRTYPE plus the JEDEC opcode at bits
 * [31..16]. Latency is fixed at zero for the simple JEDEC opcodes
 * this driver issues. HUM Ch 44 p 2986.
 * @param[in] opcode See declaration: ``uint8_t opcode``.
 * @param[in] cmd_bytes See declaration: ``uint8_t cmd_bytes``.
 * @param[in] addr_bytes See declaration: ``uint8_t addr_bytes``.
 * @param[in] data_bytes See declaration: ``uint8_t data_bytes``.
 * @param[in] is_write See declaration: ``uint8_t is_write``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_make_cdt(uint8_t opcode,
                                  uint8_t cmd_bytes,
                                  uint8_t addr_bytes,
                                  uint8_t data_bytes,
                                  uint8_t is_write)
{
  /* CMD is a 16-bit field at CDT[31:16]. The command-manual engine
   * transmits CMDSIZE bytes MSB-first starting at bit 31, so an
   * N-byte command must be LEFT-justified inside the 16-bit field:
   * a 1-byte opcode belongs at bits [31:24], a 2-byte opcode pair at
   * [31:16]. Placing a 1-byte opcode at [23:16] (a bare ``opcode <<
   * 16``) makes the controller clock out the zero byte at [31:24]
   * instead -- the flash then sees command 0x00 for every 0x9F /
   * 0x05 / 0x06 / 0x02 / 0x20 op, never answers, and RDID floats to
   * 0x00FFFFFF even though CMDCMP fires. Mirrors FSP
   * ``r_ospi_b_direct_transfer`` which stores ``command`` already
   * left-aligned in the 16-bit CMD field. HUM Ch 44 p 2986. */
  const uint8_t  cmd_shift = (uint8_t)(8U * (2U - (cmd_bytes & k_ra8_xspi_cdt_mask_cmdsize)));
  const uint32_t cmd_word  = ((uint32_t)opcode << cmd_shift) & (uint32_t)k_ra8_xspi_cdt_mask_cmd;
  return (((uint32_t)cmd_bytes & k_ra8_xspi_cdt_mask_cmdsize) << k_ra8_xspi_cdt_pos_cmdsize) |
         (((uint32_t)addr_bytes & k_ra8_xspi_cdt_mask_addsize) << k_ra8_xspi_cdt_pos_addsize) |
         (((uint32_t)data_bytes & k_ra8_xspi_cdt_mask_datasize) << k_ra8_xspi_cdt_pos_datasize) |
         (((uint32_t)is_write & k_ra8_xspi_cdt_mask_trtype) << k_ra8_xspi_cdt_pos_trtype) |
         (cmd_word << k_ra8_xspi_cdt_pos_cmd);
}

/**
 * @enum ra8_spi_flash_resp_bytes_t
 * @brief Per-opcode response-byte counts for read-direction commands.
 *
 * @details
 * The xSPI manual-command engine drives DATASIZE clocks on the bus
 * after the opcode (and address, if any) so the device can stream
 * its response bytes into CDD0/CDD1. For 0x05 RDSR the device sends
 * 1 byte; for 0x9F RDID it sends the JEDEC triplet (3 bytes). With
 * DATASIZE=0 the controller never clocks the response phase and
 * CDD0 stays at whatever it held before the transfer -- which is
 * why an earlier version of this driver always read WIP=0 and
 * tripped LevelX into believing erases had completed instantly.
 * IS25LX512M datasheet Ch 8.6 (RDSR) + Ch 8.13 (RDID).
 */
typedef enum : uint8_t {
  k_ra8_xspi_resp_bytes_status = 1U, /**< RDSR returns 1 status byte. */
  k_ra8_xspi_resp_bytes_jedec  = 3U, /**< RDID returns MFR+TYPE+CAP.  */
} ra8_spi_flash_resp_bytes_t;

/**
 * @brief Validate a ``[flash_addr, flash_addr + len)`` window against the
 *        3-byte JEDEC address space.
 *
 * @details
 * Two single-condition checks (no compound decision): the start address
 * must lie inside the 2^24-byte window ``ADDSIZE=3`` can encode, and the
 * transfer must not run past its end. The subtraction form of the second
 * check cannot overflow because the first check already bounded
 * ``flash_addr``. Rejecting here prevents the controller from silently
 * truncating the address to 24 bits on the wire and landing the command
 * on the wrong sector.
 *
 * @param[in] flash_addr First flash byte address of the transfer.
 * @param[in] len        Transfer length in bytes (0 allowed for erase).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The whole window is 3-byte addressable.
 * @retval k_ra8_err_invalid_arg The window starts or ends past 2^24.
 *
 * @pre ``len`` was already bounded by the caller (<= ``k_ra8_xspi_max_xfer``).
 * @pre The caller passes the transfer's true byte extent.
 * @post No state is modified (pure comparison).
 * @post The return reflects only the addressability comparison.
 *
 * @note Thread-safe (pure comparison).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flash_range_check(uint32_t flash_addr, uint32_t len)
{
  if (flash_addr >= (uint32_t)k_ra8_xspi_addr_space_3byte) {
    return k_ra8_err_invalid_arg;
  }
  if (len > ((uint32_t)k_ra8_xspi_addr_space_3byte - flash_addr)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Wait for a manual XSPI command to retire, then clear its status bits.
 *
 * @details
 * Bounded poll of the XSPI ``INTS`` register for the ``CMDCMP`` (command
 * complete) flag. On the host build each poll iteration consults the
 * ``ra8_fake_mmio`` seam (the i2c / i3c / sdhi status-poll pattern): the
 * ``tests/mocks/src/ra8_fake_xspi_flash.c`` model services the pending ``TRREQ``
 * kick synchronously inside the consult and the loop then observes the
 * CMDCMP flag the model raised, while a fault test arms the seam on
 * ``INTS`` to force the timeout / continuation legs. On completion it
 * clears every pending status bit via ``INTC = INTS``, mirroring FSP
 * ``r_ospi_b_direct_transfer``.
 *
 * @param[in,out] reg XSPI register block; ``INTS`` is polled and ``INTC`` written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             ``CMDCMP`` was observed and the retired status
 *                              bits cleared.
 * @retval k_ra8_err_hw_timeout ``CMDCMP`` never asserted within the spin budget.
 *
 * @pre ``reg`` is a valid, powered XSPI register block.
 * @pre A manual command was just issued (``CDCTL0.TRREQ`` set).
 * @post On success every retired ``INTS`` status bit is cleared.
 * @post On failure the register state is left unchanged.
 *
 * @note Not thread-safe; the XSPI manual-command path is single-owner.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_command_done(volatile r_xspi_regs_t* reg)
{
  ra8_err_t wait = k_ra8_err_hw_timeout;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_xspi_cmd_spin; i++) {
    /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
    const bool cmdcmp = ((reg->INTS & (uint32_t)k_ra8_xspi_ints_mask_cmdcmp) != 0U);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
    if (ra8_fake_mmio_poll(&reg->INTS, i, cmdcmp)) {
#else
    if (cmdcmp) {
#endif
      wait = k_ra8_ok;
      break;
    }
  }
  if (wait != k_ra8_ok) {
    return wait;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* FSP r_ospi_b_direct_transfer clears every pending status bit at the
   * end of a manual command via ``INTC = INTS``. */
  reg->INTC = reg->INTS;
  return k_ra8_ok;
}

ra8_err_t priv_ra8_xspi_kick_command(volatile r_xspi_regs_t* reg)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDCTL0 |= k_ra8_xspi_cdctl0_mask_trreq;
  return internal_wait_command_done(reg);
}

ra8_err_t priv_ra8_xspi_issue_simple_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Populate CDBUF slot 0 per FSP ``r_ospi_b_direct_transfer``:
   * CDT carries opcode + size encoding, CDA/CDD0/CDD1 are zeroed
   * because there is no address phase and no payload. CDCTL1/CDCTL2
   * are periodic-mode fields (PEREXP / PERMSK in FSP) -- leave
   * them at zero for one-shot manual commands. */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_cdt] = internal_make_cdt(opcode,
                                                           k_ra8_xspi_cdt_cmdsize_1,
                                                           k_ra8_xspi_cdt_addsize_0,
                                                           0U,
                                                           k_ra8_xspi_cdt_trtype_read);
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data1] = 0U;
  return priv_ra8_xspi_kick_command(reg);
}

/**
 * @brief Issue a 1-byte opcode that returns 1..8 response data bytes.
 *
 * @details
 * Used by RDSR (0x05) / RDID (0x9F): no address phase, but we MUST
 * tell the controller how many response bytes to clock in via
 * ``DATASIZE``. Caller reads the response out of CDBUF[CDD0/CDD1]
 * after CMDCMP. HUM Ch 44 p 2986; IS25LX512M datasheet Ch 8.6 + 8.13.
 *
 * @param[in] reg        xSPI register block.
 * @param[in] opcode     JEDEC opcode (0x05 or 0x9F).
 * @param[in] resp_bytes 1..8 response bytes to clock into CDBUF.
 *
 * @return ``k_ra8_ok`` on success or the underlying CMDCMP timeout.
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_issue_read_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode, uint8_t resp_bytes)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_cdt] = internal_make_cdt(opcode,
                                                           k_ra8_xspi_cdt_cmdsize_1,
                                                           k_ra8_xspi_cdt_addsize_0,
                                                           resp_bytes,
                                                           k_ra8_xspi_cdt_trtype_read);
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data1] = 0U;
  return priv_ra8_xspi_kick_command(reg);
}

/**
 * @brief Build CDBUF[0] for a single CDD0/CDD1 chunk of an opcode + addr.
 *
 * @details
 * Used by the read path (TRTYPE=read) and by the program path
 * (TRTYPE=write) to programme the per-chunk header. Address phase
 * is fixed at 3 bytes for the JEDEC opcodes in use; ``data_bytes``
 * is 0..8 (one slot's worth). FSP equivalent: the body of
 * ``r_ospi_b_direct_transfer`` that builds ``cdtbuf0``.
 * @param[in] reg See declaration: ``volatile r_xspi_regs_t* reg``.
 * @param[in] opcode See declaration: ``uint8_t                 opcode``.
 * @param[in] addr See declaration: ``uint32_t                addr``.
 * @param[in] data_bytes See declaration: ``uint8_t                 data_bytes``.
 * @param[in] is_write See declaration: ``uint8_t                 is_write``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_build_chunk_header(volatile r_xspi_regs_t* reg,
                                        uint8_t                 opcode,
                                        uint32_t                addr,
                                        uint8_t                 data_bytes,
                                        uint8_t                 is_write)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_cdt] = internal_make_cdt(opcode,
                                                           k_ra8_xspi_cdt_cmdsize_1,
                                                           k_ra8_xspi_cdt_addsize_3,
                                                           data_bytes,
                                                           is_write);
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_addr] = addr;
}

/**
 * @brief Read one manual-command slot (<= 8 bytes) from flash into ``buf``.
 *
 * @details
 * Issues a single JEDEC 0x03 read of ``chunk`` bytes at ``flash_addr``
 * and copies the CDBUF data words (CDD0 = bytes 0..3, CDD1 = bytes 4..7)
 * into ``buf``. ``chunk`` must be <= ``k_ra8_xspi_cdt_max_data_bytes`` (8)
 * because the manual-command engine only carries 8 data bytes per
 * transfer; ``ra8_xspi_flash_read`` loops this helper to cover arbitrary
 * lengths.
 *
 * @param[in]  reg        xSPI register block (already gated open).
 * @param[in]  flash_addr Source flash byte address for this chunk.
 * @param[out] buf        Destination buffer for ``chunk`` bytes.
 * @param[in]  chunk      Byte count for this transfer (1..8).
 *
 * @return ::ra8_err_t outcome of the chunk read.
 * @retval k_ra8_ok             Chunk read and copied.
 * @retval k_ra8_err_hw_timeout CMDCMP timeout from the read command.
 *
 * @pre ``reg != nullptr`` and ``buf != nullptr``.
 * @pre ``chunk`` is in ``[1..8]``.
 * @post On success ``buf[0..chunk-1]`` holds the flash data.
 * @post No state outside ``buf`` and the CDBUF slot is modified.
 *
 * @note Not thread-safe; caller serialises bus access.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flash_read_chunk(volatile r_xspi_regs_t* reg,
                                           uint32_t                flash_addr,
                                           uint8_t*                buf,
                                           uint32_t                chunk)
{
  internal_build_chunk_header(reg,
                              k_ra8_spi_flash_op_read,
                              flash_addr,
                              (uint8_t)chunk,
                              k_ra8_xspi_cdt_trtype_read);
  const ra8_err_t wait = priv_ra8_xspi_kick_command(reg);
  if (wait != k_ra8_ok) {
    return wait;
  }
  for (uint32_t i = 0U; i < chunk; i++) {
    const uint8_t cdbuf_word_idx =
      (i < 4U) ? (uint8_t)k_ra8_xspi_cdbuf_idx_data0 : (uint8_t)k_ra8_xspi_cdbuf_idx_data1;
    /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
    /* CDBUF data words: bytes 0..3 in CDD0, bytes 4..7 in CDD1. */
    const uint32_t word = reg->CDBUF[cdbuf_word_idx];
    buf[i]              = (uint8_t)(word >> ((i % 4U) * 8U));
  }
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_flash_read(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if ((len == 0U) || (len > k_ra8_xspi_max_xfer)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  const ra8_err_t rng = internal_flash_range_check(flash_addr, len);
  if (rng != k_ra8_ok) {
    return rng;
  }

  /* The manual-command engine carries only k_ra8_xspi_cdt_max_data_bytes
   * (8) data bytes per transfer, so walk the request in 8-byte chunks.
   * (HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986.) */
  uint32_t off = 0U;
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > (uint32_t)k_ra8_xspi_cdt_max_data_bytes) {
      chunk = (uint32_t)k_ra8_xspi_cdt_max_data_bytes;
    }
    const ra8_err_t e = internal_flash_read_chunk(reg, flash_addr + off, &buf[off], chunk);
    if (e != k_ra8_ok) {
      return e;
    }
    off += chunk;
  }
  return k_ra8_ok;
}

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
  internal_build_chunk_header(reg,
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
  const ra8_err_t rng = internal_flash_range_check(flash_addr, len);
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
  const ra8_err_t rng = internal_flash_range_check(flash_addr, 0U);
  if (rng != k_ra8_ok) {
    return rng;
  }

  const ra8_err_t wren = priv_ra8_xspi_issue_simple_opcode(reg, k_ra8_spi_flash_op_write_enable);
  if (wren != k_ra8_ok) {
    return wren;
  }

  /* Programme a JEDEC 0x20 sector-erase with 3-byte address.
   * Erase has no payload, so DATASIZE=0 and TRTYPE=write. */
  internal_build_chunk_header(reg,
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

ra8_err_t ra8_xspi_flash_read_status(uint8_t instance, uint8_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* RDSR returns 1 status byte; DATASIZE must be 1, not 0, or the
   * controller never clocks the response and CDD0 stays stale. See
   * IS25LX512M datasheet Ch 8.6. */
  const ra8_err_t e = internal_issue_read_opcode(reg,
                                                 k_ra8_spi_flash_op_read_status,
                                                 (uint8_t)k_ra8_xspi_resp_bytes_status);
  if (e != k_ra8_ok) {
    return e;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  *out_status = (uint8_t)(reg->CDBUF[(uint8_t)k_ra8_xspi_cdbuf_idx_data0] & k_xspi_byte_mask);
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_flash_read_id(uint8_t instance, uint32_t* out_id)
{
  RA8_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* RDID returns 3 JEDEC bytes (MFR/TYPE/CAP); DATASIZE must be 3.
   * IS25LX512M datasheet Ch 8.13 "Read Identification (RDID)". */
  const ra8_err_t e = internal_issue_read_opcode(reg,
                                                 k_ra8_spi_flash_op_read_id,
                                                 (uint8_t)k_ra8_xspi_resp_bytes_jedec);
  if (e != k_ra8_ok) {
    return e;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* JEDEC 0x9F returns MFR, MEMTYPE, CAPACITY in that byte order.
   * Repack into ``(mfr << 16) | (type << 8) | cap`` as documented
   * in the public API header. */
  const uint32_t word = reg->CDBUF[(uint8_t)k_ra8_xspi_cdbuf_idx_data0];
  *out_id = ((word & k_xspi_byte_mask) << 16U) | (((word >> 8U) & k_xspi_byte_mask) << 8U) |
            ((word >> 16U) & k_xspi_byte_mask);
  return k_ra8_ok;
}
