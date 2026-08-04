/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file ra8_pcntr.h
 * @brief CPU1-safe combined direction+level PORT primitive over PCNTR1/PCNTR2
 * @ingroup grp_hal_system
 *
 * @details
 * A deliberately tiny, header-only GPIO primitive built directly on the per-port
 * PCNTR registers (`ra8_port_regs.h`). It exists so the secondary Cortex-M33
 * (CPU1) image -- which is built `-ffreestanding` and links **no** `ra8_hal`
 * (its only project include path is `libs/ra8_core/inc`, plus `libs/ra8_hal/inc`
 * for the two freestanding-clean PORT headers) -- can drive an on-chip pin
 * through a HAL call instead of hand-rolling raw MMIO.
 *
 * ## Why this is separate from `ra8_port_utils.h`
 *
 * The high-level `ra8_gpio_*` driver (`gpio.c`) claims each pin through the
 * pin-validator, logs through `ra8_log`, and programmes direction via the PFS
 * register bank. Every one of those pulls infrastructure the freestanding M33
 * image does not link. This primitive intentionally depends on **nothing** but
 * `ra8_port_regs.h` (the register layout + the `ra8_port()` accessor) and the
 * typed enums in `ra8_port_constants.h` -- all of which compile clean under
 * `-ffreestanding -fno-builtin` -- so it links into the CPU1 ELF unchanged, and
 * it is host-testable on the `ra8_fake_mmap` peripheral-RAM backing exactly like
 * the rest of the PORT layer.
 *
 * ## Register model
 *
 * Each port's PCNTR1 packs the direction latch `PDR` in bits [15:0] and the
 * output latch `PODR` in bits [31:16]; PCNTR2 returns the live pin state `PIDR`
 * in bits [15:0] (read-only). See `ra8_port_regs.h` and HUM Ch 20 "I/O Ports".
 *
 * @code{.c}
 * // Hold PORT6 pin 0 (EK-RA8D2 LED1, BLUE / P600) an output and drive it high:
 * (void)ra8_pcntr_set_output(k_ra8_port_6, k_ra8_pin_0, k_ra8_level_high);
 *
 * ra8_level_t sw1 = k_ra8_level_high;
 * (void)ra8_pcntr_read(k_ra8_port_0, k_ra8_pin_9, &sw1); // SW1 (P009), active-low
 * @endcode
 *
 * ## Combined direction+level, and why read-modify-write
 *
 * `ra8_pcntr_set_output()` sets BOTH direction (output) and level in one call --
 * the combined-idiom the CPU1 images need -- via a read-modify-write of PCNTR1
 * that preserves every other pin on the port. A bare full-word store to PCNTR1
 * (the ad-hoc form this primitive replaces) would silently force all sibling
 * pins to input with a cleared output latch; the RMW here does not, so the
 * primitive is safe to reuse on a shared port while remaining a drop-in
 * substitute at the single-pin CPU1 call sites (their ports carry no other
 * driven pins, so the observable pin state is identical).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"

/**
 * @brief Hold a pin an output and drive it to a level, via PCNTR1 (combined).
 *
 * @details
 * Read-modify-writes the port's PCNTR1 so the addressed pin's direction latch
 * (`PDR`) becomes output (1) and its output latch (`PODR`) reflects @p level,
 * while every other pin's `PDR`/`PODR` bit is preserved. This is the single-call
 * "set direction and level together" primitive the freestanding CPU1 images
 * need; it performs one PCNTR1 read and one PCNTR1 write and touches nothing
 * else (no PFS, no pin-validator, no logging).
 *
 * @param[in] port  Port index (`k_ra8_port_0` .. `k_ra8_port_max`).
 * @param[in] pin   Pin index within the port (`k_ra8_pin_0` .. `k_ra8_pin_max`).
 * @param[in] level Target output level (`k_ra8_level_low` / `k_ra8_level_high`).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                    Pin configured as output and driven to @p level.
 * @retval k_ra8_err_gpio_invalid_port @p port is out of range.
 * @retval k_ra8_err_gpio_invalid_pin  @p pin is out of range.
 * @retval k_ra8_err_hw_unmapped       The port has no register window (host fake only).
 *
 * @pre The IOPORT module clock is on (always-on after reset on the RA8D2).
 * @pre @p pin powers up routed to PORT (no PFS peripheral-mux setup is done here).
 * @post On success, `PCNTR1.PDR[pin] == 1` (output) and `PCNTR1.PODR[pin]`
 *       equals @p level.
 * @post On success, no other pin's `PDR`/`PODR` bit on @p port is modified.
 *
 * @note Not thread-safe and not ISR-safe: the PCNTR1 read-modify-write is not
 *       atomic, so a concurrent update to another pin of the same port can race.
 *       Use during single-threaded init or with the relevant interrupts masked.
 * @since 0.1.0
 */
[[nodiscard]] static inline ra8_err_t
ra8_pcntr_set_output(ra8_port_t port, ra8_pin_t pin, ra8_level_t level)
{
  if ((uint8_t)port > (uint8_t)k_ra8_port_max) {
    return k_ra8_err_gpio_invalid_port;
  }
  if ((uint8_t)pin > (uint8_t)k_ra8_pin_max) {
    return k_ra8_err_gpio_invalid_pin;
  }

  volatile r_port_regs_t* port_regs = ra8_port(port);
  if (port_regs == nullptr) {
    return k_ra8_err_hw_unmapped;
  }

  const uint32_t pdr_mask  = (uint32_t)(1UL << (uint32_t)pin);
  const uint32_t podr_mask = pdr_mask << (uint32_t)k_ra8_pcntr_high_half_shift;

  /* HUM Ch 20.2 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 -- read {PODR, PDR}
   * so the sibling pins on this port are preserved across the update. */
  uint32_t pcntr1 = port_regs->PCNTR1;
  pcntr1 |= pdr_mask; /* PDR[pin] = 1 -> this pin is an output. */
  if (level == k_ra8_level_high) {
    pcntr1 |= podr_mask; /* PODR[pin] = 1 -> drive high. */
  } else {
    pcntr1 &= ~podr_mask; /* PODR[pin] = 0 -> drive low. */
  }
  /* HUM Ch 20.2 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 -- write {PODR, PDR}:
   * commit the pin as an output at the requested level. */
  port_regs->PCNTR1 = pcntr1;
  return k_ra8_ok;
}

/**
 * @brief Read a pin's live level from PCNTR2 (`PIDR`).
 *
 * @details
 * Reads the port's PCNTR2 and reports the addressed pin's live input-data bit
 * (`PIDR[pin]`) as a level. Works for a pin held an input or an output (PIDR
 * reflects the actual pad state either way). Performs a single PCNTR2 read and
 * touches nothing else. Any active-low interpretation (e.g. a push button) is
 * left to the caller.
 *
 * @param[in]  port      Port index (`k_ra8_port_0` .. `k_ra8_port_max`).
 * @param[in]  pin       Pin index within the port (`k_ra8_pin_0` .. `k_ra8_pin_max`).
 * @param[out] out_level Receives the pin's level (`k_ra8_level_low` / `k_ra8_level_high`).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                    `*out_level` holds the live pin level.
 * @retval k_ra8_err_null_ptr          @p out_level was `nullptr`.
 * @retval k_ra8_err_gpio_invalid_port @p port is out of range.
 * @retval k_ra8_err_gpio_invalid_pin  @p pin is out of range.
 * @retval k_ra8_err_hw_unmapped       The port has no register window (host fake only).
 *
 * @pre The IOPORT module clock is on (always-on after reset on the RA8D2).
 * @pre @p out_level points at writable storage.
 * @post On success, `*out_level` reflects `PCNTR2.PIDR[pin]`.
 * @post On any failure, `*out_level` is left unmodified.
 *
 * @note ISR-safe and reentrant: a single volatile read of a read-only register,
 *       no shared state.
 * @since 0.1.0
 */
[[nodiscard]] static inline ra8_err_t
ra8_pcntr_read(ra8_port_t port, ra8_pin_t pin, ra8_level_t* out_level)
{
  if (out_level == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((uint8_t)port > (uint8_t)k_ra8_port_max) {
    return k_ra8_err_gpio_invalid_port;
  }
  if ((uint8_t)pin > (uint8_t)k_ra8_pin_max) {
    return k_ra8_err_gpio_invalid_pin;
  }

  volatile r_port_regs_t* port_regs = ra8_port(port);
  if (port_regs == nullptr) {
    return k_ra8_err_hw_unmapped;
  }

  /* HUM Ch 20.2 "PCNTR2/EIDR/PIDR : Port Control Register 2" p 841 -- read PIDR: the live
   * pin state in the low half of the register. */
  const uint32_t pidr     = port_regs->PCNTR2;
  const uint32_t pin_mask = (uint32_t)(1UL << (uint32_t)pin);
  *out_level              = ((pidr & pin_mask) != 0U) ? k_ra8_level_high : k_ra8_level_low;
  return k_ra8_ok;
}

#ifdef __cplusplus
}
#endif
