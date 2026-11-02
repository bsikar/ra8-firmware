//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Link stubs for the netif facades' driver externs (RA8FW-1084).
//!
//! Every ra8_io test root links the whole archive, so each one pulls in the
//! netif facades, whose c6link and net_pal driver externs no test but the
//! three netif roots defines. Tests that do not exercise the netif units
//! pull this in with `comptime { _ = @import("netif_stubs.zig"); }`; the
//! dedicated netif roots define their own recording stubs instead and must
//! not import this file.

export fn ra8_c6link_eth_send(_: ?*anyopaque, _: ?[*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_c6link_poll(_: ?*anyopaque, _: u16, _: ?*anyopaque) c_int {
    return 0;
}
export fn ra8_net_pal_send_frame(_: ?[*]const u8, _: u16) u16 {
    return 0;
}
export fn ra8_net_pal_recv_frame(_: ?[*]u8, _: ?*u16) u16 {
    return 0;
}
