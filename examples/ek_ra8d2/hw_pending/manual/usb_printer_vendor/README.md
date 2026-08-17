# usb_printer_vendor

Native, bare-metal USB **composite** device that enumerates two interfaces in one
configuration, so a single plug-in exercises both native device-class layers
(#265):

| IF | Class            | EPs                          | Layer            |
|----|------------------|------------------------------|------------------|
| 0  | Printer 7/1/2    | bulk OUT 0x01, bulk IN 0x82  | `ra8_usb_pprn`   |
| 1  | Vendor 0xFF      | bulk IN 0x81, bulk OUT 0x02  | `ra8_usb_pvnd`   |

The printer interface accepts a job on bulk OUT and echoes it to the console, and
answers `GET_PORT_STATUS` / `GET_DEVICE_ID` / `SOFT_RESET` (USB Printer Class 1.1
sec 4.2) from the class layer's port-status shadow. The vendor interface is a
bulk OUT to bulk IN loopback for a host libusb or WinUSB script.

Unlike the ThreadX + USBX device demos, this app has **no RTOS and no USBX**: the
native `ra8_usb` driver carries no chapter-9 responder, so `main.c` runs a small
polled chapter-9 loop answering the standard `GET_DESCRIPTOR` / `SET_ADDRESS` /
`SET_CONFIGURATION` requests and handing class and vendor SETUPs to the two class
layers. The pure SETUP router and descriptor tables live in
`src/usb_printer_vendor_ch9.c`, host-tested with MC/DC.

## Pinout (USB-FS)

| Net           | Pin    | Source                          |
|---------------|--------|---------------------------------|
| USB_FS_VBUS   | P4_07  | `k_ra8_board_usbfs_pin_vbus`    |
| USB_FS_VBUSEN | P5_00  | `k_ra8_board_usbfs_pin_vbusen`  |
| USB_FS_DP     | P8_14  | `k_ra8_board_usbfs_pin_dp`      |
| USB_FS_DM     | P8_15  | `k_ra8_board_usbfs_pin_dm`      |

## Blocked on

A real USB host on J11. Enumeration is automatable off-target -- the emulated
chapter-9 host walks the descriptors, the polled EP0 SETUP handling, and the DVSQ
powered -> default -> address -> configured progression to completion -- but both
**data** paths need a PC:

- a print job sent to the printer interface, after which the bytes appear on the
  console and `g_usb_print_jobs` advances;
- a libusb or WinUSB script writing bulk OUT and reading the same bytes back from
  bulk IN, after which `g_usb_vendor_loops` advances per round trip.

So the disposition is enumeration proven, print job and loopback pending.
