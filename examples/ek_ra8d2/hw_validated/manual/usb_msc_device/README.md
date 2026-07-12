# usb_msc_device

USB Mass Storage Class smoke test for the EK-RA8D2 USB-FS port.

After flash, the EK-RA8D2 enumerates as a removable drive backed by a
512 KiB RAM disk (blank -- no filesystem; the host formats it). The
USBX device storage class serves the BOT/SCSI transport; the demo
supplies the RAM-backed read/write/status hooks.

## Build

```
make build
make flash
```

## Verify (Linux -- the HIL bench path)

```
lsusb                                # "EK-RA8D2 RAM Disk" (1209:000b)
# kernel log shows "[sdX] Attached SCSI removable disk"
sudo mkfs.vfat /dev/sdX
udisksctl mount -b /dev/sdX
# write + read back a file to exercise WRITE(10)/READ(10)
```

## Verify (macOS) -- validated 2026-06-12 on real hardware

macOS first shows "The disk you attached was not readable by this
computer" -- expected for a blank disk; the device is attached:

```
diskutil list external          # shows a 524.3 KB external physical disk
diskutil eraseDisk MS-DOS RA8D2 MBR diskN
echo hello > /Volumes/RA8D2/HELLO.TXT && sync
diskutil unmount diskNs1 && diskutil mount diskNs1
cat /Volumes/RA8D2/HELLO.TXT    # survives remount -- data lives on the board
```

Full chain proven against macOS: enumerate -> INQUIRY (SPC-2 / response
data format 2 via the first-party `port/usbx/` override) -> READ
CAPACITY -> partition + FAT12 format over WRITE(10) -> mount -> file
roundtrip across unmount/remount.

Getting macOS to accept the device took two first-party fixes (the
Linux path tolerated both defects):

- the vendored USBX INQUIRY handler reports RESPONSE DATA FORMAT = 0
  (SCSI-1) and ignores the EVPD bit; macOS answers that with a
  Bulk-Only Mass Storage Reset and abandons the device. Replaced by
  `port/usbx/ux_device_class_storage_inquiry.c` (SPC-2 + VPD pages
  0x00/0x80).
- the DCD bridge gained strand recovery for stashed bulk-IN transfers
  (PID=NAK with a loaded bank) and a transactional BEMPSTS ack for
  multi-packet IN streaming, plus a JLink-readable BOT event trace
  ring (`s_trace` / `s_trace_seq`) that made the failure visible.

## VID / PID

VID = 0x1209 (pid.codes free-for-experiments range), PID locally
chosen. Bench use only.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED2 init/toggle (P303 per EK-RA8D2
v1 UM Table 24 p 31). USB-FS pin set (P407 / P500 / P814 / P815) is
routed PSEL 0x13 as in `usb_cdc_echo`.
