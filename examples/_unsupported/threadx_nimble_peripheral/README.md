# threadx_nimble_peripheral

> **Status: UNVALIDATED SCAFFOLD (issue #286).** This app links and
> passes the static gates, but has **never been hardware-validated** and
> is **not emulator-gated**: `ra8_emulator` models no RA8D2 BLE controller / HCI
> mailbox, so nothing in CI exercises the air interface, and the
> underlying `ra8_ble` transport is itself unproven on this board (#86,
> #91). The `port/nimble/` host port + ThreadX Native Porting Layer it
> depends on are link-only scaffold. This app stays under
> `examples/_unsupported/` until a NimBLE app is driven to real hardware
> validation and promoted. The "Verify" steps below describe the
> *intended* end state, not a confirmed result.

NimBLE-based replacement for `examples/ble_peripheral`. Same Battery
Service profile (UUID 0x180F + Battery Level char 0x2A19, Read |
Notify), same `EK-RA8D2` advertised local name, same 10-second
battery-decrement loop -- but the host stack is now Apache NimBLE
running on Eclipse ThreadX, glued to our `ra8_ble` driver via the
adapter under `port/nimble/`.

## Topology

```
+----------------------+        +-----------------------+
|  NimBLE host stack   | <----> |  port/nimble adapter  | <----> ra8_ble HCI mailbox
| (host/ble_hs.h API)  |        |  ble_hci_ra8_ble.c     |        (ra8_ble_hci_send_*)
+----------------------+        |  nimble_npl_threadx.c |
                                +-----------------------+
                                            ^
                                            |
                                +-----------------------+
                                | Eclipse ThreadX (NPL) |
                                |  TX_MUTEX / TX_QUEUE  |
                                |  TX_SEMAPHORE / TIMER |
                                +-----------------------+
```

- `ble_hci_ra8_ble.c` -- NimBLE HCI transport adapter. Implements
  `ble_transport_to_ll_cmd_impl` / `ble_transport_to_ll_acl_impl`
  (host -> controller) on top of `ra8_ble_hci_send_command` /
  `ra8_ble_hci_send_acl_data`. Inbound traffic flows through
  `ra8_ble_attach_event_handler` / `ra8_ble_attach_acl_handler`
  callbacks that re-pack into NimBLE buffers and call
  `ble_transport_to_hs_evt` / `ble_transport_to_hs_acl`.
- `nimble_npl_threadx.c` -- ThreadX implementation of the NimBLE
  Native Porting Layer.

## Build

```
make threadx_nimble_peripheral
```

That forwards to the per-app Makefile, which configures cmake with
`-DRA8_USE_THREADX=ON -DRA8_USE_NIMBLE=ON` and produces
`build/threadx_nimble_peripheral.elf` / `.hex` / `.bin`.

## Verify

1. Flash: `make flash` from this directory.
2. Open `nRF Connect for Mobile` (Nordic Semiconductor) on a phone.
3. Scan for `EK-RA8D2`, tap to connect.
4. Expand the Battery Service (0x180F).
5. Tap the down-arrow on the Battery Level characteristic to enable
   notifications.
6. Watch the value tick down once every 10 seconds.

## BLE patch image gap (Phase 1.3 of roadmap)

The RA8D2 BLE controller requires a Renesas-supplied firmware patch
image at boot. `ra8_ble_open` currently stubs the patch-load loop
(`ra8_ble.h` documents this), so on real silicon `ra8_ble_open` will
report success but no air activity will happen until the production
patch loader is wired in.

This / Phase 1.3 task wires the *software* path (HCI transport bridge,
NPL, and GATT skeleton) so that it links. That path has never been
exercised on silicon: the patch-load gap is one of several unproven
links (patch loader, `ra8_ble` transport, the NimBLE port itself), and
none of them has been demonstrated end-to-end. Do not read this app as
"one blocker away from working."

## Files

- `main.c` -- application entry; CGC, SCI8, ra8_ble_open, ThreadX
  bring-up, NimBLE port init, battery loop.
- `vector_table.c`, `system_init.c`, `secure_exception.c`,
  `trustzone_init.{c,h}` -- per-app boot files (copied from
  `examples/threadx_filex_demo`).
- `linker_script.ld` -- per-app memory map.
- `CMakeLists.txt`, `Makefile` -- per-app build wrappers.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31). The BLE controller is on-chip; no external
pins are required. SCI8 console on PD02 / PD03 per UM Table 13 p 24.

Pin assignments and API usage checked (on paper only, not on silicon)
against EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev 1.01) Tables 13 p
24 / 24 p 31, and Bluetooth Core 5.3 + Apache NimBLE host/transport API.
This is a documentation-citation record, not a hardware-validation
record -- see the status banner at the top.
