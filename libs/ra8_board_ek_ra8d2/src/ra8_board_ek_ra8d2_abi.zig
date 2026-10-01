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
const camera_mode = @import("internal/camera_mode.zig");
const clocks = @import("internal/clocks.zig");
const clock_profile = @import("internal/clock_profile.zig");
const clock_types = @import("internal/clock_types.zig");
const console_stream = @import("internal/console_stream.zig");
const dualcore = @import("internal/dualcore.zig");
const ethernet = @import("internal/ethernet.zig");
const hal = @import("internal/hal.zig");
const stream = @import("internal/stream.zig");
const uart_console = @import("internal/uart_console.zig");
const usb_port = @import("internal/usb_port.zig");
const vocab = @import("internal/vocab.zig");

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

export fn ra8_board_clocks_init(out_rates: ?*hal.ClockRates) u32 {
    return clocks.init(out_rates);
}

export fn ra8_board_uart_console_init(baud: u32) u32 {
    return uart_console.init(baud);
}

// The pointer-and-length unwrapping stays here, in the C shape, so the console
// itself speaks slices. Order matters and matches the header: an empty write
// succeeds whatever `data` is, and only then does a null pointer fail.
export fn ra8_board_uart_console_write(data: ?[*]const u8, len: usize) u32 {
    if (len == 0) return vocab.Err.ok;
    const bytes = data orelse return vocab.Err.invalid_arg;
    return uart_console.write(bytes[0..len]);
}

export fn ra8_board_uart_console_read(out: ?[*]u8, cap: usize, out_len: ?*usize) u32 {
    const filled = out_len orelse return vocab.Err.invalid_arg;
    filled.* = 0;
    if (cap == 0) return vocab.Err.ok;
    const buf = out orelse return vocab.Err.invalid_arg;
    const result = uart_console.read(buf[0..cap]);
    filled.* = result.filled;
    return result.err;
}

export fn ra8_board_uart_console_flush() u32 {
    return uart_console.flush();
}

export fn priv_ra8_board_uart_console_is_up() bool {
    return uart_console.isUp();
}

export fn ra8_board_ethernet_init() u32 {
    return ethernet.init();
}

export fn priv_ra8_board_eth_eswm_bring_up(out_eswclk_hz: ?*u32) u32 {
    const out = out_eswclk_hz orelse return vocab.Err.null_ptr;
    return ethernet.eswmBringUp(out);
}

export fn priv_ra8_board_eth_etha_to_config() u32 {
    return ethernet.ethaToConfig();
}

export fn ra8_board_camera_get_ceu_config(
    mode: u8,
    frame_bytes_max: u32,
    out_config: ?*camera_mode.BoardConfig,
) u32 {
    const out = out_config orelse return vocab.Err.null_ptr;
    return camera_mode.get(mode, frame_bytes_max, out);
}
