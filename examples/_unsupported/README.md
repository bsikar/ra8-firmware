# examples/_unsupported/

Apps in this tier are committed for completeness and as reference
implementations, but they cannot be hardware-validated on a stock
**EK-RA8D2 v1** evaluation kit. Each one needs extra hardware, a vendor
binary blob, or a Renesas-specific add-on board that this project does
not own.

Either acquire the additional hardware listed below or expect bit-rot:
nothing in CI flashes these apps to silicon, so a refactor that breaks
one of them will not be caught by the usual smoke tests. They still
have to compile (CI cross-builds every app) and they still have to
satisfy the same lint/format/MC/DC gates as the EVM-validated tier.

| App                              | Requires                                                |
|----------------------------------|---------------------------------------------------------|
| audio_loopback                   | External audio amplifier + speaker / line-out hardware  |
| usb_audio_device                 | External audio amplifier + speaker / line-out hardware  |
| ble_peripheral                   | BLE radio module + Renesas vendor patch image           |
| threadx_nimble_peripheral        | BLE radio module + Renesas vendor patch image           |
| threadx_ble_central              | BLE radio module + Renesas vendor patch image           |
| threadx_ble_mesh_node            | BLE radio module + Renesas vendor patch image           |
| threadx_https_client             | RSIP BIST vendor blob (encrypted asset image)           |
| ptp_time_transmitter             | PTP-aware Ethernet switch                               |
| motor_3phase                     | Renesas MCK motor-control daughter board                |
| threadx_sdcard_demo              | SD card slot + an SD card                               |

> **NimBLE apps are link-only scaffold (issue #286).**
> `threadx_nimble_peripheral`, `threadx_ble_central`, and
> `threadx_ble_mesh_node` sit on the `port/nimble/` host port + ThreadX
> Native Porting Layer, which link and pass the static gates but have
> **never been hardware-validated** and are **not sim-gated** --
> `board_sim` models no RA8D2 BLE controller / HCI mailbox, and the
> `ra8_ble` transport underneath is itself unproven on this board
> (#86, #91). `ble_peripheral` uses the separate hand-written
> `ra8_ble_host` stack over the same unproven transport, so it is
> equally unvalidated. Read every BLE app here as a compile-and-link
> reference, not a working feature, until one is driven to real
> hardware validation and promoted out of this tier.

If you add a hardware-dependent app, drop it under this directory so
the next person scanning the tree can immediately tell what we can and
cannot validate.

## Companion tier

See [`examples/ek_ra8d2/`](../ek_ra8d2/README.md) for the apps we
hardware-validate every release on the stock EVM.
