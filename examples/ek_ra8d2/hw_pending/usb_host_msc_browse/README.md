# usb_host_msc_browse

Native USB **host-mode** MSC (Mass Storage Class) browser for the
EK-RA8D2. Plug a USB thumb drive into the board's **USB-FS** Type-C jack,
and the firmware enumerates it, runs a SCSI INQUIRY -> READ_CAPACITY(10)
-> READ(10) sequence on LBA 0, and dumps the first 64 bytes of the
device's MBR over the J-Link OB CDC virtual COM port.

Targets **USB-FS** (12 Mbps): simpler PHY than HS and it sidesteps the
HS SET_ADDRESS-stall blocker. Pins: VBUS P4_07, VBUSEN P5_00 (supplies
bus power in host mode), D+/D- P8_14/P8_15, all PSEL 0x13.

## Bring-up status (2026-06-12): WORKING END TO END

The full ladder completes on real hardware, repeatably across resets:

```
ra8d2 host: enum attempt won=4
ra8d2 host: device attached vid=0x24A9 pid=0x205A max-lun=18
ra8d2 host: address 1 assigned
ra8d2 host: msc iface eps in=0x01 out=0x02
ra8d2 host: configured, pipes ready
ra8d2 host: INQUIRY vendor="        " product="                " rev="    "
ra8d2 host: capacity blocks=245760000 block_size=512
ra8d2 host: MBR sector 0 first 64 bytes:
00 00 ... (64 bytes)
ra8d2 host: mbr sig @510 = 55 AA (ok)
```

(The `max-lun` field of the attach line carries the descriptor byte
count during bring-up; this stick reports blank INQUIRY strings, which
is legal -- the CSW signature/tag/status are validated per exchange.)

The ladder retries every 5 s, so inserting or reseating a drive is
picked up automatically. Host-mode learnings (SACK/SIGN-gated SETUP,
NRDY retry, BCLR-before-status, DATA1 SQSET, CFIFOSEL settle) live in
`libs/ra_hal/src/ra_usb.c` with HUM citations.

This is the hardware-test counterpart to the host-side MSC class
layer in `libs/ra_hal/src/ra_usb_hmsc.c`.

## What you need

- **EK-RA8D2** with the on-board J-Link OB powered up via the J10
  Type-C cable (J10 also powers the board).
- **A USB Type-C cable** for J7.
- **A USB mass-storage device** (thumb drive, USB SSD enclosure, ...).
  It must enumerate as `class=0x08 / subclass=0x06 / protocol=0x50`
  (Bulk-Only Transport SCSI) -- the overwhelming majority of consumer
  thumb drives qualify.
- **A serial terminal** (picocom / screen / minicom) to read the
  J-Link OB CDC log channel at 115200 8N1.

## Test recipe

1. Build + flash:

   ```sh
   make usb_host_msc_browse
   make -C examples/usb_host_msc_browse flash
   ```

2. Open the J-Link OB CDC port at 115200 8N1:

   ```sh
   # macOS:
   picocom -b 115200 /dev/cu.usbmodem<serial>
   # Linux:
   picocom -b 115200 /dev/ttyACM0
   ```

   You should see:

   ```
   ra8d2 host: ready, plug a USB drive into J7
   ```

3. Plug a USB thumb drive into J7. After enumeration finishes
   (typically 100 - 500 ms) the firmware prints:

   ```
   ra8d2 host: device attached vid=0xXXXX pid=0xXXXX max-lun=0
   ra8d2 host: INQUIRY vendor="..." product="..." rev="..."
   ra8d2 host: capacity blocks=N block_size=512
   ra8d2 host: MBR sector 0 first 64 bytes:
   EB 58 90 4D 53 44 4F 53 35 2E 30 00 02 08 20 00
   02 00 00 00 00 F8 00 00 3F 00 FF 00 00 00 00 00
   ...
   ```

   `LED1` (P6_00) lights solid on attach. `LED2` (P3_03) blinks once
   per SCSI op (3 blinks total: INQUIRY, READ_CAPACITY, READ(10)).

   The sequence runs once per attach. To rerun: replug the drive and
   reset the EVM.

## Pinout

| Net               | Pin     | PFS PSEL                | Notes                     |
|-------------------|---------|-------------------------|---------------------------|
| SCI8 TXD8 (log)   | PD_02   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| SCI8 RXD8 (log)   | PD_03   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| USBHS_VBUS sense  | P4_08   | 0x14 (USBHS)            | Only PFS-muxed HS pin.    |
| USBHSDP / USBHSDM | dedi.   | none                    | Hardwired HS PHY balls.   |
| LED1 (attach)     | P6_00   | k_ra_psel_gpio (0)      | Lights solid on attach.   |
| LED2 (SCSI op)    | P3_03   | k_ra_psel_gpio (0)      | Blinks per SCSI op.       |

## Build + flash

```sh
make usb_host_msc_browse                     # cross-compile
make -C examples/usb_host_msc_browse flash   # flash via J-Link OB
```

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 / LED2 init/toggle (P600 / P303
per EK-RA8D2 v1 UM Table 24 p 31). USBHS_VBUS sense (P408) is the only
PFS-muxed USBHS pin (UM Table 28 p 34); DP / DM are dedicated PHY
balls. SCI8 console on PD02 / PD03 per UM Table 13 p 24.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Tables 13 p 24 / 24 p 31 / 28 p 34, USB Mass Storage Class
Bulk-Only Transport 1.0, and HUM (R01UH1065EJ0130) Ch "USBHS".

## HIL plan

**Requires physical stim -- needs an external USB thumb drive on J7
(USB-HS).** Same USB-host-mode situation as `usb_host_cdc_echo` and
`usb_host_keyboard`: the chip is waiting for a USB Mass Storage
device. Pi USB gadget (libcomposite g_mass_storage backed by a
file) could emulate one, but no such service is configured.

Also blocked by the USB HS SET_ADDRESS stall documented in
`hw_pending/README.md`.

Stays in `hw_pending/` -- USB HS hardware/firmware blocked AND no Pi
USB-gadget mass-storage scaffolding exists.
