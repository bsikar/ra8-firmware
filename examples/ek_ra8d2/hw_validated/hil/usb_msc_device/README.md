# usb_msc_device

USB Mass Storage Class smoke test for the EK-RA8D2 USB-FS port.

After flash, the EK-RA8D2 enumerates as a tiny removable drive backed
by 32 KB of SRAM, pre-formatted with FAT12 and containing a single
file `README.TXT` whose contents are `Hello from RA8D2!\r\n`. LED2
(P3_03) toggles per host SCSI command so the bench operator can see
when the host probes the drive.

## Build

```
make build
make flash
```

## Verify (Linux)

```
lsusb                                # "Brighton Sikarskie EK-RA8D2 RAM Disk"
udisksctl mount -b /dev/sdX1         # most distros auto-mount
ls   /run/media/$USER/RA8D2/
cat  /run/media/$USER/RA8D2/README.TXT     # prints "Hello from RA8D2!"
```

## Verify (macOS)

The volume mounts as `/Volumes/RA8D2`:

```
ls /Volumes/RA8D2
cat /Volumes/RA8D2/README.TXT
```

## FAT12 layout

64 sectors x 512 bytes = 32 KB:

| Sector | Purpose             |
| ------ | ------------------- |
| 0      | Boot sector / BPB   |
| 1..2   | FAT #1              |
| 3..4   | FAT #2              |
| 5..6   | Root directory      |
| 7      | Cluster 2 (README)  |
| 8..63  | Free space          |

See `main.c` for the byte-level FAT layout and Microsoft FAT 1.03
section references.

## VID / PID

VID = 0x1209 (pid.codes free-for-experiments range), PID locally
chosen. Bench use only.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED2 init/toggle (P303 per EK-RA8D2
v1 UM Table 24 p 31). USB-FS pin set (P407 / P500 / P814 / P815) is
the only routing the chip exposes for the on-board J11 Type-C USB-FS
receptacle (UM Table 22 p 30).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 22 p 30 + Table 24 p 31, USB Mass Storage Class
Bulk-Only Transport 1.0, and HUM (R01UH1065EJ0130) Ch "USBFS".

## HIL plan

**HIL-able after firmware fix -- currently halts in
`ra_exception_halt_loop` during init.** Demoted alongside
`usb_cdc_echo` and `usb_hid_device` on 2026-05-18 (commit 1f46ad3b).
The existing `hil.conf` is parked at `HIL_MODE=alive` /
`HIL_BOOT_S=2` and the per-app `hil.conf` already sketches the
post-fix gate ("Pi-as-USB-host hil_usb_msc_browse mode: host mounts
the chip as a block device, lists the partition, reads back a
file").

Proposed promotion gate: Pi enumerates the chip as a USB Mass
Storage device (`/dev/sdX`), mounts it read-only, lists the root
directory, and confirms an expected sentinel file is present and
readable. A new `hil_usb_msc_browse` helper wraps that flow.

Stays in `hw_pending/` until the init halt is root-caused.
