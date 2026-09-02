/**
 * @file ra8_xspi.h
 * @brief xSPI / Octo-SPI driver (flash read/program/erase + ID/status)
 * @ingroup grp_hal_memory
 *
 * @details
 * Driver surface for the RA8D2 xSPI controller in direct-command
 * mode. Supports initialisation at a given link-layer IO width, raw
 * command-buffer writes, and a minimal set of SPI NOR flash
 * operations (read, page program, sector erase, status/ID read).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ospi_regs.h"

/**
 * @brief Initialise an xSPI instance in the chosen link-layer IO mode.
 *
 * @param[in] instance xSPI instance (0 or 1).
 * @param[in] mode     Desired LIOCFG mode (1S/2S/4S/8S/8D).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_init(uint8_t instance, ra8_xspi_lio_mode_t mode);

/**
 * @brief Issue a raw command via the direct-command registers.
 *
 * @param[in] instance xSPI channel (0 or 1).
 * @param[in] cmd_buf  Up to 16 bytes of command / address / data.
 * @param[in] len      Number of bytes in `cmd_buf`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xspi_direct_command(uint8_t instance, const uint8_t* cmd_buf, uint8_t len);

/**
 * @brief Read `len` bytes from external flash into `buf`.
 *
 * @details
 * Builds a standard 0x03 (1S-1S-1S) read command sequence in 8-byte
 * manual-command chunks, kicks each via `CDCTL0.TRREQ`, polls
 * `INTS.CMDCMP` for completion, and copies the CDD0/CDD1 response
 * words into `buf`. Host unit tests exercise the identical sequence
 * against the register-level NOR model in
 * `tests/mocks/src/ra8_fake_xspi_flash.c`.
 *
 * @param[in]  instance   xSPI instance (0 or 1).
 * @param[in]  flash_addr Flash offset to read from (3-byte addressable).
 * @param[out] buf        Destination buffer. Must not be NULL.
 * @param[in]  len        Number of bytes to read.
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr`    if buf is NULL or instance out of range.
 * @return `k_ra8_err_invalid_arg` if len == 0, len > `k_ra8_xspi_max_xfer`,
 *         or `[flash_addr, flash_addr + len)` exceeds the 2^24-byte window
 *         a 3-byte JEDEC address can reach.
 * @return `k_ra8_err_hw_timeout`  if a read command never retires (CMDCMP).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xspi_flash_read(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len);

/**
 * @brief Program (write) `len` bytes at `flash_addr`.
 *
 * @details
 * Sequence per chunk is: Write Enable -> Page Program -> Poll WIP,
 * with chunks clamped to the 8-byte manual-command slot and to
 * 256-byte NOR page boundaries. Every build drives the identical
 * xSPI register sequence; host unit tests run it against the
 * register-level NOR model in `tests/mocks/src/ra8_fake_xspi_flash.c`.
 *
 * @param[in] instance   xSPI instance.
 * @param[in] flash_addr Flash offset (3-byte addressable).
 * @param[in] data       Source buffer.
 * @param[in] len        Number of bytes to program.
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr`    if data or instance is invalid.
 * @return `k_ra8_err_invalid_arg` if len == 0, len > `k_ra8_xspi_max_xfer`,
 *         or `[flash_addr, flash_addr + len)` exceeds the 2^24-byte window
 *         a 3-byte JEDEC address can reach.
 * @return `k_ra8_err_hw_timeout`  if a WREN / PP command never retires.
 * @return `k_ra8_err_timeout`     if the WIP bit never clears.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xspi_flash_program(uint8_t instance, uint32_t flash_addr, const uint8_t* data, uint32_t len);

/**
 * @brief Erase the 4 KiB sector containing `flash_addr`.
 *
 * @param[in] instance   xSPI instance.
 * @param[in] flash_addr Address within the target sector (3-byte
 *                       addressable).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr`    if `instance` is out of range.
 * @return `k_ra8_err_invalid_arg` if `flash_addr` exceeds the 2^24-byte
 *         window a 3-byte JEDEC address can reach.
 * @return `k_ra8_err_hw_timeout`  if the WREN / SE command never retires.
 * @return `k_ra8_err_timeout`     if WIP does not clear.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_flash_erase_sector(uint8_t instance, uint32_t flash_addr);

/**
 * @brief Read the flash Status Register (opcode 0x05).
 *
 * @param[in]  instance   xSPI instance.
 * @param[out] out_status Status-register value (WIP is bit 0).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if pointer is NULL or instance out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_flash_read_status(uint8_t instance, uint8_t* out_status);

/**
 * @brief Read the JEDEC ID (opcode 0x9F), returning 24 bits in a uint32_t.
 *
 * @details
 * The returned word packs the three JEDEC bytes as:
 * `(manufacturer << 16) | (mem_type << 8) | capacity`. Compatible
 * with Macronix MX25, Winbond W25, Renesas AT25, and ISSI IS25 parts.
 *
 * @param[in]  instance xSPI instance.
 * @param[out] out_id   24-bit JEDEC ID in a 32-bit word.
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if pointer is NULL or instance out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_flash_read_id(uint8_t instance, uint32_t* out_id);

/**
 * @typedef ra8_xspi_event_fn_t
 * @brief xSPI event callback.
 */
typedef void (*ra8_xspi_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Tear down the xSPI instance (disable + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_deinit(uint8_t instance);

/**
 * @brief Read the xSPI COMSTT busy/error mask.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_get_status(uint8_t instance, uint32_t* out_mask);

/**
 * @brief Clear transfer-complete / error bits in INTS via INTC.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_clear_status(uint8_t instance, uint32_t mask);

/**
 * @brief Attach a completion callback.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xspi_attach_handler(uint8_t instance, ra8_xspi_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an xSPI event -- snapshot INTS + fire callback.
 *
 * @details
 * Called from the xSPI transfer-complete / error ISR (HUM Ch 49
 * "xSPI", p 2581) to snapshot INTS and invoke the registered handler.
 * Out-of-range ``instance`` and unattached slots are silently dropped
 * so spurious IRQs are harmless.
 *
 * @param[in] instance xSPI controller instance index (0 or 1).
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``instance`` < 2.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post INTS latch is left for the caller to ack via
 *       ``ra8_xspi_clear_status``.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra8_xspi_dispatch(uint8_t instance);

/**
 * @brief Put an xSPI instance into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_enter_stop(uint8_t instance);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_exit_stop(uint8_t instance);

/**
 * @brief Enable XIP (execute-in-place) memory-mapped read mode.
 *
 * @details
 * Programs ``CMCTLCH[0/1]`` with the supplied XiP enter/exit codes,
 * sets ``BMCTL0 = 0x55`` to map the target window read-only on the
 * system bus, and arms ``CMCTLCH.XIPEN``. Tracks FSP
 * ``R_OSPI_B_XipEnter`` / ``r_ospi_b_xip(true)``. Cite: HUM Ch 44
 * "Octal Serial Peripheral Interface (OSPI)" p 2986.
 *
 * @param[in] instance    xSPI instance (0 or 1).
 * @param[in] enter_code  Vendor-specific XIP enter byte (e.g. 0xA5).
 * @param[in] exit_code   Vendor-specific XIP exit byte (e.g. 0x5A).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 *
 * @pre Driver has been initialized via `ra8_xspi_init()`.
 * @pre A read command set has been programmed in `CMCFGCS[n]`.
 * @post `BMCTL0` reads as `k_ra8_xspi_bmctl0_read_only`.
 * @post `CMCTLCH[0/1].XIPEN` is set.
 *
 * @note Not thread-safe; call from a single bring-up context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_xip_enter(uint8_t instance, uint8_t enter_code, uint8_t exit_code);

/**
 * @brief Disable XIP mode and restore command-only access.
 *
 * @details
 * Clears ``CMCTLCH[0/1].XIPEN`` and zeroes out the XIP enter/exit
 * codes, then resets ``BMCTL0`` so further direct-command transfers
 * are not blocked by an active memory-mapped path. Tracks FSP
 * ``R_OSPI_B_XipExit`` / ``r_ospi_b_xip(false)``.
 *
 * @param[in] instance xSPI instance (0 or 1).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 *
 * @pre Driver is open and previously entered XIP via `ra8_xspi_xip_enter()`.
 * @post `CMCTLCH[0/1] == 0` and XIPEN is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_xip_exit(uint8_t instance);

/**
 * @brief Enable or disable memory-mapped (XIP) read mode.
 *
 * @details
 * Programs the ``CMCFGCS[0]`` read-command slot with the supplied
 * opcode + address-byte width, mirrors FSP ``r_ospi_b_xip(true/false)``
 * by toggling the per-channel ``CMCTLCH.XIPEN`` bits, and arms the
 * read-only system-bus mapping via ``BMCTL0``. The matching ``write``
 * command is left at zero because XiP windows are read-only by design.
 * Cite: HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986.
 *
 * @param[in] instance   xSPI instance (0 or 1).
 * @param[in] enable     true to enable XIP, false to tear it down.
 * @param[in] read_cmd   JEDEC read opcode (e.g. 0xEB / 0xEE / 0x0B).
 * @param[in] addr_bytes Address byte count (3 or 4).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 * @return `k_ra8_err_invalid_arg` if `addr_bytes` is neither 3 nor 4.
 *
 * @pre Driver has been initialized via `ra8_xspi_init()`.
 * @post On enable: ``CMCFGCS`` slot 0 carries (read_cmd, addr_bytes)
 *       and ``CMCTLCH[0/1].XIPEN`` is set.
 * @post On disable: ``CMCTLCH[0/1].XIPEN`` is cleared and
 *       ``BMCTL0 == k_ra8_xspi_bmctl0_read_write``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xspi_set_xip_mode(uint8_t instance, bool enable, uint8_t read_cmd, uint8_t addr_bytes);

/**
 * @brief Enable or disable double-data-rate (DDR / DTR) link mode.
 *
 * @details
 * Sets / clears ``LIOCFGCS[0].DDREN`` (HUM Ch 44 p 2986). Callers
 * must already have selected an appropriate protocol mode (typically
 * ``k_ra8_xspi_lio_8d8d8d``) via ``ra8_xspi_init()``.
 *
 * @param[in] instance xSPI instance (0 or 1).
 * @param[in] enable   true: DDR; false: SDR.
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 *
 * @pre Driver is open.
 * @post ``LIOCFGCS[0]`` reflects the requested DDREN state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_set_dtr_mode(uint8_t instance, bool enable);

/**
 * @brief Run the data-strobe (DQS) calibration sequence.
 *
 * @details
 * Project-native equivalent of FSP ``R_OSPI_B_AutoCalibrate`` for
 * target 0. Programmes the ``CCCTLCS[0..7]`` calibration descriptor
 * with FSP-derived defaults, sets ``CAEN`` to start the controller's
 * automatic phase-scan state machine, then polls until the controller
 * clears ``CAEN`` itself or the bounded spin budget expires. On
 * success the controller updates ``WRAPCFG.DSSFTCS0`` with the
 * winning sample-shift code. Cite: HUM Ch 44 p 2986.
 *
 * @param[in] instance xSPI instance (0 or 1).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 * @return `k_ra8_err_hw_timeout` if calibration never completes.
 *
 * @pre Driver was initialized in an OPI / 8D mode.
 * @post ``CCCTL0.CAEN`` is 0.
 * @post On host builds the calibration step is a no-op that returns
 *       ``k_ra8_ok`` (no real DQS line to phase-scan).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_calibrate_dqs(uint8_t instance);

/**
 * @brief Suspend a long-running flash erase / program operation.
 *
 * @details
 * Issues the JEDEC suspend opcode (``0x75``) via the manual-command
 * engine, polls ``CMDCMP``, and lets the SPI device pause its
 * in-flight erase or program so that interactive reads can be served.
 * Cite: HUM Ch 44 p 2986.
 *
 * @param[in] instance xSPI instance (0 or 1).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 * @return `k_ra8_err_hw_timeout` if CMDCMP never asserts.
 *
 * @pre Driver is open.
 * @post Flash device is in suspended state; ``ra8_xspi_resume()`` is
 *       required before further write traffic.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_suspend(uint8_t instance);

/**
 * @brief Resume a previously suspended erase / program.
 *
 * @details
 * Issues the JEDEC resume opcode (``0x7A``) via the manual-command
 * engine and polls ``CMDCMP`` for completion. Pairs 1:1 with
 * ``ra8_xspi_suspend()``.
 *
 * @param[in] instance xSPI instance (0 or 1).
 *
 * @return `k_ra8_ok` on success.
 * @return `k_ra8_err_null_ptr` if `instance` is out of range.
 * @return `k_ra8_err_hw_timeout` if CMDCMP never asserts.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_resume(uint8_t instance);

/**
 * @brief Issue the JEDEC software-reset pair (RSTEN 0x66 + RST 0x99).
 *
 * @details
 * Drives the two-step JEDEC reset sequence used by every modern SPI
 * NOR vendor (ISSI IS25LX, Macronix MX25, Winbond W25, Micron MT25Q):
 * first ``RSTEN`` (Reset Enable, opcode 0x66) and then ``RST`` (Reset,
 * opcode 0x99). After ``RST`` returns the device must be left
 * undisturbed for tRPH (~50 us) before the next CS assertion; the
 * caller is responsible for that wait (we use ``ra8_delay_ms(1)``).
 *
 * The two protocol modes the driver exercises are:
 *   - 1S-1S-1S (``cmd_bytes = 1``): single 8-bit opcode on DQ0 only.
 *     This is the form the chip will recognise after a power-on reset.
 *   - 8D-8D-8D (``cmd_bytes = 2``): two 8-bit opcodes shipped as
 *     ``opcode | (~opcode << 8)`` on all eight DQ lines on both clock
 *     edges. This is the form the chip will recognise if a previous
 *     boot left it in OPI mode and the SoC was warm-reset (POR is the
 *     only thing that clears the volatile-config-register choice of
 *     protocol on IS25LX512M).
 *
 * To recover from an unknown protocol state at cold-or-warm boot the
 * board bring-up should call this function once in each protocol mode:
 * one of the two will be the no-op (the chip ignores the wrong-shape
 * opcode) and the other will actually reset the chip back to 1S-1S-1S.
 *
 * Cite: IS25LX512M datasheet Ch 8.20 "Reset Enable (RSTEN)" and
 *       Ch 8.21 "Reset (RST)" (p 39); HUM Ch 44 p 2986 for the manual-
 *       command engine fields used to send the opcodes.
 *
 * @param[in] instance  xSPI instance (0 or 1).
 * @param[in] cmd_bytes Number of opcode bytes to ship per command:
 *                      ``1`` for 1S-1S-1S SPI, ``2`` for 8D-8D-8D OPI.
 *
 * @return ``k_ra8_ok`` on success.
 * @return ``k_ra8_err_null_ptr`` if ``instance`` is out of range.
 * @return ``k_ra8_err_invalid_arg`` if ``cmd_bytes`` is neither 1 nor 2.
 * @return ``k_ra8_err_hw_timeout`` if the controller never raises
 *         CMDCMP for either RSTEN or RST.
 *
 * @pre Driver has been initialized via ``ra8_xspi_init()`` in a
 *      protocol mode that matches ``cmd_bytes``.
 * @pre Caller will leave the chip undisturbed for tRPH (>=50 us)
 *      after this returns before issuing the next command.
 *
 * @post The flash device, if it recognised the opcode pair, has
 *       reverted its volatile-config registers to power-on defaults
 *       and is back in 1S-1S-1S extended SPI.
 *
 * @note Not thread-safe. Intended to be called only during bring-up.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xspi_software_reset(uint8_t instance, uint8_t cmd_bytes);

#ifdef __cplusplus
}
#endif
