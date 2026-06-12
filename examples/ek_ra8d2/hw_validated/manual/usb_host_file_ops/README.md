# usb_host_file_ops

Native USB **host-mode** file-operations exerciser for the EK-RA8D2.
Plug a USB thumb drive into **either USB jack** -- the firmware
alternates between the **USB-HS** port (J7, 480 Mbps) and the **USB-FS**
port (12 Mbps) every retry cycle until a drive answers, printing
`probing USB-HS` / `probing USB-FS` as it goes. Once attached it mounts
the drive's FAT/exFAT volume through `ra_fs` (block I/O bridged onto the
`ra_usb_hmsc` SCSI READ(10)/WRITE(10) calls), then proves every core
file operation against the real medium with a printed verdict per step:

1. cleanup -- best-effort unlink of leftover test files
2. `listdir /` -- every root entry printed (name, size, `<dir>`)
3. write a new file `USBTEST.TXT` (known 67-byte payload)
4. open + read back + byte-compare the payload
5. `listdir` must show `USBTEST.TXT`
6. rename `USBTEST.TXT` -> `USBDONE.TXT`
7. old name must be gone, new name must read back intact
8. `listdir` must show `USBDONE.TXT` and not `USBTEST.TXT`
9. unlink `USBDONE.TXT`; final `listdir` must not show it

A clean run ends with `ALL FILE OPS PASSED` and a solid LED1. The suite
is self-cleaning: the drive's directory is bit-identical before and
after (the test names are removed again).

## Validation status (2026-06-12): PASSING ON BOTH PORTS, REAL HARDWARE

Validated against a 128 GB consumer stick (vid=0x24A9 pid=0x205A) that
is **GPT-partitioned** (macOS GUID scheme: protective MBR + EFI System
Partition + exFAT Basic Data partition), on **both** the USB-HS jack
(480 Mbps, `port=USB-HS`) and the USB-FS jack (12 Mbps, three
consecutive clean runs; the failover from the empty HS port is exactly
the printed flow below):

```
ra8d2 fileops: ready, plug a USB drive into either USB jack
ra8d2 fileops: probing USB-HS
ra8d2 fileops: FAIL ladder err=0x00000203
ra8d2 fileops: probing USB-FS
ra8d2 fileops: device attached vid=0x24A9 pid=0x205A port=USB-FS
ra8d2 fileops: mounted fs=exfat base_lba=411648
ra8d2 fileops: [1/9] cleanup leftovers
ra8d2 fileops: [2/9] baseline root listing
ra8d2 fileops: -- listdir / --
ra8d2 fileops:   - .Spotlight-V100 <dir>
ra8d2 fileops:   - TS(2) = 15 s.pdf size=85544734
...
ra8d2 fileops: entries=11
ra8d2 fileops: [3/9] write USBTEST.TXT len=67
ra8d2 fileops: [4/9] read back + verify payload
ra8d2 fileops: [5/9] listdir must show USBTEST.TXT
...
ra8d2 fileops: [6/9] rename USBTEST.TXT -> USBDONE.TXT
ra8d2 fileops: [7/9] old name gone, new name intact
ra8d2 fileops: [8/9] listdir must show USBDONE.TXT only
ra8d2 fileops: [9/9] unlink USBDONE.TXT
ra8d2 fileops: entries=11
ra8d2 fileops: ALL FILE OPS PASSED
```

This run exercises, on real hardware, the `ra_fs` GPT partition
discovery (protective MBR type 0xEE -> "EFI PART" header -> Basic Data
entry preferred over the EFI System Partition), the exFAT write path
(directory entry-set creation + allocation bitmap), exFAT `listdir`,
the in-place exFAT `rename` (Stream NameLength/NameHash patch + Name
entry rebuild + SetChecksum), and exFAT `unlink` (in-use bit clear +
cluster free). If the mount fails, the firmware dumps the partition
table and the first sectors (LBA 0/1/2) so an unsupported layout is
identifiable straight from the log.

## What you need

- **EK-RA8D2** with the on-board J-Link OB powered via the J10 Type-C
  cable (J10 also powers the board).
- **A USB mass-storage thumb drive** in either the USB-HS jack (J7) or
  the USB-FS jack. FAT12/16/32 and exFAT volumes are supported, on
  superfloppy, MBR, or GPT layouts.
- **A serial terminal** on the J-Link OB CDC port at 115200 8N1.

The suite writes two small files (`USBTEST.TXT`, `USBDONE.TXT`) to the
drive's root and removes them again. Existing data is never touched,
but do not use a drive whose contents you cannot afford to risk.

## Test recipe (manual HIL)

1. Build + flash:

   ```sh
   make usb_host_file_ops
   make -C examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops flash
   ```

2. Open the J-Link OB CDC port at 115200 8N1:

   ```sh
   picocom -b 115200 /dev/cu.usbmodem<serial>   # macOS
   picocom -b 115200 /dev/ttyACM0               # Linux
   ```

3. Insert a thumb drive into either USB jack. The app probes USB-HS
   and USB-FS alternately (one port per 5 s retry cycle), so the drive
   is picked up automatically wherever it sits; watch the nine step
   verdicts and the final `ALL FILE OPS PASSED`.

`LED2` (P3_03) toggles per completed step; `LED1` (P6_00) lights solid
on a full pass. Any failing step prints `FAIL <step> err=0x...` and the
suite retries from enumeration.

## Pinout

| Net               | Pin     | PFS PSEL                | Notes                     |
|-------------------|---------|-------------------------|---------------------------|
| SCI8 TXD8 (log)   | PD_02   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| SCI8 RXD8 (log)   | PD_03   | k_ra_psel_sci_async (4) | Same as `uart_hello`.     |
| USBHS_VBUS sense  | P4_08   | 0x14 (USBHS)            | Only PFS-muxed HS pin.    |
| USBHSDP / USBHSDM | dedi.   | none                    | Hardwired HS PHY balls.   |
| J7 host power     | PD_07   | k_ra_psel_gpio (0)      | HIGH = U18 supplies J7.   |
| USBFS_VBUS sense  | P4_07   | 0x13 (USBFS)            | FS attach detect.         |
| USB_VBUSEN (FS)   | P5_00   | 0x13 (USBFS)            | Controller drives VBUS.   |
| USBFS D+          | P8_14   | 0x13 (USBFS)            | PFS-muxed (not a ball).   |
| USBFS D-          | P8_15   | 0x13 (USBFS)            | PFS-muxed (not a ball).   |
| LED1 (pass)       | P6_00   | k_ra_psel_gpio (0)      | Solid on full pass.       |
| LED2 (step)       | P3_03   | k_ra_psel_gpio (0)      | Toggles per step.         |

## BSP usage

Uses `ra_board_ek_ra8d2` for LED1/LED2 (P600/P303, EK-RA8D2 v1 UM Table
24 p 31) and for switching SW4-8 to the Host position through the U15
I/O expander (`ra_board_io_expander_set_usbhs_host_mode`). J7 host
power is PD07 HIGH (UM Sec 6.2 p 34, U18, 2 A budget). USBHS_VBUS
sense P408 is the only PFS-muxed USBHS pin (UM Table 28 p 34).

## Why manual

Requires a physical USB thumb drive in one of the host jacks -- the
HIL bench has no USB gadget that can emulate a mass-storage device,
and the suite intentionally mutates a real removable medium. HIL CI
builds the app but a human must insert a drive and read the final
verdict.

This is the filesystem-layer counterpart to
`hw_pending/usb_host_msc_browse` (raw enumerate/INQUIRY/READ ladder);
the USB host engine learnings live in `libs/ra_hal/src/ra_usb.c` and
the class layer in `libs/ra_hal/src/ra_usb_hmsc.c`.
