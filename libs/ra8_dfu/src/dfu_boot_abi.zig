//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for the pure boot logic. Keeps the five `ra8_dfu_*` symbols
//! `ra8_dfu.h` declares at the ABI the C bootloader and
//! `tests/misc/src/test_ra8_dfu_boot.c` already call, and does nothing else:
//! the decisions live in `internal/`.

const crc32 = @import("crc32");
const image = @import("image");
const slot = @import("slot");

export fn ra8_dfu_crc32(data: ?[*]const u8, len: u32) u32 {
    const bytes = if (data) |ptr| ptr[0..len] else &[_]u8{};
    return crc32.compute(bytes);
}

export fn ra8_dfu_hdr_valid(hdr: ?*const image.Header, computed_crc: u32) bool {
    const header = hdr orelse return false;
    return image.headerValid(header, computed_crc);
}

export fn ra8_dfu_run_target_valid(entry: u32, img_len: u32) bool {
    return image.runTargetValid(entry, img_len);
}

export fn ra8_dfu_select_slot(a_valid: bool, a_seq: u32, b_valid: bool, b_seq: u32) u8 {
    return @intFromEnum(slot.select(
        .{ .valid = a_valid, .seq = a_seq },
        .{ .valid = b_valid, .seq = b_seq },
    ));
}

export fn ra8_dfu_boot_decide(
    dfu_trigger: bool,
    a_valid: bool,
    a_seq: u32,
    b_valid: bool,
    b_seq: u32,
) u8 {
    return @intFromEnum(slot.decide(
        dfu_trigger,
        .{ .valid = a_valid, .seq = a_seq },
        .{ .valid = b_valid, .seq = b_seq },
    ));
}
