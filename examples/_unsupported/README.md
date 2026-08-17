# examples/_unsupported/

Apps that cannot be hardware-validated on a stock **EK-RA8D2 v1**: each needs
extra hardware, a vendor binary blob, or a Renesas add-on board this project
does not own. They are kept as reference implementations, and they still have to
cross-compile and satisfy every static gate -- but nothing in CI flashes them,
so expect bit-rot that a refactor will not catch.

| App | Needs |
|---|---|
| `audio_loopback` | External audio amplifier + speaker / line-out |
| `usb_audio_device` | External audio amplifier + speaker / line-out |
| `threadx_nimble_peripheral` | ESP32-C6 BLE controller + HCI link |
| `threadx_https_client` | RSIP BIST vendor blob (encrypted asset image) |
| `motor_3phase` | Renesas MCK motor-control daughter board |
| `threadx_sdcard_demo` | SD card slot + an SD card |

> **`threadx_nimble_peripheral` is link-only scaffold (#286).** It drives the
> NimBLE host APIs over [`port/nimble/`](../../port/nimble/), links cleanly, and
> has never been hardware-validated or emulator-gated -- `ra8_emulator` models no
> HCI link, and the `ra8_ble` transport underneath is itself unproven on this
> board (#86, #91). Read it as a compile-and-link reference, not a feature.

A new hardware-dependent app belongs here, so the next person scanning the tree
can tell at a glance what can and cannot be validated. The apps that *are*
validated every release live in [`../ek_ra8d2/`](../ek_ra8d2/README.md).
