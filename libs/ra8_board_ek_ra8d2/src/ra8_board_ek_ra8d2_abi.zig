//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI of the ported half of the EK-RA8D2 board layer, and nothing
//! else. Every name here is the one the unchanged `inc/` headers already
//! declare, so no consumer changes.
//!
//! No `-Dabi-prefix` option, unlike `ra8_board_ra8p1`: that board needs a
//! renamed archive because its coverage suite links it alongside the default
//! EK-RA8D2 objects. This layer *is* the default, so nothing links two copies
//! of it and a prefix would buy nothing.

const bringup = @import("internal/bringup.zig");
const clock_profile = @import("internal/clock_profile.zig");
const clock_types = @import("internal/clock_types.zig");
const console_stream = @import("internal/console_stream.zig");
const dualcore = @import("internal/dualcore.zig");
const stream = @import("internal/stream.zig");
const usb_port = @import("internal/usb_port.zig");

export fn ra8_board_shared_ram(out: ?*dualcore.SharedRam) u32 {
    return dualcore.describe(out);
}

export fn ra8_board_console_stream(out: ?*stream.IoStream) u32 {
    return console_stream.bind(out);
}

export fn ra8_board_usb_port_init(port: u32, role: u32) u32 {
    return usb_port.init(port, role);
}

export fn ra8_board_bringup(cfg: ?*const bringup.Cfg, out: ?*bringup.Rates) u32 {
    return bringup.run(cfg, out);
}

export fn ra8_board_clock_profile_to_chip(
    module: clock_types.Module,
    out_chip: ?*clock_types.Module,
) u32 {
    return clock_profile.toChip(module, out_chip);
}

export fn ra8_board_clock_profile_count(kind: u8) u8 {
    return clock_profile.count(kind);
}

export fn ra8_board_clock_profile_bind(clk: ?*clock_types.FwClock) u32 {
    return clock_profile.bind(clk);
}

export fn ra8_board_clock() *const clock_types.FwClock {
    return clock_profile.handle();
}
