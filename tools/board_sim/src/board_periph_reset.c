/**
 * @file board_periph_reset.c
 * @brief Reset-cause status register (RSTSRn) model for board_sim
 *
 * @details
 * Models the RA8D2 reset-status registers (ra8_reset_regs.h, ra8_reset.c) so a
 * firmware can read back WHY it booted -- power-on, software reset, or watchdog
 * reset -- which the sparse fallback could not provide (the cause always read
 * back as cleared). The flags are sticky across a board_sim warm reboot (the
 * @c --reboot path and the @c AIRCR.SYSRESETREQ / watchdog-timeout triggers in
 * main.c), so an example such as @c reset_cause_demo sees
 * @c k_ra8_reset_cause_software on the second boot after it asks for a software
 * reset, exactly as on silicon.
 *
 * The RSTSRn live inside the R_SYSTEM block; this model claims only the two
 * small windows that hold them, leaving the rest of R_SYSTEM to the sparse
 * fallback and the other R_SYSTEM models (LVD status, VBATT backup):
 *  - @c RSTSR1 (32-bit) at @c 0x4001E0C0 -- IWDTRF / WDTRF / SWRF / ... .
 *  - @c RSTSR0 (8-bit) at @c 0x4001EA40 -- PORF / LVDxRF / DPSRSTF -- plus the
 *    adjacent @c RSTSR2 (CWSF) and @c RSTSR3.
 *
 * Clear semantics match the hardware: the RSTSRn flag bits are write-0-to-clear
 * ("only 0 can be written; to clear, read 1 then write 0"), so a write keeps
 * only the bits that are both currently set and written as 1, and a written 0
 * clears that bit.
 *
 * At cold boot (process start / a power-on reboot) PORF is set and the other
 * flags clear. The reboot path in main.c calls ::board_periph_reset_set_cause
 * to record the new cause (clearing PORF, setting SWRF for a software reset or
 * WDTRF / IWDTRF for a watchdog reset) just before it re-enters the firmware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief RSTSR1 window geometry (ra8_reset_regs.h). */
typedef enum : uint64_t {
  k_rstsr1_base = 0x4001E0C0UL, /**< RSTSR1 (32-bit).        */
  k_rstsr1_span = 0x04UL,       /**< Single 32-bit register. */
} reset1_geom_t;

/** @brief RSTSR0 / RSTSR2 / RSTSR3 window geometry (ra8_reset_regs.h). */
typedef enum : uint64_t {
  k_rstsr0_base   = 0x4001EA40UL, /**< RSTSR0 (8-bit) window base.      */
  k_rstsr0_span   = 0x0CUL,       /**< Covers RSTSR0 / RSTSR2 / RSTSR3. */
  k_rstsr0_off_r0 = 0x00UL,       /**< RSTSR0 at the window base.       */
  k_rstsr0_off_r2 = 0x04UL,       /**< RSTSR2 (CWSF).                   */
  k_rstsr0_off_r3 = 0x08UL,       /**< RSTSR3.                          */
} reset0_geom_t;

/** @brief RSTSR flag bit masks (ra8_reset_regs.h). */
typedef enum : uint32_t {
  k_rstsr0_porf   = 0x01U,       /**< RSTSR0.PORF power-on reset (bit 0).    */
  k_rstsr1_iwdtrf = 0x00000001U, /**< RSTSR1.IWDTRF independent WDT (bit 0). */
  k_rstsr1_wdtrf  = 0x00000002U, /**< RSTSR1.WDTRF watchdog reset (bit 1).   */
  k_rstsr1_swrf   = 0x00000004U, /**< RSTSR1.SWRF software reset (bit 2).    */
} reset_flag_t;

/** @brief Per-tick order slot for the reset block (relative order only). */
typedef enum : uint32_t {
  k_reset_block_order = 178U, /**< After the VBATT-backup block; report order. */
} reset_order_t;

/**
 * @brief Sticky reset-cause flags (survive a warm reboot).
 *
 * @details Held across the block reset hook (it leaves them untouched) so the
 * cause the reboot recorded is still readable on the next boot. Cleared only at
 * process start (static zero-init), where the constructor then seeds PORF: the
 * first-ever boot is a power-on.
 */
static uint32_t s_rstsr1; /**< RSTSR1 sticky flags. */
static uint8_t  s_rstsr0; /**< RSTSR0 sticky flags. */
static uint8_t  s_rstsr2; /**< RSTSR2 (CWSF).       */
static uint8_t  s_rstsr3; /**< RSTSR3.              */

/* A pending warm-reboot request raised by a peripheral model (e.g. the WDT on a
 * watchdog-reset underflow). The run loop polls board_periph_reset_take_request
 * each chunk and performs the reboot. 0 = none; otherwise the cause to latch. */
static uint32_t s_reboot_req_cause;

/** @brief Reboot-cause codes a peripheral can request via _request_reboot. */
typedef enum : uint32_t {
  k_reboot_cause_none = 0U, /**< No pending request.              */
  k_reboot_cause_wdt  = 1U, /**< Watchdog-0 reset (RSTSR1.WDTRF). */
  k_reboot_cause_iwdt = 2U, /**< Independent-watchdog reset.      */
} reboot_cause_t;

void board_periph_reset_request_reboot(bool watchdog, bool iwdt)
{
  if (watchdog) {
    s_reboot_req_cause = (uint32_t)k_reboot_cause_wdt;
  } else if (iwdt) {
    s_reboot_req_cause = (uint32_t)k_reboot_cause_iwdt;
  }
}

bool board_periph_reset_take_request(bool* out_watchdog, bool* out_iwdt)
{
  if ((out_watchdog == nullptr) || (out_iwdt == nullptr)) {
    return false;
  }
  if (s_reboot_req_cause == (uint32_t)k_reboot_cause_none) {
    return false;
  }
  *out_watchdog      = (s_reboot_req_cause == (uint32_t)k_reboot_cause_wdt);
  *out_iwdt          = (s_reboot_req_cause == (uint32_t)k_reboot_cause_iwdt);
  s_reboot_req_cause = (uint32_t)k_reboot_cause_none;
  return true;
}

void board_periph_reset_set_cause(bool power_on, bool software, bool watchdog, bool iwdt)
{
  if (power_on) {
    /* A power-on / cold reboot: PORF is the cause; leave it set (the reset model
     * retains it across the warm reboot) and assert nothing else. */
    s_rstsr0 |= (uint8_t)k_rstsr0_porf;
    return;
  }
  /* Any non-power-on reset clears PORF and latches the specific cause flag(s). */
  s_rstsr0 &= (uint8_t)~(uint8_t)k_rstsr0_porf;
  if (software) {
    s_rstsr1 |= (uint32_t)k_rstsr1_swrf;
  }
  if (watchdog) {
    s_rstsr1 |= (uint32_t)k_rstsr1_wdtrf;
  }
  if (iwdt) {
    s_rstsr1 |= (uint32_t)k_rstsr1_iwdtrf;
  }
}

/** @brief Reset hook: the cause flags are sticky, so this preserves them. */
static void reset_reset(void)
{
  /* Intentionally a no-op: the RSTSRn flags must survive a warm reboot so the
   * firmware can read the cause that triggered it. They clear only at process
   * start (static init) where the constructor seeds PORF. */
}

/** @brief MMIO read inside the RSTSR1 window. */
static uint64_t reset1_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return s_rstsr1;
}

/** @brief MMIO write inside the RSTSR1 window (write-0-to-clear flags). */
static void reset1_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  /* Keep only bits that are set AND written as 1; a written 0 clears the bit. */
  s_rstsr1 &= (uint32_t)value;
}

/** @brief MMIO read inside the RSTSR0 / RSTSR2 / RSTSR3 window. */
static uint64_t reset0_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_rstsr0_base;
  if (off == (uint64_t)k_rstsr0_off_r0) {
    return s_rstsr0;
  }
  if (off == (uint64_t)k_rstsr0_off_r2) {
    return s_rstsr2;
  }
  if (off == (uint64_t)k_rstsr0_off_r3) {
    return s_rstsr3;
  }
  return 0U;
}

/** @brief MMIO write inside the RSTSR0 / RSTSR2 / RSTSR3 window. */
static void reset0_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_rstsr0_base;
  if (off == (uint64_t)k_rstsr0_off_r0) {
    s_rstsr0 &= (uint8_t)value; /* write-0-to-clear */
  } else if (off == (uint64_t)k_rstsr0_off_r2) {
    s_rstsr2 = (uint8_t)value; /* RSTSR2.CWSF is write-1 set; shadow it */
  } else if (off == (uint64_t)k_rstsr0_off_r3) {
    s_rstsr3 = (uint8_t)value;
  }
}

/** @brief End-of-run reset section: the latched cause flags. */
static void reset_report(void)
{
  if ((s_rstsr1 == 0U) && ((s_rstsr0 & (uint8_t)k_rstsr0_porf) == 0U)) {
    return; /* nothing notable to report */
  }
  (void)fprintf(stderr,
                "  RESET-CAUSE   : RSTSR0=0x%02X RSTSR1=0x%08X (%s%s%s%s)\n",
                (unsigned)s_rstsr0,
                s_rstsr1,
                ((s_rstsr0 & (uint8_t)k_rstsr0_porf) != 0U) ? "POR " : "",
                ((s_rstsr1 & (uint32_t)k_rstsr1_swrf) != 0U) ? "SW " : "",
                ((s_rstsr1 & (uint32_t)k_rstsr1_wdtrf) != 0U) ? "WDT " : "",
                ((s_rstsr1 & (uint32_t)k_rstsr1_iwdtrf) != 0U) ? "IWDT " : "");
}

/** @brief RSTSR1 block descriptor (owns the report). */
static const board_periph_block_t k_reset1_block = {
  .base   = (uint64_t)k_rstsr1_base,
  .span   = (uint64_t)k_rstsr1_span,
  .order  = (uint32_t)k_reset_block_order,
  .read   = reset1_read,
  .write  = reset1_write,
  .tick   = nullptr,
  .reset  = reset_reset,
  .report = reset_report,
  .name   = "RESET-RSTSR1",
};

/** @brief RSTSR0 / RSTSR2 / RSTSR3 block descriptor. */
static const board_periph_block_t k_reset0_block = {
  .base   = (uint64_t)k_rstsr0_base,
  .span   = (uint64_t)k_rstsr0_span,
  .order  = (uint32_t)k_reset_block_order,
  .read   = reset0_read,
  .write  = reset0_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "RESET-RSTSR0",
};

/** @brief Register the reset-status windows + seed PORF (cold boot). */
[[gnu::constructor]] static void reset_block_register(void)
{
  s_rstsr0 |= (uint8_t)k_rstsr0_porf; /* first-ever boot is a power-on reset */
  board_periph_register_block(&k_reset1_block);
  board_periph_register_block(&k_reset0_block);
}
