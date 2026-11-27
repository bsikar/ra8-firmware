# c6_ble_nimble

Apache NimBLE 1.10.0 GATT peripheral running on EK-RA8D2 + ThreadX over the
ESP32-C6 BLE controller (`ble_hci_c6link` -> `ra8_c6link` -> `ESP_HCI_IF`).

## Behaviour

1. Initializes `ra8_esp_hosted_port_init()`, opens `ra8_c6link`, waits for the
   ESP32-C6 ready event, opens `ra8_ble`, and binds `ble_hci_c6link_init()` +
   `ble_hci_ra8_ble_init()`.
2. Calls `nimble_port_init()`, registers GAP (`0x1800`), GATT (`0x1801`), and
   Battery Service (`0x180F` / characteristic `0x2A19` Read | Notify), and binds
   the NimBLE RAM attribute/CCCD store.
3. Spawns the HCI dispatch worker pumping `ble_hci_ra8_ble_dispatch()` (gated
   by `DATA_READY` and serialized against host TX) and runs `nimble_port_run()`.
4. On NimBLE host sync, ensures identity address, sets advertising fields
   (`"EK-RA8D2"`, Flags + Complete 16-bit UUID `0x180F`), starts connectable
   undirected GAP advertising (`BLE_GAP_CONN_MODE_UND`, `BLE_GAP_DISC_MODE_GEN`),
   and logs `c6_nimble: PASS nimble_c6_peripheral ready (advertising)`.
5. Handles central GAP connections, logging
   `c6_nimble: PASS central connected conn_handle=<n> peer_addr=<mac>`, MTU
   updates, CCCD subscriptions, and disconnects (automatically restarting
   advertising on disconnect).

Hardware requirements match `examples/ek_ra8d2/hw_validated/c6/` (`SW4`
`1=OFF 2=OFF 3=ON 4=OFF`, C6 harness on Pmod1 / J26, C6 self-powered over USB).
