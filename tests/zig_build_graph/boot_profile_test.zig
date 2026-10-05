//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8FW-626: the Zig graph honours BOOT_PROFILE the way
//! cmake/ra8_app/sources.cmake does, so tz_nsc_cgc_usb links the board's
//! src/boot/ns_usb_handoff/ boot units (SAU setup and the BLXNS hand-off)
//! instead of the default system_init.c fallback.
const std = @import("std");
const graph = @import("build_graph");
const sources = graph.cross_sources;
const app_table = sources.app_table;

fn appNamed(name: []const u8) app_table.CrossApp {
    for (app_table.cross_apps) |app| {
        if (std.mem.eql(u8, app.name, name)) return app;
    }
    @panic("no such app");
}

test "tz_nsc_cgc_usb names the ns_usb_handoff boot profile" {
    const app = appNamed("tz_nsc_cgc_usb");
    try std.testing.expectEqualStrings("ns_usb_handoff", app.boot_profile.?);
}

test "a profile resolves to the board's profile directory" {
    const app = appNamed("tz_nsc_cgc_usb");
    const path = sources.profileBootPath(std.testing.allocator, app, "system_init.c").?;
    defer std.testing.allocator.free(path);
    try std.testing.expectEqualStrings(
        "libs/ra8_board_ek_ra8d2/src/boot/ns_usb_handoff/system_init.c",
        path,
    );
}

test "tz_nsc_cgc_usb carries the NS memory map defines" {
    try std.testing.expect(appNamed("tz_nsc_cgc_usb").local.ns_memory_map);
}

test "an app with no profile has no middle rung" {
    const app = appNamed("threadx_stkof");
    try std.testing.expectEqual(@as(?[]const u8, null), sources.profileBootPath(std.testing.allocator, app, "system_init.c"));
}
