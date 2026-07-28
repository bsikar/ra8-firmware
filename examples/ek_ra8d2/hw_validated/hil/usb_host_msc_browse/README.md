# usb_host_msc_browse (USB host MSC browse over the self-loop)

Validates the first-party USB **host** MSC stack (`ra8_usb_hmsc`) with no external
drive: the board hosts on one jack and **emulates the peripheral** on the other
over the loop cable (J7 HS host <-> J11 FS device). One image runs both roles.

- **USBFS (J11) = device (emulated peripheral):** a ThreadX + USBX Mass-Storage
  class exposing the 1 MiB MRAM window at `0x02000000` as a read-only synthesized
  FAT16 volume (`MRAM.BIN`).
- **USBHS (J7) = host:** the polled first-party host MSC stack (`ra8_usb_hmsc` +
  `ra8_fs`). It enumerates the device, mounts the FAT16 volume, then **browses**
  it -- reads the root directory over `READ(10)` and parses the file entry (name
  + size) -- before a raw byte-for-byte read-back and the write-protect check.

The directory browse is what distinguishes this from the raw read-verify
self-tests. The original version needed a real USB thumb drive in J7; the
self-loop stands in for it, so this now runs unattended on the HIL bench.

## Result (validated 2026-06-15 on real hardware)

```
host up on USB-HS, probing the loop...
enumerated vid=0x1209 pid=0x000E over the loop cable
mounted fs=fat16
browsed root, file=MRAM    BIN size=1048576 bytes
verified 1048576 bytes vs MRAM in 6181 ms (165 KiB/s)
write rejected (code 0x00000204), MRAM protected
USB HOST MSC BROWSE PASS
```

## HIL

`uart_scrape` gate on `USB HOST MSC BROWSE PASS`. Board-only (loop cable fitted);
no external device. Pinout / VID-PID identical to `usb_selftest_hs_host`.
