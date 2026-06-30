/**
 * @file ra_sdramc.c
 * @brief SDRAM controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 SDRAM controller block (the SDRAMC sub-block
 * of R_BUS, HUM Ch 15 "Buses"). Brings up the on-board SDRAM of the
 * EK-RA8D2 -- an ISSI IS42S32160F, 512 Mbit (64 MB) organised as
 * 16M x 32 bits, 13 row / 9 column / 4 bank -- mapped at
 * `k_ra_sdram_base_addr` (0x68000000).
 *
 * Bring-up follows the HUM SDRAMC initialization sequence and FSP
 * `R_BSP_SdramInit`: route the parallel-bus pins, enable the SDCLK
 * output, run the hardware init sequencer, issue the LMR command,
 * program the timing/refresh registers, then enable SDRAM access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sdramc.h"

#include <stdint.h>

#include "ra8d2_pfs_regs.h"
#include "ra8d2_sdramc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_register_protection.h"

static const char* s_tag = "SDRAM";

/**
 * @enum ra_sdramc_reg_val_t
 * @brief SDRAMC register values for the EK-RA8D2 IS42S32160F SDRAM.
 *
 * @details
 * The device is a 512 Mbit x32 SDR SDRAM (13 row / 9 column / 4
 * bank). SDCLK runs at BCLK = 125 MHz (8 ns). Timing fields hold a
 * conservative cycle count: RCD/RP carry a spare cycle over the
 * datasheet minimum, and the auto-refresh request interval (900
 * cycles = 7.2 us) sits comfortably under the 7.8 us-per-row JEDEC
 * limit so a slow-side SDCLK tolerance cannot starve a row. The HUM
 * (Ch 15.3.20 SDTR / 15.3.15 SDRFCR) encodes RP/RAI as (cycles - 1)
 * and RCD/CL/REFW likewise as one-less-than-cycles.
 *
 * @invariant All values target a 32-bit data bus, CAS latency 3.
 */
typedef enum : uint32_t {
  k_ra_sdccr_cfg_32bit = 0x10UL,       /**< SDCCR: BSIZE=01 (32-bit), EXENB=0.         */
  k_ra_sdccr_enable    = 0x11UL,       /**< SDCCR: BSIZE=01, EXENB=1.                  */
  k_ra_sdamod_be       = 0x01UL,       /**< SDAMOD.BE=1: continuous access on.         */
  k_ra_sdadr_mxc_9col  = 0x01UL,       /**< SDADR.MXC=01b: 9-bit column shift.         */
  k_ra_sdir_init       = 0x0088UL,     /**< SDIR: ARFI=11cyc, ARFC=8x, PRC=3cyc.       */
  k_ra_sdmod_lmr       = 0x0230UL,     /**< SDMOD: WB=single, CL=3, BT=seq, BL=1.      */
  k_ra_sdrfcr_init     = 0xB383UL,     /**< SDRFCR: REFW=12cyc, refresh/900cyc(7.2us). */
  k_ra_sdtr_init       = 0x00053703UL, /**< SDTR: RAS=6,RCD=4,RP=4,WR=2,CL=3 cyc.      */
  k_ra_sdramc_kick     = 0x01UL,       /**< Generic "set bit 0" kick value.            */
} ra_sdramc_reg_val_t;

/**
 * @enum ra_sdramc_sdsr_t
 * @brief SDRAM Status Register (SDSR) bit masks.
 *
 * @details HUM Ch 15.3.22 "SDSR : SDRAM Status Register" p 613:
 * the SDRAMC init/LMR/self-refresh steps must wait for the relevant
 * status bit to clear before issuing the next register write.
 */
typedef enum : uint8_t {
  k_ra_sdsr_mrsst = 0x01U, /**< MRSST (bit 0): mode-register-set in progress.                */
  k_ra_sdsr_inist = 0x08U, /**< INIST (bit 3): init sequence in progress.                    */
  k_ra_sdsr_srfst = 0x10U, /**< SRFST (bit 4): self-refresh transition/recovery in progress. */
  k_ra_sdsr_all   = 0x19U, /**< MRSST | INIST | SRFST: all blocking bits.                    */
} ra_sdramc_sdsr_t;

/**
 * @enum ra_sdramc_sdself_t
 * @brief SDRAM Self-Refresh Control Register (SDSELF) bit values.
 *
 * @details HUM Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control
 * Register" p 606: the SFEN bit controls the self-refresh state.
 * Setting SFEN=1 initiates an auto-refresh cycle then self-refresh;
 * clearing it ends self-refresh and auto-refresh resumes.
 */
typedef enum : uint8_t {
  k_ra_sdself_sfen = 0x01U, /**< SFEN (bit 0): self-refresh enable. */
} ra_sdramc_sdself_t;

/**
 * @enum ra_sdramc_spin_t
 * @brief Upper bound for the SDSR status-poll loops (NASA P10 Rule 2).
 */
typedef enum : uint32_t {
  k_ra_sdramc_spin_max = 1000000U, /**< ~ms at 1 GHz; init takes < 1 us. */
} ra_sdramc_spin_t;

/**
 * @var s_sdram_bus_pins
 * @brief The 57 EK-RA8D2 parallel-bus pins the SDRAMC drives.
 *
 * @details
 * The exact MCU-pin assignments are transcribed from the EK-RA8D2
 * v1 User's Manual, Table 30 "SDRAM Assignments": A0-A12, BA0-BA1,
 * DQ0-DQ31, CKE, SDCLK, DQM0-DQM3, WE, CAS, RAS, CS. Each is muxed
 * to the external-bus function with PFS PSEL = ::k_ra_psel_bus.
 *
 * @note File-scope constant; not modified at runtime.
 * @since 0.1.0
 */
static const ra_port_pin_t s_sdram_bus_pins[] = {
  /* A0..A12 */
  RA_PIN(k_ra_port_10, k_ra_pin_3),
  RA_PIN(k_ra_port_10, k_ra_pin_2),
  RA_PIN(k_ra_port_10, k_ra_pin_1),
  RA_PIN(k_ra_port_10, k_ra_pin_0),
  RA_PIN(k_ra_port_5, k_ra_pin_3),
  RA_PIN(k_ra_port_5, k_ra_pin_4),
  RA_PIN(k_ra_port_5, k_ra_pin_5),
  RA_PIN(k_ra_port_5, k_ra_pin_6),
  RA_PIN(k_ra_port_5, k_ra_pin_7),
  RA_PIN(k_ra_port_5, k_ra_pin_8),
  RA_PIN(k_ra_port_5, k_ra_pin_9),
  RA_PIN(k_ra_port_5, k_ra_pin_10),
  RA_PIN(k_ra_port_6, k_ra_pin_8),
  /* BA0, BA1 */
  RA_PIN(k_ra_port_13, k_ra_pin_0),
  RA_PIN(k_ra_port_12, k_ra_pin_15),
  /* DQ0..DQ31 */
  RA_PIN(k_ra_port_3, k_ra_pin_2),
  RA_PIN(k_ra_port_3, k_ra_pin_1),
  RA_PIN(k_ra_port_3, k_ra_pin_0),
  RA_PIN(k_ra_port_1, k_ra_pin_12),
  RA_PIN(k_ra_port_1, k_ra_pin_13),
  RA_PIN(k_ra_port_1, k_ra_pin_14),
  RA_PIN(k_ra_port_1, k_ra_pin_15),
  RA_PIN(k_ra_port_6, k_ra_pin_9),
  RA_PIN(k_ra_port_10, k_ra_pin_11),
  RA_PIN(k_ra_port_10, k_ra_pin_12),
  RA_PIN(k_ra_port_10, k_ra_pin_13),
  RA_PIN(k_ra_port_10, k_ra_pin_14),
  RA_PIN(k_ra_port_6, k_ra_pin_10),
  RA_PIN(k_ra_port_6, k_ra_pin_11),
  RA_PIN(k_ra_port_6, k_ra_pin_12),
  RA_PIN(k_ra_port_6, k_ra_pin_13),
  RA_PIN(k_ra_port_12, k_ra_pin_14),
  RA_PIN(k_ra_port_12, k_ra_pin_13),
  RA_PIN(k_ra_port_12, k_ra_pin_12),
  RA_PIN(k_ra_port_12, k_ra_pin_11),
  RA_PIN(k_ra_port_12, k_ra_pin_10),
  RA_PIN(k_ra_port_12, k_ra_pin_9),
  RA_PIN(k_ra_port_12, k_ra_pin_8),
  RA_PIN(k_ra_port_12, k_ra_pin_7),
  RA_PIN(k_ra_port_12, k_ra_pin_6),
  RA_PIN(k_ra_port_12, k_ra_pin_5),
  RA_PIN(k_ra_port_12, k_ra_pin_4),
  RA_PIN(k_ra_port_12, k_ra_pin_3),
  RA_PIN(k_ra_port_12, k_ra_pin_2),
  RA_PIN(k_ra_port_12, k_ra_pin_1),
  RA_PIN(k_ra_port_12, k_ra_pin_0),
  RA_PIN(k_ra_port_6, k_ra_pin_7),
  /* CKE, SDCLK, DQM0, DQM1, DQM2, DQM3, WE, CAS, RAS, CS */
  RA_PIN(k_ra_port_10, k_ra_pin_6),
  RA_PIN(k_ra_port_10, k_ra_pin_15),
  RA_PIN(k_ra_port_6, k_ra_pin_14),
  RA_PIN(k_ra_port_10, k_ra_pin_5),
  RA_PIN(k_ra_port_6, k_ra_pin_15),
  RA_PIN(k_ra_port_10, k_ra_pin_4),
  RA_PIN(k_ra_port_10, k_ra_pin_8),
  RA_PIN(k_ra_port_10, k_ra_pin_9),
  RA_PIN(k_ra_port_10, k_ra_pin_10),
  RA_PIN(k_ra_port_8, k_ra_pin_13),
};

/**
 * @brief Route every SDRAM parallel-bus pin to the external-bus mux.
 *
 * @details
 * Walks ::s_sdram_bus_pins, muxing each to PSEL = ::k_ra_psel_bus and
 * raising its drive strength to high-speed/high -- the 125 MHz SDCLK
 * pin requires it per the RA8D2 datasheet, and the address/data/
 * control pins need it to meet the SDRAM setup/hold window.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   All bus pins routed.
 * @retval k_ra_err_gpio_conflict    A pin is already owned elsewhere.
 * @retval k_ra_err_gpio_invalid_pin A pin index was rejected.
 *
 * @pre IOPORT power is on (MSTPCRB cleared for IOPORT).
 * @pre Caller runs single-threaded during board init.
 * @post On k_ra_ok every bus pin carries its SDRAMC signal.
 * @post On error the routing is left partially applied.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_sdramc_route_pins(void)
{
  const uint32_t count = (uint32_t)(sizeof(s_sdram_bus_pins) / sizeof(s_sdram_bus_pins[0]));
  for (uint32_t i = 0U; i < count; ++i) {
    const ra_err_t route_err =
      ra_pfs_route_peripheral(s_sdram_bus_pins[i], k_ra_psel_bus, "ra_sdramc");
    if (route_err != k_ra_ok) {
      return route_err;
    }
    const ra_err_t drive_err =
      ra_pfs_set_drive_strength(s_sdram_bus_pins[i], k_ra_pfs_dscr_high_speed_high);
    if (drive_err != k_ra_ok) {
      return drive_err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Spin until the masked SDSR status bits clear.
 *
 * @details
 * The HUM requires confirming that the relevant SDSR bits are 0
 * before SDICR / SDMOD / further control writes. The loop is bounded
 * by ::k_ra_sdramc_spin_max so a wedged controller cannot hang init.
 *
 * @param[in] mask SDSR bit mask to wait on (::ra_sdramc_sdsr_t).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok             Masked bits read 0.
 * @retval k_ra_err_hw_timeout Bits never cleared within the bound.
 *
 * @pre mask is a subset of ::k_ra_sdsr_all.
 * @pre The SDRAMC register block is mapped.
 * @post On k_ra_ok (SDSR & mask) == 0.
 * @post On timeout the SDRAMC is left untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_sdramc_wait(uint8_t mask)
{
  volatile r_sdramc_regs_t* const reg = ra_sdramc();
  for (uint32_t spin = 0U; spin < (uint32_t)k_ra_sdramc_spin_max; ++spin) {
    if ((uint8_t)(reg->SDSR & mask) == 0U) {
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "sdramc: SDSR status bits never cleared");
  return k_ra_err_hw_timeout;
}

ra_err_t ra_sdramc_init(void)
{
  const ra_err_t pin_err = internal_sdramc_route_pins();
  if (pin_err != k_ra_ok) {
    ra_log_error(s_tag, "sdramc: bus-pin routing failed");
    return pin_err;
  }

  volatile r_sdramc_regs_t* const reg = ra_sdramc();

  /* HUM Ch 15.6.10: confirm SDSR is clear, then program the
   * one-shot SDRAM Initialization Register. SDIR is writable only
   * once after reset. */
  ra_err_t err = internal_sdramc_wait((uint8_t)k_ra_sdsr_all);
  if (err != k_ra_ok) {
    return err;
  }
  reg->SDIR  = (uint16_t)k_ra_sdir_init;
  reg->SDCCR = (uint8_t)k_ra_sdccr_cfg_32bit;

  /* Enable the SDCLK output to the device (PRCR-gated, CGC group). */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sdramc_sdckocr() = (uint8_t)k_ra_sdramc_kick;
  }

  /* Run the hardware init sequencer (precharge-all + auto-refresh). */
  reg->SDICR = (uint8_t)k_ra_sdramc_kick;
  err        = internal_sdramc_wait((uint8_t)k_ra_sdsr_inist);
  if (err != k_ra_ok) {
    return err;
  }

  /* Access + endian mode, then issue the LMR command (SDMOD).
   * SDAMOD.BE = 1 keeps multi-beat accesses continuous so the
   * sub-word address increments correctly. */
  reg->SDAMOD = (uint8_t)k_ra_sdamod_be;
  reg->SDCMOD = 0U;
  err         = internal_sdramc_wait((uint8_t)k_ra_sdsr_all);
  if (err != k_ra_ok) {
    return err;
  }
  reg->SDMOD = (uint16_t)k_ra_sdmod_lmr;
  err        = internal_sdramc_wait((uint8_t)k_ra_sdsr_mrsst);
  if (err != k_ra_ok) {
    return err;
  }

  /* Timing, address-multiplex, refresh -- then enable SDRAM access. */
  reg->SDTR   = (uint32_t)k_ra_sdtr_init;
  reg->SDADR  = (uint8_t)k_ra_sdadr_mxc_9col;
  reg->SDRFCR = (uint16_t)k_ra_sdrfcr_init;
  reg->SDRFEN = (uint8_t)k_ra_sdramc_kick;
  reg->SDCCR  = (uint8_t)k_ra_sdccr_enable;

  ra_log_info(s_tag, "sdramc_init (64 MiB @ 0x68000000)");
  return k_ra_ok;
}

/* =============================================================================
 * lifecycle + power
 * =============================================================================
 */

ra_err_t ra_sdramc_deinit(void)
{
  volatile r_sdramc_regs_t* reg = ra_sdramc();
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  reg->SDRFEN = 0U;
  /* HUM Ch 15.3.11 "SDCCR : SDC Control Register" p 605 */
  reg->SDCCR = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_set_refresh_interval(uint16_t sdrfcr)
{
  /* HUM Ch 15.3.15 "SDRFCR : SDRAM Refresh Control Register" p 607 */
  ra_sdramc()->SDRFCR = sdrfcr;
  return k_ra_ok;
}

ra_err_t ra_sdramc_get_status(uint8_t* out_enabled)
{
  RA_CHECK_NULL_PTR(out_enabled, s_tag, "out_enabled must not be nullptr");
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  *out_enabled = ra_sdramc()->SDRFEN;
  return k_ra_ok;
}

ra_err_t ra_sdramc_enter_stop(void)
{
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  ra_sdramc()->SDRFEN = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_exit_stop(void)
{
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  ra_sdramc()->SDRFEN = (uint8_t)k_ra_sdramc_kick;
  return k_ra_ok;
}

ra_err_t ra_sdramc_enter_self_refresh(void)
{
  volatile r_sdramc_regs_t* const reg = ra_sdramc();

  /* Precondition 1: all SDSR status bits must be clear before writing
   * SDSELF.SFEN (HUM Table 15.8 requirement). */
  /* HUM Ch 15.3.22 "SDSR : SDRAM Status Register" p 613 */
  if ((uint8_t)(reg->SDSR & (uint8_t)k_ra_sdsr_all) != 0U) {
    ra_log_error(s_tag, "sdramc: SDSR busy on self-refresh entry");
    return k_ra_err_invalid_state;
  }

  /* Precondition 2: SDSELF.SFEN must be 0 -- already in self-refresh
   * is a caller error. */
  /* HUM Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606 */
  if ((uint8_t)(reg->SDSELF & (uint8_t)k_ra_sdself_sfen) != 0U) {
    ra_log_error(s_tag, "sdramc: already in self-refresh");
    return k_ra_err_invalid_state;
  }

  /* Disable auto-refresh before asserting self-refresh. */
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  reg->SDRFEN = 0U;

  /* Assert SFEN: the hardware issues a final auto-refresh then
   * switches the SDRAM to self-refresh mode. */
  /* HUM Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606 */
  reg->SDSELF = (uint8_t)k_ra_sdself_sfen;

  /* Wait for the transition to complete (SRFST clears when the
   * SDRAM has entered self-refresh). */
  const ra_err_t err = internal_sdramc_wait((uint8_t)k_ra_sdsr_srfst);
  if (err != k_ra_ok) {
    return err;
  }

  ra_log_info(s_tag, "sdramc: entered self-refresh");
  return k_ra_ok;
}

ra_err_t ra_sdramc_exit_self_refresh(void)
{
  volatile r_sdramc_regs_t* const reg = ra_sdramc();

  /* Precondition 1: SDSELF.SFEN must be 1 -- must be in self-refresh
   * before exiting. */
  /* HUM Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606 */
  if ((uint8_t)(reg->SDSELF & (uint8_t)k_ra_sdself_sfen) == 0U) {
    ra_log_error(s_tag, "sdramc: not in self-refresh on exit");
    return k_ra_err_invalid_state;
  }

  /* Precondition 2: SDSR.SRFST must be clear -- no transition may be
   * in progress when writing SDSELF. */
  /* HUM Ch 15.3.22 "SDSR : SDRAM Status Register" p 613 */
  if ((uint8_t)(reg->SDSR & (uint8_t)k_ra_sdsr_srfst) != 0U) {
    ra_log_error(s_tag, "sdramc: SDSR SRFST busy on self-refresh exit");
    return k_ra_err_invalid_state;
  }

  /* Clear SFEN: the hardware performs the self-refresh clearing
   * cycles (count set by SDRFCR.REFW) then returns to normal. */
  /* HUM Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606 */
  reg->SDSELF = 0U;

  /* Wait for the recovery to complete (SRFST clears when auto-
   * refresh is ready to resume). */
  const ra_err_t err = internal_sdramc_wait((uint8_t)k_ra_sdsr_srfst);
  if (err != k_ra_ok) {
    return err;
  }

  /* Re-enable the auto-refresh engine. */
  /* HUM Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608 */
  reg->SDRFEN = (uint8_t)k_ra_sdramc_kick;

  ra_log_info(s_tag, "sdramc: exited self-refresh");
  return k_ra_ok;
}
