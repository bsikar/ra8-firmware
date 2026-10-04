//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the static library: every C ABI file `ra8_c6link` ships.
//!
//! The pump and dispatcher ABI files sit beside the main membrane rather than
//! inside it because they call the C RPC decoder, which the host test of the
//! membrane does not link.

comptime {
    _ = @import("ra8_c6link_abi.zig");
    _ = @import("ra8_c6link_pump_abi.zig");
    _ = @import("ra8_c6link_dispatch_abi.zig");
    _ = @import("ra8_c6link_field_abi.zig");
    _ = @import("ra8_c6link_emit_abi.zig");
    _ = @import("ra8_c6link_lifecycle_abi.zig");
    _ = @import("ra8_c6link_ready_abi.zig");
    _ = @import("ra8_c6link_eth_abi.zig");
    _ = @import("ra8_c6link_fw_abi.zig");
    _ = @import("ra8_c6link_wifi_abi.zig");
    _ = @import("ra8_c6link_bare_abi.zig");
    _ = @import("ra8_c6link_take_abi.zig");
    _ = @import("ra8_c6link_resp_abi.zig");
    _ = @import("ra8_c6link_ap_info_abi.zig");
    _ = @import("ra8_c6link_mac_abi.zig");
    _ = @import("ra8_c6link_sta_abi.zig");
}
