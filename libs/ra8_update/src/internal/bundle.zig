//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure parser, bounds/alignment/overlap validator, and serializer helpers for
//! the signed N-image `"RA8U"` update bundle format (`inc/ra8_bundle.h`).

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");

pub const magic: u32 = 0x5241_3855; // "RA8U"
pub const format_version: u16 = 1;
pub const header_bytes: u16 = 32;
pub const desc_bytes: u32 = 64;
pub const sig_slot_bytes: u32 = 64;
pub const page_align: u32 = 32;
pub const max_images: u16 = 4;

/// `ra8_bundle_target_t`.
pub const Target = enum(u32) {
    ra8_app = 1,
    esp32_c6 = 2,
    display = 3,

    pub fn fromRaw(raw: u32) ?Target {
        return switch (raw) {
            1 => .ra8_app,
            2 => .esp32_c6,
            3 => .display,
            else => null,
        };
    }
};

/// `ra8_bundle_payload_fmt_t`.
pub const PayloadFmt = enum(u16) {
    raw = 0,
    rot1_img = 1,
    c6_img = 2,

    pub fn fromRaw(raw: u16) ?PayloadFmt {
        return switch (raw) {
            0 => .raw,
            1 => .rot1_img,
            2 => .c6_img,
            else => null,
        };
    }
};

/// `ra8_bundle_hdr_t` (32 bytes, little-endian on wire).
pub const Header = extern struct {
    magic: u32,
    format_version: u16,
    header_bytes: u16,
    bundle_version: u32,
    image_count: u16,
    flags: u16,
    total_bytes: u32,
    payloads_offset: u32,
    sig_len: u32,
    reserved: u32,
};

/// `ra8_bundle_image_desc_t` (64 bytes, little-endian on wire).
pub const ImageDesc = extern struct {
    target: u32,
    format: u16,
    flags: u16,
    image_version: u32,
    compat_proto_ver: u32,
    offset: u32,
    size: u32,
    reserved0: u32,
    reserved1: u32,
    sha256: [pl.sha256_bytes]u8,
};

comptime {
    std.debug.assert(@sizeOf(Header) == 32);
    std.debug.assert(@offsetOf(Header, "magic") == 0);
    std.debug.assert(@offsetOf(Header, "format_version") == 4);
    std.debug.assert(@offsetOf(Header, "header_bytes") == 6);
    std.debug.assert(@offsetOf(Header, "bundle_version") == 8);
    std.debug.assert(@offsetOf(Header, "image_count") == 12);
    std.debug.assert(@offsetOf(Header, "flags") == 14);
    std.debug.assert(@offsetOf(Header, "total_bytes") == 16);
    std.debug.assert(@offsetOf(Header, "payloads_offset") == 20);
    std.debug.assert(@offsetOf(Header, "sig_len") == 24);
    std.debug.assert(@offsetOf(Header, "reserved") == 28);

    std.debug.assert(@sizeOf(ImageDesc) == 64);
    std.debug.assert(@offsetOf(ImageDesc, "target") == 0);
    std.debug.assert(@offsetOf(ImageDesc, "format") == 4);
    std.debug.assert(@offsetOf(ImageDesc, "flags") == 6);
    std.debug.assert(@offsetOf(ImageDesc, "image_version") == 8);
    std.debug.assert(@offsetOf(ImageDesc, "compat_proto_ver") == 12);
    std.debug.assert(@offsetOf(ImageDesc, "offset") == 16);
    std.debug.assert(@offsetOf(ImageDesc, "size") == 20);
    std.debug.assert(@offsetOf(ImageDesc, "reserved0") == 24);
    std.debug.assert(@offsetOf(ImageDesc, "reserved1") == 28);
    std.debug.assert(@offsetOf(ImageDesc, "sha256") == 32);
}

fn readU16(bytes: []const u8, off: usize) u16 {
    return std.mem.readInt(u16, bytes[off..][0..2], .little);
}

fn readU32(bytes: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, bytes[off..][0..4], .little);
}

/// Byte offset of the ECDSA-P256 raw signature slot inside the container.
pub fn sigOffset(hdr: *const Header) u32 {
    return @as(u32, header_bytes) + @as(u32, hdr.image_count) * desc_bytes;
}

/// Expected `payloads_offset` given a validated `image_count`.
pub fn expectedPayloadsOffset(count: u16) u32 {
    return @as(u32, header_bytes) + @as(u32, count) * desc_bytes + sig_slot_bytes;
}

/// Parse and validate a 32-byte bundle header without reading descriptors.
pub fn parseHeader(raw: []const u8, max_bytes: u32) error{ InvalidArg, InvalidSize }!Header {
    if (raw.len < header_bytes) return error.InvalidSize;
    const hdr = Header{
        .magic = readU32(raw, 0),
        .format_version = readU16(raw, 4),
        .header_bytes = readU16(raw, 6),
        .bundle_version = readU32(raw, 8),
        .image_count = readU16(raw, 12),
        .flags = readU16(raw, 14),
        .total_bytes = readU32(raw, 16),
        .payloads_offset = readU32(raw, 20),
        .sig_len = readU32(raw, 24),
        .reserved = readU32(raw, 28),
    };

    if (hdr.magic != magic) return error.InvalidArg;
    if (hdr.format_version != format_version) return error.InvalidArg;
    if (hdr.header_bytes != header_bytes) return error.InvalidArg;
    if (hdr.image_count == 0 or hdr.image_count > max_images) return error.InvalidArg;
    if (hdr.flags != 0 or hdr.reserved != 0) return error.InvalidArg;
    if (hdr.sig_len == 0 or hdr.sig_len > sig_slot_bytes) return error.InvalidSize;

    const exp_pay = expectedPayloadsOffset(hdr.image_count);
    if (hdr.payloads_offset != exp_pay) return error.InvalidSize;
    if (hdr.total_bytes < exp_pay or hdr.total_bytes > max_bytes) return error.InvalidSize;
    return hdr;
}

/// Decode one 64-byte image descriptor.
pub fn parseDesc(raw: []const u8) error{InvalidSize}!ImageDesc {
    if (raw.len < desc_bytes) return error.InvalidSize;
    var desc = ImageDesc{
        .target = readU32(raw, 0),
        .format = readU16(raw, 4),
        .flags = readU16(raw, 6),
        .image_version = readU32(raw, 8),
        .compat_proto_ver = readU32(raw, 12),
        .offset = readU32(raw, 16),
        .size = readU32(raw, 20),
        .reserved0 = readU32(raw, 24),
        .reserved1 = readU32(raw, 28),
        .sha256 = undefined,
    };
    @memcpy(&desc.sha256, raw[32..64]);
    return desc;
}

/// Parse and structurally validate all `hdr.image_count` descriptors.
pub fn parseAndValidateDescs(
    hdr: *const Header,
    raw_descs: []const u8,
    out_descs: []ImageDesc,
) error{ InvalidArg, InvalidSize }!void {
    const count: usize = @intCast(hdr.image_count);
    const need_bytes = count * desc_bytes;
    if (raw_descs.len < need_bytes or out_descs.len < count) return error.InvalidSize;

    var prev_aligned_end: u64 = hdr.payloads_offset;
    var seen_targets: u32 = 0;

    for (0..count) |idx| {
        const d = try parseDesc(raw_descs[idx * desc_bytes ..][0..desc_bytes]);
        if (Target.fromRaw(d.target) == null) return error.InvalidArg;
        if (PayloadFmt.fromRaw(d.format) == null) return error.InvalidArg;
        if (d.reserved0 != 0 or d.reserved1 != 0) return error.InvalidArg;
        if (d.size == 0) return error.InvalidSize;

        const target_bit = @as(u32, 1) << @as(u5, @intCast(d.target));
        if ((seen_targets & target_bit) != 0) return error.InvalidArg;
        seen_targets |= target_bit;

        if ((d.offset % page_align) != 0) return error.InvalidArg;
        if (@as(u64, d.offset) < prev_aligned_end) return error.InvalidArg;

        const end: u64 = @as(u64, d.offset) + @as(u64, d.size);
        if (end > @as(u64, hdr.total_bytes)) return error.InvalidSize;

        prev_aligned_end = std.mem.alignForward(u64, end, page_align);
        out_descs[idx] = d;
    }
}

/// Locate a validated descriptor matching `target`, if present.
pub fn findTarget(descs: []const ImageDesc, target: Target) ?ImageDesc {
    for (descs) |d| {
        if (d.target == @backingInt(target)) return d;
    }
    return null;
}

/// Check companion link protocol compatibility (`0` = no version constraint).
pub fn compatProtoAllowed(desc: *const ImageDesc, required_proto: u32) bool {
    if (required_proto == 0 or desc.compat_proto_ver == 0) return true;
    return desc.compat_proto_ver == required_proto;
}

/// Map internal parse error to canonical `ra8_err_t` value.
pub fn mapParseErr(e: error{ InvalidArg, InvalidSize }) u16 {
    return switch (e) {
        error.InvalidArg => Err.invalid_arg.raw(),
        error.InvalidSize => Err.invalid_size.raw(),
    };
}
