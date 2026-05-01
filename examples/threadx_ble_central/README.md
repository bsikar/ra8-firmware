# threadx_ble_central

NimBLE GATT-client central demo on ThreadX. Uses the
`libs/ra_ble_host/inc/ra_ble_gatt_client.h` and
`libs/ra_ble_host/inc/ra_ble_security.h` APIs.

Build:

    make threadx_ble_central

Pair against `examples/threadx_nimble_peripheral` running on a second
EK-RA8D2 to exchange Battery Service notifications.
