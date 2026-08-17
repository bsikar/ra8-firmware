# usb_msc_sdcard

Exposes the Pmod2 SD-over-SPI card as a real, **writable** USB Mass-Storage drive
at its full CSD-derived capacity. Plug the board's USB-FS receptacle (J11) into a
computer and the card mounts with whatever filesystem it already carries -- copy
a `.epub` or `.rabook` straight onto the device, eject, done. This is the
e-reader ingestion transport (#206): no card pulling, no snapshot window, no
synthesized FAT volume anywhere.

At boot the card is enumerated over SCI0 Simple-SPI and its CSD capacity sizes
the single MSC logical unit. **With no card seated the device deliberately never
attaches** -- an empty drive is worse than none. Each SCSI `READ(10)` runs as one
CMD18 multi-block streak with per-block CRC16 verification, and each `WRITE(10)`
chunk as one CMD25 streak with the data-response and busy handshake verified per
block, so a host file copy passes through as back-to-back streaks.

## How WRITE(10) is enabled, unlike the read-only siblings

USBX checks the LUN's `ux_slave_class_storage_media_read_only_flag` **before**
calling any media callback. The read-only examples (`usb_msc_mram`,
`usb_selftest_microsd`) set it true, so the class itself answers
`DATA PROTECT / WRITE PROTECTED` and their media-write hooks never run at all.
This app sets it false, so `MODE SENSE` reports the medium writable and hosts
mount read-write, and the class streams the bulk-OUT data phase into the write
callback in buffer-sized runs.

On a media failure that callback stores the SCSI sense triple -- `MEDIUM ERROR /
PERIPHERAL DEVICE WRITE FAULT`, or `ILLEGAL REQUEST / LBA OUT OF RANGE` for a
bounds miss -- and returns an error; the class then stalls the OUT endpoint,
fails the CSW, and serves that triple through the host's next `REQUEST SENSE`.
The host therefore learns that the sectors did **not** land.

## SINGLE OWNER WARNING

While a host has this drive mounted, **the host owns the card.** The firmware
must not touch it concurrently -- no `ra8_fs` mount, no shelf or library scan, no
reads "on the side". Here the only card user after boot is the USBX storage
thread, which serializes all access. Any future app combining USB export with an
on-device reader must gate the two modes exclusively; concurrent access
interleaves SD commands mid-transaction and corrupts the filesystem.

Writes go straight to the card with no device-side cache, so there is no flush
step beyond the host's own unmount.

## Blocked on

A real PC on J11 doing the copy. The scripted host available off-target
enumerates the device and drives INQUIRY / READ CAPACITY / READ(10) against the
modelled card, but it never issues `WRITE(10)` -- so the writable half, which is
the reason this app exists, can only be proven by mounting it on a computer,
copying a multi-megabyte file, re-mounting, and verifying it reads back
byte-identical. Pulling the card and checking it in a reader proves the bytes
live on the card rather than in device-side RAM.

The USB-HS receptacle (J7) is unused here; an HS variant can follow
`usb_msc_mram_hs` once the FS path is hardware-validated.

## Diagnostics and pinout

`s_usb_msc_sdcard_blocks` latches the CSD capacity and reads 0 when no card is
present; the `s_dbg_*` statics track SD bring-up result, worker progress, media
errors, and read/write streak and block counts for a J-Link probe (re-resolve
them with `arm-none-eabi-nm`).

FS device pins: P4_07 VBUS sense, P5_00 VBUSEN GPIO low, P8_14/P8_15 data.
microSD: Pmod2 SCI0 Simple-SPI with a GPIO chip select idling high. The device
advertises VID 0x1209 / PID 0x0019 -- bench use only.
