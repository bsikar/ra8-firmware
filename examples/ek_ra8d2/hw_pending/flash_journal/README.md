# flash_journal

Erase + program + read-back round-trip of a 16-byte counter record
on the on-board IS25LX512M Octo-SPI flash (U16) every second.

Banner on success:
- `g_fj_match` advances by >= 3 in 5 s windows
- `g_fj_mismatch` stays at 0
- `g_fj_jedec_id` reads non-FFFFFF (real JEDEC manufacturer + memory
  type bytes for the IS25LX512M)

## Why this is `hw_pending` -- and what the actual root cause is

`g_fj_jedec_id` reads `0x00FFFFFF` (bus floating, no CIPO drive). The
**root cause is the physical SW4 DIP-switch configuration**, NOT a dead
chip and NOT a firmware bug. The on-board Octo-SPI flash (U16, a Macronix
**MX25UM25645G**, manufacturer `0xC2`) is electrically isolated from the
MCU's OCTA bus unless the SW4 switches are set correctly.

Per the Renesas FSP EK-RA8D2 `ospi_b` example, **all 8 SW4 switches must be
set OFF** to use the on-board Octo-SPI flash. The board ships with them
otherwise, leaving the flash disconnected.

### The firmware cannot fix this (bench-proven, 2026-06)

This demo drives the **U15 PI4IOE5V6408 I/O expander** to *request* the
all-OFF layout (`ra_board_io_expander_set_octospi_active`, writes `0xFF`).
Reading U15 back over RIIC1 shows the override is programmed perfectly but
**defeated by the physical switches**:

```
U15 output latch (reg 0x05) = 0xFF   (we command all lines HIGH = all OFF)
U15 IODIR        (reg 0x03) = 0xFF   (all pins are outputs)
U15 Hi-Z         (reg 0x07) = 0x00   (outputs enabled, not Hi-Z)
U15 input level  (reg 0x0F) = 0x00   (actual pins read LOW -- switches win)
```

The expander latches `0xFF` but the pins stay at `0x00`: the physical SW4
switches overpower the push-pull expander outputs and hold the lines LOW
(= switches ON = Octo-SPI inactive). So the I/O-expander override is
best-effort only and **cannot** connect the flash.

### What was verified correct

* OSPI controller works -- CMDCMP fires, all registers programme.
* Pin map is correct, cross-checked against the FSP `ospi_b` example
  pin config: `P100=SIO0, P101=SIO3, P102=SIO4, P103=SIO2, P104=CS1,
  P106=RESET, P800=SIO5, P801=DQS, P802=SIO6, P803=SIO1, P804=SIO7,
  P808=SCLK` -- all match the BSP `s_xspi_octa_pins` table.
* A GPIO bit-bang of `0x9F` (bypassing the OSPI controller entirely) with
  validated CS/CLK/SI stimulus also reads `0xFF` on every data line --
  consistent with the bus being disconnected by SW4, not a dead chip.

> The earlier "JTAG-confirmed dead chip" conclusion (commit `436a3cb6`,
> 2026-05-19) was **wrong** -- it was confounded by SW4 being in the
> Octo-SPI-inactive position during testing.

## How to use the flash / graduate to `hw_validated/hil/`

1. **Physically set all 8 SW4 switches to OFF** (at minimum SW4-3 and
   SW4-4). This is the actual fix; the firmware cannot do it.
2. Re-run `flash_journal`; `g_fj_jedec_id` should read `0xC2...`
   (Macronix) and `g_fj_match` should advance.
3. Then flip the hil.conf to a real pass gate and promote.

The HAL-side diagnostics (g_fj_last_step / g_fj_last_counter /
g_fj_last_echoed / g_fj_jedec_id / g_fj_expander_err) remain for any
future bench session.

## Build + flash

```sh
make flash_journal
make -C examples/ek_ra8d2/hw_pending/flash_journal flash
```
