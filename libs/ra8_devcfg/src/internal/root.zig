//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Hardware-agnostic core of the per-device configuration record: the record
//! schema constants, the IEEE 802.3 CRC-32, the explicit little-endian codec,
//! the four validation gates and the stale-slot selection rule.
//!
//! Nothing here touches a backing store or module state. The injected store
//! seam, the cache and the `ra8_err_t` mapping live in the ABI membrane, which
//! is what keeps every branch below drivable from a plain Zig test.

const std = @import("std");

/// Record geometry, magic and extra-MRAM placement (`ra8_devcfg_layout_t`).
pub const magic: u32 = 0x52413843; // "RA8C"
pub const schema_ver: u16 = 1;
pub const hdr_bytes: u32 = 32;
pub const body_bytes: u32 = 96;
pub const record_len: u32 = 128;
pub const slot_bytes: u32 = 192;
pub const sig_headroom: u32 = 64;
pub const page_bytes: u32 = 32;
pub const copy0_off: u32 = 0x00000040;
pub const copy1_off: u32 = 0x00000100;

/// Fixed byte lengths of the variable-width body fields
/// (`ra8_devcfg_field_len_t`).
pub const serial_len: usize = 16;
pub const panel_serial_len: usize = 12;
pub const panel_lut_len: usize = 8;
pub const touch_cal_len: usize = 36;
pub const hdr_rsvd_len: usize = 12;
pub const body_rsvd_len: usize = 10;

/// Byte offset of every field inside a serialised record (`ra8_devcfg_off_t`).
pub const off_magic: usize = 0;
pub const off_schema: usize = 4;
pub const off_reclen: usize = 6;
pub const off_seq: usize = 8;
pub const off_crc: usize = 12;
pub const off_flags: usize = 16;
pub const off_hdr_rsvd: usize = 20;
pub const off_serial: usize = 32;
pub const off_panel_serial: usize = 48;
pub const off_panel_lut: usize = 60;
pub const off_touch_cal: usize = 68;
pub const off_mfg_date: usize = 104;
pub const off_key_id: usize = 108;
pub const off_hw_rev: usize = 112;
pub const off_fixture: usize = 114;
pub const off_vcom: usize = 116;
pub const off_body_rsvd: usize = 118;

/// Plausible-range guard for the stored VCOM magnitude, millivolts
/// (`ra8_devcfg_vcom_range_t`).
pub const vcom_min_mv: u16 = 500;
pub const vcom_max_mv: u16 = 4000;

/// Per-record status flags stored in the header `flags` word
/// (`ra8_devcfg_flags_t`).
pub const flag_provisioned: u32 = 0x00000001;
pub const flag_vcom_valid: u32 = 0x00000002;
pub const flag_touch_valid: u32 = 0x00000004;

/// IEEE 802.3 CRC-32 parameters, shared with `ra8_epd_cal` and
/// `ra8_touch_cal` so every calibration record in the tree checks the same way.
const crc_init: u32 = 0xFFFFFFFF;
const crc_poly: u32 = 0xEDB88320;

/// Decoded per-unit payload, the 96-byte record body (`ra8_devcfg_body_t`).
pub const Body = extern struct {
    serial: [serial_len]u8 = @splat(0),
    panel_serial: [panel_serial_len]u8 = @splat(0),
    panel_lut_id: [panel_lut_len]u8 = @splat(0),
    touch_cal: [touch_cal_len]u8 = @splat(0),
    mfg_date: u32 = 0,
    device_key_id: u32 = 0,
    hw_rev: u16 = 0,
    fixture_id: u16 = 0,
    panel_vcom_mv: u16 = 0,
};

/// Decoded record: the body plus the header discriminators
/// (`ra8_devcfg_record_t`).
pub const Record = extern struct {
    body: Body = .{},
    flags: u32 = 0,
    seq: u32 = 0,
    schema_version: u16 = 0,
};

comptime {
    // The body is a C struct laid out by the compiler, not the on-storage
    // format; the codec below owns the byte order. These asserts pin the
    // in-memory shape the unchanged header promises its C callers.
    if (@offsetOf(Body, "panel_serial") != 16) @compileError("body panel_serial offset");
    if (@offsetOf(Body, "panel_lut_id") != 28) @compileError("body panel_lut_id offset");
    if (@offsetOf(Body, "touch_cal") != 36) @compileError("body touch_cal offset");
    if (@offsetOf(Body, "mfg_date") != 72) @compileError("body mfg_date offset");
    if (@offsetOf(Body, "device_key_id") != 76) @compileError("body device_key_id offset");
    if (@offsetOf(Body, "hw_rev") != 80) @compileError("body hw_rev offset");
    if (@offsetOf(Body, "fixture_id") != 82) @compileError("body fixture_id offset");
    if (@offsetOf(Body, "panel_vcom_mv") != 84) @compileError("body panel_vcom_mv offset");
    if (@sizeOf(Body) != 88) @compileError("body size");
    if (@offsetOf(Record, "flags") != 88) @compileError("record flags offset");
    if (@offsetOf(Record, "seq") != 92) @compileError("record seq offset");
    if (@offsetOf(Record, "schema_version") != 96) @compileError("record schema_version offset");
    if (@sizeOf(Record) != 100) @compileError("record size");

    // Layout invariants the header states in prose.
    if (copy1_off - copy0_off < slot_bytes) @compileError("copies overlap");
    if (hdr_bytes + body_bytes != record_len) @compileError("record_len != hdr + body");
    if (slot_bytes - record_len != sig_headroom) @compileError("signature headroom");
    if (vcom_min_mv == 0) @compileError("vcom_min_mv must reject 0");
    if (vcom_min_mv > vcom_max_mv) @compileError("vcom window inverted");
}

/// Compute the IEEE 802.3 CRC-32 of a byte span. Bit-banged reflected-
/// polynomial form, no lookup table, matching the C implementation exactly.
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = crc_init;
    for (data) |byte| {
        crc ^= byte;
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = 0 -% (crc & 1);
            crc = (crc >> 1) ^ (crc_poly & mask);
        }
    }
    return crc ^ crc_init;
}

/// Store a little-endian 16-bit value into a blob.
pub fn packLe16(dst: []u8, val: u16) void {
    std.mem.writeInt(u16, dst[0..2], val, .little);
}

/// Store a little-endian 32-bit value into a blob.
pub fn packLe32(dst: []u8, val: u32) void {
    std.mem.writeInt(u32, dst[0..4], val, .little);
}

/// Load a little-endian 16-bit value from a blob.
pub fn unpackLe16(src: []const u8) u16 {
    return std.mem.readInt(u16, src[0..2], .little);
}

/// Load a little-endian 32-bit value from a blob.
pub fn unpackLe32(src: []const u8) u32 {
    return std.mem.readInt(u32, src[0..4], .little);
}

/// Serialise a record into a `record_len` byte buffer: body at its field
/// offsets, then the header, whose CRC covers the body span only. That split
/// is what makes the header-last commit meaningful.
pub fn serialize(rec: *const Record, seq: u32, buf: *[record_len]u8) void {
    @memset(buf, 0);
    const b = &rec.body;
    @memcpy(buf[off_serial..][0..serial_len], &b.serial);
    @memcpy(buf[off_panel_serial..][0..panel_serial_len], &b.panel_serial);
    @memcpy(buf[off_panel_lut..][0..panel_lut_len], &b.panel_lut_id);
    @memcpy(buf[off_touch_cal..][0..touch_cal_len], &b.touch_cal);
    packLe32(buf[off_mfg_date..], b.mfg_date);
    packLe32(buf[off_key_id..], b.device_key_id);
    packLe16(buf[off_hw_rev..], b.hw_rev);
    packLe16(buf[off_fixture..], b.fixture_id);
    packLe16(buf[off_vcom..], b.panel_vcom_mv);

    const crc = crc32(buf[hdr_bytes..][0..body_bytes]);
    packLe32(buf[off_magic..], magic);
    packLe16(buf[off_schema..], schema_ver);
    packLe16(buf[off_reclen..], @intCast(record_len));
    packLe32(buf[off_seq..], seq);
    packLe32(buf[off_crc..], crc);
    packLe32(buf[off_flags..], rec.flags);
}

/// Decode a validated record buffer. Integrity is `copyValid`'s job and is
/// deliberately not repeated here.
pub fn deserialize(buf: *const [record_len]u8, out: *Record) void {
    const b = &out.body;
    @memcpy(&b.serial, buf[off_serial..][0..serial_len]);
    @memcpy(&b.panel_serial, buf[off_panel_serial..][0..panel_serial_len]);
    @memcpy(&b.panel_lut_id, buf[off_panel_lut..][0..panel_lut_len]);
    @memcpy(&b.touch_cal, buf[off_touch_cal..][0..touch_cal_len]);
    b.mfg_date = unpackLe32(buf[off_mfg_date..]);
    b.device_key_id = unpackLe32(buf[off_key_id..]);
    b.hw_rev = unpackLe16(buf[off_hw_rev..]);
    b.fixture_id = unpackLe16(buf[off_fixture..]);
    b.panel_vcom_mv = unpackLe16(buf[off_vcom..]);
    out.flags = unpackLe32(buf[off_flags..]);
    out.seq = unpackLe32(buf[off_seq..]);
    out.schema_version = unpackLe16(buf[off_schema..]);
}

/// Report whether a serialised copy passes every integrity gate: magic, known
/// schema, expected `record_len`, CRC-32 over the body. The gates stay in the
/// C's order and each stands alone, because the MC/DC vectors in
/// `tests/misc/src/test_ra8_devcfg.c` flip exactly one at a time.
pub fn copyValid(buf: *const [record_len]u8) bool {
    if (unpackLe32(buf[off_magic..]) != magic) {
        return false; // not our record / blank window
    }
    if (unpackLe16(buf[off_schema..]) > schema_ver) {
        return false; // newer, unknown schema -- reject, never guess
    }
    if (unpackLe16(buf[off_reclen..]) != @as(u16, @intCast(record_len))) {
        return false; // length not what this schema expects
    }
    return unpackLe32(buf[off_crc..]) == crc32(buf[hdr_bytes..][0..body_bytes]);
}

/// Choose the copy offset a new record overwrites. Prefers an invalid slot so
/// a valid record is never clobbered; with both valid, targets the lower `seq`
/// so the newest survives the write window.
pub fn targetOffset(valid0: bool, seq0: u32, valid1: bool, seq1: u32) u32 {
    if (!valid0) return copy0_off;
    if (!valid1) return copy1_off;
    return if (seq0 <= seq1) copy0_off else copy1_off;
}

/// The sequence a new record is stamped with: one past the newest valid copy.
pub fn nextSeq(valid0: bool, seq0: u32, valid1: bool, seq1: u32) u32 {
    const s0: u32 = if (valid0) seq0 else 0;
    const s1: u32 = if (valid1) seq1 else 0;
    const newest = if (s0 >= s1) s0 else s1;
    return newest +% 1;
}

/// Secondary sanity guard behind the explicit valid flag: rejects the poison
/// values 0 and 0xFFFF and anything outside the plausible e-paper VCOM span.
pub fn vcomInRange(mv: u16) bool {
    return (mv >= vcom_min_mv) and (mv <= vcom_max_mv);
}
