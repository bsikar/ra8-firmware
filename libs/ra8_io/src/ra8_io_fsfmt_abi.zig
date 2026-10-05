//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_fsfmt.h (RA8FW-738): the format registry, its
//! register-time contract checks, and the built-in FAT and exFAT formats,
//! whose operations are thin wrappers over the ra8_fs facade. Replaces
//! ra8_io_fsfmt.c, which is deleted. ra8_fs is reached as externs with
//! opaque handles, so ra8_io still does not import it.

const ns = @import("ra8_io_vfs_namespace_abi.zig");

const tag = "ra8_io_fsfmt";

pub const Format = ns.Format;
pub const Ops = ns.Ops;
pub const Caps = ns.Caps;

const ok = ns.ok;
const err_no_mem = ns.err_no_mem;
const err_invalid_arg = ns.err_invalid_arg;
const err_not_found = ns.err_not_found;
const err_null_ptr = ns.err_null_ptr;

/// k_ra8_io_fsfmt_max and the per-format name limits (ra8_io_fsfmt.h).
pub const max_formats: usize = 8;
pub const fat_max_name_utf8: u16 = 741;
pub const exfat_max_name_utf8: u16 = 192;

/// ra8_fs_type_t values (ra8_fs_types.h).
pub const fs_type_unknown: c_int = 0;
pub const fs_type_fat12: c_int = 12;
pub const fs_type_fat16: c_int = 16;
pub const fs_type_fat32: c_int = 32;
pub const fs_type_exfat: c_int = 64;

/// Mirror of ra8_fs_dir_t: 640 bytes of u64-aligned cursor state plus a guard.
pub const FsDir = extern struct {
    state: [640]u8 align(8) = .{0} ** 640,
    is_open: bool = false,
};

/// Mirror of ra8_fs_dirent_t.
pub const FsDirent = extern struct {
    name: [742]u8 = .{0} ** 742,
    size_bytes: u64 = 0,
    attr: u8 = 0,
};

comptime {
    if (@sizeOf(FsDir) != 648 or @alignOf(FsDir) != 8) @compileError("FsDir layout");
    if (@sizeOf(FsDirent) != ns.dirent_bytes) @compileError("FsDirent size");
    if (@offsetOf(FsDirent, "size_bytes") != 744 or @offsetOf(FsDirent, "attr") != 752) @compileError("FsDirent layout");
}

const H = ?*anyopaque;
const Path = [*:0]const u8;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_fs_probe(backend: *const anyopaque, out_type: *c_int) c_int;
extern fn ra8_fs_mount(backend: *const anyopaque, out: *H) c_int;
extern fn ra8_fs_unmount(mount: H) c_int;
extern fn ra8_fs_open(mount: H, path: Path, mode: c_int, out: *H) c_int;
extern fn ra8_fs_close(file: H) c_int;
extern fn ra8_fs_read(file: H, buf: *anyopaque, bytes: u32, out_read: *u32) c_int;
extern fn ra8_fs_write(file: H, buf: *const anyopaque, bytes: u32) c_int;
extern fn ra8_fs_seek(file: H, offset: u64) c_int;
extern fn ra8_fs_tell(file: ?*const anyopaque, out: *u64) c_int;
extern fn ra8_fs_size(file: ?*const anyopaque, out: *u64) c_int;
extern fn ra8_fs_stat(mount: H, path: Path, out: *ns.FsStat) c_int;
extern fn ra8_fs_dir_open(mount: H, path: Path, dir: *FsDir) c_int;
extern fn ra8_fs_dir_next(dir: *FsDir, out: *FsDirent, out_entry: *bool) c_int;
extern fn ra8_fs_dir_close(dir: *FsDir) c_int;
extern fn ra8_fs_unlink(mount: H, path: Path) c_int;
extern fn ra8_fs_rename(mount: H, old_path: Path, new_path: Path) c_int;
extern fn ra8_fs_mkdir(mount: H, path: Path) c_int;
extern fn ra8_fs_rmdir(mount: H, path: Path) c_int;
extern fn ra8_fs_free_space(mount: H, out: *anyopaque) c_int;

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

// The built-in operations: each forwards to the ra8_fs facade.
fn nativeMount(backend: *const anyopaque, out: *H) callconv(.c) c_int {
    var mount: H = null;
    const e = ra8_fs_mount(backend, &mount);
    if (e != ok) return e;
    out.* = mount;
    return ok;
}
fn nativeUnmount(mount: H) callconv(.c) c_int {
    return ra8_fs_unmount(mount);
}
fn nativeOpen(mount: H, path: Path, mode: u8, out: *H) callconv(.c) c_int {
    var file: H = null;
    const e = ra8_fs_open(mount, path, mode, &file);
    if (e != ok) return e;
    out.* = file;
    return ok;
}
fn nativeClose(file: H) callconv(.c) c_int {
    return ra8_fs_close(file);
}
fn nativeRead(file: H, buf: *anyopaque, bytes: u32, out_read: *u32) callconv(.c) c_int {
    return ra8_fs_read(file, buf, bytes, out_read);
}
fn nativeWrite(file: H, buf: *const anyopaque, bytes: u32) callconv(.c) c_int {
    return ra8_fs_write(file, buf, bytes);
}
fn nativeSeek(file: H, offset: u64) callconv(.c) c_int {
    return ra8_fs_seek(file, offset);
}
fn nativeTell(file: ?*const anyopaque, out: *u64) callconv(.c) c_int {
    return ra8_fs_tell(file, out);
}
fn nativeSize(file: ?*const anyopaque, out: *u64) callconv(.c) c_int {
    return ra8_fs_size(file, out);
}
fn nativeStat(mount: H, path: Path, out: *ns.FsStat) callconv(.c) c_int {
    return ra8_fs_stat(mount, path, out);
}

/// Walk one directory with a stack cursor, closing it whatever happened.
fn nativeListdir(mount: H, path: Path, cb: ns.ListdirCb, cb_ctx: ?*anyopaque) callconv(.c) c_int {
    var dir = FsDir{};
    var err = ra8_fs_dir_open(mount, path, &dir);
    while (err == ok) {
        var entry = FsDirent{};
        var present = false;
        err = ra8_fs_dir_next(&dir, &entry, &present);
        if (err != ok or !present) break;
        cb(@ptrCast(&entry.name), entry.attr, entry.size_bytes, cb_ctx);
    }
    if (dir.is_open) {
        const closed = ra8_fs_dir_close(&dir);
        if (err == ok) err = closed;
    }
    return err;
}

fn nativeDirOpen(mount: H, path: Path, state: *anyopaque, state_bytes: u32) callconv(.c) c_int {
    if (state_bytes < @sizeOf(FsDir)) return err_no_mem;
    const dir: *FsDir = @ptrCast(@alignCast(state));
    dir.* = .{};
    return ra8_fs_dir_open(mount, path, dir);
}
fn nativeDirNext(state: H, out: *anyopaque, out_entry: *bool) callconv(.c) c_int {
    return ra8_fs_dir_next(@ptrCast(@alignCast(state.?)), @ptrCast(@alignCast(out)), out_entry);
}
fn nativeDirClose(state: H) callconv(.c) c_int {
    return ra8_fs_dir_close(@ptrCast(@alignCast(state.?)));
}
fn nativeUnlink(mount: H, path: Path) callconv(.c) c_int {
    return ra8_fs_unlink(mount, path);
}
fn nativeRename(mount: H, old_path: Path, new_path: Path) callconv(.c) c_int {
    return ra8_fs_rename(mount, old_path, new_path);
}
fn nativeMkdir(mount: H, path: Path) callconv(.c) c_int {
    return ra8_fs_mkdir(mount, path);
}
fn nativeRmdir(mount: H, path: Path) callconv(.c) c_int {
    return ra8_fs_rmdir(mount, path);
}
fn nativeFreeSpace(mount: H, out: *anyopaque) callconv(.c) c_int {
    return ra8_fs_free_space(mount, out);
}

fn probedType(backend: *const anyopaque) ?c_int {
    var t: c_int = fs_type_unknown;
    if (ra8_fs_probe(backend, &t) != ok) return null;
    return t;
}
fn exfatProbe(backend: *const anyopaque) callconv(.c) bool {
    return probedType(backend) == fs_type_exfat;
}
fn fatProbe(backend: *const anyopaque) callconv(.c) bool {
    const t = probedType(backend) orelse return false;
    return t == fs_type_fat12 or t == fs_type_fat16 or t == fs_type_fat32;
}

fn nativeOps(comptime probe: *const fn (*const anyopaque) callconv(.c) bool) Ops {
    return .{
        .probe = @ptrCast(probe),
        .mount = @ptrCast(&nativeMount),
        .unmount = @ptrCast(&nativeUnmount),
        .open = @ptrCast(&nativeOpen),
        .close = @ptrCast(&nativeClose),
        .read = @ptrCast(&nativeRead),
        .write = @ptrCast(&nativeWrite),
        .seek = @ptrCast(&nativeSeek),
        .tell = @ptrCast(&nativeTell),
        .size = @ptrCast(&nativeSize),
        .stat = &nativeStat,
        .listdir = &nativeListdir,
        .dir_open = &nativeDirOpen,
        .dir_next = &nativeDirNext,
        .dir_close = &nativeDirClose,
        .unlink = &nativeUnlink,
        .rename = &nativeRename,
        .mkdir = &nativeMkdir,
        .rmdir = &nativeRmdir,
        .free_space = @ptrCast(&nativeFreeSpace),
    };
}

fn nativeCaps(max_name_len: u16) Caps {
    return .{
        .directory_workspace_bytes = @sizeOf(FsDir),
        .max_name_len = max_name_len,
        .max_open_directories = 0xFFFF,
        .directory_workspace_align = @alignOf(FsDir),
        .supports_mkdir = true,
        .supports_rmdir = true,
        .supports_streaming_write = true,
        .supports_timestamps = true,
        .supports_free_space = true,
        .supports_dir_cursor = true,
        .unicode_names = true,
    };
}

const fat_ops = nativeOps(&fatProbe);
const exfat_ops = nativeOps(&exfatProbe);
const fmt_fat = Format{ .name = "fat", .caps = nativeCaps(fat_max_name_utf8), .ops = &fat_ops };
const fmt_exfat = Format{ .name = "exfat", .caps = nativeCaps(exfat_max_name_utf8), .ops = &exfat_ops };

var reg: [max_formats]*const Format = undefined;
var count: usize = 0;

fn validateDirCursorCaps(f: *const Format) c_int {
    const a = f.caps.directory_workspace_align;
    if (f.ops.dir_open == null or f.ops.dir_next == null or f.ops.dir_close == null) return err_invalid_arg;
    if (f.caps.directory_workspace_bytes == 0 or f.caps.max_open_directories == 0) return err_invalid_arg;
    if (a == 0 or (a & (a - 1)) != 0) return err_invalid_arg;
    return ok;
}

/// Each advertised capability needs the operation that provides it.
fn validateSingleOpCaps(f: *const Format) c_int {
    const c = f.caps;
    const o = f.ops;
    if (c.supports_streaming_write and o.write == null) return err_invalid_arg;
    if (c.supports_mkdir and o.mkdir == null) return err_invalid_arg;
    if (c.supports_rmdir and o.rmdir == null) return err_invalid_arg;
    if (c.supports_free_space and o.free_space == null) return err_invalid_arg;
    if (c.supports_sync and o.sync == null) return err_invalid_arg;
    if (c.atomic_rename and o.rename == null) return err_invalid_arg;
    return ok;
}

fn validateCaps(f: *const Format) c_int {
    if (f.caps.supports_dir_cursor) {
        const e = validateDirCursorCaps(f);
        if (e != ok) return logged(e, "dir-cursor capability mismatch");
    }
    const e = validateSingleOpCaps(f);
    if (e != ok) return logged(e, "capability mismatch");
    if (f.caps.durable_sync and !f.caps.supports_sync) return err_invalid_arg;
    return ok;
}

fn requireOp(raw: ?*const anyopaque, comptime message: [*:0]const u8) c_int {
    return if (raw == null) nullPtr(message) else ok;
}

fn validateRequiredOps(f: *const Format) c_int {
    const o = f.ops;
    const group1 = [_]c_int{
        requireOp(o.probe, "probe must not be nullptr"),
        requireOp(o.mount, "mount must not be nullptr"),
        requireOp(o.unmount, "unmount must not be nullptr"),
        requireOp(o.open, "open must not be nullptr"),
        requireOp(o.close, "close must not be nullptr"),
        requireOp(o.read, "read must not be nullptr"),
    };
    for (group1) |e| if (e != ok) return logged(e, "missing mandatory operation");
    if (o.seek == null) return nullPtr("seek must not be nullptr");
    if (o.tell == null) return nullPtr("tell must not be nullptr");
    if (o.size == null) return nullPtr("size must not be nullptr");
    if (o.stat == null) return nullPtr("stat must not be nullptr");
    if (o.listdir == null) return nullPtr("listdir must not be nullptr");
    return ok;
}

pub export fn ra8_io_fsfmt_init() c_int {
    count = 0;
    var e = ra8_io_fsfmt_register(&fmt_exfat);
    if (e != ok) return logged(e, "register exfat");
    e = ra8_io_fsfmt_register(&fmt_fat);
    if (e != ok) return logged(e, "register fat");
    return ok;
}

pub export fn ra8_io_fsfmt_register(fmt: ?*const Format) c_int {
    const f = fmt orelse return nullPtr("fmt must not be nullptr");
    if (f.name == null) return nullPtr("fmt->name must not be nullptr");
    if (@intFromPtr(f.ops) == 0) return nullPtr("fmt->ops must not be nullptr");
    var e = validateRequiredOps(f);
    if (e != ok) return logged(e, "missing mandatory operation");
    e = validateCaps(f);
    if (e != ok) return logged(e, "capability mismatch");
    if (count >= max_formats) return err_no_mem;
    reg[count] = f;
    count += 1;
    return ok;
}

pub export fn ra8_io_fsfmt_get_builtin(fs_type: c_int, out: ?*?*const Format) c_int {
    const o = out orelse return nullPtr("out must not be nullptr");
    if (fs_type == fs_type_exfat) {
        o.* = &fmt_exfat;
        return ok;
    }
    if (fs_type == fs_type_fat12 or fs_type == fs_type_fat16 or fs_type == fs_type_fat32) {
        o.* = &fmt_fat;
        return ok;
    }
    return err_invalid_arg;
}

const ProbeFn = *align(1) const fn (*const anyopaque) callconv(.c) bool;

pub export fn ra8_io_fsfmt_probe(backend: ?*const anyopaque, out: ?*?*const Format) c_int {
    const b = backend orelse return nullPtr("backend must not be nullptr");
    const o = out orelse return nullPtr("out must not be nullptr");
    for (reg[0..count]) |f| {
        const probe: ProbeFn = @ptrCast(f.ops.probe.?);
        if (probe(b)) {
            o.* = f;
            return ok;
        }
    }
    return err_not_found;
}
