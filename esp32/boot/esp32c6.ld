/*
 * esp32c6.ld -- linker script for the ESP32-C6 RAM-app spike.
 *
 * Places the entire image in HP SRAM (512 KiB at 0x40800000, unified I/D) so
 * the ROM/esptool download path can load it to a single region and jump to
 * _start. For this RAM image the load address (LMA) equals the virtual address
 * (VMA), so the .data copy in start.S is a no-op -- but the script still emits
 * __data_lma / __data_start / __data_end so the same crt0 works unchanged once
 * a flash-XIP image (LMA in flash) is introduced (roadmap).
 *
 * Base address and size: TRM Ch 5.3.1 "Address Mapping" Table 5.3-1 p 171.
 *
 * Symbols consumed by boot/start.S:
 *   __global_pointer$  gp base for gp-relative addressing
 *   __stack_top        initial sp (top of the .stack region)
 *   __bss_start/__bss_end    zero-fill range
 *   __data_lma/__data_start/__data_end   .data copy range
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Reserved call stack for the single HP core (grows down from __stack_top). */
__stack_size = 0x4000;

MEMORY
{
    HPSRAM (rwx) : ORIGIN = 0x40800000, LENGTH = 512K
}

/*
 * Split the image into a read+execute segment (code, rodata) and a read+write
 * segment (data, bss, stack). The HP SRAM is physically unified I/D so both
 * live in one region, but modelling the two PT_LOAD segments with correct flags
 * keeps the ELF free of an RWX-segment warning and documents intent.
 *   FLAGS: PF_R=4, PF_W=2, PF_X=1  ->  R+X = 5, R+W = 6
 */
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.text.boot))     /* _start must lead the image. */
        *(.text .text.*)
    } > HPSRAM :text

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > HPSRAM :text

    .data : ALIGN(4)
    {
        __data_start = .;
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > HPSRAM :data
    __data_lma = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > HPSRAM :data

    .stack (NOLOAD) : ALIGN(16)
    {
        __stack_bottom = .;
        . += __stack_size;
        . = ALIGN(16);
        __stack_top = .;
    } > HPSRAM :data

    /* Drop toolchain metadata that must not land in the binary image. */
    /DISCARD/ :
    {
        *(.comment)
        *(.riscv.attributes)
        *(.eh_frame)
    }
}
