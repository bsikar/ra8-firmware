# usb_msc_mram

Plug the EK-RA8D2's USB-FS port into any computer and the chip's
**onboard 1 MiB MRAM shows up as a file**.

The board enumerates as a USB Mass Storage device whose single LUN is
a **read-only synthesized FAT16 volume**: the boot sector, FAT, and
root directory are generated on the fly in the media-read callback,
and the data clusters map 1:1 onto the MRAM code window at
`0x02000000`. The root directory carries one file, `MRAM.BIN`
(1,048,576 bytes, read-only attribute) -- open or copy it and you are
reading the chip's flash over USB, live. The LUN reports
write-protected via MODE SENSE and rejects WRITE(10) with DATA PROTECT
sense, so hosts mount it read-only and the MRAM is never touched.

Built on the same ThreadX + USBX storage scaffold as `usb_msc_device`
(which proved the BOT/SCSI transport against Linux and macOS).

## Build

```
make build
make flash
```

## Verify (macOS) -- validated 2026-06-12 on real hardware

The volume auto-mounts read-only as `/Volumes/RA8D2 MRAM`:

```
ls -l "/Volumes/RA8D2 MRAM/"            # MRAM.BIN, 1048576 bytes
# Reference dump of the same window over SWD:
#   JLinkExe> savebin /tmp/mram_ref.bin 0x02000000 0x100000
cmp "/Volumes/RA8D2 MRAM/MRAM.BIN" /tmp/mram_ref.bin && echo IDENTICAL
```

`IDENTICAL` means every byte the host read over USB equals the byte
the debugger reads over SWD -- the onboard flash, end to end.

## Verify (Linux)

```
lsusb                                    # "EK-RA8D2 MRAM" (1209:000c)
udisksctl mount -b /dev/sdX1 2>/dev/null || sudo mount -o ro /dev/sdX /mnt
md5sum /mnt/MRAM.BIN                     # compare vs a JLink savebin dump
```

## Synthesized FAT16 layout

| LBA       | Content                                              |
| --------- | ---------------------------------------------------- |
| 0         | Boot sector / BPB (FAT16, 512 B/sector, 1 sec/clus)  |
| 1..17     | FAT (sequential chain: cluster c -> c+1, 2049 = EOC) |
| 18..49    | Root directory (volume label + MRAM.BIN entry)       |
| 50..2097  | Clusters 2..2049 -> MRAM `0x02000000` + offset       |
| 2098..4145| Padding clusters (read as zeros, never referenced)   |

The data region is padded to 4096 clusters so the cluster count
crosses the 4085 FAT16 threshold (MS FAT spec 1.03 sec 3.5).

## VID / PID

VID = 0x1209 (pid.codes free-for-experiments range), PID = 0x000C,
locally chosen. Bench use only.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31; toggles per SCSI read). USB-FS pin set
(P407 / P500 / P814 / P815) is routed PSEL 0x13 as in `usb_cdc_echo`.
