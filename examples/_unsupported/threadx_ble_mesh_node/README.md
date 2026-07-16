# threadx_ble_mesh_node

> **Status: UNVALIDATED SCAFFOLD (issue #286).** This app links and
> passes the static gates, but has **never been hardware-validated** and
> is **not sim-gated**: `board_sim` models no RA8D2 BLE controller / HCI
> mailbox, and the underlying `ra8_ble` transport is itself unproven on
> this board (#86, #91). The `port/nimble/` host port + ThreadX Native
> Porting Layer it depends on are link-only scaffold. This app stays
> under `examples/_unsupported/` until a NimBLE app is driven to real
> hardware validation and promoted.

NimBLE Bluetooth Mesh node demo on ThreadX. Brings up an unprovisioned
node hosting a Configuration Server + Generic OnOff Server using the
`libs/ra8_ble_host/inc/ra8_ble_mesh.h` API.

Build:

    make threadx_ble_mesh_node

Use the nRF Mesh app (Nordic Semiconductor) to provision the node.

Uses `ra8_board_ek_ra8d2` BSP for LED feedback (per EK-RA8D2 v1 UM
Table 24 p 31). The BLE controller is on-chip; no external pins are
required.

Pin assignments and API usage checked (on paper only, not on silicon)
against EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev 1.01) Table 24 p
31, and Bluetooth Mesh Profile 1.1 + Apache NimBLE Mesh API. This is a
documentation-citation record, not a hardware-validation record -- see
the status banner at the top.
