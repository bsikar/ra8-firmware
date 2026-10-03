//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the poll pump: `priv_c6link_pump`, which
//! `src/ra8_c6link_internal.h` declares and `ra8_c6link.c` calls.
//!
//! `ra8_c6link_t` comes from the public header through translate-c, so the
//! transport, the two frame buffers and the staged-payload fields are read at
//! the offsets the C compiler gives them. Routing a data frame stays in C
//! (`priv_c6link_dispatch`) until the dispatcher ports.

const pump = @import("internal/pump.zig");
const frame = @import("internal/frame.zig");
const Err = @import("abi_err.zig");

/// The public `ra8_c6link.h` view of the handle and its counters.
pub const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignas", "_Alignas");
    @cInclude("stdbool.h");
    @cInclude("ra8_c6link.h");
});

/// `ra8_c6link_rx_view_t`: where a classified frame's payload is.
pub const RxView = extern struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};

extern fn priv_c6link_dispatch(link: *c.ra8_c6link_t, view: *const RxView) callconv(.c) bool;

comptime {
    if (@sizeOf(pump.Stats) != @sizeOf(c.ra8_c6link_stats_t)) @compileError("ra8_c6link_stats_t size drifted");
    for (@typeInfo(pump.Stats).@"struct".fields) |field| {
        if (@offsetOf(pump.Stats, field.name) != @offsetOf(c.ra8_c6link_stats_t, field.name)) {
            @compileError("ra8_c6link_stats_t." ++ field.name ++ " offset drifted");
        }
    }
    if (frame.Frame.bytes != c.k_ra8_c6link_frame_bytes) @compileError("frame size drifted");
    if (pump.Timing.hs_wait_ms != c.k_ra8_c6link_hs_wait_ms) @compileError("hs_wait_ms drifted");
    if (pump.Timing.hs_poll_ms != c.k_ra8_c6link_hs_poll_ms) @compileError("hs_poll_ms drifted");
    if (pump.Timing.hs_giveup != c.k_ra8_c6link_hs_giveup) @compileError("hs_giveup drifted");
    if (pump.Timing.gap_ms != c.k_ra8_c6link_gap_ms) @compileError("gap_ms drifted");
    if (Err.spi_error != c.k_ra8_err_spi_error) @compileError("k_ra8_err_spi_error drifted");
    if (Err.hw_timeout != c.k_ra8_err_hw_timeout) @compileError("k_ra8_err_hw_timeout drifted");
}

/// The open handle seen as a pump port.
const LinkPort = struct {
    link: *c.ra8_c6link_t,

    pub fn handshakeActive(self: LinkPort) bool {
        const sample = self.link.transport.handshake_active orelse return false;
        return sample(self.link.transport.ctx);
    }

    pub fn delayMs(self: LinkPort, ms: u16) void {
        const delay = self.link.transport.delay_ms orelse return;
        delay(self.link.transport.ctx, ms);
    }

    pub fn tx(self: LinkPort) []u8 {
        return &self.link.tx;
    }

    pub fn rx(self: LinkPort) []const u8 {
        return &self.link.rx;
    }

    pub fn takeStaged(self: LinkPort) ?pump.Staged {
        if (self.link.tx_len == 0) return null;
        const staged: pump.Staged = .{ .if_type = self.link.tx_if, .len = self.link.tx_len };
        self.link.tx_len = 0;
        return staged;
    }

    pub fn transfer(self: LinkPort) bool {
        const clock = self.link.transport.transfer orelse return false;
        return clock(self.link.transport.ctx, &self.link.tx, &self.link.rx, frame.Frame.bytes) == c.k_ra8_ok;
    }

    pub fn dispatch(self: LinkPort, view: frame.View) bool {
        const out: RxView = .{ .offset = view.offset, .len = view.len, .if_type = view.if_type, .if_num = view.if_num };
        return priv_c6link_dispatch(self.link, &out);
    }
};

/// `priv_c6link_pump`: clock up to `max_transactions` transactions.
///
/// Publishes `stats` on the handle for the dispatcher while the run lasts and
/// clears it after. Returns `k_ra8_err_spi_error` when the transport faults and
/// `k_ra8_err_hw_timeout` when nothing was clocked at all.
pub export fn priv_c6link_pump(
    link: ?*c.ra8_c6link_t,
    max_transactions: u16,
    stats: ?*c.ra8_c6link_stats_t,
) callconv(.c) u16 {
    const handle = link orelse return Err.null_ptr;
    const counters = stats orelse return Err.null_ptr;
    handle.stats = counters;
    defer handle.stats = null;

    return switch (pump.run(LinkPort{ .link = handle }, max_transactions, @ptrCast(counters))) {
        .ok => Err.ok,
        .bus_fault => Err.spi_error,
        .timeout => Err.hw_timeout,
    };
}
