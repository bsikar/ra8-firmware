#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Whole-file selftest fixtures for ``check_linker_scripts.py``.

This module is DATA only: the deliberately malformed and deliberately-tricky
linker scripts the checker's ``--selftest`` drives its rules against. The
enforcement logic and the builder-style fixtures that need the checker's own
tables (``_synth_option_script``, ``_sram_fixture``) stay in
``check_linker_scripts.py``.

The split is deliberate and follows ``lint_coverage_rules.py``: keeping the
static fixtures here means neither file drifts toward being the 1000-line
checker the file-size gate rejects, and a fixture edit is reviewable on its own.

Nothing here imports the checker, so the two files cannot form an import cycle.
"""

# A malformed script: no licence header, a region missing LENGTH, a tab indent,
# and an output section placed in a typo'd region. Must draw LD001-LD005.
MALFORMED = """\
/* A linker script with no licence header. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
\t.text : { *(.text) } > FLSAH
    .data : { *(.data) } > RAM
}
"""

# Legal but deliberately awkward: ENTRY and a bogus region name appear inside
# comments, a region name is a substring of another, and the header sits below
# a long banner. Nothing here is a real finding.
TRICKY = """\
/*
 * A perfectly legal script that mentions ENTRY(bogus_reset) and a
 * region called NOWHERE inside this comment, and places nothing in
 * either. It also talks about > NOWHERE as prose.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 64K
    RAM_EXT (rwx) : ORIGIN = 0x60000000, LENGTH = 8M
}

SECTIONS
{
    .text : { *(.text) } > RAM_EXT
    .data : { *(.data) } > RAM AT> RAM_EXT
}
"""


# Option-setting layout: a phantom data-flash region plus a wrong OFS0 address
# (both must draw LD007), and a correct-address twin that must stay silent.
OFS_BAD = """\
/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    DATA_FLASH (rw) : ORIGIN = 0x27000000, LENGTH = 16K
    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K
}

PROVIDE(OFS0_ADDR = 0x0300A100);

SECTIONS
{
    .text : { *(.text) } > MRAM
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS_CFG
}
"""

OFS_GOOD = """\
/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K
}

PROVIDE(OFS0_ADDR = 0x02C9F040);

SECTIONS
{
    .text : { *(.text) } > MRAM
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS_CFG
}
"""
