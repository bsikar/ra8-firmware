//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two sequences the driver offers, and the controller lifecycle both
//! share. Enumerate, address, configure, download, then either read the
//! image back or let the device commit it.

const Err = @import("err").Err;
const attach = @import("attach");
const control = @import("control");
const download = @import("download");
const hal = @import("hal");
const proto = @import("proto");
const verify = @import("verify");

/// What a sequence establishes about the device it drove.
pub const Report = struct {
    /// Enumerated `idProduct`, once the descriptor has been read.
    product_id: u32 = 0,
    /// Upload blocks that byte-matched. Always zero for `.program`.
    blocks_ok: u32 = 0,
    /// First differing upload block, if one differed.
    mismatch: ?u32 = null,
};

/// Which sequence to run.
pub const Mode = enum {
    /// Download, abort, then upload and byte-compare. The self-test flow.
    round_trip,
    /// Download, then end-of-download so the device commits. The flash flow.
    program,
};

/// Bring the device from powered to configured, filling in `product_id`.
fn reachConfigured(
    comptime H: type,
    speed: hal.Speed,
    report: *Report,
) Err {
    var desc: [proto.DeviceDescriptor.len]u8 = @splat(0);
    const found = attach.hunt(H, speed, &desc);
    if (!found.isOk()) return found;
    report.product_id = control.productId(&desc);

    const addressed = control.setAddress(H, speed);
    if (!addressed.isOk()) return addressed;
    return control.setConfiguration(H, speed);
}

/// Run one sequence end to end, with the controller already initialized.
pub fn run(
    comptime H: type,
    speed: hal.Speed,
    image: []const u8,
    mode: Mode,
    report: *Report,
) Err {
    const configured = reachConfigured(H, speed, report);
    if (!configured.isOk()) return configured;

    switch (mode) {
        .round_trip => {
            const sent = download.run(H, speed, image, .abort);
            if (!sent.isOk()) return sent;
            var outcome = verify.Outcome{};
            const compared = verify.uploadAndCompare(H, speed, image, &outcome);
            report.blocks_ok = outcome.blocks_ok;
            report.mismatch = outcome.mismatch;
            return compared;
        },
        .program => return download.run(H, speed, image, .manifest),
    }
}

/// Initialize the controller, run `mode`, and tear the controller back down
/// if anything failed.
///
/// A successful run leaves the controller up: the caller still has a device
/// enumerated on it, and on the program path that device is mid-commit.
pub fn drive(
    comptime H: type,
    speed: hal.Speed,
    image: []const u8,
    mode: Mode,
    report: *Report,
) Err {
    const started = H.init(speed);
    if (!started.isOk()) return started;
    const err = run(H, speed, image, mode, report);
    if (!err.isOk()) _ = H.deinit(speed);
    return err;
}
