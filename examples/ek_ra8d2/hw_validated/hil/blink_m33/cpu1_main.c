/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/blink_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: blink LED1 forever
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33.
 * It is compiled as a wholly separate ELF (`-mcpu=cortex-m33`) and embedded into
 * the M85 ELF as a `.cpu1_image` blob; the M85 releases this core at runtime via
 * `ra8_cpu1_release` (HUM Ch 2.9.1 "CPU control registers"), then sleeps.
 *
 * Its job is intentionally tiny so the dual-core plumbing is what stands out: it
 * drives board LED1 (BLUE, P600 = PORT6 pin 0) high and low forever with a
 * bounded busy-delay between toggles. A blinking LED that the M85 never touches
 * is honest proof the M33 came out of reset and is executing its own code -- on
 * silicon and in the ra8_emulator GPIO/LED view alike.
 *
 * @note The M33 deliberately does NOT call `ra8_log`. On hardware each core has
 *       its own CoreSight ITM and the ra8_emulator echoes only the primary
 *       core's ITM, so an M33 `ra8_log` line would be invisible in the fake.
 *       The proof-of-life is the LED transition the M85 never drives.
 * @note Only PCNTR1 is touched (direction + output level); the LED pins power up
 *       routed to PORT, so no PmnPFS / PWPR pin-function setup is needed here.
 *
 * @since 0.1.0
 */

#include <stdint.h>

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
 * @brief MMIO address of the PORT6 control register driving LED1.
 * @details The EK-RA8D2 LED1 (BLUE) is P600 = PORT6 pin 0. The PORT block base
 *          is 0x40400000 with a 0x20 per-port stride, so PORT6 PCNTR1 lives at
 *          0x40400000 + 6 * 0x20 = 0x404000C0 (HUM Ch 20 "I/O Ports").
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_port6_pcntr1_addr = 0x404000C0U, /**< PORT6 PCNTR1: {PODR[31:16], PDR[15:0]}. */
} m33_port_addr_t;

/**
 * @enum m33_blink_const_t
 * @brief PCNTR1 bit fields for LED1 and the blink-loop delay bound.
 * @details PCNTR1 packs the direction latch PDR in its low half and the output
 *          latch PODR in its high half, so pin 0 is held an output (PDR bit 0)
 *          while its level is set by PODR bit 0 (bit 16 of the word).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_led1_pdr_out = 0x00000001U, /**< PCNTR1 PDR pin 0 = output direction.         */
  k_led1_podr_on = 0x00010000U, /**< PCNTR1 PODR pin 0 = drive high (bit 16).     */
  k_blink_spins  = 3000000U,    /**< Bounded busy-delay spins per LED half-cycle. */
} m33_blink_const_t;

/**
 * @brief CPU1 application loop: blink LED1 (BLUE, P600) forever.
 *
 * @details Holds PORT6 pin 0 an output via PCNTR1.PDR and toggles its PODR level
 * on each pass, with a bounded busy-delay between toggles so the blink is
 * visible. The M85 never writes this pin, so the transitions prove the M33 is
 * running. See the file header.
 *
 * @return This function never returns.
 * @note Control never leaves the blink loop.
 *
 * @pre `cpu1_reset_handler` has initialised `.data`/`.bss`.
 * @pre The PORT6 block is reachable at ::k_port6_pcntr1_addr.
 * @post PORT6 pin 0 is configured as an output.
 * @post The LED1 level alternates on the configured cadence forever.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  volatile uint32_t* pcntr1 = (volatile uint32_t*)k_port6_pcntr1_addr;
  uint32_t           level  = 0U;

  while (1) {
    for (volatile uint32_t d = 0U; d < (uint32_t)k_blink_spins; d++) {
      __asm volatile("nop");
    }
    level ^= 1U;
    uint32_t value = (uint32_t)k_led1_pdr_out;
    if (level != 0U) {
      value |= (uint32_t)k_led1_podr_on;
    }
    /* HUM Ch 20.2 "PCNTR1 : Port Control Register 1" p 840 -- write {PODR, PDR}:
     * hold PORT6 pin 0 an output and drive LED1 (BLUE, P600) to the new level. */
    *pcntr1 = value;
  }
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then enter `cpu1_main`.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra8_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @note Control passes to `cpu1_main`, which loops forever.
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

  cpu1_main();
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
 * @post The LED stops toggling, which is observable from the M85.
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
#ifndef RA8_OFF_TARGET
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
