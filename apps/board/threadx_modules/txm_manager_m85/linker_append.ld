/*
 * txm_manager_m85: where the packed module sits (RA8FW-795).
 *
 * The M85 Module Manager loads txm_hello_m33 in place, so the blob needs a
 * fixed, 1 KiB-aligned home in MRAM and a symbol naming its first byte (the
 * preamble). Same shape as txm_manager_cpu1's `.txm_module` in MRAM_CPU1.
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
    } > MRAM
}
