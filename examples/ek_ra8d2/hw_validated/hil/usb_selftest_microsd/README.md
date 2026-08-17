# usb_selftest_microsd

Brings the board's Pmod2 microSD card up as a USB drive and verifies it against
itself on-chip -- no PC in the loop. The two USB ports are cabled to each other
and one image runs both USB stacks plus the SCI0 Simple-SPI SD driver.

At boot the device snapshots the card's FAT/exFAT boot region into RAM with
`ra8_sdmmc_spi_read_block` and confirms the `0x55AA` boot signature. USBFS (J11)
then exposes that window as a single read-only logical unit, serving media-read
from the snapshot so the class thread never touches the live card. USBHS (J7) is
the polled first-party host stack (`ra8_usb_hmsc`), which streams the window
back over raw multi-block `READ(10)` and byte-checks every sector against the
snapshot.

Because the host compares what came over USB against the exact image the device
read off the card, the loop proves the SD read path and the USB transport
deliver the card's real content intact.

## Read-only by design

The card holds the user's filesystem, so this app calls no
`ra8_sdmmc_spi_write_block` anywhere. A host-*writable* microSD drive would need
a writable LUN plus the device bulk-OUT WRITE(10) path; `usb_selftest_ospi_rw`
covers that shape against flash instead.

## microSD bring-up (mirrors sd_font_render)

`ra8_pfs_route_peripheral` routes the Pmod2 SCK/CIPO/COPI to the SCI async PSEL;
CS is a plain GPIO held low across each SD command and idle high, because the
SCI's hardware CS pulses per byte and SD SPI mode cannot tolerate that.
`ra8_sci_spi_init` opens SCI0 Simple-SPI at the slow init clock and
`ra8_sdmmc_spi_init` enumerates the card and negotiates up.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. microSD: Pmod2 SCI0
Simple-SPI (SCK/CIPO/COPI PSEL sci_async, CS GPIO). The device advertises VID
0x1209 with a per-app PID; bench use only.
