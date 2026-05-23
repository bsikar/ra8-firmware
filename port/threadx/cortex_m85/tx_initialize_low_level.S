/*
 * Project: ra8d2-firmware
 * File:    port/threadx/cortex_m85/tx_initialize_low_level.S
 * Brief:   ThreadX Cortex-M85 low-level init, RA8D2-tuned
 *
 * Replaces the upstream ports/cortex_m85/gnu/src/tx_initialize_low_level.S
 * so we do not collide with the project's vector table or fault
 * trampolines. Specifically:
 *
 *   - The project's `vector_table.c` already provides (and pins to MRAM)
 *     `g_ra_vector_table_start`. The upstream port writes VTOR with a
 *     symbol called `_vectors`, which is not what we use; here we leave
 *     VTOR alone (SystemInit set it correctly during Reset_Handler).
 *
 *   - The project's `vector_table.c` provides the HardFault / UsageFault
 *     trampolines that hand off to ra_exception_report(). The upstream
 *     port's tx_initialize_low_level.S re-defines those handlers, which
 *     would multi-define link errors. We omit them here.
 *
 *   - SysTick_Handler is supplied by `vector_table.c` (it routes to
 *     `_tx_timer_interrupt`); we configure SysTick here to fire at
 *     TX_TIMER_TICKS_PER_SECOND (1000 by default for this firmware).
 *
 *   - System clock at this point is the boot-default MOCO ~8.4 MHz
 *     (matches blink_hal). When ra_cgc_init() upgrades the clock,
 *     `ra_threadx_systick_retune()` (C) reprograms SysTick.LOAD so the
 *     1 ms tick is preserved -- not implemented in this scaffold; the
 *     demo runs entirely on MOCO so the tick stays accurate.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif

/* Apps that use ThreadX are expected to bring CPUCLK0 up to its
 * post-cgc target (1 GHz on the EK-RA8D2 / RA8D2 group) BEFORE
 * calling tx_kernel_enter.  Reload uses the post-cgc rate so 1 ms
 * tick is actually 1 ms wall-clock, not 1 ms scaled by the
 * 8.4 MHz reset clock.  1 GHz / 1000 = 1,000,000 -- fits the
 * 24-bit SysTick.LOAD register (max 0xFFFFFF == 16,777,215). */
                .equ    RA_BOOT_CLOCK_HZ,        1000000000
                .equ    RA_THREADX_TICK_HZ,      1000
                .equ    RA_SYSTICK_RELOAD,       ((RA_BOOT_CLOCK_HZ / RA_THREADX_TICK_HZ) - 1)

/* NVIC / SCB register offsets (relative to 0xE000E000). */
                .equ    NVIC_BASE,               0xE000E000
                .equ    SYST_RVR_OFFSET,         0x14    /* SysTick reload */
                .equ    SYST_CVR_OFFSET,         0x18    /* SysTick current value */
                .equ    SYST_CSR_OFFSET,         0x10    /* SysTick control / status */
                .equ    SHPR2_OFFSET,            0xD1C   /* System handlers 8-11 priority */
                .equ    SHPR3_OFFSET,            0xD20   /* System handlers 12-15 priority */
                .equ    DWT_CTRL,                0xE0001000

/*
 * VOID _tx_initialize_low_level(VOID)
 *
 * Called once from `_tx_initialize_kernel_enter` before the scheduler
 * starts. By the time we get here the project's Reset_Handler has run
 * SystemInit (VTOR set, FPU enabled, IRQs masked) and copied .data /
 * zeroed .bss. We just have to:
 *
 *   1. Save the system stack pointer for ISR processing.
 *   2. Programme SysTick to fire at TX_TIMER_TICKS_PER_SECOND.
 *   3. Lower SVC priority below PendSV (PendSV must be the lowest).
 *   4. Tell ThreadX where free RAM begins via _tx_initialize_unused_memory.
 *
 * For the demo the unused-memory pointer is set to a known sentinel so
 * the scheduler does not page-fault on free-RAM lookups; the demo uses
 * fully static thread stacks so it never reads through the pointer.
 */
                .section .text._tx_initialize_low_level
                .balign 4
                .syntax unified
                .eabi_attribute Tag_ABI_align_preserved, 1
                .global  _tx_initialize_low_level
                .thumb_func
                .type _tx_initialize_low_level, function
_tx_initialize_low_level:

                /* IRQs already masked by SystemInit; restate for safety. */
                CPSID   i

                /* Save the boot main-stack pointer (= end of SRAM, set
                 * from the linker-script symbol via the vector table)
                 * for any future ISR processing that needs it. */
                LDR     r0, =_tx_thread_system_stack_ptr
                LDR     r1, =g_ra_vector_table_start
                LDR     r1, [r1]
                STR     r1, [r0]

                /* Tell ThreadX where the first byte of unused RAM lives.
                 * We pin it to the linker-provided sentinel; the demo
                 * uses static stacks so this only has to be a valid
                 * (non-NULL) pointer. */
                LDR     r0, =_tx_initialize_unused_memory
                LDR     r1, =g_ra_threadx_unused_memory_start
                STR     r1, [r0]

                /* Enable the DWT cycle counter (used by tx_trace and the
                 * project's profiling shims). */
                LDR     r0, =DWT_CTRL
                LDR     r1, [r0]
                ORR     r1, r1, #1
                STR     r1, [r0]

                /* Programme SysTick: reload at RA_SYSTICK_RELOAD,
                 * processor-clock source, exception enabled. */
                LDR     r0, =NVIC_BASE
                LDR     r1, =RA_SYSTICK_RELOAD
                STR     r1, [r0, #SYST_RVR_OFFSET]
                MOV     r1, #0
                STR     r1, [r0, #SYST_CVR_OFFSET]
                MOV     r1, #0x7
                STR     r1, [r0, #SYST_CSR_OFFSET]

                /* Priority configuration: SVC = 0xFF (lowest), PendSV =
                 * 0xFF (lowest). SysTick = 0x40. SHPR2 holds slot 11
                 * (SVC) in byte 3. SHPR3 holds slot 14 (PendSV) in
                 * byte 2 and slot 15 (SysTick) in byte 3. */
                LDR     r1, =0xFF000000
                STR     r1, [r0, #SHPR2_OFFSET]
                LDR     r1, =0x40FF0000
                STR     r1, [r0, #SHPR3_OFFSET]

                /* Latch the global flag that tells the shared
                 * SysTick_Handler in libs/ra_core/src/ra_time.c
                 * that ThreadX has finished priming its timer
                 * subsystem. Until this is set, the handler MUST
                 * NOT call _tx_timer_interrupt -- the kernel
                 * timer infrastructure is uninitialised between
                 * ra_time_init() (which arms SysTick at 1 kHz
                 * from C) and tx_kernel_enter() (which runs us);
                 * a stray timer ISR in that window faults with
                 * UFSR.INVPC and bricks the boot. Reproduced on
                 * threadx_netx_tcp_echo because the
                 * ra_board_ethernet_init() between those two
                 * calls is long enough that SysTick always
                 * fires inside it. Storage lives in
                 * port/threadx/cortex_m85/tx_systick_ready.c
                 * (zero-initialised in .bss). */
                LDR     r0, =g_ra_threadx_systick_ready
                MOV     r1, #1
                STR     r1, [r0]

                BX      lr
                .size _tx_initialize_low_level, .-_tx_initialize_low_level

                .end
