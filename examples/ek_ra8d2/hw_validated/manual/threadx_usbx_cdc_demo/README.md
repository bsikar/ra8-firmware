# threadx_usbx_cdc_demo

Eclipse ThreadX + USBX CDC ACM echo demo on the EK-RA8D2's on-board
USB-FS receptacle (J11). Same hardware test as `examples/usb_cdc_echo`
but using USBX's class layer (`ux_device_class_cdc_acm_initialize`)
instead of the project's hand-rolled `ra8_usb_cdc` layer.

## What it does

A single ThreadX worker brings the USBX device stack up against the
project's `ra8_usb` DCD bridge, registers the CDC ACM class, attaches
the bus (raises the D+ pull-up), and then loops:

```
ux_device_class_cdc_acm_read -> ux_device_class_cdc_acm_write
```

LED1 toggles once per echoed byte so a host-side typing test produces
a visible blink pattern.

USB device is enumerated with VID/PID `0x1209:0x000A` (pid.codes test
range) at FS, configuration 1 (CDC), endpoints EP1 IN bulk (64 B), EP2
OUT bulk (64 B), EP3 IN interrupt (8 B), as a single CDC ACM
interface association.

## Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)

| Net           | Pin    | PFS PSEL                | Direction                      |
|---------------|--------|-------------------------|--------------------------------|
| USB_FS_VBUS   | P4_07  | k_ra8_psel_usb_fs (0x13) | VBUS sense (peripheral input). |
| USB_FS_VBUSEN | P5_00  | k_ra8_psel_usb_fs (0x13) | VBUS-enable drive (output).    |
| USB_FS_DP     | P8_14  | k_ra8_psel_usb_fs (0x13) | D+ data line.                  |
| USB_FS_DM     | P8_15  | k_ra8_psel_usb_fs (0x13) | D- data line.                  |

## Sequence

1. `ra8_cgc_init()` -- standard FSP-quickstart clock tree.
2. `ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, ...)` + `ra8_time_init`.
3. `ra8_board_led_init(k_ra8_board_led1)`.
4. `ra8_pfs_route_peripheral` for the four USB-FS pins.
5. `ra8_isr_globals_enable` then `tx_kernel_enter`.
6. Worker (4 KiB stack, prio 8): `ux_system_initialize` (16 KiB pool)
   -> `ux_device_stack_initialize` -> `ux_device_class_cdc_acm_initialize`
   -> `ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs)` ->
   `ra8_usb_device_attach(true)` -> echo loop.

## Verification (macOS)

After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates as
`/dev/cu.usbmodem*`. Open it with picocom or screen and type
characters; every byte echoes back and LED1 toggles per byte.

```sh
picocom -b 115200 /dev/cu.usbmodemXXXX
```

## Build + flash

From the repo root:

```sh
make threadx_usbx_cdc_demo
bash scripts/dev/flash.sh build/threadx_usbx_cdc_demo/threadx_usbx_cdc_demo.hex
```

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (per EK-RA8D2 v1 UM
Table 24 "EK-RA8D2 Board LED Functions" p 31). The USB-FS pin set
(P407 / P500 / P814 / P815) is the only routing the chip exposes for
the on-board J11 USB-FS receptacle (EK-RA8D2 v1 UM Table 22 "USB Full
Speed Port Pin Assignments" p 30); main.c programs this pin set
directly via `ra8_pfs_route_peripheral`.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual
(R20UT5523EG0101 Rev 1.01) Table 22 p 30 + Table 24 p 31, HUM
(R01UH1065EJ0130) Ch "USBFS", and Eclipse USBX CDC ACM class API.

## HIL plan

**HIL-able after firmware fix -- currently halts in
`ra8_exception_halt_loop` during init.** Demoted from
`hw_validated/hil/` on 2026-05-18 (commit 1f46ad3b) for exactly this
reason; the existing `hil.conf` is parked at `HIL_MODE=alive` /
`HIL_BOOT_S=2` and will start passing once the init halt is
root-caused and fixed.

The Pi can act as USB host today (`scripts/hil/usb_test.sh` already
covers `usb_cdc_echo`). Once the init halt is fixed, this app
should run a `hil_usb_cdc_echo` style gate: Pi enumerates the chip
as `/dev/ttyACMx`, writes a known string, expects an echo within a
timeout.

Stays in `hw_pending/` until the init halt is root-caused.
