# flash_journal

Erase + program + read-back round-trip of a 16-byte counter record
on the on-board IS25LX512M Octo-SPI flash (U16) every second.

Banner on success:
- `g_fj_match` advances by >= 3 in 5 s windows
- `g_fj_mismatch` stays at 0
- `g_fj_jedec_id` reads non-FFFFFF (real JEDEC manufacturer + memory
  type bytes for the IS25LX512M)

## Status: working (#44 resolved, 2026-06-10)

The flash reads and round-trips correctly. Verified live on EK-RA8D2 v1:

```
g_fj_jedec_id = 0x009D5A1A   (ISSI IS25LX512M: mfr 0x9D, type 0x5A, cap 0x1A)
g_fj_match    advancing       (erase -> program -> read-back -> compare passes)
g_fj_mismatch = 0
g_fj_last_step = 4            (compare OK)
```

### Root cause (it was a firmware chip-select bug, not hardware)

`g_fj_jedec_id` used to read `0x00FFFFFF`. The cause was in `ra_xspi`, not
the board: the on-board IS25LX512M's chip-select (`OSPI_FLASH_S_L`, P104)
is wired to the xSPI controller's **CS1**, but `ra_xspi_init` hard-coded
**CS0**, so it strobed an unconnected pin and the data line floated. The
Renesas FSP `ospi_b` example confirms it (`module.driver.ospi_b.channel =
channel.1`). A second missing step was the controller-driven
`LIOCTL.RSTCS` reset pulse, which returns the part to 1S SPI after a prior
OPI/DOPI session. Both are fixed in `libs/ra_hal/src/ra_xspi.c`.

It was proven a firmware issue (not hardware/switch/power) by having
J-Link's own OSPI flash loader erase/program/verify arbitrary patterns at
`0x90000000` -- which only works if the part is electrically reachable.

### Earlier conclusions that were WRONG (corrected)

* "Physical SW4 isolates the flash / firmware cannot fix it" -- false. With
  SW4 all-OFF (the correct Octo-SPI-active layout) the flash works once the
  driver targets CS1. The `0xF8` value read back from the U15 expander is
  the **expected** all-SW4-OFF state, not an isolation indicator.
* "Macronix MX25UM25645G, mfr 0xC2" -- the part is an **ISSI IS25LX512M**
  (mfr 0x9D). `ra_board_io_expander_set_octospi_active` now writes `0xF8`
  (OSPI_OE_L low), matching the FSP `board_cfg_switch_init`.
* "JTAG-confirmed dead chip" (commit `436a3cb6`) -- also wrong.

The HAL-side diagnostics (g_fj_last_step / g_fj_last_counter /
g_fj_last_echoed / g_fj_jedec_id / g_fj_expander_err) remain for any future
bench session.

## Build + flash

```sh
make flash_journal
make -C examples/ek_ra8d2/hw_pending/flash_journal flash
```
