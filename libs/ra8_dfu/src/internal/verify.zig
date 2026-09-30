//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DFU_UPLOAD phase: read the image back a block at a time and compare
//! it against what was sent.

const Err = @import("err").Err;
const download = @import("download");
const hal = @import("hal");
const proto = @import("proto");

const block_bytes = proto.Session.transfer_size;

/// What a verify pass establishes.
pub const Outcome = struct {
    /// Blocks that byte-matched, counted from the start.
    blocks_ok: u32 = 0,
    /// First differing or short block, if the pass failed.
    mismatch: ?u32 = null,
};

/// Read `image` back over DFU_UPLOAD and compare block by block.
///
/// Stops at the first block that differs or comes back short, and names it,
/// because which block first diverged is the whole diagnostic value: a
/// count of failures further in says nothing a bisect would not.
pub fn uploadAndCompare(
    comptime H: type,
    speed: hal.Speed,
    image: []const u8,
    out: *Outcome,
) Err {
    const blocks = download.blockCount(@intCast(image.len));
    out.blocks_ok = 0;
    var block: u16 = 0;
    while (block < blocks) : (block += 1) {
        var got: [block_bytes]u8 = @splat(0);
        const setup = hal.Setup{
            .bm_request_type = proto.Bm.class_if_in,
            .b_request = proto.Request.dfu_upload,
            .w_value = block,
            .w_index = proto.Session.interface,
            .w_length = block_bytes,
        };
        var received: u16 = 0;
        const err = H.controlXfer(speed, &setup, &got, block_bytes, &received);
        if (!err.isOk()) return err;
        if (received != block_bytes) {
            out.mismatch = block;
            return .invalid_size;
        }
        const offset = @as(usize, block) * block_bytes;
        if (!eql(&got, image[offset..][0..block_bytes])) {
            out.mismatch = block;
            return .invalid_state;
        }
        out.blocks_ok += 1;
    }
    return .ok;
}

fn eql(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |left, right| {
        if (left != right) return false;
    }
    return true;
}
