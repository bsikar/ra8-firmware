//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the mount table and open-stream half of inc/ra8_io_vfs.h
//! (RA8FW-736): named mounts (native ra8_fs or a probed format), the
//! generic open-file facade, capability and free-space queries, and the
//! name/path resolvers the namespace unit reaches as externs. Replaces
//! ra8_io_vfs.c, which is deleted. ra8_io_fsfmt.c is still C and is reached
//! through ra8_io_fsfmt_get_builtin and ra8_io_fsfmt_probe.

const ns = @import("ra8_io_vfs_namespace_abi.zig");

const tag = "ra8_io_vfs";

pub const Slot = ns.Slot;
pub const Format = ns.Format;
pub const Caps = ns.Caps;

const ok = ns.ok;
const err_no_mem = ns.err_no_mem;
const err_invalid_arg = ns.err_invalid_arg;
const err_invalid_state = ns.err_invalid_state;
const err_not_found = ns.err_not_found;
const err_not_supported = ns.err_not_supported;
const err_busy = ns.err_busy;
const err_null_ptr = ns.err_null_ptr;
/// k_ra8_err_exists (ra8_err.h).
pub const err_exists: c_int = 0x10C;

/// k_ra8_io_vfs_max_mounts / k_ra8_io_vfs_max_files / k_ra8_io_vfs_name_max.
pub const max_mounts: usize = 4;
pub const max_files: usize = 4;
pub const name_max: usize = ns.name_max;

/// ra8_fs_type_t values the native check asks the format registry for.
pub const fs_type_fat16: u8 = 16;
pub const fs_type_exfat: u8 = 64;

/// ra8_fs_mode_t (ra8_fs_types.h).
pub const mode_read: u8 = 0;
pub const mode_write: u8 = 1;
pub const mode_append: u8 = 2;

/// Prefix of ra8_fs_mount_t: ra8_fs_backend_t is five pointer-sized words,
/// then the detected type. Only `type` is read here.
pub const FsMount = extern struct {
    backend: [5]usize,
    type: u8,
};

/// struct ra8_io_vfs_file (opaque in the header).
pub const File = extern struct {
    format: ?*const Format = null,
    file_ctx: ?*anyopaque = null,
    mount_index: u8 = 0,
    in_use: bool = false,
};

const UnmountFn = *align(1) const fn (?*anyopaque) callconv(.c) c_int;
const MountFn = *align(1) const fn (*const anyopaque, *?*anyopaque) callconv(.c) c_int;
const OpenFn = *align(1) const fn (?*anyopaque, [*:0]const u8, u8, *?*anyopaque) callconv(.c) c_int;
const CtxFn = *align(1) const fn (?*anyopaque) callconv(.c) c_int;
const ReadFn = *align(1) const fn (?*anyopaque, *anyopaque, u32, *u32) callconv(.c) c_int;
const WriteFn = *align(1) const fn (?*anyopaque, *const anyopaque, u32) callconv(.c) c_int;
const SeekFn = *align(1) const fn (?*anyopaque, u64) callconv(.c) c_int;
const QueryFn = *align(1) const fn (?*const anyopaque, *u64) callconv(.c) c_int;
const SpaceFn = *align(1) const fn (?*anyopaque, *anyopaque) callconv(.c) c_int;

comptime {
    const p = @sizeOf(usize);
    if (@offsetOf(FsMount, "type") != 5 * p) @compileError("FsMount layout");
    if (@offsetOf(File, "mount_index") != 2 * p or @offsetOf(File, "in_use") != 2 * p + 1) @compileError("File layout");
}

var table: [max_mounts]Slot = .{zeroSlot()} ** max_mounts;
var files: [max_files]File = .{File{}} ** max_files;

/// The all-zero slot `(vfs_slot_t){}` is; its format pointer is null.
fn zeroSlot() Slot {
    return @bitCast([_]u8{0} ** @sizeOf(Slot));
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_io_fsfmt_get_builtin(fs_type: u8, out: *?*const Format) c_int;
extern fn ra8_io_fsfmt_probe(backend: *const anyopaque, out: *?*const Format) c_int;

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

fn op(comptime T: type, raw: ?*const anyopaque) ?T {
    return @ptrCast(raw);
}

/// The slot's format; a slot in use always has one.
fn fmtOf(slot: *const Slot) *const Format {
    return slot.format;
}

export fn priv_ra8_io_vfs_streq(a: [*:0]const u8, b: [*:0]const u8) bool {
    for (0..name_max) |i| {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

/// A mount name: 1..15 bytes, no ':' and no '/'.
fn nameOk(name: [*:0]const u8) bool {
    var i: usize = 0;
    while (i < name_max) : (i += 1) {
        const c = name[i];
        if (c == 0) break;
        if (c == ':' or c == '/') return false;
    }
    return i != 0 and i < name_max;
}

fn copyName(dst: *[name_max]u8, src: [*:0]const u8) void {
    var i: usize = 0;
    while (i < name_max - 1) : (i += 1) {
        dst[i] = src[i];
        if (src[i] == 0) return;
    }
    dst[i] = 0;
}

export fn priv_ra8_io_vfs_find(name: [*:0]const u8, out_index: ?*u8) ?*Slot {
    for (&table, 0..) |*slot, i| {
        if (!slot.in_use) continue;
        if (priv_ra8_io_vfs_streq(@ptrCast(&slot.name), name)) {
            if (out_index) |o| o.* = @intCast(i);
            return slot;
        }
    }
    return null;
}

fn freeMount() ?*Slot {
    for (&table) |*slot| if (!slot.in_use) return slot;
    return null;
}

export fn priv_ra8_io_vfs_split(path: [*:0]const u8, out_name: [*]u8, out_sub: *?[*:0]const u8) c_int {
    var i: usize = 0;
    while (i < name_max) : (i += 1) {
        const c = path[i];
        if (c == 0) return err_invalid_arg;
        if (c == ':') break;
        out_name[i] = c;
    }
    if (i >= name_max) return err_invalid_arg;
    out_name[i] = 0;
    out_sub.* = path + i + 1;
    return ok;
}

export fn priv_ra8_io_vfs_resolve(path: [*:0]const u8, out_slot: *?*Slot, out_index: ?*u8, out_sub: *?[*:0]const u8) c_int {
    var name: [name_max]u8 = undefined;
    const e = priv_ra8_io_vfs_split(path, &name, out_sub);
    if (e != ok) return e;
    out_slot.* = priv_ra8_io_vfs_find(@ptrCast(&name), out_index) orelse return err_not_found;
    return ok;
}

/// Native means the slot's format is the built-in FAT or exFAT descriptor.
fn isNative(format: *const Format) bool {
    var native: ?*const Format = null;
    _ = ra8_io_fsfmt_get_builtin(fs_type_fat16, &native);
    if (native == format) return true;
    _ = ra8_io_fsfmt_get_builtin(fs_type_exfat, &native);
    return native == format;
}

fn storeMount(slot: *Slot, name: [*:0]const u8, format: *const Format, mount_ctx: ?*anyopaque, owned: bool) void {
    copyName(&slot.name, name);
    slot.format = format;
    slot.mount_ctx = mount_ctx;
    slot.owned = owned;
    slot.native = isNative(format);
    slot.in_use = true;
}

fn canOpen(format: *const Format, mode: u8) c_int {
    if (mode != mode_read and mode != mode_write and mode != mode_append) return err_invalid_arg;
    if (mode == mode_read) return ok;
    if (format.caps.read_only or !format.caps.supports_streaming_write) return err_not_supported;
    if (format.ops.write == null) return err_not_supported;
    return ok;
}

fn freeFile() ?*File {
    for (&files) |*f| if (!f.in_use) return f;
    return null;
}

/// A handle from this table that is still open.
fn liveFile(file: *const File) bool {
    for (&files) |*f| if (f == file) return f.in_use;
    return false;
}

/// Unmount an owned slot, then zero it; returns the unmount status.
fn initSlot(slot: *Slot) c_int {
    var e: c_int = ok;
    if (slot.in_use and slot.owned) e = op(UnmountFn, slot.format.ops.unmount).?(slot.mount_ctx);
    slot.* = zeroSlot();
    return e;
}

export fn ra8_io_vfs_init_slot_test(slot: *Slot) c_int {
    return initSlot(slot);
}

pub export fn ra8_io_vfs_init() c_int {
    var first: c_int = ok;
    for (&files) |*f| f.* = .{};
    for (&table) |*slot| {
        const e = initSlot(slot);
        if (first == ok) first = e;
    }
    return first;
}

/// The checks mount and mount_auto share: a valid, unused name and a free slot.
fn claim(name: [*:0]const u8, out: **Slot) c_int {
    if (!nameOk(name)) return err_invalid_arg;
    if (priv_ra8_io_vfs_find(name, null) != null) return err_exists;
    out.* = freeMount() orelse return err_no_mem;
    return ok;
}

pub export fn ra8_io_vfs_mount(name: ?[*:0]const u8, mount: ?*FsMount) c_int {
    const n = name orelse return nullPtr("name must not be nullptr");
    const m = mount orelse return nullPtr("mount must not be nullptr");
    var slot: *Slot = undefined;
    const c = claim(n, &slot);
    if (c != ok) return c;
    var format: ?*const Format = null;
    const rc = ra8_io_fsfmt_get_builtin(m.type, &format);
    if (rc != ok) return logged(rc, "native type");
    storeMount(slot, n, format.?, m, false);
    return ok;
}

pub export fn ra8_io_vfs_mount_auto(name: ?[*:0]const u8, backend: ?*const anyopaque) c_int {
    const n = name orelse return nullPtr("name must not be nullptr");
    const b = backend orelse return nullPtr("backend must not be nullptr");
    var slot: *Slot = undefined;
    const c = claim(n, &slot);
    if (c != ok) return c;
    var format: ?*const Format = null;
    const probed = ra8_io_fsfmt_probe(b, &format);
    if (probed != ok) return logged(probed, "probe format");
    var ctx: ?*anyopaque = null;
    const mounted = op(MountFn, format.?.ops.mount).?(b, &ctx);
    if (mounted != ok) return logged(mounted, "mount format");
    storeMount(slot, n, format.?, ctx, true);
    return ok;
}

pub export fn ra8_io_vfs_unmount(name: ?[*:0]const u8) c_int {
    const n = name orelse return nullPtr("name must not be nullptr");
    var index: u8 = 0;
    const slot = priv_ra8_io_vfs_find(n, &index) orelse return err_not_found;
    for (&files) |*f| if (f.in_use and f.mount_index == index) return err_busy;
    var e: c_int = ok;
    if (slot.owned) e = op(UnmountFn, slot.format.ops.unmount).?(slot.mount_ctx);
    slot.* = zeroSlot();
    return e;
}

/// Resolve `path` for opening in `mode`; logs like RA8_RETURN_ON_ERROR.
fn openTarget(path: [*:0]const u8, index: ?*u8, out_slot: **Slot, out_sub: *[*:0]const u8) c_int {
    var slot: ?*Slot = null;
    var sub: ?[*:0]const u8 = null;
    const rc = priv_ra8_io_vfs_resolve(path, &slot, index, &sub);
    if (rc != ok) return logged(rc, "resolve");
    out_slot.* = slot.?;
    out_sub.* = sub.?;
    return ok;
}

fn dispatchOpen(slot: *Slot, sub: [*:0]const u8, mode: u8, out_ctx: *?*anyopaque) c_int {
    const rc = op(OpenFn, slot.format.ops.open).?(slot.mount_ctx, sub, mode, out_ctx);
    return if (rc != ok) logged(rc, "open") else ok;
}

pub export fn ra8_io_vfs_open(path: ?[*:0]const u8, mode: u8, out_file: ?*?*anyopaque) c_int {
    const p = path orelse return nullPtr("path must not be nullptr");
    const out = out_file orelse return nullPtr("out_file must not be nullptr");
    var slot: *Slot = undefined;
    var sub: [*:0]const u8 = undefined;
    var rc = openTarget(p, null, &slot, &sub);
    if (rc != ok) return rc;
    if (!slot.native) return err_not_supported;
    rc = canOpen(fmtOf(slot), mode);
    if (rc != ok) return logged(rc, "open capability");
    var ctx: ?*anyopaque = null;
    rc = dispatchOpen(slot, sub, mode, &ctx);
    if (rc != ok) return rc;
    out.* = ctx;
    return ok;
}

pub export fn ra8_io_vfs_file_open(path: ?[*:0]const u8, mode: u8, out_file: ?*?*File) c_int {
    const p = path orelse return nullPtr("path must not be nullptr");
    const out = out_file orelse return nullPtr("out_file must not be nullptr");
    var slot: *Slot = undefined;
    var sub: [*:0]const u8 = undefined;
    var index: u8 = 0;
    var rc = openTarget(p, &index, &slot, &sub);
    if (rc != ok) return rc;
    rc = canOpen(fmtOf(slot), mode);
    if (rc != ok) return logged(rc, "open capability");
    const file = freeFile() orelse return err_no_mem;
    var ctx: ?*anyopaque = null;
    rc = dispatchOpen(slot, sub, mode, &ctx);
    if (rc != ok) return rc;
    file.* = .{ .format = slot.format, .file_ctx = ctx, .mount_index = index, .in_use = true };
    out.* = file;
    return ok;
}

/// An open handle, or the error a NULL or stale one gets.
fn handle(file: ?*File) error{ Null, Stale }!*File {
    const f = file orelse return error.Null;
    if (!liveFile(f)) return error.Stale;
    return f;
}

fn handleErr(e: error{ Null, Stale }) c_int {
    return switch (e) {
        error.Null => nullPtr("file must not be nullptr"),
        error.Stale => err_invalid_state,
    };
}

pub export fn ra8_io_vfs_file_close(file: ?*File) c_int {
    const f = handle(file) catch |e| return handleErr(e);
    const e = op(CtxFn, f.format.?.ops.close).?(f.file_ctx);
    f.* = .{};
    return e;
}

pub export fn ra8_io_vfs_file_read(file: ?*File, buf: ?*anyopaque, bytes: u32, out_read: ?*u32) c_int {
    if (file == null) return nullPtr("file must not be nullptr");
    const b = buf orelse return nullPtr("buf must not be nullptr");
    const o = out_read orelse return nullPtr("out_read must not be nullptr");
    const f = handle(file) catch |e| return handleErr(e);
    return op(ReadFn, f.format.?.ops.read).?(f.file_ctx, b, bytes, o);
}

pub export fn ra8_io_vfs_file_write(file: ?*File, buf: ?*const anyopaque, bytes: u32) c_int {
    if (file == null) return nullPtr("file must not be nullptr");
    const b = buf orelse return nullPtr("buf must not be nullptr");
    const f = handle(file) catch |e| return handleErr(e);
    const fmt = f.format.?;
    if (fmt.caps.read_only or !fmt.caps.supports_streaming_write) return err_not_supported;
    const w = op(WriteFn, fmt.ops.write) orelse return err_not_supported;
    return w(f.file_ctx, b, bytes);
}

pub export fn ra8_io_vfs_file_seek(file: ?*File, offset_bytes: u64) c_int {
    const f = handle(file) catch |e| return handleErr(e);
    return op(SeekFn, f.format.?.ops.seek).?(f.file_ctx, offset_bytes);
}

/// tell and size: same checks, different op and out-name.
fn query(file: ?*File, out: ?*u64, comptime field: []const u8, comptime msg: [*:0]const u8) c_int {
    if (file == null) return nullPtr("file must not be nullptr");
    const o = out orelse return nullPtr(msg);
    const f = handle(file) catch |e| return handleErr(e);
    return op(QueryFn, @field(f.format.?.ops.*, field)).?(f.file_ctx, o);
}

pub export fn ra8_io_vfs_file_tell(file: ?*File, out_offset: ?*u64) c_int {
    return query(file, out_offset, "tell", "out_offset must not be nullptr");
}

pub export fn ra8_io_vfs_file_size(file: ?*File, out_bytes: ?*u64) c_int {
    return query(file, out_bytes, "size", "out_bytes must not be nullptr");
}

pub export fn ra8_io_vfs_file_sync(file: ?*File) c_int {
    const f = handle(file) catch |e| return handleErr(e);
    const fmt = f.format.?;
    if (!fmt.caps.supports_sync) return err_not_supported;
    const s = op(CtxFn, fmt.ops.sync) orelse return err_not_supported;
    return s(f.file_ctx);
}

pub export fn ra8_io_vfs_get_caps(name: ?[*:0]const u8, out: ?*Caps) c_int {
    const n = name orelse return nullPtr("name must not be nullptr");
    const o = out orelse return nullPtr("out must not be nullptr");
    const slot = priv_ra8_io_vfs_find(n, null) orelse return err_not_found;
    o.* = slot.format.caps;
    return ok;
}

pub export fn ra8_io_vfs_free_space(name: ?[*:0]const u8, out: ?*anyopaque) c_int {
    const n = name orelse return nullPtr("name must not be nullptr");
    const o = out orelse return nullPtr("out must not be nullptr");
    const slot = priv_ra8_io_vfs_find(n, null) orelse return err_not_found;
    if (!slot.format.caps.supports_free_space) return err_not_supported;
    const f = op(SpaceFn, slot.format.ops.free_space) orelse return err_not_supported;
    return f(slot.mount_ctx, o);
}
