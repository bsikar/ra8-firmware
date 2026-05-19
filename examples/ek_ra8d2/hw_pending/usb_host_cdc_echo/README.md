# usb_host_cdc_echo

Native USB **host-mode** CDC ACM smoke test for the EK-RA8D2. The
inverse of `examples/usb_cdc_echo`: where that app makes the
EK-RA8D2 show up to a laptop as a `/dev/cu.usbmodem*`, this app
makes the EK-RA8D2 act as a USB host that enumerates an attached
CDC-ACM peripheral and runs a byte-mirror echo loop over its bulk
pipes.

This app uses the **on-board USB-HS receptacle (J7 Type-C)** on the
EK-RA8D2 v1, **not** the USB-FS port (J11). The HS PHY block on the
RA8D2 is used so the firmware exercises the High-Speed enumeration
+ bulk-transfer paths in `libs/ra_hal/src/ra_usb.c` and
`libs/ra_hal/src/ra_usb_hcdc.c`.

## What you need

- **EK-RA8D2** with the on-board J-Link OB powered up via the J10
  debug Type-C cable (this also powers the board).
- **A CDC-ACM USB peripheral** to plug into J7. Any of the
  following will work:
  - Another EK-RA8D2 running `examples/usb_cdc_echo` (the natural
    pair: that app surfaces a CDC-ACM device on its USB-FS port).
  - A USB-to-serial adapter that enumerates as CDC-ACM
    (anything based on the standard CDC ACM class -- not FTDI or
    CP210x -- those use vendor-specific protocols).
  - A Linux phone in CDC-ACM tethering / serial-debug mode.
  - Any microcontroller dev board with a CDC-ACM stack flashed on it.
- **A USB Type-C cable** for J7 (or a USB-A-to-Type-C cable plus a
  CDC-ACM peripheral that has its own Type-C jack -- if the
  peripheral has a Type-A or micro-B port, use the appropriate
  Type-C-to-X cable).
- **A serial terminal** (picocom / screen / minicom) to read the
  J-Link OB CDC log channel at 115200 8N1.

## EK-RA8D2 v1 jumper / switch checklist (USB-HS host)

For the EK-RA8D2 v1 board, USB-HS host mode does not need any
jumper changes from the factory default -- the FSP `usb_hcdc`
example readme states for EK-RA8D2 simply: "Connect Type-C USB High
Speed port (J7) of the board" with no further jumper notes (unlike
EK-RA8D1 / EK-RA6M5 where SW1-6 / J7 / J17 had to be reconfigured).

Sanity checks before flashing:

| Item   | Expected state                                           |
|--------|----------------------------------------------------------|
| J10    | Type-C cable to laptop -- J-Link OB power + CDC bridge.  |
| J7     | Type-C cable to the CDC-ACM peripheral under test.       |
| J29    | EVM USB-HS VBUS source jumper -- factory default.        |
| SW1    | Boot mode switch -- factory default (SCI boot disabled). |

If your peripheral does not enumerate, double-check:

1. The peripheral really speaks USB CDC ACM (not FTDI, CP210x,
   PL2303, vendor-specific HID, etc.). Plug it into your laptop
   first and confirm `lsusb` shows class 0x02 (Communications) +
   subclass 0x02 (ACM).
2. The peripheral can run on bus power (the EK-RA8D2 USB-HS port
   provides the standard 5 V / 500 mA host budget; high-draw
   peripherals will need their own power).
3. J7 is using the TYPE-C connector and not its sibling J11
   (USB-FS). They are on opposite sides of the board.

## Test recipe

1. Flash the firmware:

   ```sh
   make usb_host_cdc_echo
   make -C examples/usb_host_cdc_echo flash
   ```

2. Open the J-Link OB CDC port at 115200 8N1 (this is the same
   serial bridge `uart_hello` prints to). Find the device node
   first:

   ```sh
   # macOS:
   ls /dev/cu.usbmodem*
   # Linux:
   ls /dev/ttyACM*
   ```

   Then attach with picocom or screen:

   ```sh
   # macOS:
   picocom -b 115200 /dev/cu.usbmodem<serial>
   screen 115200 /dev/cu.usbmodem<serial>

   # Linux:
   picocom -b 115200 /dev/ttyACM0
   minicom -D /dev/ttyACM0 -b 115200
   ```

   You should see:

   ```
   ra8d2 host: ready, waiting for CDC-ACM device on USB-HS (J7)
   ```

3. Plug the CDC-ACM peripheral into J7. After enumeration completes
   (typically 100-300 ms) the host firmware prints:

   ```
   ra8d2 host: device attached vid=0xXXXX pid=0xXXXX
   ```

   `LED1` (P6_00) lights solid to signal "device attached and
   pipes open".

4. Have the peripheral send bytes towards the EK-RA8D2. Every byte
   the peripheral sends comes straight back at it (the EK-RA8D2
   drains its bulk-IN pipe and re-queues the bytes on bulk-OUT).
   `LED2` (P3_03) toggles per byte echoed. Every 64 bytes echoed
   the J-Link OB log line shows:

   ```
   ra8d2 host: bytes echoed=64
   ra8d2 host: bytes echoed=128
   ra8d2 host: bytes echoed=192
   ...
   ```

   Hook the two boards back-to-back to make a pair: flash this app
   on board A and `usb_cdc_echo` on board B; from a third
   terminal, write to board B's `/dev/cu.usbmodem*` and the bytes
   bounce: laptop -> board B -> board A -> board B -> laptop. Both
   pairs of LEDs (LED1 on each board for "attached", LED2 on board
   A for echo traffic, LED1 on board B per byte) should pulse in
   lock-step.

## Pinout

| Net               | Pin     | PFS PSEL                | Notes                     |
|-------------------|---------|-------------------------|---------------------------|
| SCI8 TXD8 (log)   | PD_02   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| SCI8 RXD8 (log)   | PD_03   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| USBHS_VBUS sense  | P4_08   | 0x14 (USBHS)            | Only PFS-muxed HS pin.    |
| USBHSDP / USBHSDM | dedi.   | none                    | Hardwired HS PHY balls.   |
| LED1 (attach)     | P6_00   | k_ra_psel_gpio (0)      | Lights solid on attach.   |
| LED2 (echo)       | P3_03   | k_ra_psel_gpio (0)      | Toggles per byte echoed.  |

## What the firmware does

1. `ra_cgc_init()` -- XTAL + PLL1 up, CPUCLK0 = 1 GHz, PCLKA = 125
   MHz, SCICLK = PLL1R / 4 = 100 MHz. Same tree `uart_hello` uses.
2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
3. `ra_pfs_route_peripheral()` for PD_02 / PD_03 -> SCI async, then
   `ra_sci_init(8, &cfg)` at 115200 8N1.
4. `ra_pfs_route_peripheral()` for P4_08 -> USBHS_VBUS (PSEL = 0x14).
5. `ra_gpio_output_init()` for LED1 (attach) and LED2 (echo).
6. `ra_usb_hcdc_init(k_ra_usb_speed_hs)` -- flips the USBHS
   controller into host mode through `ra_usb_host_init`. Sets
   `SYSCFG.HSE = 1` (high-speed PHY enable), `SYSCFG.DCFM = 1`
   (host mode), `SYSCFG.DRPD = 1` (D+/D- pull-downs on for host),
   `SYSCFG.USBE = 1` (controller enabled). Programs the DCP
   max-packet to 64. Arms the chapter-9 enumeration step machine
   but leaves `DVSTCTR0.UACT = 0` until a J-state attach is seen
   on `SYSSTS0.LNST`.
7. `ra_usb_hcdc_attach_callback(on_attach, NULL)` registers the
   thunk that fires once the descriptor walk identifies a CDC-ACM
   control + data interface pair on the attached device.
8. `ra_isr_globals_enable()` -- drop PRIMASK so the controller's
   USBI / USBR vectors can dispatch.
9. Print "ra8d2 host: ready..." over SCI8.
10. Main loop:
    - On the first attach, print "device attached vid=... pid=..."
    - `ra_usb_hcdc_recv(buf, 512, &got)` to drain bulk-IN.
    - `ra_usb_hcdc_send(buf, got)` to re-queue on bulk-OUT.
    - `ra_gpio_toggle(LED2)` per byte echoed.
    - Print "bytes echoed=N" every 64 bytes.
    - 1 ms idle when the pipe is empty.

## Build + flash

From the repo root:

```sh
make usb_host_cdc_echo                     # cross-compile
make -C examples/usb_host_cdc_echo flash   # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/ek_ra8d2/hw_pending/usb_host_cdc_echo/
make
make flash
make clean
```

## Debugging

```sh
make -C examples/usb_host_cdc_echo ozone   # SEGGER Ozone GUI
make -C examples/usb_host_cdc_echo debug   # gdb attached via JLinkGDBServer
```

Useful SWD probes (USBHS register window @ 0x40351000):

```
mem32 0x40351000 1     # SYSCFG    -- expect HSE=1, DCFM=1, DRPD=1, USBE=1
mem32 0x40351008 1     # SYSSTS0   -- LNST line state, IDMON, VBSTS
mem32 0x4035101C 1     # DVSTCTR0  -- USBRST, RESUME, UACT, RHST, SPEED
mem32 0x40351040 1     # CFIFOSEL
mem32 0x40351056 1     # INTSTS0   -- ATTCH (attach), DVST (state change)
mem32 0x4035105A 1     # FRMNUM    -- frame number (advances when UACT=1)
mem32 0x40351078 1     # USBADDR   -- assigned device address
mem32 0x40400B08 1     # P408 PFS  -- expected PSEL = 0x14
```

## Findings worth keeping

- **The HS PHY data lines do not go through PFS.** P4_08 is the
  only PFS-muxed pin the EK-RA8D2 v1 USBHS port uses. Cross-checked
  against `ra-fsp-examples/example_projects/ek_ra8d2/usb_hcdc/`
  which sets `p408.usbhs.usbhs_vbus` and nothing else.
- **PSEL value 0x14 is shared between USBHS and QSPI.** The shared
  `ra_psel_t` enum in `ra_gpio_constants.h` does not yet expose a
  `k_ra_psel_usb_hs` symbol because of this overlap. The
  `ra_mpc_psel_t` enum in `ra_mpc.h` carries the constant under
  `k_ra_mpc_psel_usbhs`. This app pins it locally as
  `k_usb_host_psel_usb_hs = 0x14` for clarity.
- **EK-RA8D2 v1 needs no jumper changes for USBHS host mode.** The
  factory default routes VBUS through the EVM-side power switch,
  so the host can supply bus power without flipping J29 (unlike
  EK-RA8D1 / EK-RA6M5 which need explicit USBHS jumper / switch
  flips).
- **Host bring-up is asynchronous.** `ra_usb_hcdc_init` only arms
  the step machine; the actual chapter-9 walk runs from the
  controller's interrupt path. The main loop must spin on
  `s_state.attached` (set from `usb_host_on_attach`) before
  driving bulk traffic.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 / LED2 init/toggle (P600 / P303
per EK-RA8D2 v1 UM Table 24 p 31). USBHS_VBUS sense lives on P408 and
is the only PFS-muxed USBHS pin -- DP / DM are dedicated PHY balls per
UM Table 28 "USB High Speed Port Pin Assignments" p 34. SCI8 console
on PD02 / PD03 per UM Table 13 p 24.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Tables 13 p 24 / 24 p 31 / 28 p 34, USB CDC PSTN 1.20, and
HUM (R01UH1065EJ0130) Ch "USBHS".

## HIL plan

**Requires physical stim -- needs an external USB CDC peripheral on
J7 (USB-HS).** The chip is in USB *host* mode and waits for a USB
device to be plugged into the J7 Type-C jack. The HIL bench has the
J7 cable wired back to the Pi, but the Pi is currently configured as
USB *host* on that cable, not as a USB CDC peripheral. A Pi
USB-gadget configuration (libcomposite g_serial) could expose a CDC
peripheral, but that scaffolding does not exist in the harness
today.

Also blocked by the USB HS bring-up issue documented in
`hw_pending/README.md` ("chip reaches USB Address state but stalls
after SET_ADDRESS").

To make this HIL-able: (a) fix the USB HS SET_ADDRESS stall, and
(b) add a Pi USB-gadget service (configfs-based libcomposite) that
exposes /dev/ttyGS0 to the chip when the J7 cable is plugged. Then a
`uart_scrape` mode on the chip's SCI8 console could assert the
expected enumeration banner appears.

Stays in `hw_pending/` -- USB HS hardware/firmware blocked AND no Pi
USB-gadget scaffolding exists.
