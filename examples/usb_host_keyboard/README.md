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
   with shifted symbols `!@#$%^&*()`, Enter (`\n`), Backspace (`\b`),
   Tab (`\t`), Space, and the common punctuation row
   `-=[]\;'`,./` (with their shifted variants `_+{}|:~<>?`). Escape
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
