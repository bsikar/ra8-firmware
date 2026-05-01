# threadx_ble_mesh_node

NimBLE Bluetooth Mesh node demo on ThreadX. Brings up an unprovisioned
node hosting a Configuration Server + Generic OnOff Server using the
`libs/ra_ble_host/inc/ra_ble_mesh.h` API.

Build:

    make threadx_ble_mesh_node

Use the nRF Mesh app (Nordic Semiconductor) to provision the node.
