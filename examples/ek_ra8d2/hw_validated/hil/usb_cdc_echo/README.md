# usb_cdc_echo

Native USB device-mode CDC ACM smoke test for the EK-RA8D2. Brings up
the USB-FS controller (USBFS @ 0x40250000) in device mode via the
hand-written `ra_usb` + `ra_usb_cdc` stack and exposes a serial echo
endpoint to the host. Every byte the host writes is echoed back, with
a visible LED1 (P6_00) toggle per byte so traffic on the wire is
obvious without a logic analyser.

This app uses the **on-board USB-FS receptacle** on the EK-RA8D2,
**not** the USB-HS receptacle. Both sockets are physically wired but
only USB-FS is exercised here -- HS needs an external HS PHY config
that this firmware does not bring up.

## Status: hardware bring-up

The firmware uses the FSP-aligned USB-FS pinout from the EK-RA8D2 v1
User's Manual (cross-checked against
`ra-fsp-examples/example_projects/ek_ra8d2/usb_pcdc/`):

| Net           | Pin    | PFS PSEL                | Direction                        |
|---------------|--------|-------------------------|----------------------------------|
| USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) | VBUS sense (peripheral input).   |
| USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) | VBUS-enable drive (output).      |
| USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) | D+ data line (analog buffer).    |
| USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) | D- data line (analog buffer).    |

## VID / PID

For laptop-side hardware bring-up only:

- **VID = 0x1209** -- pid.codes free-for-experiments range.
- **PID = 0x000A** -- locally chosen, do not redistribute.
- **Manufacturer string** = "Brighton Sikarskie".
- **Product string**      = "EK-RA8D2 CDC Echo".

These are documented for reference; the underlying chapter-9 stack
in `libs/ra_hal/src/ra_usb.c` answers GET_DESCRIPTOR using whatever
descriptor table the device-mode driver carries. If the host
enumerates the device under a different VID/PID, that means the
descriptor table inside the HAL still needs wiring up -- the README
records the design intent.

## Test on macOS

After flashing, the EK-RA8D2's **USB-FS** receptacle should enumerate
on the host. Find the device node:

```sh
ls /dev/cu.usbmodem*
# /dev/cu.usbmodem<serial>1
```

Open it RDWR with picocom or screen (RDONLY tools like a bare `cat`
will show no bytes because macOS gates CDC forwarding on DTR, which
asserts only when the port is opened for write):

```sh
# picocom (recommended):
picocom -b 9600 /dev/cu.usbmodem*

# screen:
screen 9600 /dev/cu.usbmodem*

# minicom:
minicom -D /dev/cu.usbmodem* -b 9600
```

Type any character. Each byte should:

1. Echo back to the terminal (the firmware mirrors the byte stream).
2. Toggle LED1 (P6_00) on the EK-RA8D2 -- one toggle per byte.

To exit picocom: `Ctrl-A Ctrl-X`. To exit screen: `Ctrl-A k y`.

The line coding is accepted but ignored -- the firmware is a pure
byte mirror, so the baud rate you pass to picocom does not affect
anything (USB CDC throughput is not gated on the negotiated baud).

## Test on Linux

```sh
ls /dev/ttyACM*
# /dev/ttyACM0

# With picocom:
picocom -b 9600 /dev/ttyACM0
# Or screen:
screen /dev/ttyACM0 9600
```

Same behaviour: type a byte, see it echo back, watch LED1 toggle.

## Test on Windows

The device shows up as a virtual COM port (`COMn`). Open it with
PuTTY or TeraTerm at any baud rate; bytes echo through and LED1
toggles per byte.

## Build + flash

From the repo root:

```sh
make usb_cdc_echo                       # cross-compile
make -C examples/usb_cdc_echo flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/ek_ra8d2/hw_validated/hil/usb_cdc_echo/
make
make flash
make clean
```

## What the firmware does

1. `ra_cgc_init()` brings up XTAL + PLL1 (CPUCLK0 = 1 GHz, PCLKA =
   125 MHz). MRMS PFB flush, VSCR voltage scaling, MRMS wait-state
   programming all handled inside the CGC driver -- no per-app
   workarounds.
2. `ra_time_init(cpuclk0_hz)` sets up SysTick for `ra_delay_ms`.
3. `ra_gpio_output_init(k_ra_pin_led1, low)` for the per-byte
   traffic indicator.
4. `ra_pfs_route_peripheral()` for P4_07, P5_00, P8_14, P8_15 with
   PSEL = `k_ra_psel_usb_fs` (0x13).
5. `ra_nsc_usb_init(k_ra_usb_speed_fs)` -- NSC veneer, lands inside
   the secure-side `ra_usb_device_init`. Releases the USBFS MSTP
   gate, drives `SYSCFG.SCKE` high, clears `DRPD`, sets `USBE`,
   programs the C/D0/D1 FIFO access width to 16-bit, loads DCP
   max-packet = 64, unmasks BEMPE | BRDYE | NRDYE | DVSE | CTRE |
   VBSE.
6. `ra_usb_cdc_init(k_ra_usb_speed_fs)` -- configures PIPE1 (bulk
   IN, EP1 IN, 64 B), PIPE2 (bulk OUT, EP2 OUT, 64 B), PIPE6
   (interrupt IN, EP3 IN, 8 B). Seeds 9600/8/N/1 line coding,
   DTR/RTS = 0.
7. `ra_nsc_usb_attach(k_ra_usb_speed_fs, true)` -- raises the D+
   pull-up so the host begins enumeration (SET_ADDRESS,
   GET_DESCRIPTOR, SET_CONFIGURATION).
8. Loop: `ra_usb_cdc_recv` to drain bulk-OUT, `ra_usb_cdc_send` to
   re-queue on bulk-IN, `ra_gpio_toggle(k_ra_pin_led1)` per byte,
   1 ms idle when the pipe is empty.

## Debugging

```sh
make -C examples/usb_cdc_echo ozone     # SEGGER Ozone GUI
make -C examples/usb_cdc_echo debug     # gdb attached via JLinkGDBServer
```

Useful SWD probes (USBFS register window):

```
mem32 0x40250000 1     # SYSCFG    -- expected USBE=1, DRPD=0, DPRPU=1 (post-attach)
mem32 0x40250004 1     # BUSWAIT   -- bus access wait
mem32 0x40250008 1     # SYSSTS0   -- LNST line state, IDMON, VBSTS
mem32 0x4025001C 1     # DVSTCTR0  -- USBRST, RESUME, UACT, RHST
mem32 0x40250040 1     # CFIFOSEL
mem32 0x40250056 1     # INTSTS0   -- CTRT (SETUP-pending), DVST (state change)
mem32 0x4025005A 1     # FRMNUM    -- frame number (advances when bus active)
mem32 0x40250078 1     # USBADDR   -- host-assigned address (post-SET_ADDRESS)
mem32 0x40400B07 1     # P407 PFS  -- expected PSEL = 0x13
mem32 0x40400B40 1     # P500 PFS  -- expected PSEL = 0x13
mem32 0x40400D0E 1     # P814 PFS  -- expected PSEL = 0x13
mem32 0x40400D0F 1     # P815 PFS  -- expected PSEL = 0x13
```

## Findings worth keeping

- **Pin set is fixed by the EK-RA8D2 board** -- USB-FS lines come
  out of the chip at P407/P500/P814/P815 and there is no alternate
  routing that exits to the on-board receptacle. Re-derived from
  the FSP `usb_pcdc` example's `pincfg` block.
- **The CDC class layer here does not own the descriptor table.**
  Class-specific SETUPs (SET_LINE_CODING, GET_LINE_CODING,
  SET_CONTROL_LINE_STATE) are handled inside `ra_usb_cdc_handle_setup`,
  but standard chapter-9 (GET_DESCRIPTOR, SET_CONFIGURATION) lives
  inside `ra_usb_device_init`'s SETUP path. VID / PID / strings are
  documented above for traceability.
- **macOS gates RDONLY** -- `cat /dev/cu.usbmodem*` returns nothing;
  use picocom / screen / minicom which open RDWR and assert DTR.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31). USB-FS pin set (P407 / P500 / P814 / P815) is
the only routing the chip exposes for the on-board J11 Type-C USB-FS
receptacle (UM Table 22 p 30); main.c programs this pin set directly
via `ra_pfs_route_peripheral`.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 22 p 30 + Table 24 p 31, USB CDC PSTN 1.20, and HUM
(R01UH1065EJ0130) Ch "USBFS".
