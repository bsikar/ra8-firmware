# threadx_nimble_peripheral

A GATT Battery Service peripheral built on the Apache NimBLE host stack running
over Eclipse ThreadX, glued to the `ra8_ble` transport by the adapter under
`port/nimble/`. That adapter is the point of the app: a NimBLE HCI transport
bridge on one side and a ThreadX implementation of NimBLE's Native Porting
Layer on the other.

## It has never produced air activity, and cannot

**The RA8D2 has no on-chip BLE radio.** No vendor firmware patch can add a
controller that is absent from the hardware.

BLE on this board means an ESP32-C6 companion carrying the controller, reached
over the `ra8_ble` HCI transport seam. What is wired here is the *software*
path only -- transport bridge, porting layer and GATT skeleton -- so that it
links and is host-tested. It is not emulator-gated either: `ra8_emulator`
models no HCI link, and the `ra8_ble` transport underneath is itself unproven
on this board (#86, #91, #286).

Read it as a compile-and-link reference. Do not read it as one blocker away
from working.
