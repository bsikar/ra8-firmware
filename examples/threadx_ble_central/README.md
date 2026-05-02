# threadx_ble_central

NimBLE GATT-client central demo on ThreadX. Uses the
`libs/ra_ble_host/inc/ra_ble_gatt_client.h` and
`libs/ra_ble_host/inc/ra_ble_security.h` APIs.

Build:

    make threadx_ble_central

Pair against `examples/threadx_nimble_peripheral` running on a second
EK-RA8D2 to exchange Battery Service notifications.

Uses `ra_board_ek_ra8d2` BSP for LED feedback (per EK-RA8D2 v1 UM
Table 24 p 31). The BLE controller is on-chip; no external pins are
required.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 24 p 31, and Bluetooth Core 5.3 + Apache NimBLE GATT
client API.
