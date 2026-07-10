# update/ -- OTA (A/B) + USB update + two-USB self-test (roadmap)

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

## 2. USB update

The C6 exposes a USB device (native USB-Serial-JTAG or a CDC/DFU class, TBD)
that accepts an app image and writes it to the inactive OTA slot, then hands off
to the OTA rollback logic above. This reuses the OTA slot/verify/commit machinery
so there is one update path, two transports (OTA over the air, USB over the wire).

## 3. Two-USB self-test (needs only the dev board)

The EVM's two USB ports let the update path be proven **with no external rig**:

```
   port 1 (sender)  --- host loop / on-board bridge --->  port 2 (receiver)
        |                                                      |
   streams the app image                             writes inactive slot,
   from a known-good build                           verifies SHA-256, commits
```

One port **sends** the update image and the other **receives** it, so a single
board exercises the entire download-verify-commit-rollback chain. The same image
is then fed into (a) the OTA path and (b) the `sim/` simulator, giving three
independent checks of one artifact before any over-the-air update is trusted.

## Shape (planned)

```
update/
  ota_state.c      A/B state region read/commit + boot-attempt/rollback logic
  ota_apply.c      write inactive slot, verify appended SHA-256, mark pending
  usb_update.c     USB transport -> ota_apply
  selftest_2usb.c  drive port1->port2 loop, assert commit + rollback
```

`make -C esp32 flash` (today a roadmap stub pointing at `tools/esp_flash.py`)
is the manual sibling of these paths: same image, downloaded over the ROM serial
protocol during bring-up before OTA/USB update exist.
