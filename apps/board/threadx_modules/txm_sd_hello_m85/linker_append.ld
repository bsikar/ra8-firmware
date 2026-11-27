/*
 * txm_sd_hello_m85: where fallback packed `.ra8app` images sit.
 *
 * The image carries signed `.ra8app` modules in MRAM: a fixed, 1 KiB-aligned
 * home and symbols naming its first and one-past-last bytes, so when no
 * micro-SD card is present, card.readApp falls back to these verified images.
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
