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
