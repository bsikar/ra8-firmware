# cap_touch_demo

Capacitive Touch Sensing Unit (CTSU2) self-cap demo for the EK-RA8D2.
Exercises the full `ra_ctsu` driver API: open with self-cap multi-scan,
calibrate per electrode, set per-electrode threshold, scan, fetch raw
counts, and report a touch event over SCI8 when raw > threshold.

## Status: untested on hardware

The application compiles clean and exercises the driver API end-to-end
under the host unit-test stub, but the EK-RA8D2 v1 silk-pad to TS-line
mapping for the four enabled electrodes was **not** confirmed against
the User's Manual at authoring time. Update
`k_cap_touch_pin_electrodes[]` in `main.c` once the manual table is
checked.

## Where to touch on the EVM

The CTSU is internally indexed as TS00..TS35. Whichever physical pads
on the EK-RA8D2 silk-screen are wired to `TS00..TS03` are the active
electrodes for this demo (the bottom four bits of the electrode mask
are set in `k_cap_touch_electrode_mask`). The expected pads on the
EK-RA8D2 v1 board are:

```
TODO: confirm against EK-RA8D2 v1 manual
```

When a finger touches an active pad the firmware:

1. Toggles **LED1** (P6_00).
2. Prints `"touched <id>\r\n"` on the J-Link OB CDC port at 115200
   8N1, where `<id>` is the two-digit decimal electrode index.

## Pinout (placeholder)

```
TXD8 -- PD_02       (PFS PSEL = k_ra_psel_sci_async, 0x04)
RXD8 -- PD_03       (PFS PSEL = k_ra_psel_sci_async, 0x04)
TS00 -- P4_00       (PFS PSEL = 0x0C)   /* TODO: confirm */
TS01 -- P4_01       (PFS PSEL = 0x0C)   /* TODO: confirm */
TS02 -- P4_02       (PFS PSEL = 0x0C)   /* TODO: confirm */
TS03 -- P4_03       (PFS PSEL = 0x0C)   /* TODO: confirm */
LED1 -- P6_00       (GPIO output, low at reset)
```

The PSEL code 0x0C for CTSU touch-sense lines is from HUM Ch 20.4
"Peripheral I/O Table". Once `ra_gpio_constants.h` grows a
`k_ra_psel_ctsu` constant the placeholder cast in `main.c` should be
replaced with the named enumerator.

## Build + flash

From the repo root:

```sh
make cap_touch_demo                       # cross-compile -> examples/cap_touch_demo/build/cap_touch_demo.elf
make -C examples/cap_touch_demo flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/cap_touch_demo/
make
make flash
make clean
```

## What the firmware does

1. `ra_cgc_init()` brings up XTAL + PLL1 (CPUCLK0 = 1 GHz, PCLKA =
   125 MHz, PCLKB = 62.5 MHz).
2. `ra_pfs_route_peripheral()` routes `PD02 / PD03` to SCI8 and the
   four placeholder electrode pins to CTSU.
3. `ra_sci_init(8, 115200 8N1)` opens the J-Link diagnostic stream.
4. `ra_mstp_init()` and `ra_ctsu_open()` power the CTSU in self-cap
   multi-scan mode (`CTSUCR1.MD = 1`, `CTSUDCLKC = PCLKB / 16`).
5. Per-electrode `ra_ctsu_calibrate()` writes a baseline offset into
   the driver state via the FSP-style 3-dummy-scan averaging.
6. `ra_ctsu_set_threshold(i, 256)` per electrode (FSP example default).
7. Loop: `ra_ctsu_scan_start()` then 50 ms wait, then per-electrode
   `ra_ctsu_get_data` + threshold comparison + LED1 toggle + SCI8
   print.

## Tuning

`k_cap_touch_threshold` (256 LSB) is the FSP default and works well
for a thin acrylic overlay. Thicker overlays will need the threshold
lowered; bare PCB pads can usually stay at 256. Re-measure with
`ra_ctsu_get_data` while the pad is untouched to fix the per-electrode
baseline noise floor before tightening the threshold.

## Debugging

```sh
make -C examples/cap_touch_demo ozone   # SEGGER Ozone GUI
make -C examples/cap_touch_demo debug   # gdb attached via JLinkGDBServer
```

Useful SWD probes (HUM Ch 51 register window):

```
mem32 0x40380000 32   # CTSU register window
mem32 0x40380004 1    # CTSUSR -- scan running?
mem32 0x40380020 1    # CTSUSC0 / CTSUSC1 -- last raw count
mem32 0x40400B40 1    # PD00 PFS -- SCI8 / CTSU mode bits
```
