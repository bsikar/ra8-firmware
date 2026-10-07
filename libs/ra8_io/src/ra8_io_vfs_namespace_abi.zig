//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the path-namespace half of inc/ra8_io_vfs.h (RA8FW-721): unlink,
//! same-mount rename, stat, listdir, bounded directory cursors, mkdir and
//! rmdir. Each call resolves "name:sub" to a mount slot, checks the format's
//! advertised capability, then dispatches to that format's ops table. A
//! rename across two mounts is refused, since this facade does no
//! cross-format copy. Replaces ra8_io_vfs_namespace.c, which is deleted; the
//! mount table and its resolve/split/find/streq helpers live in
//! ra8_io_vfs_abi.zig and are reached as externs.

const tag = "ra8_io_vfs_namespace";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_no_mem: c_int = 0x102;
pub const err_invalid_arg: c_int = 0x103;
pub const err_invalid_state: c_int = 0x104;
pub const err_not_found: c_int = 0x106;
pub const err_not_supported: c_int = 0x107;
pub const err_busy: c_int = 0x109;
pub const err_null_ptr: c_int = 0x504;

/// k_ra8_io_vfs_name_max (ra8_io_vfs.h): mount name bytes incl NUL.
pub const name_max: usize = 16;
/// sizeof(ra8_fs_dirent_t); this unit only zeroes it.
pub const dirent_bytes: usize = 760;

/// Mirror of ra8_io_fsfmt_caps_t.
pub const Caps = extern struct {
    directory_workspace_bytes: u32 = 0,
    max_name_len: u16 = 0,
    max_open_directories: u16 = 0,
    directory_workspace_align: u8 = 0,
    read_only: bool = false,
    supports_mkdir: bool = false,
    supports_rmdir: bool = false,
    supports_streaming_write: bool = false,
    supports_timestamps: bool = false,
    supports_free_space: bool = false,
    supports_dir_cursor: bool = false,
    supports_sync: bool = false,
    atomic_rename: bool = false,
    durable_sync: bool = false,
    unicode_names: bool = false,
    case_sensitive: bool = false,
};

/// Mirror of ra8_fs_timestamp_t: a civil datetime plus two validity flags.
pub const Timestamp = extern struct {
    year: u16 = 0,
    utc_offset_min: i16 = 0,
    fields: [6]u8 = @splat(0),
    valid: bool = false,
    utc_offset_valid: bool = false,
};

/// Mirror of ra8_fs_stat_t (fields this unit reads, padded to full size).
pub const FsStat = extern struct {
    size_bytes: u64 = 0,
    first_cluster: u32 = 0,
    created: Timestamp = .{},
    modified: Timestamp = .{},
    accessed: Timestamp = .{},
    attr: u8 = 0,
    is_directory: bool = false,
};

/// Mirror of ra8_io_vfs_stat_t.
pub const VfsStat = extern struct {
    size_bytes: u64 = 0,
    created: Timestamp = .{},
    modified: Timestamp = .{},
    accessed: Timestamp = .{},
    attr: u8 = 0,
    is_directory: bool = false,
    exists: bool = false,
};

pub const ListdirCb = *const fn (?[*:0]const u8, u8, u64, ?*anyopaque) callconv(.c) void;
const Opaque = ?*const anyopaque;
pub const StatFn = *const fn (?*anyopaque, [*:0]const u8, *FsStat) callconv(.c) c_int;
pub const ListdirFn = *const fn (?*anyopaque, [*:0]const u8, ListdirCb, ?*anyopaque) callconv(.c) c_int;
pub const DirOpenFn = *const fn (?*anyopaque, [*:0]const u8, *anyopaque, u32) callconv(.c) c_int;
pub const DirNextFn = *const fn (?*anyopaque, *anyopaque, *bool) callconv(.c) c_int;
pub const DirCloseFn = *const fn (?*anyopaque) callconv(.c) c_int;
pub const PathFn = *const fn (?*anyopaque, [*:0]const u8) callconv(.c) c_int;
pub const RenameFn = *const fn (?*anyopaque, [*:0]const u8, [*:0]const u8) callconv(.c) c_int;

/// Mirror of ra8_io_fsfmt_ops_t, in header order.
pub const Ops = extern struct {
    probe: Opaque = null,
    mount: Opaque = null,
    unmount: Opaque = null,
    open: Opaque = null,
    close: Opaque = null,
    read: Opaque = null,
    write: Opaque = null,
    seek: Opaque = null,
    tell: Opaque = null,
    size: Opaque = null,
    sync: Opaque = null,
    stat: ?StatFn = null,
    listdir: ?ListdirFn = null,
    dir_open: ?DirOpenFn = null,
    dir_next: ?DirNextFn = null,
    dir_close: ?DirCloseFn = null,
    unlink: ?PathFn = null,
    rename: ?RenameFn = null,
    mkdir: ?PathFn = null,
    rmdir: ?PathFn = null,
    free_space: Opaque = null,
};

/// Mirror of ra8_io_fsfmt_t.
pub const Format = extern struct {
    name: ?[*:0]const u8 = null,
    caps: Caps = .{},
    ops: *const Ops,
};

/// Mirror of vfs_slot_t (ra8_io_vfs_internal.h). A free slot in the zeroed
/// table holds a NULL format, so the pointer is optional here as it is in C.
pub const Slot = extern struct {
    name: [name_max]u8 = @splat(0),
    format: ?*const Format = null,
    mount_ctx: ?*anyopaque = null,
    owned: bool = false,
    native: bool = false,
    in_use: bool = false,
};

/// Mirror of ra8_io_vfs_dir_t.
pub const Dir = extern struct {
    format: ?*const Format = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    is_open: bool = false,
};

comptime {
    if (@sizeOf(Caps) != 24 or @offsetOf(Caps, "supports_dir_cursor") != 15) @compileError("Caps layout");
    if (@sizeOf(Timestamp) != 12 or @sizeOf(FsStat) != 56 or @offsetOf(FsStat, "attr") != 48) @compileError("FsStat layout");
    if (@sizeOf(VfsStat) != 48 or @offsetOf(VfsStat, "exists") != 46) @compileError("VfsStat layout");
    const p = @sizeOf(usize);
    if (@offsetOf(Format, "caps") != p or @offsetOf(Format, "ops") != p + 24) @compileError("Format layout");
    if (@offsetOf(Ops, "stat") != 11 * p or @offsetOf(Ops, "unlink") != 16 * p) @compileError("Ops layout");
    if (@offsetOf(Slot, "mount_ctx") != 16 + p or @offsetOf(Slot, "in_use") != 18 + 2 * p) @compileError("Slot layout");
    if (@offsetOf(Dir, "is_open") != 2 * p + 4) @compileError("Dir layout");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn priv_ra8_io_vfs_streq(a: [*:0]const u8, b: [*:0]const u8) bool;
extern fn priv_ra8_io_vfs_find(name: [*:0]const u8, out_index: ?*u8) ?*Slot;
extern fn priv_ra8_io_vfs_split(path: [*:0]const u8, out_name: [*]u8, out_sub: *?[*:0]const u8) c_int;
extern fn priv_ra8_io_vfs_resolve(path: [*:0]const u8, out_slot: *?*Slot, out_index: ?*u8, out_sub: *?[*:0]const u8) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// RA8_RETURN_ON_ERROR's log: the message, then the code.
fn logged(rc: c_int, message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    ra8_log_emit_error_val(tag, "Error", @bitCast(rc));
    return rc;
}

/// A resolved path: the mount slot and the sub-path inside it.
const Target = struct { slot: *Slot, sub: [*:0]const u8 };

/// Resolve, logging like RA8_RETURN_ON_ERROR when `log` is set.
fn resolve(path: [*:0]const u8, out: *Target, log: bool) c_int {
    var slot: ?*Slot = null;
    var sub: ?[*:0]const u8 = null;
    const rc = priv_ra8_io_vfs_resolve(path, &slot, null, &sub);
    if (rc != ok) return if (log) logged(rc, "resolve") else rc;
    out.* = .{ .slot = slot.?, .sub = sub.? };
    return ok;
}

/// Shared path for unlink/mkdir/rmdir: refuse read-only mounts, a missing
/// capability, or a missing op, then dispatch.
fn mutatePath(path: ?[*:0]const u8, comptime op: []const u8, comptime cap: ?[]const u8) c_int {
    const p = path orelse return nullPtr("path must not be nullptr");
    var t: Target = undefined;
    const rc = resolve(p, &t, true);
    if (rc != ok) return rc;
    const fmt = t.slot.format.?;
    if (fmt.caps.read_only) return err_not_supported;
    if (cap) |c| if (!@field(fmt.caps, c)) return err_not_supported;
    const f = @field(fmt.ops.*, op) orelse return err_not_supported;
    return f(t.slot.mount_ctx, t.sub);
}

pub export fn ra8_io_vfs_unlink(path: ?[*:0]const u8) callconv(.c) c_int {
    return mutatePath(path, "unlink", null);
}

pub export fn ra8_io_vfs_mkdir(path: ?[*:0]const u8) callconv(.c) c_int {
    return mutatePath(path, "mkdir", "supports_mkdir");
}

pub export fn ra8_io_vfs_rmdir(path: ?[*:0]const u8) callconv(.c) c_int {
    return mutatePath(path, "rmdir", "supports_rmdir");
}

pub export fn ra8_io_vfs_rename(old_path: ?[*:0]const u8, new_path: ?[*:0]const u8) callconv(.c) c_int {
    const old = old_path orelse return nullPtr("old_path must not be nullptr");
    const new = new_path orelse return nullPtr("new_path must not be nullptr");
    var old_name: [name_max]u8 = undefined;
    var new_name: [name_max]u8 = undefined;
    var old_sub: ?[*:0]const u8 = null;
    var new_sub: ?[*:0]const u8 = null;
    var rc = priv_ra8_io_vfs_split(old, &old_name, &old_sub);
    if (rc != ok) return logged(rc, "old path");
    rc = priv_ra8_io_vfs_split(new, &new_name, &new_sub);
    if (rc != ok) return logged(rc, "new path");
    const old_z: [*:0]const u8 = @ptrCast(&old_name);
    if (!priv_ra8_io_vfs_streq(old_z, @ptrCast(&new_name))) return err_invalid_arg;
    const slot = priv_ra8_io_vfs_find(old_z, null) orelse return err_not_found;
    if (slot.format.?.caps.read_only) return err_not_supported;
    const f = slot.format.?.ops.rename orelse return err_not_supported;
    return f(slot.mount_ctx, old_sub.?, new_sub.?);
}

pub export fn ra8_io_vfs_stat(path: ?[*:0]const u8, out: ?*VfsStat) callconv(.c) c_int {
    const p = path orelse return nullPtr("path must not be nullptr");
    const o = out orelse return nullPtr("out must not be nullptr");
    o.* = .{};
    var t: Target = undefined;
    const rc = resolve(p, &t, true);
    if (rc != ok) return rc;
    var st: FsStat = .{};
    const e = t.slot.format.?.ops.stat.?(t.slot.mount_ctx, t.sub, &st);
    if (e == err_not_found) return ok;
    if (e != ok) return e;
    o.* = .{
        .size_bytes = st.size_bytes,
        .created = st.created,
        .modified = st.modified,
        .accessed = st.accessed,
        .attr = st.attr,
        .is_directory = st.is_directory,
        .exists = true,
    };
    return ok;
}

pub export fn ra8_io_vfs_listdir(path: ?[*:0]const u8, cb: ?ListdirCb, ctx: ?*anyopaque) callconv(.c) c_int {
    const p = path orelse return nullPtr("path must not be nullptr");
    const callback = cb orelse return nullPtr("cb must not be nullptr");
    var t: Target = undefined;
    const rc = resolve(p, &t, true);
    if (rc != ok) return rc;
    return t.slot.format.?.ops.listdir.?(t.slot.mount_ctx, t.sub, callback, ctx);
}

pub export fn ra8_io_vfs_dir_requirements(
    path: ?[*:0]const u8,
    out_bytes: ?*u32,
    out_align: ?*u8,
    out_max_open: ?*u16,
) callconv(.c) c_int {
    const p = path orelse return err_null_ptr;
    const bytes = out_bytes orelse return err_null_ptr;
    const alignment = out_align orelse return err_null_ptr;
    const max_open = out_max_open orelse return err_null_ptr;
    bytes.* = 0;
    alignment.* = 0;
    max_open.* = 0;
    var t: Target = undefined;
    const rc = resolve(p, &t, false);
    if (rc != ok) return rc;
    const caps = &t.slot.format.?.caps;
    if (!caps.supports_dir_cursor) return err_not_supported;
    bytes.* = caps.directory_workspace_bytes;
    alignment.* = caps.directory_workspace_align;
    max_open.* = caps.max_open_directories;
    return ok;
}

pub export fn ra8_io_vfs_dir_open(
    path: ?[*:0]const u8,
    directory: ?*Dir,
    workspace: ?*anyopaque,
    workspace_bytes: u32,
) callconv(.c) c_int {
    const p = path orelse return err_null_ptr;
    const dir = directory orelse return err_null_ptr;
    const ws = workspace orelse return err_null_ptr;
    if (dir.is_open) return err_busy;
    var t: Target = undefined;
    const rc = resolve(p, &t, false);
    if (rc != ok) return rc;
    const fmt = t.slot.format.?;
    if (!fmt.caps.supports_dir_cursor) return err_not_supported;
    if (workspace_bytes < fmt.caps.directory_workspace_bytes) return err_no_mem;
    // An align of 0 imposes nothing (the C modulo by 0 is UDIV's 0 on the M85).
    const want: usize = fmt.caps.directory_workspace_align;
    if (want != 0 and @intFromPtr(ws) % want != 0) return err_invalid_arg;
    const opened = fmt.ops.dir_open.?(t.slot.mount_ctx, t.sub, ws, workspace_bytes);
    if (opened != ok) return opened;
    dir.* = .{ .format = fmt, .state = ws, .state_bytes = workspace_bytes, .is_open = true };
    return ok;
}

pub export fn ra8_io_vfs_dir_next(directory: ?*Dir, out: ?*anyopaque, out_entry: ?*bool) callconv(.c) c_int {
    const dir = directory orelse return err_null_ptr;
    const o = out orelse return err_null_ptr;
    const entry = out_entry orelse return err_null_ptr;
    const fmt = dir.format orelse return err_invalid_state;
    if (!dir.is_open) return err_invalid_state;
    @memset(@as([*]u8, @ptrCast(o))[0..dirent_bytes], 0);
    entry.* = false;
    return fmt.ops.dir_next.?(dir.state, o, entry);
}

pub export fn ra8_io_vfs_dir_close(directory: ?*Dir) callconv(.c) c_int {
    const dir = directory orelse return err_null_ptr;
    const fmt = dir.format orelse return err_invalid_state;
    if (!dir.is_open) return err_invalid_state;
    const closed = fmt.ops.dir_close.?(dir.state);
    dir.* = .{};
    return closed;
}
