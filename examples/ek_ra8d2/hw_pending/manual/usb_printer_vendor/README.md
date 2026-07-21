# usb_printer_vendor

Native, bare-metal USB **composite** device for the EK-RA8D2 that enumerates
two interfaces in one configuration so a single plug-in exercises both native
device-class layers (issue #265):

| IF | Class            | EPs                          | Layer            |
|----|------------------|------------------------------|------------------|
| 0  | Printer 7/1/2    | bulk OUT 0x01, bulk IN 0x82  | `ra8_usb_pprn`   |
| 1  | Vendor 0xFF      | bulk IN 0x81, bulk OUT 0x02  | `ra8_usb_pvnd`   |

- **Printer (IF0):** accepts a print job on bulk OUT and echoes it to the
  UART console; answers `GET_PORT_STATUS` / `GET_DEVICE_ID` / `SOFT_RESET`
  (USB Printer Class 1.1 sec 4.2) from the class layer's port-status shadow.
- **Vendor (IF1):** bulk OUT -> bulk IN loopback for a host libusb / WinUSB
  script.

Unlike the ThreadX + USBX device demos, this app has **no RTOS and no USBX**:
the native `ra8_usb` driver carries no chapter-9 responder, so `main.c` runs a
small polled chapter-9 loop that answers the standard `GET_DESCRIPTOR` /
`SET_ADDRESS` / `SET_CONFIGURATION` requests and hands the class / vendor
SETUPs to the two class layers. The pure SETUP router + descriptor tables live
in `src/usb_printer_vendor_ch9.c` (host-tested with MC/DC in
`tests/test_app_usb_printer_vendor.c`).

## Pinout (USB-FS)

| Net           | Pin    | Source                          |
|---------------|--------|---------------------------------|
| USB_FS_VBUS   | P4_07  | `k_ra8_board_usbfs_pin_vbus`    |
| USB_FS_VBUSEN | P5_00  | `k_ra8_board_usbfs_pin_vbusen`  |
| USB_FS_DP     | P8_14  | `k_ra8_board_usbfs_pin_dp`      |
| USB_FS_DM     | P8_15  | `k_ra8_board_usbfs_pin_dm`      |

## Build + run

```
make                       # -> build/usb_printer_vendor.elf
make flash                 # J-Link load
```

Console (J-Link OB VCOM, 115200 8N1) prints:

```
USB PRINTER+VENDOR READY        # device-side bring-up succeeded
USB PRINTER+VENDOR CONFIGURED   # a host enumerated + configured the device
```

## Verification

### board_sim (SIL, headless, no hardware)

The emulated chapter-9 host walks the enumeration script against the polled
responder and the run reaches `device CONFIGURED`:

```
scripts/sim/smoke.sh usb_printer_vendor
```

This is the automated gate: it proves the descriptors, the polled EP0 SETUP
handling, and the DVSQ powered -> default -> address -> configured progression
end to end, with no attached PC.

### Hardware bench (manual, needs a host PC on J11)

The device-side halves are complete, but the two data paths need a real USB
host to drive them:

- **TODO(host-side print job):** send a job to the printer interface, e.g. on
  Linux `echo "hello" > /dev/usb/lp0` (or a CUPS raw queue). The bytes appear
  on the UART console and `g_usb_print_jobs` advances.
- **TODO(host-side vendor loopback):** a libusb / WinUSB script writes bulk OUT
  EP 0x02 and reads the same bytes back from bulk IN EP 0x81;
  `g_usb_vendor_loops` advances per round trip.

Because those steps cannot run without an attached PC, this app lives in
`hw_pending/` and its disposition is **sim-validated enumeration, hardware
print-job / loopback pending** (see the issue).
