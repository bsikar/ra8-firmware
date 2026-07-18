/**
 * @file ra8_xspi_internal.h
 * @brief Module-private seam shared between the ra8_xspi translation units.
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Not part of the public API. The OSPI / xSPI driver is split across two
 * translation units for file-size reasons:
 *
 * - ``ra8_xspi.c``       -- block/clock/reset bring-up, ``ra8_xspi_init``,
 *                          raw direct-command, lifecycle + IRQ + power +
 *                          XIP/DTR/DQS-calibration + software-reset surface.
 * - ``ra8_xspi_flash.c`` -- the manual-command engine (CDT/CDBUF builders +
 *                          CMDCMP poll + TRREQ kick) and the JEDEC NOR-flash
 *                          read / program / erase / status / id operations.
 *
 * This header carries exactly the symbols that cross that TU boundary:
 *
 * - The ``ra8_xspi_timeouts_t`` and ``ra8_xspi_cdbuf_idx_t`` file-scope enum
 *   blocks, which both TUs reference.
 * - Prototypes for the two manual-command helpers defined in
 *   ``ra8_xspi_flash.c`` but also called from the lifecycle surface in
 *   ``ra8_xspi.c`` (suspend / resume / software-reset).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ospi_regs.h"

/**
 * @enum ra8_xspi_timeouts_t
 * @brief Bounded spin budgets for host + target builds.
 */
typedef enum : uint32_t {
  /**
   * @brief CMDCMP poll budget for a single manual-command transfer.
   *
   * @details
   * Sized for the worst-case ``ra8_xspi`` JEDEC command at the chip's
   * default 1S-1S-1S clock (which on the RA8D2 powers up at the OCTA
   * peripheral-clock reset rate, on the order of a few MHz before any
   * higher-frequency PLL is selected). A 1-byte opcode + 3-byte
   * address + 8 data bytes = 12 bytes = 96 bits. At 2 MHz that is
   * ~48 us. The IS25LX512M datasheet Ch 8 "Command Set" lists the
   * longest *non-write* command (RDID + 3 bytes payload) at well
   * under 100 us. We pick 50,000 iterations of the tight register
   * read loop -- at ~5 cycles per iteration on the 1 GHz Cortex-M85
   * that is roughly 250 us, two orders of magnitude over the longest
   * command but small enough that a wedged controller (pin route
   * missing, OSPI not enumerated, etc.) returns ``hw_timeout`` to
   * the caller within sub-millisecond time instead of stalling the
   * HIL probe window for tens of seconds. The previous 1,000,000-
   * iteration budget made each failed round-trip take 5+ seconds, so
   * ``g_fj_match`` / ``g_fj_mismatch`` both read 0 for the entire
   * 5 s memprobe window. HUM Ch 44 "Octal Serial Peripheral
   * Interface (OSPI)" p 2986; IS25LX512M datasheet Ch 8.
   */
  k_ra8_xspi_cmd_spin = 50000U, /**< CMDCMP poll budget. */
  /**
   * @brief WIP-poll iteration cap for program / erase finish.
   *
   * @details
   * Per IS25LX512M datasheet Ch 16 "AC Characteristics", a sector
   * erase ranges typical 30 ms, max 500 ms; a page program is
   * typical 75 us, max 700 us. Each iteration of this loop performs
   * one RDSR command which itself bounds at ``k_ra8_xspi_cmd_spin``
   * iterations (~250 us worst-case). 4,000 outer iterations therefore
   * yields a ~1 second worst-case wall-clock budget -- comfortably
   * over the IS25LX512M max-sector-erase spec but bounded enough that
   * a hung peripheral surfaces to the caller in human time.
   */
  k_ra8_flash_program_timeout_us = 32000U, /**< Max ~8 s for program / erase. */
  /**
   * @brief OCTACKCR SREQ/SRDY handshake budget.
   *
   * @details
   * Mirrors ``k_ra8_canfd_ckcr_spin`` in ``ra8_canfd.c``. Each iteration
   * reads OCTACKCR; the CGC asserts OCTACKSRDY within a small number
   * of OCTACLK cycles (HUM Ch 9.2.45 p 360). 262144 iterations on a
   * 1 GHz Cortex-M85 with ~5 cycles per register-read poll is roughly
   * 1.3 ms -- two orders of magnitude over the documented wait but
   * bounded enough that a stuck handshake (e.g. MOCO not running)
   * surfaces as ``hw_timeout`` in sub-millisecond time. Shares its
   * order of magnitude with the USB / CANFD CKSRDY waits.
   */
  k_ra8_xspi_ckcr_spin = 262144U, /**< OCTACKCR SREQ/SRDY budget. */
} ra8_xspi_timeouts_t;

/**
 * @enum ra8_xspi_cdbuf_idx_t
 * @brief Word indices into CDBUF slot 0 used by this driver.
 *
 * @details
 * FSP names these ``CDBUF[0].CDT``, ``CDBUF[0].CDA``,
 * ``CDBUF[0].CDD0``, ``CDBUF[0].CDD1`` (HUM Ch 44 p 2986). The
 * flat ``r_xspi_regs_t::CDBUF[16]`` array indexes them as
 * ``slot * 4 + word``. This driver only uses slot 0.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdbuf_idx_cdt   = 0U, /**< CDBUF[0] CDT  -- type/opcode word. */
  k_ra8_xspi_cdbuf_idx_addr  = 1U, /**< CDBUF[1] CDA  -- flash address.    */
  k_ra8_xspi_cdbuf_idx_data0 = 2U, /**< CDBUF[2] CDD0 -- data bytes 0..3.  */
  k_ra8_xspi_cdbuf_idx_data1 = 3U, /**< CDBUF[3] CDD1 -- data bytes 4..7.  */
} ra8_xspi_cdbuf_idx_t;

/**
 * @brief Kick a prepared manual-command transfer by raising TRREQ.
 *
 * @details
 * Defined in ``ra8_xspi_flash.c``. FSP ``r_ospi_b_direct_transfer`` waits
 * for any prior in-flight TRREQ to self-clear before pushing a new
 * request, then sets TRREQ=1 and waits for it to self-clear again. We
 * mirror the FSP "self-clear" semantics by polling ``INTS.CMDCMP``.
 * Promoted to TU-external linkage so the lifecycle surface in
 * ``ra8_xspi.c`` (software-reset path) can reuse it. HUM Ch 44 p 2986.
 *
 * @param[in] reg xSPI register block (already gated open by the caller).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok             CMDCMP observed; transfer retired.
 * @retval k_ra8_err_hw_timeout CMDCMP never fired within the budget.
 *
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre The CDBUF slot has been fully populated by the caller.
 * @post On success the controller is idle and INTS is cleared.
 * @post On timeout no further register state is modified.
 *
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_xspi_kick_command(volatile r_xspi_regs_t* reg);

/**
 * @brief Build CDBUF[0] for a 1-byte opcode with no address / no data.
 *
 * @details
 * Defined in ``ra8_xspi_flash.c``. Populates CDBUF slot 0 per FSP
 * ``r_ospi_b_direct_transfer`` (CDT carries opcode + size encoding,
 * CDA/CDD0/CDD1 zeroed) and kicks the transfer. Promoted to
 * TU-external linkage so the lifecycle surface in ``ra8_xspi.c``
 * (suspend / resume) can reuse it. HUM Ch 44 p 2986.
 *
 * @param[in] reg    xSPI register block (already gated open by the caller).
 * @param[in] opcode JEDEC opcode to issue (no address, no payload).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok             Command dispatched and CMDCMP cleared.
 * @retval k_ra8_err_hw_timeout Underlying CMDCMP timeout.
 *
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre The controller is idle (no manual command in flight).
 * @post On success the opcode has been clocked out and CMDCMP cleared.
 * @post No flash address or payload words are transmitted.
 *
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_xspi_issue_simple_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode);

#ifdef __cplusplus
}
#endif
