# update/ -- OTA (A/B) + USB-HS update (via the RA8) + self-test (roadmap)

> **Superseded for the production path by `../docs/UPDATE_PIPELINE.md`** (one
> signed bundle, RA8-side staging, transports as producers) and, on the C6
> apply side, by esp-hosted's host-pushed co-processor OTA if the co-processor
> direction in `../docs/DIRECTION.md` is adopted. This file remains as the
> C6-side implementation notes for a first-party A/B receiver (strategy B/D).

The firmware-update story for the ESP32-C6 companion IC, mirroring the RA8
tree's `ra_dfu` / `ra_ota` work. Nothing here is implemented yet; this is the
plan the spike commits to. The payload for every path below is the Espressif
app image produced by `tools/esp_mkimage.py`.

## 1. OTA with A/B partitions + rollback

Two application slots plus a small state region:

```
  slot A     app image (esp_mkimage.py output)
  slot B     app image (the other one)
  state      active slot, pending slot, boot-attempt counter, health flag
```

- A minimal our-own 2nd-stage loader (roadmap; the C6 ROM boots it) reads the
  state region and jumps into the **active** slot.
- An update writes the **inactive** slot, verifies the appended SHA-256, marks
  it **pending**, and reboots.
- The new image must set its **health flag** within a boot-attempt budget; if it
  does not (crash loop, bad image), the loader **rolls back** to the last-known-
  good slot. This is the same anti-brick discipline as the RA8 DFU bootloader
  (attempt counter + copy-to-run + header-last commit).

## 2. USB update -- exposed from the RA8's USB HS, not the C6

**The fast USB in this system is on the RA8, not the C6.** The RA8D2 has a
High-Speed USB device (480 Mbps); the C6 only has a Full-Speed (12 Mbps)
USB-Serial-JTAG. So the production USB-update path is **exposed from the RA8's
USB HS** -- the fastest port we have -- and the C6 is updated *through* the RA8:

```
  host  --USB HS 480 Mbps-->  RA8  --companion link (SPI/UART)-->  C6
        pushes the C6 image    relays it                writes inactive slot,
        (reuses ra_dfu/ra_usb)  over the link            verifies SHA-256, commits
```

The RA8 ingests the C6 app image over its USB HS (reusing the RA8 tree's
`ra_dfu` / `ra_usb_pmsc` machinery), then streams it to the C6 over the
inter-chip companion link, where the C6's OTA slot/verify/commit/rollback logic
(section 1) writes and commits it. So on the C6 side the "USB update" is really a
**companion-link receiver** feeding `ota_apply` -- the USB HS itself lives on the
RA8.

**The C6's own USB-Serial-JTAG is a bring-up / console / direct-flash aid during
development only -- NOT the production fast-update transport.**

One update path, three transports: OTA over the air (C6-native Wi-Fi), USB HS
over the wire (ingested by the RA8, relayed to the C6), and the dev-only C6
serial for bench flashing.

## 3. Self-test on the RA8's two USB peripherals (needs only the EK-RA8D2)

The EK-RA8D2 exposes **two** USB peripherals -- USB HS and USB FS -- so the
USB-update transport can be proven on one RA8 board with no external host:

```
  RA8 USB HS (sender)  --- on-board loop / bridge --->  RA8 USB FS (receiver)
       streams a known-good                        ingests it as if from a host,
       C6 app image                                relays over the companion link,
                                                    commits + rolls back on the C6
```

One RA8 USB port **sends** the C6 update image and the other **receives** it, so
a single RA8 board drives the whole host -> RA8 -> companion-link -> C6
download-verify-commit-rollback chain end to end. The same image is then fed
into (a) the C6 OTA path and (b) the `sim/` simulator, giving three independent
checks of one artifact before any over-the-air update is trusted. (The USB-HS
ingress + the two-port loop live on the RA8 side of the monorepo, in `ra_dfu` /
`ra_usb`; this directory holds the C6 receiver + OTA logic.)

## Shape (planned)

```
update/                (C6 side)
  ota_state.c      A/B state region read/commit + boot-attempt/rollback logic
  ota_apply.c      write inactive slot, verify appended SHA-256, mark pending
  link_update.c    companion-link (SPI/UART) receiver -> ota_apply
                   (the bytes arrive from the RA8, which got them over USB HS)
```

On the RA8 side (existing tree, roadmap wiring): `ra_dfu` / `ra_usb_pmsc`
ingest the C6 image over USB HS and relay it across the companion link; a
`selftest_usbhs_loop` drives USB HS -> USB FS on one board.

`make -C esp32 flash` (today a roadmap stub pointing at `tools/esp_flash.py`)
is the manual sibling of these paths: same image, downloaded over the C6 ROM
serial protocol during bring-up before OTA / USB-HS update exist.
