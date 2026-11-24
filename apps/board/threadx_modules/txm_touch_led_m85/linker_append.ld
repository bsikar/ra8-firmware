/*
 * txm_touch_led_m85: where the packed `.ra8app` sits (RA8FW-1262).
 *
 * The image carries the touch-to-LED module's signed `.ra8app` in MRAM: a
 * fixed, 1 KiB-aligned home and symbols naming its first and one-past-last
 * bytes, so the manager copies the whole blob out to SRAM before admitting
 * it. Same shape as txm_manager_ra8p1's `.txm_module` home.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

SECTIONS
{
    .txm_module : ALIGN(1024)
    {
        g_ra8_ls_txm_module_start = .;
        KEEP(*(.txm_module))
        g_ra8_ls_txm_module_end = .;
    } > MRAM
}
