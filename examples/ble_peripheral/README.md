# ble_peripheral

Bluetooth Low Energy peripheral smoke test for the EK-RA8D2. Brings up
the BLE controller via `ra_ble_open`, layers the `ra_ble_host` stack on
top in the peripheral GAP role (Bluetooth Core 5.3 Vol 3 Part C 2.2.2),
registers a Battery Service (UUID 0x180F, Bluetooth Assigned Numbers
3.4) with a single Battery Level characteristic (UUID 0x2A19, Read |
Notify -- Bluetooth Core 5.3 Vol 3 Part G 3.3.1.1), and starts
connectable undirected advertising under the local-name `EK-RA8D2`.

The main loop decrements the battery level by one every ten seconds
(wrapping back to 100 at zero). When a connected peer subscribes to
notifications via the CCCD (UUID 0x2902), the new value is pushed out
via `ra_ble_host_gatt_notify`. Connection / disconnection / subscribe
events are surfaced through the registered host event callback and
logged over SCI8.

## Test from a phone (nRF Connect for Mobile)

1. Install **nRF Connect for Mobile** (Nordic Semiconductor) on
   Android or iOS.
2. Power the EK-RA8D2 and flash the firmware.
3. Open nRF Connect, tap **SCANNER -> Start scanning**.
4. The device should show up in the list as **EK-RA8D2**.
5. Tap **CONNECT**.
6. Expand **Battery Service (0x180F)** -> **Battery Level (0x2A19)**.
7. Tap the down-arrow (notify icon) to enable notifications.
8. Watch the value tick down by one every ten seconds.

## Test from Linux (`bluetoothctl`)

```sh
bluetoothctl
[bluetooth]# scan on
# wait for "EK-RA8D2" to appear
[bluetooth]# scan off
[bluetooth]# pair  <addr>
[bluetooth]# connect <addr>
[bluetooth]# menu gatt
[gatt]# list-attributes
# find the Battery Level value handle
[gatt]# select-attribute <handle>
[gatt]# read
# value should be 0x32 (= 50) on the first connection, then decrement
[gatt]# notify on
# subsequent reads / notifications show the value tick down
```

## SCI8 logs

The on-board J-Link OB CDC bridge (PD_02 / PD_03 -- SCI8 at 115200 8N1)
prints:

- `ble: advertising` once init completes.
- `ble: connected` on each `LE_Connection_Complete` event.
- `ble: subscribed` on each CCCD-write that enables notifications.
- `ble: write` on every characteristic write.
- `ble: disconnected` on each link teardown.

## Build + flash

```sh
make ble_peripheral
make -C examples/ble_peripheral flash
```

## What the firmware does

1. `ra_cgc_init()` -- XTAL + PLL1 -> CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
3. SCI8 logging on PD_02 / PD_03 via `ra_pfs_route_peripheral` +
   `ra_sci_init` at 115200 8N1.
4. `ra_ble_host_init({ role = peripheral, name = "EK-RA8D2" })`.
5. `ra_ble_host_attach_event_handler` to log connect / subscribe /
   disconnect / write events.
6. Register Battery Service (0x180F) + Battery Level (0x2A19, Read |
   Notify).
7. Build a 31-byte legacy AD payload (Flags + Complete UUID16 List +
   Complete Local Name) and `ra_ble_host_advertise_start` at 100 ms
   interval.
8. Loop: every 10 s decrement the battery value, push it into the
   value cache via `ra_ble_host_gatt_set_value`, and call
   `ra_ble_host_gatt_notify` to fan out a Handle Value Notification to
   subscribed peers.

## Pinout

This example only uses SCI8 (PD_02 / PD_03) for SCI logging plus LED1
(P6_00) for visual heartbeat. The BLE controller is on the on-chip
radio block; no external pins are required.
