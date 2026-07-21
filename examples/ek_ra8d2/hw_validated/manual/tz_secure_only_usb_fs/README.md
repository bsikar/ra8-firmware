# tz_secure_only_usb_fs

USB-FS device-mode CDC ACM echo running entirely in the Secure world (no
TrustZone partition, no NSC veneers). The device enumerates as a virtual
serial port; every byte sent on bulk-OUT is echoed back on bulk-IN.

## Hardware

| Item | Connector |
|------|-----------|
| USB-FS device port (DUT) | **J11** (USB-C, full-speed PHY) |
| J-Link OB debug port (flash + RTT log) | **J10** (USB-C) |
| VBUS source for J11 | On-board USB-PD controller -- the host PC does **not** power the rail |

Plug **both** USB cables to the host: J10 for flashing/RTT, J11 for the
CDC enumeration.

## Build

```sh
cd examples/ek_ra8d2/tz_secure_only_usb_fs
make build
```

Artifacts land under `build/` (`tz_secure_only_usb_fs.elf` / `.hex` / `.bin`).

## Flash

```sh
make flash
```

Drives `JLinkExe -device R7KA8D2KF_CPU0 -if SWD` via `scripts/dev/flash.sh`
and loads the `.hex` into MRAM at `0x02000000`.

## Verify

1. Confirm the host enumerated the device:
   ```sh
   ls /dev/cu.usbmodem*
   ```
   Expect `/dev/cu.usbmodem000000011` (the trailing `1` is the CDC
   interface index appended to the iSerial `00000001`).

2. Round-trip a payload through bulk OUT/IN:
   ```sh
   python3 scripts/hil/usb/cdc_echo_test.py --tty /dev/cu.usbmodem000000011
   ```
   Expect `OK: round-tripped 10 bytes`. Auto-detect form:
   `python3 scripts/hil/usb/cdc_echo_test.py --auto fs`.

3. (Optional) Inspect the descriptor:
   ```sh
   ioreg -p IOUSB -l -w 0 | grep -A3 EK-RA8D2
   ```

## Expected output

`ioreg` snippet:
```
    | |   "USB Product Name" = "EK-RA8D2 CDC Echo!"
    | |   "USB Vendor Name" = "Renesas"
    | |   "USB Serial Number" = "00000001"
```

Test script:
```
$ python3 scripts/hil/usb/cdc_echo_test.py --tty /dev/cu.usbmodem000000011
OK: round-tripped 10 bytes
```

## Troubleshooting

- **No `/dev/cu.usbmodem*` after flash** -- unplug J11 for ~5 s and
  re-plug; macOS occasionally caches a stale endpoint after a J-Link
  reset.
- **`LOCKUP` printed on RTT at boot** -- `make clean && make build &&
  make flash`; the CGC bring-up is sensitive to stale `.bss` from a
  previous half-flashed image.
- **Test script reports "no data received"** -- another terminal (screen,
  minicom, picocom) may be holding the tty open; close it and retry.
