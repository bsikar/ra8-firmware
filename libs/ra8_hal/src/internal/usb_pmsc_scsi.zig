//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB mass-storage (BBB) SCSI command handlers (RA8FW-594). Pure: the
//! storage callbacks and the CDB are passed in. SPC-4 INQUIRY, SBC-3 READ
//! CAPACITY(10), REQUEST SENSE, MODE SENSE(6), READ(10) and WRITE(10).

pub const ok: u16 = 0;
pub const err_invalid_size: u16 = 0x105;

pub const inquiry_len: u32 = 36;
pub const read_capacity_len: u32 = 8;
pub const request_sense_len: u32 = 18;
pub const mode_sense_len: u32 = 4;
pub const block_size_default: u32 = 512;

const inq_vendor = 8;
const inq_product = 16;
const inq_revision = 32;

pub const ReadFn = *const fn (ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]u8) callconv(.C) u16;
pub const WriteFn = *const fn (ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]const u8) callconv(.C) u16;
pub const CapacityFn = *const fn (ctx: ?*anyopaque, count: *u32, size: *u32) callconv(.C) u16;
pub const InquiryFn = *const fn (ctx: ?*anyopaque, vendor8: [*]u8, product16: [*]u8, revision4: [*]u8) callconv(.C) u16;

/// `ra8_usb_pmsc_storage_t`.
pub const Storage = extern struct {
    read_block: ReadFn,
    write_block: WriteFn,
    get_capacity: CapacityFn,
    get_inquiry: InquiryFn,
    ctx: ?*anyopaque,
};

/// `ra8_usb_pmsc_state_data_t`; storage stays in ra8_usb_pmsc.c.
pub const State = extern struct {
    initialized: bool,
    storage_attached: bool,
    speed: u8,
    bot_state: u8,
    storage: Storage,
    cbw_tag: u32,
    cbw_data_length: u32,
    cbw_dir_in: bool,
    cbw_lun: u8,
    cbw_cdb: [16]u8,
    cbw_cdb_len: u8,
    last_data_len: u32,
};

/// READ(10)/WRITE(10) LBA (bytes 2..5) and block count (bytes 7..8), big-endian.
pub const Rw10 = struct { lba: u32, count: u32 };

pub fn decodeRw10(cdb: *const [16]u8) Rw10 {
    const lba = @as(u32, cdb[2]) << 24 | @as(u32, cdb[3]) << 16 | @as(u32, cdb[4]) << 8 | cdb[5];
    return .{ .lba = lba, .count = @as(u32, cdb[7]) << 8 | cdb[8] };
}

fn packBe(v: u32, dst: [*]u8) void {
    dst[0] = @truncate(v >> 24);
    dst[1] = @truncate(v >> 16);
    dst[2] = @truncate(v >> 8);
    dst[3] = @truncate(v);
}

pub fn inquiry(s: *const Storage, buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    if (capacity < inquiry_len) return err_invalid_size;
    @memset(buf[0..inquiry_len], 0);
    buf[1] = 0x80; // removable
    buf[2] = 0x04; // SPC-4
    buf[3] = 0x02; // response format
    buf[4] = 0x1F; // 36 - 5
    // Pad the strings first so a backend that writes fewer bytes still conforms.
    @memset(buf[inq_vendor..inquiry_len], ' ');
    const err = s.get_inquiry(s.ctx, buf + inq_vendor, buf + inq_product, buf + inq_revision);
    if (err != ok) return err;
    out_len.* = inquiry_len;
    return ok;
}

pub fn readCapacity(s: *const Storage, buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    if (capacity < read_capacity_len) return err_invalid_size;
    var count: u32 = 0;
    var size: u32 = 0;
    const err = s.get_capacity(s.ctx, &count, &size);
    if (err != ok) return err;
    packBe(if (count == 0) 0 else count - 1, buf);
    packBe(size, buf + 4);
    out_len.* = read_capacity_len;
    return ok;
}

/// Fixed-format sense, response code 0x70, additional length 10.
pub fn requestSense(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    if (capacity < request_sense_len) return err_invalid_size;
    @memset(buf[0..request_sense_len], 0);
    buf[0] = 0x70;
    buf[7] = 0x0A;
    out_len.* = request_sense_len;
    return ok;
}

/// Header-only MODE SENSE(6) response, mode data length 3.
pub fn modeSense(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    if (capacity < mode_sense_len) return err_invalid_size;
    @memset(buf[0..mode_sense_len], 0);
    buf[0] = 0x03;
    out_len.* = mode_sense_len;
    return ok;
}

fn blockSize(s: *const Storage, size: *u32) u16 {
    var count: u32 = 0;
    const err = s.get_capacity(s.ctx, &count, size);
    if (err == ok and size.* == 0) size.* = block_size_default;
    return err;
}

pub fn read10(s: *const Storage, cdb: *const [16]u8, buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    const rw = decodeRw10(cdb);
    if (rw.count == 0) {
        out_len.* = 0;
        return ok;
    }
    var size: u32 = 0;
    const cap_err = blockSize(s, &size);
    if (cap_err != ok) return cap_err;
    const bytes = rw.count *% size;
    if (bytes > capacity) return err_invalid_size;
    const err = s.read_block(s.ctx, rw.lba, rw.count, buf);
    if (err != ok) return err;
    out_len.* = bytes;
    return ok;
}

pub fn write10(s: *const Storage, cdb: *const [16]u8, buf: [*]const u8, out_len: *u32) u16 {
    const rw = decodeRw10(cdb);
    if (rw.count == 0) {
        out_len.* = 0;
        return ok;
    }
    var size: u32 = 0;
    const cap_err = blockSize(s, &size);
    if (cap_err != ok) return cap_err;
    const err = s.write_block(s.ctx, rw.lba, rw.count, buf);
    if (err != ok) return err;
    out_len.* = rw.count *% size;
    return ok;
}
