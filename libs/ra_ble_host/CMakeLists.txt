#
# libs/ra_ble_host -- starter Bluetooth Low Energy host stack
# (L2CAP / ATT / GATT) layered on top of libs/ra_hal/ra_ble.
#
# Scope:
#   - GAP role configuration (peripheral / central / observer / broadcaster)
#   - Connectable undirected advertising via the controller HCI commands
#     already exposed by ra_ble.
#   - L2CAP fixed channel bring-up (CID 0x0004 ATT, 0x0005 LE signaling)
#     with minimal fragmentation.
#   - ATT server PDU dispatch (Find_Information_Request,
#     Read_By_Type_Request, Read_Request, Write_Request,
#     Write_Command, Handle_Value_Notification).
#   - GATT attribute table built from registered Primary Services and
#     Characteristics, capped at 8 services / 32 characteristics.
#
# This library is currently picked up via tests/CMakeLists.txt globbing
# (the unit tests build all libs/* sources together) and via per-app
# example CMakeLists.txt that reach into inc/ and src/ directly. No
# board-side firmware target depends on it yet, so this file exists as
# a placeholder for future add_library() use.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers (tests, examples) reach into
# inc/ and src/ directly, mirroring libs/ra_net and libs/ra_tls.
# When an app starts to depend on ra_ble_host, wire up an OBJECT or
# INTERFACE library here.
