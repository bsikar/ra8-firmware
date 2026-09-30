//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_dfu/inc/ra8_dfu_host.h`: the polled on-board
//! USB-DFU host driver. The sequence lives in `internal/session.zig`; this
//! file owns the mirrored result record, the argument guards, and the one
//! place the real controller is named.
//!
//! Firmware-only by construction, exactly as the C was: the only binding
//! wired here is `hal.Hardware`, whose `extern`s are the `ra8_usb_host_*`
//! registers.

const std = @import("std");
const Err = @import("err").Err;
const hal = @import("hal");
const proto = @import("proto");
const session = @import("session");

/// Mirrors `ra8_dfu_host_result_t`.
pub const Result = extern struct {
    pid: u32,
    blocks_ok: u32,
    mismatch: u32,
    last_err: u16,
};

comptime {
    std.debug.assert(@offsetOf(Result, "pid") == 0);
    std.debug.assert(@offsetOf(Result, "blocks_ok") == 4);
    std.debug.assert(@offsetOf(Result, "mismatch") == 8);
    std.debug.assert(@offsetOf(Result, "last_err") == 12);
    std.debug.assert(@alignOf(Result) == 4);
}

/// `mismatch` when no block differed. The C's `k_rdh_mismatch_none`.
const mismatch_none: u32 = 0xFFFF_FFFF;

/// Reset `out` to the "nothing attempted yet" record.
fn clear(out: *Result) void {
    out.* = .{
        .pid = 0,
        .blocks_ok = 0,
        .mismatch = mismatch_none,
        .last_err = Err.ok.raw(),
    };
}

/// Fold a sequence report back into the C record.
fn absorb(out: *Result, report: session.Report, err: Err) void {
    out.pid = report.product_id;
    out.blocks_ok = report.blocks_ok;
    out.mismatch = report.mismatch orelse mismatch_none;
    out.last_err = err.raw();
}

/// Guard the arguments, then drive `mode`.
///
/// The image has to be a whole number of DFU blocks: the wire has no way to
/// send a short final block that the device would accept, so a ragged length
/// is a caller error rather than something to round.
fn entry(mode: session.Mode, host_speed: hal.Speed, img: ?[*]const u8, img_len: u32, result: ?*Result) u16 {
    const out = result orelse return Err.null_ptr.raw();
    const image_ptr = img orelse {
        clear(out);
        return Err.null_ptr.raw();
    };
    clear(out);

    if (img_len == 0 or (img_len % proto.Session.transfer_size) != 0) {
        out.last_err = Err.invalid_arg.raw();
        return Err.invalid_arg.raw();
    }

    var report = session.Report{};
    const image = image_ptr[0..img_len];
    const err = session.drive(hal.Hardware, host_speed, image, mode, &report);
    absorb(out, report, err);
    return err.raw();
}

/// Download the image, then read it back and byte-compare it.
pub export fn ra8_dfu_host_run(
    host_speed: hal.Speed,
    img: ?[*]const u8,
    img_len: u32,
    out: ?*Result,
) callconv(.c) u16 {
    return entry(.round_trip, host_speed, img, img_len, out);
}

/// Download the image and let the device commit it.
pub export fn ra8_dfu_host_program(
    host_speed: hal.Speed,
    img: ?[*]const u8,
    img_len: u32,
    out: ?*Result,
) callconv(.c) u16 {
    return entry(.program, host_speed, img, img_len, out);
}
