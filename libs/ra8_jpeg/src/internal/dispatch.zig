//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Marker dispatch for the baseline decoder.
//!
//! The one place that decides what a marker code means. Both drivers share it,
//! so the whole-buffer and striped paths cannot disagree about which frame
//! types are supported or when a scan begins.

const dec_ctx = @import("dec_ctx");
const segments = @import("segments");
const spec = @import("spec");

const Ctx = dec_ctx.Ctx;
const Error = dec_ctx.Error;

/// What the driver should do after a marker has been handled.
pub const Action = enum {
    /// Keep reading markers.
    cont,
    /// SOS was read; the cursor is on the entropy-coded data.
    scan,
    /// EOI was read before any scan.
    eoi,
};

/// Marker codes dispatch treats specially beyond the segment parsers.
const Code = struct {
    pub const sof1: u16 = 0xFFC1;
    pub const sof_hi: u16 = 0xFFCF;
    pub const jpg: u16 = 0xFFC8;
    pub const rst0: u16 = 0xFFD0;
    pub const rst7: u16 = 0xFFD7;
    /// A marker is the fill byte in the high half plus its code in the low.
    pub const high: u16 = 0xFF00;
};

/// Everything after the frame-type checks: the segments with parsers, the
/// restart markers that carry no payload, and the rest skipped by length.
fn tail(d: *Ctx, marker: u16, got_sof: bool, action: *Action) Error!void {
    switch (marker) {
        spec.Marker.dqt => return segments.parseDqt(d),
        spec.Marker.dht => return segments.parseDht(d),
        spec.Marker.sos => {
            // A scan with no frame header ahead of it has no geometry.
            if (!got_sof) return Error.Protocol;
            try segments.parseSos(d);
            action.* = .scan;
        },
        spec.Marker.eoi => action.* = .eoi,
        Code.rst0...Code.rst7 => {},
        else => return segments.skip(d),
    }
}

/// Read the marker at the cursor and handle it. Runs of `0xFF` fill bytes
/// ahead of the code are skipped, as T.81 allows.
pub fn step(d: *Ctx, got_sof: *bool, action: *Action) Error!void {
    action.* = .cont;

    if (d.src[d.cursor] != spec.Marker.stuff_trigger) return Error.Protocol;
    while (d.cursor < d.src.len and d.src[d.cursor] == spec.Marker.stuff_trigger) {
        d.cursor += 1;
    }
    if (d.cursor >= d.src.len) return Error.Protocol;

    const low = d.src[d.cursor];
    d.cursor += 1;
    const marker = Code.high | @as(u16, low);

    if (marker == spec.Marker.sof0) {
        try segments.parseSof0(d);
        got_sof.* = true;
        return;
    }

    // Any other SOF code is a frame type this decoder does not implement.
    // DHT and JPG share the range but are not frame headers.
    if (marker >= Code.sof1 and marker <= Code.sof_hi and
        marker != spec.Marker.dht and marker != Code.jpg)
    {
        return Error.Unsupported;
    }

    return tail(d, marker, got_sof.*, action);
}
