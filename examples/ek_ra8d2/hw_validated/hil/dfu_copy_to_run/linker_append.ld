/*
 * dfu_copy_to_run: probe words at a fixed SRAM address.
 *
 * The DFU host reads these two words straight out of SRAM to watch the
 * copy-to-run launch, so the address is part of the test contract and
 * cannot float with the link. NOLOAD: the image carries no initialiser for
 * them, the launched payload writes them at run time.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

SECTIONS
{
    .dfu_probe 0x22010000 (NOLOAD) :
    {
        KEEP(*(.dfu_probe.sentinel))
        KEEP(*(.dfu_probe.heartbeat))
    } > SRAM
}
