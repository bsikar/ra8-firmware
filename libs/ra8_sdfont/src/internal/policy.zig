//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What the font store decides: which name to open, whether a missing file may
//! be self-provisioned from the baked-in blob, and whether what came back off
//! the card is long enough to be a font.
//!
//! The filesystem is an injected seam, so every decision here runs on the host
//! against a fake volume. No externs and no exported symbols.

const shared = @import("root.zig");
const abi_types = @import("abi_types.zig");

pub const err = shared.err;
pub const limits = shared.limits;

/// Opaque `ra8_fs_file_t*`.
pub const File = *anyopaque;

/// Where the bytes that were handed back came from.
pub const Source = abi_types.Source;

/// The filesystem operations the policy needs, as a runtime seam.
///
/// Paths stay NUL-terminated because the real binding forwards them straight
/// into `ra8_fs_open`; this is the C membrane reaching one layer in rather
/// than a string convention of its own.
pub const Fs = struct {
    ctx: ?*anyopaque = null,
    open_read: *const fn (ctx: ?*anyopaque, name: [*:0]const u8, out_file: *?File) u16,
    write_file: *const fn (ctx: ?*anyopaque, name: [*:0]const u8, data: [*]const u8, len: u32) u16,
    read: *const fn (ctx: ?*anyopaque, file: File, buf: [*]u8, cap: u32, out_got: *u32) u16,
    close: *const fn (ctx: ?*anyopaque, file: File) u16,
};

/// The blob a caller may offer for self-provisioning. Absent is the common
/// case and means "fail if the card has no font".
pub const Blob = struct {
    data: ?[*]const u8 = null,
    len: u32 = 0,
};

/// `filename` is optional; NULL selects the default.
pub fn resolveName(filename: ?[*:0]const u8) [*:0]const u8 {
    return filename orelse shared.default_font_name.ptr;
}

/// Non-pointer argument check. The membrane does the NULL checks first, so
/// this only carries what survives them.
pub fn validateCapacity(cap: u32) u16 {
    if (cap == 0) {
        return err.invalid_arg;
    }
    return err.ok;
}

/// Open the font, writing it to the card first when it is genuinely absent
/// and the caller offered a blob.
///
/// The guards on the blob are deliberately separate single conditions: each
/// reason to skip provisioning is then covered by one test rather than by a
/// vector over a compound decision.
pub fn openOrProvision(
    fs: Fs,
    name: [*:0]const u8,
    blob: Blob,
    out_file: *?File,
    out_source: *Source,
) u16 {
    out_source.* = .card;

    const opened = fs.open_read(fs.ctx, name, out_file);
    if (opened != err.not_found) {
        return opened;
    }

    const data = blob.data orelse return opened;
    if (blob.len == 0) {
        return opened;
    }

    const written = fs.write_file(fs.ctx, name, data, blob.len);
    if (written != err.ok) {
        return written;
    }

    out_source.* = .provisioned;
    return fs.open_read(fs.ctx, name, out_file);
}

/// Read the font into caller storage and close the handle, whatever happened.
///
/// A short read is `no_data` rather than a success with a tiny length: the
/// callers feed this straight into a shaper that would fault on a stub.
pub fn readFont(fs: Fs, file: File, buf: [*]u8, cap: u32, out_len: *u32) u16 {
    var got: u32 = 0;
    const status = fs.read(fs.ctx, file, buf, cap, &got);
    _ = fs.close(fs.ctx, file);

    if (status != err.ok) {
        return status;
    }
    if (got < limits.min_font_bytes) {
        return err.no_data;
    }

    out_len.* = got;
    return err.ok;
}
