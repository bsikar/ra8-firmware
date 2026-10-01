//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Striped driver for the baseline decoder.
//!
//! Decodes a JPEG the caller feeds in through a pull callback, emitting one
//! MCU row of pixels at a time, so peak memory is a sliding window plus one
//! stripe rather than the whole frame. `whole.zig` solves the same problem
//! when the image and its output both fit in memory.
//!
//! The three callbacks are C function pointers, because every caller of this
//! path is C. Their error codes are opaque here: a callback failure is
//! reported as `Error.Callback` and the code it returned is left in
//! `State.callback_err` for the membrane to hand back unchanged.

const bitreader = @import("bitreader");
const dec_ctx = @import("dec_ctx");
const dispatch = @import("dispatch");
const mcu = @import("mcu");
const spec = @import("spec");
const ycc = @import("ycc");

const Ctx = dec_ctx.Ctx;

/// Failures of the striped driver itself, plus the pass-through case.
pub const Error = dec_ctx.Error || error{
    /// A caller-supplied callback returned a non-zero `ra8_err_t`, left in
    /// `State.callback_err`.
    Callback,
};

/// Bounds and shapes fixed by the public header's contract.
pub const Limit = struct {
    /// Smallest window the caller may supply, bytes.
    pub const min_window: u32 = 131072;
    /// Slide the window forward once fewer than this many bytes are unread.
    pub const scan_margin: u32 = 32768;
    /// Marker-segment budget before a stream is called hostile.
    pub const max_markers: u16 = 1024;
    /// SOI is two bytes with no payload.
    pub const soi_bytes: u32 = 2;
    /// Output channels per pixel.
    pub const gray_channels: u8 = 1;
    pub const rgb_channels: u8 = 3;
};

/// Sequential byte source: fill `buf` with up to `cap` bytes and report how
/// many through `got`. Zero means end of stream.
pub const PullFn = *const fn (
    ctx: ?*anyopaque,
    buf: [*]u8,
    cap: usize,
    got: *usize,
) callconv(.c) u16;

/// Geometry handshake, called once the frame header is known: the consumer
/// hands back the stripe buffer it wants written.
pub const GeomFn = *const fn (
    ctx: ?*anyopaque,
    width: u16,
    height: u16,
    channels: u8,
    stripe_rows: u16,
    out_stripe: *?[*]u8,
    out_stripe_cap: *u32,
) callconv(.c) u16;

/// Stripe sink, called once per MCU row with the rows it covers.
pub const RowsFn = *const fn (
    ctx: ?*anyopaque,
    px: [*]const u8,
    width: u16,
    y0: u16,
    nrows: u16,
    channels: u8,
) callconv(.c) u16;

/// Everything one striped decode needs. Held by the membrane so the decode
/// allocates nothing.
pub const State = struct {
    pull: PullFn,
    pull_ctx: ?*anyopaque,
    window: []u8,
    win_len: usize = 0,
    eof: bool = false,

    on_rows: RowsFn,
    cb_ctx: ?*anyopaque,

    stripe: []u8 = &.{},
    channels: u8 = 0,
    mcu_w: u16 = 0,
    mcu_h: u16 = 0,
    mcus_x: u16 = 0,
    mcus_y: u16 = 0,

    /// The code the failing callback returned, when `Error.Callback` is
    /// reported.
    callback_err: u16 = 0,

    dec: Ctx = .{},

    /// Run a callback and turn a non-zero code into `Error.Callback`.
    fn check(self: *State, code: u16) Error!void {
        if (code == 0) return;
        self.callback_err = code;
        return Error.Callback;
    }

    /// Pull until the window is full or the source is spent.
    fn refill(self: *State) Error!void {
        while (!self.eof and self.win_len < self.window.len) {
            var got: usize = 0;
            const code = self.pull(
                self.pull_ctx,
                self.window[self.win_len..].ptr,
                self.window.len - self.win_len,
                &got,
            );
            try self.check(code);

            if (got == 0) {
                self.eof = true;
            } else {
                self.win_len += got;
            }
        }
    }

    /// Drop `consumed` bytes off the front of the window and top it back up.
    fn slide(self: *State, consumed: usize) Error!void {
        if (consumed > 0) {
            std.mem.copyForwards(
                u8,
                self.window[0 .. self.win_len - consumed],
                self.window[consumed..self.win_len],
            );
            self.win_len -= consumed;
        }
        return self.refill();
    }
};

const std = @import("std");

/// Work out the MCU geometry from the frame header, then ask the consumer for
/// a stripe buffer big enough for one MCU row.
fn bindGeometry(st: *State, on_geom: GeomFn) Error!void {
    const d = &st.dec;

    st.channels = if (d.ncomp == 1) Limit.gray_channels else Limit.rgb_channels;
    st.mcu_w = spec.Block.dim * d.hmax;
    st.mcu_h = spec.Block.dim * d.vmax;
    st.mcus_x = (d.width + st.mcu_w - 1) / st.mcu_w;
    st.mcus_y = (d.height + st.mcu_h - 1) / st.mcu_h;

    var stripe: ?[*]u8 = null;
    var stripe_cap: u32 = 0;
    const code = on_geom(
        st.cb_ctx,
        d.width,
        d.height,
        st.channels,
        st.mcu_h,
        &stripe,
        &stripe_cap,
    );
    try st.check(code);

    const base = stripe orelse return dec_ctx.Error.Protocol;
    const needed = @as(usize, d.width) * @as(usize, st.mcu_h) * @as(usize, st.channels);
    if (stripe_cap < needed) return dec_ctx.Error.InvalidSize;

    st.stripe = base[0..stripe_cap];
}

/// Read markers out of the window until the scan starts, sliding the window
/// forward after each one so a long header chain cannot overrun it.
fn parseMarkers(st: *State, on_geom: GeomFn) Error!usize {
    var got_sof = false;
    var geom_done = false;

    for (0..Limit.max_markers) |_| {
        if (st.win_len == 0) return dec_ctx.Error.Protocol;

        st.dec.src = st.window[0..st.win_len];
        st.dec.cursor = 0;

        var action = dispatch.Action.cont;
        try dispatch.step(&st.dec, &got_sof, &action);

        if (got_sof and !geom_done) {
            try bindGeometry(st, on_geom);
            geom_done = true;
        }

        switch (action) {
            .scan => return st.dec.cursor,
            // EOI with no scan behind it means the stream carried no image.
            .eoi => return dec_ctx.Error.Protocol,
            .cont => try st.slide(st.dec.cursor),
        }
    }
    // A stream that spends the whole budget on headers is hostile.
    return dec_ctx.Error.Protocol;
}

/// Write one decoded MCU into the stripe, stopping at the right-hand edge.
fn emitMcu(st: *State, tiles: *const mcu.Tiles, mx: u16, rows: u16) void {
    const d = &st.dec;
    const stride = @as(usize, d.width) * @as(usize, st.channels);

    for (0..rows) |r| {
        for (0..st.mcu_w) |c| {
            const px = (@as(usize, mx) * @as(usize, st.mcu_w)) + c;
            if (px >= d.width) break;

            const y: i32 = tiles.luma[(r * @as(usize, st.mcu_w)) + c];

            if (st.channels == Limit.gray_channels) {
                st.stripe[(r * stride) + px] = @intCast(y);
                continue;
            }

            const cx = c / d.hmax;
            const cy = r / d.vmax;
            const cb: i32 = tiles.cb[(cy * spec.Block.dim) + cx];
            const cr: i32 = tiles.cr[(cy * spec.Block.dim) + cx];

            const rgb = ycc.toRgb(y, cb, cr);
            const idx = (r * stride) + (px * @as(usize, st.channels));
            st.stripe[idx] = rgb[0];
            st.stripe[idx + 1] = rgb[1];
            st.stripe[idx + 2] = rgb[2];
        }
    }
}

/// Slide the window forward when the entropy reader is close to running off
/// the end of it, rebasing the reader onto the moved bytes.
fn keepScanMargin(st: *State, br: *bitreader.BitReader) Error!void {
    if (st.eof or (st.win_len - br.pos) >= Limit.scan_margin) return;

    try st.slide(br.pos);
    br.pos = 0;
    br.len = @intCast(st.win_len);
}

/// Decode the scan, emitting one stripe per MCU row. The last row is short
/// when the image height is not a whole number of MCUs.
fn decodeScan(st: *State, scan_pos: usize) Error!void {
    const d = &st.dec;

    var br = bitreader.BitReader{
        .buf = st.window.ptr,
        .len = @intCast(st.win_len),
        .pos = @intCast(scan_pos),
        .acc = 0,
        .nbits = 0,
        .had_eoi = 0,
    };
    for (&d.comps) |*comp| comp.dc_pred = 0;

    var tiles = mcu.Tiles{};

    var my: u16 = 0;
    while (my < st.mcus_y) : (my += 1) {
        const y0 = @as(usize, my) * @as(usize, st.mcu_h);
        const left = @as(usize, d.height) - y0;
        const rows: u16 = @intCast(@min(left, @as(usize, st.mcu_h)));

        var mx: u16 = 0;
        while (mx < st.mcus_x) : (mx += 1) {
            try keepScanMargin(st, &br);
            try mcu.decode(d, &br, &tiles, st.mcu_w);
            emitMcu(st, &tiles, mx, rows);
        }

        const code = st.on_rows(
            st.cb_ctx,
            st.stripe.ptr,
            d.width,
            @intCast(y0),
            rows,
            st.channels,
        );
        try st.check(code);
    }
}

/// Fill the window, check the SOI, and leave the cursor on the first marker.
fn begin(st: *State) Error!void {
    try st.refill();

    if (st.win_len < Limit.soi_bytes) return dec_ctx.Error.InvalidSize;
    if (((@as(u16, st.window[0]) << 8) | st.window[1]) != spec.Marker.soi) {
        return dec_ctx.Error.Protocol;
    }
    return st.slide(Limit.soi_bytes);
}

/// Decode a baseline JPEG from `st.pull`, emitting stripes through
/// `st.on_rows`. `on_geom` is called once, between the frame header and the
/// first stripe.
pub fn decode(st: *State, on_geom: GeomFn) Error!void {
    st.dec.reset();
    try begin(st);
    const scan_pos = try parseMarkers(st, on_geom);
    return decodeScan(st, scan_pos);
}
