/**
 * @file sim_console.h
 * @brief Emulator text-console surfaces: UART echo, ITM/SWO echo, escapes
 *
 * @details
 * The two text endpoints board_sim surfaces on stdout plus the CLI string
 * decoder that feeds them:
 *
 *  - `[uart] SCIn:` lines -- the SCI_B model's transmitted bytes, assembled
 *    into lines by the TX sink installed via board_periph_sci_set_tx_sink().
 *  - `[itm] ...` lines -- the Arm CoreSight ITM stimulus-port 0 bytes ra8_log
 *    emits, seeded "ready" in PPB RAM and echoed by a memory-write hook (on
 *    real hardware these leave through the SWO pin to the J-Link console).
 *  - decode_escapes() -- turns a C-style escaped shell argument (`--input`,
 *    `--keys`, `--usb-in`) into raw bytes.
 *
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum itm_addr_t
 * @brief ITM / debug PPB addresses and TrustZone alias constants.
 *
 * @details The ITM stimulus/control words the seed-and-echo model touches,
 * the DEMCR trace-enable word (shared with the DWT cycle-counter model), and
 * the SAU / Non-secure-alias constants the TrustZone and memory-map code
 * reads from this same historical group.
 *
 * @invariant Values are Armv8-M / RA8D2 architectural addresses.
 * @see itm_seed_ready()  Seeds the enable bits at these addresses.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_itm_stim0_addr = 0xE0000000UL, /**< ITM stimulus port 0 (byte FIFO).         */
  k_itm_tcr_addr   = 0xE0000E80UL, /**< ITM Trace Control Register (ITMENA).     */
  k_itm_tenr_addr  = 0xE0000E00UL, /**< ITM Trace Enable Register (port 0).      */
  k_scb_demcr_addr = 0xE000EDFCUL, /**< Debug Exception + Monitor Control.       */
  k_sau_type_addr  = 0xE000EDD4UL, /**< SAU_TYPE (SREGION = implemented regs).   */
  k_ns_sram2_base  = 0x32100000UL, /**< SRAM2 Non-secure alias (bit[28]=1).      */
  k_ns_alias_bit   = 0x10000000UL, /**< IDAU bit[28]: NS alias of a Secure addr. */
} itm_addr_t;

/**
 * @enum itm_bits_t
 * @brief ITM enable-bit values and line sizing for the stimulus echo.
 *
 * @details The specific bits itm_seed_ready() plants so ra8_log's readiness
 * checks pass, plus the echo line-buffer cap and the SAU region-count seed
 * used by the TrustZone boot path.
 *
 * @invariant Values match the Armv8-M ITM / SAU register field definitions.
 * @see on_itm_stim_write()  Buffers bytes up to ::k_itm_line_max.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_itm_line_max     = 240U,        /**< Max chars buffered before a forced flush. */
  k_itm_tcr_itmena   = 0x00000001U, /**< TCR bit 0: ITM global enable.             */
  k_itm_tenr_port0   = 0x00000001U, /**< TENR bit 0: stimulus port 0 enabled.      */
  k_itm_stim_ready   = 0x00000001U, /**< Non-zero STIM0 read = FIFO ready.         */
  k_scb_demcr_trcena = 0x01000000U, /**< DEMCR bit 24: trace subsystem enable.     */
  k_sau_type_regs    = 0x00000008U, /**< SAU_TYPE.SREGION: M85 implements 8.       */
} itm_bits_t;

/**
 * @enum console_cfg_t
 * @brief Console-UART presentation sizing.
 *
 * @details Line-buffer capacity for the pretty `[uart] SCIn:` echo; also the
 * sizing every CLI byte-injection buffer (`--input` / `--keys` / `--usb-in`)
 * borrows.
 *
 * @invariant One line plus its NUL always fits the buffer.
 * @see console_tx_sink()  Fills the line up to this cap.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_uart_line_max = 256U, /**< Pretty-print line buffer for the [uart] prefix. */
} console_cfg_t;

/**
 * @brief Seed ITM "ready" bits and arm the stimulus-port echo hook.
 *
 * @details
 * On hardware a debugger sets DEMCR.TRCENA and enables the ITM; with none
 * attached those PPB registers read zero and ra8_log drops every byte.
 * board_sim maps the PPB as plain RAM, so this writes the enable bits
 * (DEMCR.TRCENA, ITM TCR.ITMENA, TENR port-0, a ready STIM0) and installs the
 * UC_HOOK_MEM_WRITE echo on the STIM0 word so each emitted line prints as
 * `[itm] <line>` and lands on the board_console ITM channel. The firmware
 * only ever reads these registers, so the seed persists for the run.
 *
 * @param[in,out] uc Initialised Unicorn engine with the PPB region mapped.
 * @return Nothing.
 *
 * @pre @p uc has the PPB region (0xE0000000) mapped as RAM.
 * @pre Called once before emulation starts.
 * @post The ITM enable bits are set and the stimulus echo hook is armed.
 * @post ra8_log's readiness check returns true for the run.
 * @note Not thread-safe; call during single-threaded setup.
 * @see console_tx_sink()  The UART-side echo counterpart.
 * @since 0.1.0
 */
RA8_PRIV void sim_console_install(uc_engine* uc);

/**
 * @brief Flush the pending [uart] line to stdout with its channel prefix.
 *
 * @details Terminates and prints the accumulated line as `[uart] SCIn: ...`;
 * a no-op when nothing is buffered. The run-end report calls this so bytes
 * emitted without a trailing newline still surface.
 *
 * @param[in] channel SCI channel number used in the printed prefix.
 * @return Nothing.
 * @pre The console TX sink has been collecting bytes (or the buffer is empty).
 * @pre stdout is writable.
 * @post The line buffer is empty.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see console_tx_sink()  Fills the buffer this flushes.
 * @since 0.1.0
 */
RA8_PRIV void console_flush_line(uint8_t channel);

/**
 * @brief SCI TX sink: print each transmitted byte (prefixed line + raw mirror).
 *
 * @details
 * Installed via board_periph_sci_set_tx_sink(). Printable bytes accumulate
 * into a line that is flushed -- with an @c [uart] SCIn: prefix -- on newline
 * or when the buffer fills, so console output reads cleanly in the log; CR is
 * dropped from the pretty line.
 *
 * @param[in] channel SCI channel that transmitted the byte.
 * @param[in] byte    The transmitted data byte.
 * @return Nothing.
 * @pre The board_periph SCI model routes TX bytes here.
 * @pre stdout is writable.
 * @post The byte is buffered or the completed line has been printed.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see console_flush_line()  Forces out a partial line.
 * @since 0.1.0
 */
RA8_PRIV void console_tx_sink(uint8_t channel, uint8_t byte);

/**
 * @brief Decode a C-style escaped --input string into a raw byte buffer.
 *
 * @details
 * Translates @c \\n / @c \\r / @c \\t / @c \\0 / @c \\\\ in @p in so a shell
 * argument can carry the line endings a console example expects (e.g.
 * @c --input "ping\r\n"); any other character (including an unrecognised
 * escape's backslash) is copied verbatim. Bounded by @p cap; never overruns.
 *
 * @param[in]  in  NUL-terminated source string.
 * @param[out] out Destination byte buffer.
 * @param[in]  cap Capacity of @p out in bytes.
 * @return Number of bytes written to @p out.
 * @retval 0 @p in was empty (or @p cap was 0).
 * @pre @p in is NUL-terminated and @p out holds at least @p cap bytes.
 * @pre @p cap bounds the write (enforced by the loop condition).
 * @post At most @p cap bytes of @p out are written.
 * @note Pure transformation; thread-safe.
 * @since 0.1.0
 */
RA8_PRIV uint32_t decode_escapes(const char* in, uint8_t* out, uint32_t cap);

/**
 * @brief Reset the in-flight ITM line (warm-reboot support).
 *
 * @details Drops any partially-assembled `[itm]` line so a rebooted firmware
 * starts with a clean stimulus stream; the multi-channel board_console store
 * is reset separately by the reboot path.
 *
 * @return Nothing.
 * @pre A warm reboot is re-initialising the console surfaces.
 * @post The pending ITM line buffer is empty.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see sim_console_install()  Arms the echo this resets.
 * @since 0.1.0
 */
RA8_PRIV void sim_console_reset(void);

#ifdef __cplusplus
}
#endif
