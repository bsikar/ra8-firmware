/*
 * Shared `.txm_module` section fragment for EK-RA8D2 CPU1 (Cortex-M33)
 * ThreadX Module Manager apps (RA8FW-1273).
 *
 * Composed after `libs/ra8_board_ek_ra8d2/ld/linker_script_cpu1.ld`:
 * places `.txm_module` 1 KiB aligned in MRAM_CPU1 so the module's code
 * region satisfies MPU alignment, and defines
 * `g_ra8_ls_cpu1_txm_module_start` at its preamble byte.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

SECTIONS
{
    .txm_module : ALIGN(1024)
    {
        g_ra8_ls_cpu1_txm_module_start = .;
        KEEP(*(.txm_module))
    } > MRAM_CPU1
}
