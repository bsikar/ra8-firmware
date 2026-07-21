# tz_secure_only_usb_hs

USB-HS device-mode CDC ACM echo running entirely in the Secure world (no
TrustZone partition, no NSC veneers). The device enumerates as a virtual
serial port; every byte sent on bulk-OUT is echoed back on bulk-IN.

## Hardware

| Item | Connector |
|------|-----------|
| USB-HS device port (DUT) | **J7** (USB-C, high-speed PHY with on-board UTMI) |
| J-Link OB debug port (flash + RTT log) | **J10** (USB-C) |
| VBUS source for J7 | On-board USB-PD controller -- the host PC does **not** power the rail |

Plug **both** USB cables to the host: J10 for flashing/RTT, J7 for the
CDC enumeration.

## Build

```sh
cd examples/ek_ra8d2/tz_secure_only_usb_hs
make build
```

Artifacts land under `build/` (`tz_secure_only_usb_hs.elf` / `.hex` /
`.bin`).

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
   Expect `/dev/cu.usbmodem000000021` (the trailing `1` is the CDC
   interface index appended to the iSerial `00000002`).

2. Round-trip a payload through bulk OUT/IN:
   ```sh
   python3 scripts/hil/usb/cdc_echo_test.py --tty /dev/cu.usbmodem000000021
   ```
   Expect `OK: round-tripped 10 bytes`. Auto-detect form:
   `python3 scripts/hil/usb/cdc_echo_test.py --auto hs`.

3. (Optional) Inspect the descriptor:
   ```sh
   ioreg -p IOUSB -l -w 0 | grep -A3 EK-RA8D2
   ```

## Expected output

`ioreg` snippet:
```
    | |   "USB Product Name" = "EK-RA8D2 HS CDC Echo"
    | |   "USB Vendor Name" = "Renesas"
    | |   "USB Serial Number" = "00000002"
```

Test script:
```
$ python3 scripts/hil/usb/cdc_echo_test.py --tty /dev/cu.usbmodem000000021
OK: round-tripped 10 bytes
```

## Troubleshooting

- **No `/dev/cu.usbmodem*` after flash** -- unplug J7 for ~5 s and
  re-plug; the HS PHY occasionally needs a clean re-enumeration after a
  J-Link reset.
- **`LOCKUP` printed on RTT at boot** -- `make clean && make build &&
  make flash`; the CGC + USB-HS bring-up is sensitive to stale `.bss`
  from a previous half-flashed image.
- **Device enumerates as full-speed only** -- check that J7 is plugged
  into a host port that advertises USB 2.0 high-speed (some USB-3 hubs
  drop to FS); replug directly into the host if in doubt.
- **Test script reports "no data received"** -- another terminal (screen,
  minicom, picocom) may be holding the tty open; close it and retry.
