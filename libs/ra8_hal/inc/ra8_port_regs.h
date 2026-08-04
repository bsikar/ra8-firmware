/**
 * @file ra8_port_regs.h
 * @brief IOPORT (GPIO) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The RA8D2 IOPORT controls up to 15 16-bit ports (`PORT0..PORT14`).
 * Each port occupies a 32-byte window in memory starting at
 * `0x40400000` with stride `0x20`:
 *
 * @code
 *   PORT0 @ 0x40400000
 *   PORT1 @ 0x40400020
 *   ...
 *   PORT14 @ 0x404001C0
 * @endcode
 *
 * The first 16 bytes of each window hold four 32-bit control
 * registers (PCNTR1..PCNTR4) and the remaining 16 bytes are reserved.
 *
 * ## Register map (per port)
 *
 * | Offset | Name    | RW | Layout                                     |
 * |-------:|---------|----|--------------------------------------------|
 * | 0x00   | PCNTR1  | RW | `{ PODR[31:16], PDR[15:0] }`                |
 * | 0x04   | PCNTR2  | R  | `{ EIDR[31:16], PIDR[15:0] }`               |
 * | 0x08   | PCNTR3  | W  | `{ PORR[31:16], POSR[15:0] }`               |
 * | 0x0C   | PCNTR4  | RW | `{ EORR[31:16], EOSR[15:0] }`               |
 *
 * `PDR` sets pin direction (0 = input, 1 = output). `PODR` sets or
 * reads the output latch. `PIDR` returns the current pin state.
 * `POSR` and `PORR` are the atomic set and atomic-reset registers:
 * writing a 1 bit to POSR drives that pin high, writing a 1 bit to
 * PORR drives it low, and writing a 0 to either leaves the bit
 * alone. This lets us toggle a single pin without a read-modify-write.
 *
 * ## Atomic sets beat RMW
 *
 * Always prefer POSR / PORR over PODR when the intent is "set this
 * one bit" -- otherwise two ISRs editing the same port race on the
 * read-modify-write.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_bit_constants.h"
#include "ra8_port_constants.h"

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_port0_base_addr  = 0x40400000UL, /**< RA8 port0 base address.       */
  k_ra8_port1_base_addr  = 0x40400020UL, /**< RA8 port1 base address.       */
  k_ra8_port2_base_addr  = 0x40400040UL, /**< RA8 port2 base address.       */
  k_ra8_port3_base_addr  = 0x40400060UL, /**< RA8 port3 base address.       */
  k_ra8_port4_base_addr  = 0x40400080UL, /**< RA8 port4 base address.       */
  k_ra8_port5_base_addr  = 0x404000A0UL, /**< RA8 port5 base address.       */
  k_ra8_port6_base_addr  = 0x404000C0UL, /**< RA8 port6 base address.       */
  k_ra8_port7_base_addr  = 0x404000E0UL, /**< RA8 port7 base address.       */
  k_ra8_port8_base_addr  = 0x40400100UL, /**< RA8 port8 base address.       */
  k_ra8_port9_base_addr  = 0x40400120UL, /**< RA8 port9 base address.       */
  k_ra8_port10_base_addr = 0x40400140UL, /**< RA8 port10 base address.      */
  k_ra8_port11_base_addr = 0x40400160UL, /**< RA8 port11 base address.      */
  k_ra8_port12_base_addr = 0x40400180UL, /**< RA8 port12 base address.      */
  k_ra8_port13_base_addr = 0x404001A0UL, /**< RA8 port13 base address.      */
  k_ra8_port14_base_addr = 0x404001C0UL, /**< RA8 port14 base address.      */
  k_ra8_port_stride      = 0x20UL,       /**< Bytes between adjacent ports. */
} ra8_port_addr_t;

/* =============================================================================
 * Register layout (16 bytes per port)
 * =============================================================================
 */

/**
 * @struct r_port_regs_t
 * @brief Per-port IOPORT register window.
 *
 * @details
 * Four 32-bit registers back-to-back. The full 32-byte port slot
 * leaves 16 bytes of reserved space after `PCNTR4`; we only model the
 * used prefix.
 *
 * @invariant `sizeof(r_port_regs_t) == 16`.
 */
typedef struct {
  /**
   * @brief PCNTR1 -- `{ PODR[31:16], PDR[15:0] }`.
   *
   * Write the low half to configure direction (0 = input, 1 = output),
   * write the high half to drive the output latch.
   */
  volatile uint32_t PCNTR1;
  /**
   * @brief PCNTR2 -- `{ EIDR[31:16], PIDR[15:0] }` (read-only).
   *
   * Low half reads live pin state. High half is event-input data,
   * used when the port is routed through the ELC event matrix.
   */
  volatile uint32_t PCNTR2;
  /**
   * @brief PCNTR3 -- `{ PORR[31:16], POSR[15:0] }` (write-only).
   *
   * Atomic set / atomic clear. Writing a 1 bit to POSR drives the
   * pin high; writing a 1 bit to PORR drives it low; zero bits are
   * ignored. This is race-free and should be used in preference to
   * PODR RMW.
   */
  volatile uint32_t PCNTR3;
  /**
   * @brief PCNTR4 -- `{ EORR[31:16], EOSR[15:0] }`.
   *
   * Event-driven output set / reset. Used with the ELC.
   */
  volatile uint32_t PCNTR4;
} r_port_regs_t;

/** @brief Expected byte size of the per-port register window. */
typedef enum : uint8_t {
  k_ra8_port_regs_bytes = 16U, /**< RA8 port registers bytes. */
} ra8_port_regs_size_t;

static_assert(sizeof(r_port_regs_t) == (size_t)k_ra8_port_regs_bytes,
              "r_port_regs_t must be exactly 16 bytes");

/* =============================================================================
 * Per-port accessor functions
 * =============================================================================
 */

/**
 * @brief Get the PORT register window for a given port index.
 *
 * @param[in] port `ra8_port_t` value (0..`k_ra8_port_max`).
 * @return Volatile pointer to the port's register window, or `nullptr`
 *         if `port` is out of range.
 *
 * @note The pointer is computed directly from the base address and
 *       stride, so the function is a single add + cast in release
 *       builds.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port(ra8_port_t port)
{
  if ((uint8_t)port > k_ra8_port_max) {
    return nullptr;
  }
  const uintptr_t addr = k_ra8_port0_base_addr + ((uintptr_t)port * k_ra8_port_stride);
  return (volatile r_port_regs_t*)addr;
}

/** @brief Get PORT0 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port0(void)
{
  return (volatile r_port_regs_t*)k_ra8_port0_base_addr;
}
/** @brief Get PORT1 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port1(void)
{
  return (volatile r_port_regs_t*)k_ra8_port1_base_addr;
}
/** @brief Get PORT2 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port2(void)
{
  return (volatile r_port_regs_t*)k_ra8_port2_base_addr;
}
/** @brief Get PORT3 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port3(void)
{
  return (volatile r_port_regs_t*)k_ra8_port3_base_addr;
}
/** @brief Get PORT4 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port4(void)
{
  return (volatile r_port_regs_t*)k_ra8_port4_base_addr;
}
/** @brief Get PORT5 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port5(void)
{
  return (volatile r_port_regs_t*)k_ra8_port5_base_addr;
}
/** @brief Get PORT6 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port6(void)
{
  return (volatile r_port_regs_t*)k_ra8_port6_base_addr;
}
/** @brief Get PORT7 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port7(void)
{
  return (volatile r_port_regs_t*)k_ra8_port7_base_addr;
}
/** @brief Get PORT8 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port8(void)
{
  return (volatile r_port_regs_t*)k_ra8_port8_base_addr;
}
/** @brief Get PORT9 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port9(void)
{
  return (volatile r_port_regs_t*)k_ra8_port9_base_addr;
}
/** @brief Get PORT10 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port10(void)
{
  return (volatile r_port_regs_t*)k_ra8_port10_base_addr;
}
/** @brief Get PORT11 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port11(void)
{
  return (volatile r_port_regs_t*)k_ra8_port11_base_addr;
}
/** @brief Get PORT12 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port12(void)
{
  return (volatile r_port_regs_t*)k_ra8_port12_base_addr;
}
/** @brief Get PORT13 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port13(void)
{
  return (volatile r_port_regs_t*)k_ra8_port13_base_addr;
}
/** @brief Get PORT14 registers. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_port_regs_t* ra8_port14(void)
{
  return (volatile r_port_regs_t*)k_ra8_port14_base_addr;
}

/* =============================================================================
 * Bit-helper enums for PCNTR1/2/3/4 halves
 * =============================================================================
 */

/**
 * @enum ra8_pcntr_half_t
 * @brief Bit shifts for the halves inside each PCNTR register.
 */
typedef enum : uint8_t {
  k_ra8_pcntr_low_half_shift  = 0U,  /**< PDR/PIDR/POSR/EOSR half.  */
  k_ra8_pcntr_high_half_shift = 16U, /**< PODR/EIDR/PORR/EORR half. */
} ra8_pcntr_half_t;

#ifdef __cplusplus
}
#endif
