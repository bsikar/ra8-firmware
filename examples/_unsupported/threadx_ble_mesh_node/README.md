# threadx_ble_mesh_node

NimBLE Bluetooth Mesh node demo on ThreadX. Brings up an unprovisioned
node hosting a Configuration Server + Generic OnOff Server using the
`libs/ra8_ble_host/inc/ra8_ble_mesh.h` API.

Build:

    make threadx_ble_mesh_node

Use the nRF Mesh app (Nordic Semiconductor) to provision the node.

Uses `ra8_board_ek_ra8d2` BSP for LED feedback (per EK-RA8D2 v1 UM
Table 24 p 31). The BLE controller is on-chip; no external pins are
required.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 24 p 31, and Bluetooth Mesh Profile 1.1 + Apache
NimBLE Mesh API.
