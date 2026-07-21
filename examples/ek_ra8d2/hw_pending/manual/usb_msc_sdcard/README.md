# usb_msc_sdcard (the live SD card as a writable USB drive)

Exposes the **Pmod2 SD-over-SPI card** as a real, **writable** USB
Mass-Storage drive at its **full CSD-derived capacity**. Plug the board's
USB-FS receptacle (J11) into a computer and the card mounts with whatever
filesystem it already carries -- copy a `.epub` / `.rabook` straight onto
the device, eject, done. This is the e-reader ingestion transport
(issue #206): no card pulling, no snapshot window, no synthesized FAT
volume anywhere.

- **At boot** `ra8_sdmmc_spi_transport_sci` opens SCI0 Simple-SPI on the
  Pmod2 pins, `ra8_sdmmc_spi_init` enumerates the card
  (CMD0/CMD8/ACMD41/CMD58/CMD9), and the CSD capacity sizes the MSC LUN.
  **No card seated** -> the `FAIL sd init` banner prints and the device
  deliberately never attaches (an empty drive is worse than none).
- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes
  one **writable** logical unit spanning the whole card
  (`GET_MAX_LUN` = 0).
  - **media-read** runs each SCSI `READ(10)` as one **CMD18**
    multi-block streak (`ra8_sdmmc_spi_read_blocks`, per-block CRC16
    verified).
  - **media-write** runs each `WRITE(10)` chunk as one **CMD25**
    multi-block streak (`ra8_sdmmc_spi_write_blocks`, data-response +
    busy handshake verified per block). The USBX class hands chunks of
    up to `UX_SLAVE_CLASS_STORAGE_BUFFER_SIZE` (4096 B = 8 blocks), so a
    host file copy passes through as back-to-back CMD25 streaks.

## How WRITE(10) is enabled (vs the read-only siblings)

USBX's `_ux_device_class_storage_write` checks the LUN's
`ux_slave_class_storage_media_read_only_flag` **before** calling any media
callback. The validated read-only examples (`usb_msc_mram`,
`usb_selftest_microsd`) set it `UX_TRUE`, so the class itself answers
`DATA PROTECT / WRITE PROTECTED` and their media-write hooks never run.
This app sets it `UX_FALSE`:

- `MODE SENSE` reports the medium **writable** (no WP bit), so hosts
  mount read-write;
- the class streams the bulk-OUT data phase into `sdmsc_msc_write` in
  buffer-sized runs, advancing the LBA per chunk;
- on a media failure the callback stores the sense triple
  (`MEDIUM ERROR / PERIPHERAL DEVICE WRITE FAULT`, or
  `ILLEGAL REQUEST / LBA OUT OF RANGE` for a bounds miss) in
  `media_status` and returns `UX_ERROR`; the class stalls the OUT
  endpoint, fails the CSW, and serves that triple through the host's
  next `REQUEST SENSE` -- the host knows the sectors did NOT land.

## SINGLE OWNER WARNING (host vs firmware)

While a host has this drive mounted, **the host owns the card**. The
firmware must NOT touch the card concurrently -- no `ra8_fs` mount, no
shelf/library scan, no reads "on the side". In this app the only card
user after boot is the USBX storage-class thread, which serializes all
access. Any future app that combines USB export with an on-device reader
must gate the two modes exclusively (e.g. unmount the shelf before
attaching USB); concurrent access interleaves SD commands mid-transaction
and corrupts the filesystem.

Writes go straight to the card with no device-side cache, so there is no
flush step beyond the host's own unmount (`SYNCHRONIZE CACHE` is honored
trivially).

## Bench procedure (manual, real PC -- issue #206 item 3)

1. Seat a FAT-formatted microSD card in the Pmod2 (J25) microSD adapter.
2. Flash: `make usb_msc_sdcard && make -C examples/ek_ra8d2/hw_pending/manual/usb_msc_sdcard flash`.
3. Watch the console (SCI8 / J-Link OB CDC, 115200): expect
   `usb_msc_sdcard: card <N> blocks (<M> MiB)` then
   `usb_msc_sdcard: USB MSC SDCARD READY (RW, <N> blocks)`.
4. Cable **J11 (USB-FS)** to the PC. The card auto-mounts
   (macOS: `/Volumes/<label>`; Linux: udisks; Windows: drive letter)
   at the card's real capacity, read-write.
5. Copy a file onto the drive (multi-MB proves multi-block WRITE(10)
   streaks; LED2 flickers per write, LED1 per read).
6. Unmount/eject cleanly, then re-mount (or re-plug) and verify the
   file reads back byte-identical (`cmp` / `diff`).
7. Optional cross-check: pull the card, mount it directly in a PC card
   reader, and verify the same file -- proving the bytes live on the
   card, not in any device-side RAM.

The USB-HS receptacle (J7) is not used by this app; a HS variant can
follow `usb_msc_mram_hs` once the FS path is hardware-validated.

## board_sim gate (no hardware)

`scripts/sim/smoke.sh usb_msc_sdcard` builds a 64 MiB FAT32 card
image with `tools/mkbookimg`, attaches it with `--sd`, and lets
board_sim's virtual USB host (#67) enumerate the device and drive the
MSC BOT script (INQUIRY, READ CAPACITY(10), READ(10) of sector 0)
against the live modelled card. The gate asserts:

- `USB: device CONFIGURED (MSC active)` -- chapter-9 enumeration
  completed against the real USBX device stack + USBFS DCD model;
- `USB MSC : capacity 131072 blocks x 512B, INQUIRY ok, sector read
  512 byte(s)` -- the capacity reported over the USB pipe equals the
  card image's real block count (image -> modelled CSD ->
  `ra8_sdmmc_spi` -> LUN geometry -> `READ CAPACITY`), and sector 0
  travelled card -> CMD17/CMD18 -> media-read -> BOT data phase intact.

The real-PC copy (step 4-7 above) stays a manual bench step: the sim's
scripted host does not issue `WRITE(10)`.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_usb_msc_sdcard_blocks` (CSD capacity latch; 0 = no card),
`s_dbg_sd_err` (SD bring-up result), `s_dbg_dev_step` / `s_dbg_dev_err`
(worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 parked),
`s_dbg_read_calls` / `s_dbg_read_blocks` (media-read streaks / blocks),
`s_dbg_write_calls` / `s_dbg_write_blocks` (media-write streaks /
blocks), `s_dbg_media_err` (first SD driver error on the media path).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). Console: PD_02/PD_03 SCI8. microSD: Pmod2 SCI0 Simple-SPI
(SCK/CIPO/COPI PSEL sci_async, CS GPIO idle high).

## VID / PID

Device side advertises VID 0x1209, PID 0x0019. Bench use only.
