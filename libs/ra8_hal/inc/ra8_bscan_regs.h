/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_bscan_regs.h
 * @brief JTAG / IEEE-1149.1 Boundary Scan TAP constants for the RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The RA8D2 implements an IEEE-1149.1 (JTAG) Test Access Port for
 * manufacturing-test boundary scan. Per HUM Ch 50 "Boundary Scan",
 * p 3257-3262, the four TAP registers --
 *
 *   - JTIR  : Instruction Register (4-bit, reset value 0xE = BYPASS)
 *   - JTIDR : ID Code Register     (32-bit, fixed 0x085D_A447)
 *   - JTBPR : Bypass Register      (1-bit, undefined after reset)
 *   - JTBSR : Boundary Scan Register (BSDL-defined, undefined after reset)
 *
 * -- are NOT memory-mapped into the CPU's address space. They are
 * accessed exclusively through the TDI/TDO serial chain by an external
 * JTAG controller while the device is held in reset (RES driven low).
 * The HUM is explicit (Ch 50.2.3, p 3259): "The JTBPR register cannot
 * be read from or written to by the CPU." The same applies to JTIR /
 * JTIDR / JTBSR.
 *
 * Consequently this "regs" header does not declare a register struct or
 * accessor function (there is nothing the CPU can read at a known
 * address). It instead exposes the JTAG-side constants -- instruction
 * opcodes, expected ID code, reset values, register widths -- that
 * firmware needs to validate / report the TAP configuration to a host
 * tool that drives the actual scan vectors over the four JTAG pins.
 *
 * The driver in ``ra8_bscan.c`` keeps a small CPU-side state object
 * (initialized / not-initialized, last-instruction-loaded if the host
 * tool tells us, expected ID code) so a unit test can exercise the
 * lifecycle and constant-equivalence checks even though it cannot
 * exercise an actual scan.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Register widths
 * =============================================================================
 */

/**
 * @enum ra8_bscan_width_t
 * @brief Bit-widths of the four JTAG TAP registers.
 *
 * @details
 * Sourced from HUM Table 50.3 "Boundary scan registers" (p 3258) and
 * the bit-position diagrams of JTIR (p 3258) and JTIDR (p 3258-3259).
 * JTBSR is BSDL-defined and varies per package; the device ships a
 * BSDL file separately and the scan length is not fixed by the chip.
 */
typedef enum : uint8_t {
  k_ra8_bscan_jtir_bits  = 4U,  /**< JTIR is 4 bits wide (TS[3:0]).        */
  k_ra8_bscan_jtidr_bits = 32U, /**< JTIDR is 32 bits wide (DID[31:0]).    */
  k_ra8_bscan_jtbpr_bits = 1U,  /**< JTBPR is a single bit shift register. */
} ra8_bscan_width_t;

/* =============================================================================
 * Reset values
 * =============================================================================
 */

/**
 * @enum ra8_bscan_reset_t
 * @brief Power-on-reset values of the JTAG registers.
 *
 * @details
 * From HUM Table 50.3 (p 3258). JTBPR / JTBSR are explicitly listed as
 * "Undefined" after reset and so are not modelled as constants here.
 * JTIR resets to ``0xE`` -- which is in the "Reserved" rows of the
 * TS[3:0] decode table (p 3258), one nibble below the BYPASS opcode.
 * The TAP controller forces the BYPASS path on reset regardless of the
 * raw 0xE bit pattern; the test harness only needs the literal reset
 * value when reading back the register through Shift-IR for
 * verification.
 */
typedef enum : uint32_t {
  k_ra8_bscan_jtir_reset  = 0x0EU,        /**< JTIR reset value.      */
  k_ra8_bscan_jtidr_reset = 0x085DA447UL, /**< JTIDR fixed device ID. */
} ra8_bscan_reset_t;

/* =============================================================================
 * Instruction Register opcodes (JTIR.TS[3:0])
 * =============================================================================
 */

/**
 * @enum ra8_bscan_instr_t
 * @brief JTIR Test Bit Set (TS[3:0]) command codes.
 *
 * @details
 * From the JTIR register description in HUM Ch 50.2.1 (p 3258). All
 * other 4-bit values are reserved. The driver validates that any
 * caller-supplied "current instruction" is one of these named values
 * before recording it in its state object.
 */
typedef enum : uint8_t {
  k_ra8_bscan_instr_extest         = 0x0U, /**< External test mode.        */
  k_ra8_bscan_instr_sample_preload = 0x1U, /**< Sample / preload.          */
  k_ra8_bscan_instr_idcode         = 0x3U, /**< Read JTIDR (Renesas code). */
  k_ra8_bscan_instr_clamp          = 0x5U, /**< Clamp output pins.         */
  k_ra8_bscan_instr_highz          = 0x6U, /**< High-impedance output.     */
  k_ra8_bscan_instr_bypass         = 0xFU, /**< Bypass register selected.  */
} ra8_bscan_instr_t;

/* =============================================================================
 * TAP controller pin identifiers (informational)
 * =============================================================================
 */

/**
 * @enum ra8_bscan_pin_t
 * @brief JTAG TAP pin role identifiers.
 *
 * @details
 * From HUM Table 50.2 "Boundary scan I/O pins" (p 3257). These are
 * exposed as named constants so debug logs and BSDL-driven host
 * tooling can reference the pins by symbolic role rather than by
 * package pin number (which differs per package variant).
 *
 * @note The RA8D2 does not implement the optional TRST pin (HUM
 *       Ch 50.1 note, p 3257), so reset of the TAP must be performed
 *       by holding TMS=1 for at least 5 TCK clocks.
 */
typedef enum : uint8_t {
  k_ra8_bscan_pin_tck   = 0U, /**< Test clock input.   */
  k_ra8_bscan_pin_tms   = 1U, /**< Test mode select.   */
  k_ra8_bscan_pin_tdi   = 2U, /**< Test data input.    */
  k_ra8_bscan_pin_tdo   = 3U, /**< Test data output.   */
  k_ra8_bscan_pin_count = 4U, /**< Number of TAP pins. */
} ra8_bscan_pin_t;

#ifdef __cplusplus
}
#endif
