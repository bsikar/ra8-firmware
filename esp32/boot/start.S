/*
 * start.S -- crt0 for the ESP32-C6 RAM-app spike.
 *
 * Runs first (linked into .text.boot, KEEP'd at the image head by esp32c6.ld).
 * Establishes the C runtime, then calls app_main():
 *
 *   1. gp <- __global_pointer$   (with relaxation disabled around the load so
 *                                 the linker does not rewrite it gp-relative)
 *   2. sp <- __stack_top
 *   3. zero [__bss_start, __bss_end)
 *   4. copy .data from __data_lma to [__data_start, __data_end)
 *      (a no-op for this RAM image where LMA == VMA; correct for flash-XIP)
 *   5. call app_main
 *   6. wfi spin forever if app_main returns
 *
 * rv32imac / ilp32. No TRM register access here -- pure architectural setup.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

    .section .text.boot, "ax", @progbits
    .globl  _start
    .type   _start, @function
    .align  2
_start:
    /* gp for gp-relative addressing; norelax so 'la' is not rewritten. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* Top-of-stack for the single HP core. */
    la      sp, __stack_top

    /* Zero the .bss: for (t0 = __bss_start; t0 < __bss_end; t0 += 4) *t0 = 0. */
    la      t0, __bss_start
    la      t1, __bss_end
.Lbss_loop:
    bgeu    t0, t1, .Lbss_done
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       .Lbss_loop
.Lbss_done:

    /* Copy .data: src=__data_lma, dst=[__data_start, __data_end). */
    la      t0, __data_lma
    la      t1, __data_start
    la      t2, __data_end
.Ldata_loop:
    bgeu    t1, t2, .Ldata_done
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       .Ldata_loop
.Ldata_done:

    /* Enter the application. */
    call    app_main

    /* app_main should not return; park the core if it does. */
.Lhang:
    wfi
    j       .Lhang

    .size   _start, . - _start
