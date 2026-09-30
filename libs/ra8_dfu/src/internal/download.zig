//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DFU_DNLOAD phase, and the two ways it ends.
//!
//! `abort` leaves the device enumerable so the image can be read back for
//! comparison; `manifest` sends the zero-length end-of-download that makes
//! the device commit the slot, which is what a real flash does.

const Err = @import("err").Err;
const hal = @import("hal");
const proto = @import("proto");
const status = @import("status");

const block_bytes = proto.Session.transfer_size;

/// How a download is closed out.
pub const Ending = enum {
    /// DFU_ABORT back to dfuIDLE: the device stays addressable.
    abort,
    /// Zero-length DFU_DNLOAD: the device commits and leaves dfuIDLE.
    manifest,
};

/// One DFU_DNLOAD block, then wait for the device to accept it.
fn sendBlock(comptime H: type, speed: hal.Speed, block: u16, data: []u8) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.class_if_out,
        .b_request = proto.Request.dfu_dnload,
        .w_value = block,
        .w_index = proto.Session.interface,
        .w_length = @intCast(data.len),
    };
    const err = H.controlXfer(speed, &setup, data, @intCast(data.len), null);
    if (!err.isOk()) return err;
    return status.waitFor(H, speed, proto.State.dfu_dnload_idle);
}

/// How many whole blocks an image of `len` bytes is.
pub fn blockCount(len: u32) u16 {
    return @intCast(len / block_bytes);
}

/// Send every block of `image`, then close the download the way `ending` says.
///
/// `image.len` is a whole number of blocks by the time this is reached; the
/// membrane rejects anything else, so no partial trailing block exists to
/// pad here.
pub fn run(comptime H: type, speed: hal.Speed, image: []const u8, ending: Ending) Err {
    const blocks = blockCount(@intCast(image.len));
    var block: u16 = 0;
    while (block < blocks) : (block += 1) {
        var buffer: [block_bytes]u8 = @splat(0);
        const offset = @as(usize, block) * block_bytes;
        @memcpy(&buffer, image[offset..][0..block_bytes]);
        const err = sendBlock(H, speed, block, &buffer);
        if (!err.isOk()) return err;
    }
    return switch (ending) {
        .abort => abort(H, speed),
        .manifest => endOfDownload(H, speed, blocks),
    };
}

/// DFU_ABORT, then wait for dfuIDLE.
fn abort(comptime H: type, speed: hal.Speed) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.class_if_out,
        .b_request = proto.Request.dfu_abort,
        .w_value = 0,
        .w_index = proto.Session.interface,
        .w_length = 0,
    };
    const err = H.controlXfer(speed, &setup, null, 0, null);
    if (!err.isOk()) return err;
    return status.waitFor(H, speed, proto.State.dfu_idle);
}

/// Zero-length DFU_DNLOAD at the block past the last.
///
/// No GETSTATUS follows: the device half runs on the same chip as the caller,
/// which polls its own commit flag rather than asking over the wire.
fn endOfDownload(comptime H: type, speed: hal.Speed, blocks: u16) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.class_if_out,
        .b_request = proto.Request.dfu_dnload,
        .w_value = blocks,
        .w_index = proto.Session.interface,
        .w_length = 0,
    };
    return H.controlXfer(speed, &setup, null, 0, null);
}
