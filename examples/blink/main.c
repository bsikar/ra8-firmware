/**
 * @file main.c
 * @brief Blink-LED smoke test for EK-RA8D2 with SysTick-based delay
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives the EK-RA8D2 user-LED candidate pins (P6_00, P6_01, P6_02,
 * P3_03, P10_07) at a clean 1 Hz cycle (500 ms on, 500 ms off) using
 * the Cortex-M85 SysTick timer for delay -- no busy-wait. The chip
 * boots on MOCO at ~8.4 MHz (measured via DWT.CYCCNT) until the CGC
 * brings HOCO + PLL up; SysTick reload is sized for that clock.
 *
 * @par SysTick programming sequence
 * Per Armv8-M Architecture Reference Manual section B11.1:
 *   1. Disable SysTick (clear SYST_CSR).
 *   2. Write reload value (SYST_RVR) in [0, 0xFFFFFF].
 *   3. Clear current value (SYST_CVR).
 *   4. Enable with CSR.ENABLE | CSR.CLKSOURCE (CPU clock, no IRQ).
 *   5. Poll CSR.COUNTFLAG (bit 16) -- goes 1 once after wrap,
 *      auto-clears on read.
 *
 * @par Pin programming
 * Uses the existing PFS unlock dance documented in
 * libs/ra_hal/inc/ra8d2_pfs_regs.h. We write PWPRS (the Secure
 * variant) because the Cortex-M85 boots in the Secure world on
 * RA8D2 and SystemInit currently leaves the SAU partition disabled.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}`
 * means. Short version: this file is application-layer code that
 * runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * Hardware addresses
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_pfs_base    = 0x40400800UL, /**< PmnPFS array base (HUM 20.2.4). */
  k_ra_port_base   = 0x40400000UL, /**< Per-port PCNTR1 base, stride 0x20. */
  k_ra_pmisc_pwpr  = 0x40400D0CUL, /**< PMISC.PWPR -- Non-secure. */
  k_ra_pmisc_pwprs = 0x40400D14UL, /**< PMISC.PWPRS -- Secure. */

  k_ra_systick_csr = 0xE000E010UL, /**< SYST_CSR -- control + status. */
  k_ra_systick_rvr = 0xE000E014UL, /**< SYST_RVR -- reload value (24-bit). */
  k_ra_systick_cvr = 0xE000E018UL, /**< SYST_CVR -- current value. */
} ra_blink_addr_t;

typedef enum : uint32_t {
  k_ra_systick_enable    = 1UL << 0,   /**< CSR.ENABLE. */
  k_ra_systick_clksource = 1UL << 2,   /**< CSR.CLKSOURCE: 1 = CPU clock. */
  k_ra_systick_countflag = 1UL << 16,  /**< CSR.COUNTFLAG. */
  k_ra_systick_rvr_max   = 0x00FFFFFF, /**< 24-bit reload max. */
} ra_systick_bit_t;

typedef enum : uint8_t {
  k_ra_pwpr_pfswe = 0x40U, /**< Allow PFS writes. */
  k_ra_pwpr_b0wi  = 0x80U, /**< Disallow PFS writes.*/
} ra_pwpr_bit_t;

typedef enum : uint32_t {
  k_ra_pfs_pdr_output_low = 0x00000004UL, /**< PMR=0 (GPIO), PDR=1, PODR=0. */
} ra_pfs_init_t;

/**
 * @brief CPU clock at reset (MOCO ~8 MHz on RA8D2 before CGC bring-up).
 *
 * @details
 * Measured empirically via DWT.CYCCNT in a single JLinkExe session:
 * 42,068,697 cycles in 5.000 s -> 8,413,739 Hz
 * which lines up with the RA-family MOCO nominal rating of 8 MHz with
 * some trim variation. After ``ra_cgc_init`` brings up HOCO + PLL this
 * should be replaced with the actual operating clock.
 */
typedef enum : uint32_t {
  k_ra_cpu_hz_at_reset  = 8400000U,
  k_ra_blink_ms_per_sec = 1000U,
} ra_clock_t;

/**
 * @struct ra_blink_pin_t
 * @brief One pin to drive: which port + which bit.
 */
typedef struct {
  uint8_t port; /**< 0..15. */
  uint8_t bit;  /**< 0..15. */
} ra_blink_pin_t;

/**
 * @brief EK-RA8D2 LED candidate pins.
 *
 * @details
 * Covers the canonical EK-RA family LED pins (P6_00, P6_01, P6_02 --
 * used on EK-RA8D1 / EK-RA8M1 / EK-RA6M5) plus the project's currently
 * coded ra_port_constants.h candidates (P3_03 LED2, P10_07 LED3).
 */
static const ra_blink_pin_t k_ra_blink_pins[] = {
  {.port = 6U, .bit = 0U},
  {.port = 6U, .bit = 1U},
  {.port = 6U, .bit = 2U},
  {.port = 3U, .bit = 3U},
  {.port = 10U, .bit = 7U},
};

typedef enum : uint32_t {
  k_ra_blink_pin_count = sizeof(k_ra_blink_pins) / sizeof(k_ra_blink_pins[0]),
} ra_blink_count_t;

/* =============================================================================
 * Tiny register accessors -- no HAL dependency
 * =============================================================================
 */

static inline volatile uint8_t* ra_reg8(uintptr_t addr)
{
  return (volatile uint8_t*)addr;
}

static inline volatile uint32_t* ra_reg32(uintptr_t addr)
{
  return (volatile uint32_t*)addr;
}

static inline volatile uint32_t* ra_pfs_reg(uint8_t port, uint8_t bit)
{
  /* HUM Ch 20.2.4 "PmnPFS" -- per-pin, 32-bit, indexed by
 * (port * 16 + bit) * 4 from the PFS base. */
  const uintptr_t off = (((uintptr_t)port * 16U) + (uintptr_t)bit) * 4U;
  return ra_reg32(k_ra_pfs_base + off);
}

static inline volatile uint32_t* ra_pcntr1_reg(uint8_t port)
{
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  /* low 16 = PDR, high 16 = PODR. */
  return ra_reg32(k_ra_port_base + ((uintptr_t)port * 0x20UL));
}

/* =============================================================================
 * SysTick polling-delay helpers
 * =============================================================================
 */

/**
 * @brief Halt SysTick and clear its current count.
 *
 * @pre SysTick may or may not be enabled.
 * @post SYST_CSR == 0, SYST_CVR cleared.
 */
static void ra_systick_stop(void)
{
  *ra_reg32(k_ra_systick_csr) = 0U;
  *ra_reg32(k_ra_systick_cvr) = 0U;
}

/**
 * @brief Block until ``ticks`` CPU clocks have elapsed.
 *
 * @details
 * Programs SYST_RVR with ``ticks - 1`` (24-bit), enables SysTick on
 * the CPU clock with interrupts masked, then polls COUNTFLAG. On
 * wrap, COUNTFLAG goes 1 once and auto-clears on read. Caller is
 * responsible for splitting requests larger than 0x00FFFFFF into
 * multiple calls.
 *
 * @param[in] ticks CPU cycles to wait. Must be 1.. 0x00FFFFFF.
 *
 * @pre SysTick is not currently in use elsewhere.
 * @pre ticks > 0 and ticks <= k_ra_systick_rvr_max.
 * @post SysTick has counted exactly ``ticks`` cycles.
 * @post SysTick is left disabled.
 */
static void ra_systick_wait_ticks(uint32_t ticks)
{
  ra_systick_stop();
  *ra_reg32(k_ra_systick_rvr) = (ticks - 1U) & k_ra_systick_rvr_max;
  *ra_reg32(k_ra_systick_cvr) = 0U;
  *ra_reg32(k_ra_systick_csr) = k_ra_systick_enable | k_ra_systick_clksource;

  /* Poll COUNTFLAG. Goes 1 on wrap (one-shot), auto-clears on read. */
  while ((*ra_reg32(k_ra_systick_csr) & k_ra_systick_countflag) == 0U) {
    /* wait */
  }

  ra_systick_stop();
}

/**
 * @brief Block for approximately ``ms`` milliseconds.
 *
 * @details
 * Splits the request into ``k_ra_systick_rvr_max``-tick chunks so
 * arbitrary durations work even at the 24-bit reload limit (at the
 * 32 kHz LOCO that's ~524 sec per reload, so single shot already
 * suffices for typical blink intervals).
 *
 * @param[in] ms Milliseconds to wait. ms == 0 returns immediately.
 *
 * @pre SysTick is not currently in use elsewhere.
 * @post Approximately ``ms * k_ra_cpu_hz_at_reset / 1000`` CPU cycles
 * have elapsed.
 */
static void ra_delay_ms(uint32_t ms)
{
  uint32_t total = (ms * k_ra_cpu_hz_at_reset) / k_ra_blink_ms_per_sec;
  while (total > 0U) {
    const uint32_t chunk = (total > k_ra_systick_rvr_max) ? k_ra_systick_rvr_max : total;
    ra_systick_wait_ticks(chunk);
    total -= chunk;
  }
}

/* =============================================================================
 * Pin init + drive
 * =============================================================================
 */

/**
 * @brief Configure one pin as a digital output, initial level low.
 */
static void ra_blink_pin_init(uint8_t port, uint8_t bit)
{
  /* HUM Ch 20.2.6 "PWPR" / "PWPRS" -- unlock, write, relock. The
 * Cortex-M85 boots Secure on RA8D2 so PWPRS is the active gate. */
  volatile uint8_t* pwpr  = ra_reg8(k_ra_pmisc_pwpr);
  volatile uint8_t* pwprs = ra_reg8(k_ra_pmisc_pwprs);

  *pwpr  = 0x00U;
  *pwpr  = k_ra_pwpr_pfswe;
  *pwprs = 0x00U;
  *pwprs = k_ra_pwpr_pfswe;

  *ra_pfs_reg(port, bit) = k_ra_pfs_pdr_output_low;

  *pwpr  = k_ra_pwpr_b0wi;
  *pwprs = k_ra_pwpr_b0wi;
}

/**
 * @brief Set every pin in ``k_ra_blink_pins`` to ``high`` simultaneously.
 *
 * @details
 * Writes PCNTR1 directly (PDR low half, PODR high half). Aggregates
 * all pins on the same port into a single bus write so only one bus
 * transaction lands per port.
 */
static void ra_blink_drive_all(uint8_t high)
{
  uint8_t  ports_seen[k_ra_blink_pin_count] = {};
  uint32_t pdr_mask[k_ra_blink_pin_count]   = {};
  uint32_t port_count                       = 0U;

  for (uint32_t i = 0U; i < k_ra_blink_pin_count; ++i) {
    const uint8_t port = k_ra_blink_pins[i].port;
    const uint8_t bit  = k_ra_blink_pins[i].bit;

    uint32_t slot = port_count;
    for (uint32_t j = 0U; j < port_count; ++j) {
      if (ports_seen[j] == port) {
        slot = j;
        break;
      }
    }
    if (slot == port_count) {
      ports_seen[slot] = port;
      pdr_mask[slot]   = 0U;
      port_count++;
    }
    pdr_mask[slot] |= (1UL << bit);
  }

  for (uint32_t i = 0U; i < port_count; ++i) {
    const uint32_t pdr            = pdr_mask[i];
    const uint32_t podr           = high ? pdr : 0U;
    *ra_pcntr1_reg(ports_seen[i]) = pdr | (podr << 16);
  }
}

/* =============================================================================
 * Entry
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  for (uint32_t i = 0U; i < k_ra_blink_pin_count; ++i) {
    ra_blink_pin_init(k_ra_blink_pins[i].port, k_ra_blink_pins[i].bit);
  }

  enum : uint32_t {
    k_ra_blink_half_period_ms = 500U, /**< 1 Hz square wave. */
  };

  while (1) {
    ra_blink_drive_all(1U);
    ra_delay_ms(k_ra_blink_half_period_ms);
    ra_blink_drive_all(0U);
    ra_delay_ms(k_ra_blink_half_period_ms);
  }

  return 0;
}
#pragma GCC diagnostic pop
