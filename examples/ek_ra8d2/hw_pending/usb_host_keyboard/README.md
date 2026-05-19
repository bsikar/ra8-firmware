# usb_host_keyboard

Native USB **host-mode** HID keyboard smoke test for the EK-RA8D2.
Plug a USB keyboard into the board's USB-HS Type-C jack (J7), type,
and the firmware decodes the boot-protocol HID input reports and
echoes pressed keys as ASCII over the J-Link OB CDC virtual COM port.

This is the hardware-test counterpart to the host-side HID class
layer in `libs/ra_hal/src/ra_usb_hhid.c`.

## What you need

- **EK-RA8D2** with the on-board J-Link OB powered up via the J10
  Type-C cable (J10 also powers the board).
- **A USB Type-C cable / adapter** for J7.
- **A USB keyboard** that supports the boot-protocol HID profile
  (any standard wired keyboard does; gaming keyboards with HID over
  vendor-specific protocols may not).
- **A serial terminal** (picocom / screen / minicom) at 115200 8N1.

## Test recipe

1. Build + flash:

   ```sh
   make usb_host_keyboard
   make -C examples/usb_host_keyboard flash
   ```

2. Open the J-Link OB CDC port:

   ```sh
   # macOS:
   picocom -b 115200 /dev/cu.usbmodem<serial>
   # Linux:
   picocom -b 115200 /dev/ttyACM0
   ```

   You should see:

   ```
   ra8d2 host: ready, plug a USB keyboard into J7
   ```

3. Plug the keyboard into J7. After enumeration finishes the firmware
   prints:

   ```
   ra8d2 host: device attached vid=0xXXXX pid=0xXXXX
   ```

   `LED1` (P6_00) lights solid on attach. The firmware then issues
   `SET_PROTOCOL(boot)` and `SET_IDLE(0, 0)` so the keyboard speaks
   the legacy 8-byte boot-protocol input report.

4. Type. Pressed keys are decoded to ASCII and echoed on the
   terminal:

   ```
   hello, world!
   ```

   `LED2` (P3_03) toggles per keystroke.

   Supported keys: letters `a..z` (Shift -> uppercase), digits `0..9`
   with shifted symbols `!@#$%^&*()`, Enter, Backspace,
   Tab, Space, and the common punctuation row
   `-`, `=`, `[`, `]`, backslash, `;`, apostrophe, backtick, `,`, `.`, `/`
   (with their shifted variants `_+{}|:~<>?`). Escape
   prints as the marker `^[`. Anything outside the table (function
   keys, arrow keys, F1..F12, ...) blinks LED2 but emits nothing on
   SCI8 -- that is the intentional limit of the boot-protocol decoder.

## Pinout

| Net               | Pin     | PFS PSEL                | Notes                       |
|-------------------|---------|-------------------------|-----------------------------|
| SCI8 TXD8 (log)   | PD_02   | k_ra_psel_sci_async (4) | Same as `uart_hello`.       |
| SCI8 RXD8 (log)   | PD_03   | k_ra_psel_sci_async (4) | Same as `uart_hello`.       |
| USBHS_VBUS sense  | P4_08   | 0x14 (USBHS)            | Only PFS-muxed HS pin.      |
| USBHSDP / USBHSDM | dedi.   | none                    | Hardwired HS PHY balls.     |
| LED1 (attach)     | P6_00   | k_ra_psel_gpio (0)      | Lights solid on attach.     |
| LED2 (keystroke)  | P3_03   | k_ra_psel_gpio (0)      | Toggles per fresh keypress. |

## Build + flash

```sh
make usb_host_keyboard                     # cross-compile
make -C examples/usb_host_keyboard flash   # flash via J-Link OB
```

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 / LED2 init/toggle (P600 / P303
per EK-RA8D2 v1 UM Table 24 p 31). USBHS_VBUS sense (P408) is the only
PFS-muxed USBHS pin (UM Table 28 p 34); DP / DM are dedicated PHY
balls. SCI8 console on PD02 / PD03 per UM Table 13 p 24.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Tables 13 p 24 / 24 p 31 / 28 p 34, USB HID 1.11 Boot
Keyboard profile, and HUM (R01UH1065EJ0130) Ch "USBHS".

## HIL plan

**Requires physical stim -- needs an external USB HID keyboard on J7
(USB-HS).** Same USB-host-mode situation as `usb_host_cdc_echo`: the
chip is waiting for a USB keyboard to be plugged in, and the HIL
bench cannot present one. Pi USB gadget (libcomposite g_hid) could
emulate a keyboard but no such service is configured.

Also blocked by the USB HS SET_ADDRESS stall documented in
`hw_pending/README.md`.

Stays in `hw_pending/` -- USB HS hardware/firmware blocked AND no Pi
USB-gadget keyboard scaffolding exists.
