# flash_journal

Erase + program + read-back round-trip of a 16-byte counter record
on the on-board IS25LX512M Octo-SPI flash (U16) every second.

Banner on success:
- `g_fj_match` advances by >= 3 in 5 s windows
- `g_fj_mismatch` stays at 0
- `g_fj_jedec_id` reads non-FFFFFF (real JEDEC manufacturer + memory
  type bytes for the IS25LX512M)

## Why this is `hw_pending`

The OSPI flash chip on the test board is **physically unresponsive**.
Confirmed via direct J-Link JTAG probing of XSPI0's manual-command
engine (CDBUF + CDCTL0 + INTS) on 2026-05-19:

```
=== 1S-1S-1S JEDEC ID 0x9F ===
CDBUF[2] = 0x00FFFFFF   (all data bits float high, no CIPO drive)
INTS     = 0x00000001   (controller's CMDCMP fired -- it ran the clock)

=== RDSR 0x05 ===
CDBUF[2] = 0x000000FF   (status byte = 0xFF -- bus floating)

=== 0xAB release from deep-power-down, then RDSR ===
CDBUF[2] = 0x000000FF   (still floating)

=== SFDP read at 0x00 ===
CDBUF[2] = 0xFFFFFFFF   (still floating)

=== 1S-8S-8S JEDEC ID ===
CDBUF[2] = 0x00FFFFFE   (one bit toggled -- bus may have weak drive)

=== XSPI1 manual JEDEC ID ===
CDBUF[2] = 0x00000000   (lines pulled LOW -- XSPI1 not connected to
                         this flash at all)
```

Interpretation:
* The OSPI controller itself works -- CMDCMP fires, TRREQ self-
  clears, all registers programme correctly.
* Pins are routed correctly: PFS shows P104/CS, P808/CLK, P801/DQS,
  P100/DQ0, P803/DQ1, etc. all at PSEL=0x1C (OSPI) with PMR=1.
* MSTPCRB.MSTPB16 (OSPI0) is ungated.
* OCTACKCR=0x01 (MOCO source, SRDY handshake done).
* RESET_L (P106) is driven HIGH per PFS=0x07 + PCNTR1.PODR bit 6 = 1.
* Bus reads float at 0xFF in 1S mode (consistent with the SCL/SDA
  pull-ups expected for an unpopulated bus on the chip side).

That all points to the IS25LX512M chip itself: it is not powered, not
soldered, dead, or held in some state where it ignores all commands.

## How to graduate back to `hw_validated/hil/`

Physical checks the firmware cannot do:

1. Multimeter on U16 VCC pin: confirm 3.3 V is present.
2. Probe SCLK and CS at U16 while the firmware runs ra_xspi_init +
   ra_xspi_flash_read_id: SCLK should toggle for 8 cycles
   (1S mode), CS should pulse low for the duration.
3. Visual inspection: confirm U16 is populated and pin 1 is in
   the correct orientation.
4. If U16 is dead, replacing it with a known-good IS25LX512M (or
   compatible) lets the existing firmware bring the chip up
   correctly; the HAL, OCTACKCR handshake, OCTA pin routing, and
   RESET_L pulse are all already in place.

The HAL-side diagnostics added to this demo (g_fj_last_step /
g_fj_last_counter / g_fj_last_echoed / g_fj_jedec_id) remain in
place for any future bench-debug session.

## Build + flash

```sh
make flash_journal
make -C examples/ek_ra8d2/hw_pending/flash_journal flash
```
