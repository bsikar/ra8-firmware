/**
 * @file examples/ek_ra8d2/hw_pending/lowpower_holdpage/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: hold the page in low power
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #150 low-power-mode demo. It is compiled as a wholly separate ELF
 * (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 "renders" page 0, releases this core via `ra8_cpu1_release` (HUM
 * Ch 2.9.1), and PARKS. From then on the M33 is the live core.
 *
 * Its job is the e-reader's idle / hold loop -- everything the device spends
 * almost all its time doing -- run on the slow core at a fraction of the power:
 *
 *   1. Light board LED1 (BLUE, P600 = PORT6 pin 0) as the "active core = M33"
 *      indicator. The M85 never drives this pin, so a toggling LED is honest
 *      proof the M33 is the core holding the page -- on silicon and in the
 *      board_sim GPIO/LED view alike.
 *   2. Service the user switch SW1 (P009) as the page-turn input: on a press
 *      edge the M33 advances the held `page_num` and counts the event in the
 *      shared mailbox, without waking the M85.
 *   3. Bump the mailbox heartbeat each loop so the parked M85 can narrate that
 *      the M33 is alive.
 *
 * Re-rendering a new page is M85-heavy work (ra8_gfx is Helium-tuned, 1 GHz-
 * assuming); the M33 holds the existing page and advances the page number, and
 * waking the M85 for the heavy re-render is the remaining piece tracked in #150.
 *
 * @note The M33 deliberately does NOT call `ra8_log`. board_sim echoes only the
 *       primary core's ITM, so an M33 `ra8_log` line would be invisible; the
 *       proof-of-life is the LED + the mailbox heartbeat the M85 narrates.
 * @note Only PCNTR1/PCNTR2 are touched. The LED1 / SW1 pins power up routed to
 *       PORT, so no PmnPFS / PWPR pin-function setup is needed here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "lowpower_holdpage.h"

/** @brief CPU1 stack top (slot 0 of the M33 vector table). */
extern uint32_t g_ra8_ls_cpu1_stack_top;
/** @brief CPU1 `.data` run-region start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_start;
/** @brief CPU1 `.data` run-region end. */
extern uint32_t g_ra8_ls_cpu1_data_end;
/** @brief CPU1 `.data` load image (in MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_load;
/** @brief CPU1 `.bss` start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_bss_start;
/** @brief CPU1 `.bss` end. */
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @enum m33_port_addr_t
 * @brief MMIO addresses of the PORT registers the M33 hold loop drives.
 * @details The PORT block base is 0x40400000 with a 0x20 per-port stride. LED1
 *          (BLUE) is P600 = PORT6 pin 0, so PORT6 PCNTR1 is at 0x404000C0. SW1
 *          is P009 = PORT0 pin 9, read via PORT0 PCNTR2 (the read-only register
 *          carrying PIDR) at 0x40400004. (HUM Ch 20 "I/O Ports".)
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_port6_pcntr1_addr = 0x404000C0U, /**< PORT6 PCNTR1: {PODR[31:16], PDR[15:0]} (LED1). */
  k_port0_pcntr2_addr = 0x40400004U, /**< PORT0 PCNTR2: {EIDR[31:16], PIDR[15:0]} (SW1). */
} m33_port_addr_t;

/**
 * @enum m33_hold_const_t
 * @brief PCNTR field constants and the hold-loop delay bound.
 * @details PCNTR1 packs the direction latch PDR in its low half and the output
 *          latch PODR in its high half, so LED1 (pin 0) is held an output (PDR
 *          bit 0) while its level is set by PODR bit 0 (bit 16 of the word). SW1
 *          (PORT0 pin 9) reads back in PIDR bit 9 of PCNTR2; it is active-low
 *          (pressed = 0).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_led1_pdr_out = 0x00000001U, /**< PCNTR1 PDR pin 0 = output direction.        */
  k_led1_podr_on = 0x00010000U, /**< PCNTR1 PODR pin 0 = drive high (bit 16).    */
  k_sw1_pidr_bit = 9U,          /**< SW1 = PORT0 pin 9 (PIDR bit 9).             */
  k_sw1_released = 1U,          /**< SW1 active-low: 1 = released (not pressed). */
  k_sw1_pressed  = 0U,          /**< SW1 active-low: 0 = pressed.                */
  k_hold_spins   = 3000000U,    /**< Bounded busy-delay spins per hold pass.     */
} m33_hold_const_t;

/**
 * @brief CPU1 application loop: hold the page, blink the active-core LED, poll SW1.
 *
 * @details Each pass: bump the shared mailbox heartbeat (so the parked M85 sees
 * the M33 is alive), read SW1 (P009) and on a press edge advance the held
 * page_num + count the event, then toggle LED1 (the "active core = M33"
 * indicator). A bounded busy-delay paces the loop. The M85 never drives LED1 or
 * page_num after handoff, so this loop's effects prove the M33 owns the page.
 *
 * @return This function never returns.
 * @note Control never leaves the hold loop.
 *
 * @pre `cpu1_reset_handler` has initialised `.data`/`.bss`.
 * @pre The PORT0/PORT6 blocks are reachable; the M85 published the mailbox.
 * @post LED1 (PORT6 pin 0) is configured as an output and toggles each pass.
 * @post The mailbox heartbeat advances and SW1 presses bump page_num.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_hold_page(void)
{
  volatile lowpower_mailbox_t* mb       = lowpower_mailbox();
  volatile uint32_t*           pcntr1   = (volatile uint32_t*)k_port6_pcntr1_addr;
  volatile uint32_t*           pcntr2   = (volatile uint32_t*)k_port0_pcntr2_addr;
  uint32_t                     led      = 0U;
  uint32_t                     sw1_prev = (uint32_t)k_sw1_released;

  while (1) {
    for (volatile uint32_t d = 0U; d < (uint32_t)k_hold_spins; d++) {
      __asm volatile("nop");
    }
    mb->m33_heartbeat = mb->m33_heartbeat + 1U;

    /* HUM Ch 20.2 "PCNTR2 : Port Control Register 2" p 840 -- read PIDR; bit 9
     * is SW1 (P009), active-low (pressed = 0). */
    const uint32_t pidr    = *pcntr2;
    const uint32_t sw1_now = (pidr >> (uint32_t)k_sw1_pidr_bit) & 1U;
    /* Detect a fresh press edge (was released, now pressed): a held-page turn
     * the slow core handles without waking the M85. Split conditions to avoid a
     * compound boolean decision. */
    if (sw1_prev == (uint32_t)k_sw1_released) {
      if (sw1_now == (uint32_t)k_sw1_pressed) {
        mb->sw1_events = mb->sw1_events + 1U;
        mb->page_num   = mb->page_num + 1U;
      }
    }
    sw1_prev = sw1_now;

    /* Toggle LED1 as the "active core = M33" indicator. */
    led ^= 1U;
    uint32_t value = (uint32_t)k_led1_pdr_out;
    if (led != 0U) {
      value |= (uint32_t)k_led1_podr_on;
    }
    /* HUM Ch 20.2 "PCNTR1 : Port Control Register 1" p 838 -- write {PODR, PDR}:
     * hold PORT6 pin 0 an output and drive LED1 (BLUE, P600) to the new level. */
    *pcntr1 = value;
  }
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then enter the hold loop.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra8_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @note Control passes to ::cpu1_hold_page, which loops forever.
 *
 * @pre Hardware loaded the initial SP from `.cpu1_vectors[0]`.
 * @pre The M85 released this core via the CPU1ACTCSR handshake.
 * @post `.data` mirrors its MRAM_CPU1 load image.
 * @post `.bss` is zero-filled.
 *
 * @note Entered only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  uint32_t* dst = &g_ra8_ls_cpu1_data_start;
  uint32_t* src = &g_ra8_ls_cpu1_data_load;
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_hold_page();
}

/**
 * @brief CPU1 default fault handler: park the core.
 *
 * @details Every M33 exception slot routes here. The core stops making forward
 * progress; on hardware a watchdog (if enabled) eventually resets.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post The M33 makes no further forward progress.
 * @post LED1 stops toggling and the heartbeat freezes, observable from the M85.
 *
 * @note Shared default for all CPU1 exception vectors.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fault_handler(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @var g_cpu1_vector_table
 * @brief Minimal Armv8-M (baseline) vector table for the M33 image.
 * @details Slot 0 is the initial SP, slot 1 the reset handler; the remaining
 *          core-exception slots share the fault handler. The M85 points
 *          CPU1INITVTOR at this table when it releases the core.
 * @note Placed in the `.cpu1_vectors` section by `linker_script_cpu1.ld`.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
#ifndef RA8_SIMULATOR_MODE
/* The vector table is only meaningful in the cross-compiled M33 image. The host
 * unit-test build compile-checks this TU but never links it as an executable, so
 * dropping the table there costs no coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
