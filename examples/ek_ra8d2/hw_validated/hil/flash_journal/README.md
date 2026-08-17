# flash_journal

Erase, program and read-back round-trip of a small counter record on the
on-board IS25LX512M Octo-SPI flash (U16), once a second. `g_fj_match` advances
on every clean round-trip, `g_fj_mismatch` must stay at zero, and
`g_fj_jedec_id` must read a real JEDEC ID rather than all-ones -- which is what
makes this a proof that the flash round-tripped data, not just that the firmware
looped. `g_fj_last_step`, `g_fj_last_counter`, `g_fj_last_echoed` and
`g_fj_expander_err` are there for bench sessions.

## The flash was never dead -- it was a chip-select bug (#44)

`g_fj_jedec_id` used to read `0x00FFFFFF`. The cause was in `ra8_xspi`, not the
board: the on-board flash's chip select (`OSPI_FLASH_S_L`, P104) is wired to the
xSPI controller's **CS1**, but the driver hard-coded CS0, so it strobed an
unconnected pin and the data lines floated. A second missing step was the
controller-driven `LIOCTL.RSTCS` reset pulse, which returns the part to 1S SPI
after a prior OPI/DOPI session. It was established as firmware rather than
hardware by having J-Link's own OSPI flash loader erase, program and verify
arbitrary patterns at `0x90000000`, which only works if the part is
electrically reachable.

Three conclusions recorded along the way were wrong, and are worth naming so
nobody re-derives them:

- "Physical SW4 isolates the flash, so firmware cannot fix it" -- false. With
  SW4 all-OFF, which is the correct Octo-SPI-active layout, the flash works once
  the driver targets CS1. The `0xF8` read back from the U15 expander is the
  expected all-SW4-OFF state, not an isolation indicator.
- "Macronix MX25UM25645G, manufacturer `0xC2`" -- the part is an ISSI
  IS25LX512M, manufacturer `0x9D`.
- "JTAG-confirmed dead chip" -- also wrong.
