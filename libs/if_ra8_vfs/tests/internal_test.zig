//! Zig-native tests for the pure adapter core: no VFS, no filesystem, no facade.

const std = @import("std");
const core = @import("implementation");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

fn mountBuffer(name: []const u8) [core.io_vfs_name_max]u8 {
    var buf = [_]u8{0} ** core.io_vfs_name_max;
    @memcpy(buf[0..name.len], name);
    return buf;
}

// --- timestamp -------------------------------------------------------------

test "timestamp: invalid native stays fully zeroed" {
    const native = core.NativeTimestamp{
        .value = .{ .year = 2026, .month = 9, .day = 17, .centisecond = 50 },
        .valid = false,
        .utc_offset_valid = true,
    };
    const portable = core.timestamp(&native);
    try expect(!portable.valid);
    try expect(!portable.utc_offset_valid);
    try expectEqual(@as(u16, 0), portable.value.year);
    try expectEqual(@as(u32, 0), portable.value.nanosecond);
}

test "timestamp: valid native converts centiseconds to nanoseconds" {
    const native = core.NativeTimestamp{
        .value = .{
            .year = 2026,
            .utc_offset_min = -300,
            .month = 9,
            .day = 17,
            .hour = 4,
            .minute = 5,
            .second = 6,
            .centisecond = 99,
        },
        .valid = true,
        .utc_offset_valid = true,
    };
    const portable = core.timestamp(&native);
    try expect(portable.valid);
    try expect(portable.utc_offset_valid);
    try expectEqual(@as(u32, 990_000_000), portable.value.nanosecond);
    try expectEqual(@as(u16, 2026), portable.value.year);
    try expectEqual(@as(i16, -300), portable.value.utc_offset_min);
    try expectEqual(@as(u8, 9), portable.value.month);
    try expectEqual(@as(u8, 17), portable.value.day);
    try expectEqual(@as(u8, 4), portable.value.hour);
    try expectEqual(@as(u8, 5), portable.value.minute);
    try expectEqual(@as(u8, 6), portable.value.second);
}

test "timestamp: UTC-offset validity is independent of overall validity" {
    const native = core.NativeTimestamp{
        .value = .{ .year = 1980, .utc_offset_min = 0 },
        .valid = true,
        .utc_offset_valid = false,
    };
    const portable = core.timestamp(&native);
    try expect(portable.valid);
    try expect(!portable.utc_offset_valid);
}

test "timestamp: zero centiseconds yields zero nanoseconds" {
    const native = core.NativeTimestamp{ .value = .{ .year = 2000 }, .valid = true };
    try expectEqual(@as(u32, 0), core.timestamp(&native).value.nanosecond);
}

// --- len -------------------------------------------------------------------

test "len: stops at the first NUL" {
    try expectEqual(@as(u16, 3), core.len("abc", 64));
}

test "len: empty string is zero" {
    try expectEqual(@as(u16, 0), core.len("", 64));
}

test "len: unterminated input returns the cap" {
    const raw = [_]u8{'x'} ** 8;
    try expectEqual(@as(u16, 8), core.len(&raw, 8));
}

test "len: a zero cap never reads" {
    try expectEqual(@as(u16, 0), core.len("abc", 0));
}

// --- fullPath --------------------------------------------------------------

test "fullPath: prefixes the mount name and a colon" {
    const mount = mountBuffer("ram");
    var out = [_]u8{0xAA} ** core.full_path_cap;
    try expectEqual(core.ok, core.fullPath(&mount, "/books/a.cbz", &out));
    try expect(std.mem.eql(u8, out[0..16], "ram:/books/a.cbz"));
    try expectEqual(@as(u8, 0), out[16]);
}

test "fullPath: root path becomes name:/" {
    const mount = mountBuffer("sd0");
    var out = [_]u8{0} ** core.full_path_cap;
    try expectEqual(core.ok, core.fullPath(&mount, "/", &out));
    try expect(std.mem.eql(u8, out[0..5], "sd0:/"));
    try expectEqual(@as(u8, 0), out[5]);
}

test "fullPath: an unterminated mount name is invalid_state" {
    const mount = [_]u8{'m'} ** core.io_vfs_name_max;
    var out = [_]u8{0} ** core.full_path_cap;
    try expectEqual(core.err_invalid_state, core.fullPath(&mount, "/a", &out));
}

test "fullPath: an unterminated path is invalid_size" {
    const mount = mountBuffer("ram");
    var path = [_]u8{'p'} ** core.fw_path_cap;
    var out = [_]u8{0} ** core.full_path_cap;
    try expectEqual(core.err_invalid_size, core.fullPath(&mount, &path, &out));
}

test "fullPath: the longest legal path still fits the scratch" {
    const mount = mountBuffer("mountnamefifte");
    var path = [_]u8{'a'} ** core.fw_path_cap;
    path[0] = '/';
    path[core.fw_path_cap - 2] = 0;
    var out = [_]u8{0} ** core.full_path_cap;
    try expectEqual(core.ok, core.fullPath(&mount, &path, &out));
    try expectEqual(@as(u16, 14 + 1 + core.fw_path_cap - 2), core.len(&out, core.full_path_cap));
}

// --- dirLayout -------------------------------------------------------------

test "dirLayout: an already aligned base consumes exactly the cursor" {
    const cursor_align = @alignOf(core.DirectoryState);
    const layout = core.dirLayout(0x1000, @intCast(cursor_align));
    try expectEqual(@as(usize, 0x1000), layout.cursor);
    try expectEqual(@as(usize, @sizeOf(core.DirectoryState)), layout.cursor_end);
    try expectEqual(@as(usize, @sizeOf(core.DirectoryState)), layout.consumed);
}

test "dirLayout: an unaligned base rounds the cursor up" {
    const layout = core.dirLayout(0x1001, 1);
    try expectEqual(@as(usize, 0), layout.cursor % @alignOf(core.DirectoryState));
    try expect(layout.cursor > 0x1001);
    try expectEqual(layout.cursor + @sizeOf(core.DirectoryState) - 0x1001, layout.consumed);
}

test "dirLayout: a wide native alignment pushes the workspace out" {
    const layout = core.dirLayout(0x2000, 64);
    try expectEqual(@as(usize, 0), layout.workspace % 64);
    try expect(layout.workspace >= layout.cursor + @sizeOf(core.DirectoryState));
    try expectEqual(layout.workspace - 0x2000, layout.consumed);
}

test "dirLayout: a zero native alignment wraps exactly as the C did" {
    const layout = core.dirLayout(0x3000, 0);
    try expectEqual(@as(usize, 0), layout.workspace);
    try expect(layout.consumed > 0x3000);
}

test "dirCursorBase: matches the cursor under the cursor's own alignment" {
    try expectEqual(core.dirLayout(0x4007, 1).cursor, core.dirCursorBase(0x4007));
}

// --- hex6 / stagePath ------------------------------------------------------

test "hex6: renders six uppercase digits, most significant first" {
    var out = [_]u8{0} ** core.stage_hex_digits;
    core.hex6(&out, 0xABCDEF);
    try expect(std.mem.eql(u8, &out, "ABCDEF"));
}

test "hex6: pads a small value with leading zeroes" {
    var out = [_]u8{0} ** core.stage_hex_digits;
    core.hex6(&out, 1);
    try expect(std.mem.eql(u8, &out, "000001"));
}

test "stagePath: replaces the leaf with an 8.3 sibling" {
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.ok, core.stagePath("/books/a.cbz", 0x2A, &out));
    try expect(std.mem.eql(u8, out[0..19], "/books/TX00002A.TMP"));
    try expectEqual(@as(u8, 0), out[19]);
}

test "stagePath: a root-level destination stages beside it" {
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.ok, core.stagePath("/a.bin", 1, &out));
    try expect(std.mem.eql(u8, out[0..13], "/TX000001.TMP"));
}

test "stagePath: the identifier is masked to six hex digits" {
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.ok, core.stagePath("/a", 0xFF123456, &out));
    try expect(std.mem.eql(u8, out[0..13], "/TX123456.TMP"));
}

test "stagePath: an unterminated destination is invalid_size" {
    const destination = [_]u8{'d'} ** core.fw_path_cap;
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.err_invalid_size, core.stagePath(&destination, 0, &out));
}

test "stagePath: a leaf too deep for the stage name is invalid_size" {
    var destination = [_]u8{'d'} ** core.fw_path_cap;
    destination[0] = '/';
    destination[core.fw_path_cap - 12] = '/';
    destination[core.fw_path_cap - 2] = 0;
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.err_invalid_size, core.stagePath(&destination, 0, &out));
}

test "stagePath: the deepest directory that still fits is accepted" {
    var destination = [_]u8{'d'} ** core.fw_path_cap;
    destination[0] = '/';
    destination[core.fw_path_cap - 14] = '/';
    destination[core.fw_path_cap - 2] = 0;
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.ok, core.stagePath(&destination, 0, &out));
    try expectEqual(@as(u16, core.fw_path_cap - 1), core.len(&out, core.fw_path_cap));
}

// --- copyPath / mountName --------------------------------------------------

test "copyPath: copies through the terminator" {
    var out = [_]u8{0xFF} ** core.fw_path_cap;
    try expectEqual(core.ok, core.copyPath(&out, "/x/y"));
    try expect(std.mem.eql(u8, out[0..5], "/x/y\x00"));
}

test "copyPath: an unterminated path is invalid_size" {
    const path = [_]u8{'p'} ** core.fw_path_cap;
    var out = [_]u8{0} ** core.fw_path_cap;
    try expectEqual(core.err_invalid_size, core.copyPath(&out, &path));
}

test "mountName: accepts a bounded separator-free name" {
    var out = [_]u8{0xFF} ** core.io_vfs_name_max;
    try expectEqual(core.ok, core.mountName(&out, "ram"));
    try expect(std.mem.eql(u8, out[0..4], "ram\x00"));
}

test "mountName: rejects an empty name" {
    var out = [_]u8{0} ** core.io_vfs_name_max;
    try expectEqual(core.err_invalid_arg, core.mountName(&out, ""));
}

test "mountName: rejects a colon" {
    var out = [_]u8{0} ** core.io_vfs_name_max;
    try expectEqual(core.err_invalid_arg, core.mountName(&out, "ra:m"));
}

test "mountName: rejects a slash" {
    var out = [_]u8{0} ** core.io_vfs_name_max;
    try expectEqual(core.err_invalid_arg, core.mountName(&out, "ra/m"));
}

test "mountName: rejects an unterminated name" {
    const name = [_]u8{'n'} ** core.io_vfs_name_max;
    var out = [_]u8{0} ** core.io_vfs_name_max;
    try expectEqual(core.err_invalid_arg, core.mountName(&out, &name));
}

test "mountName: the longest legal name is fifteen bytes plus NUL" {
    var out = [_]u8{0} ** core.io_vfs_name_max;
    try expectEqual(core.ok, core.mountName(&out, "mountnamefiftee"));
    try expectEqual(@as(u8, 0), out[15]);
}

// --- modeNative ------------------------------------------------------------

test "modeNative: read, write-truncate and append have native equivalents" {
    var native: u8 = 0xFF;
    try expectEqual(core.ok, core.modeNative(core.open_read, &native));
    try expectEqual(core.fs_mode_read, native);
    try expectEqual(core.ok, core.modeNative(core.open_write_truncate, &native));
    try expectEqual(core.fs_mode_write, native);
    try expectEqual(core.ok, core.modeNative(core.open_append, &native));
    try expectEqual(core.fs_mode_append, native);
}

test "modeNative: create-new has no faithful equivalent and leaves the output" {
    var native: u8 = 0x7F;
    try expectEqual(core.err_not_supported, core.modeNative(core.open_create_new, &native));
    try expectEqual(@as(u8, 0x7F), native);
}

// --- requirements ----------------------------------------------------------

test "requirements: adds the wrapper and the alignment slack" {
    const got = core.requirements(640, 8).?;
    try expectEqual(@as(u32, @sizeOf(core.DirectoryState) + 8 - 1 + 640), got.bytes);
}

test "requirements: the outer alignment is the wider of the two" {
    const cursor_align: u8 = @intCast(@alignOf(core.DirectoryState));
    try expectEqual(@as(u8, 64), core.requirements(16, 64).?.alignment);
    try expectEqual(cursor_align, core.requirements(16, 1).?.alignment);
}

test "requirements: an unrepresentable total is refused" {
    try expect(core.requirements(0xFFFF_FFFF, 8) == null);
}

test "requirements: a degenerate zero alignment keeps the C arithmetic" {
    // The C computed sizeof + align - 1 + bytes in uint64_t, so a zero
    // alignment simply loses one byte of slack rather than overflowing. The
    // capacity guards in dir_open are what actually refuse such a format.
    const got = core.requirements(0, 0).?;
    try expectEqual(@as(u32, @sizeOf(core.DirectoryState) - 1), got.bytes);
    try expectEqual(@as(u8, @intCast(@alignOf(core.DirectoryState))), got.alignment);
}

// --- capabilities ----------------------------------------------------------

test "capabilities: FAT caps the file size and the component name" {
    const caps = core.capabilities(false, false, 700, 8, 3);
    try expectEqual(core.fs_fat_max_file_bytes, caps.max_file_bytes);
    try expectEqual(core.fat_name_max_bytes, caps.name_max_bytes);
    try expectEqual(@as(u16, 3), caps.max_open_directories);
    try expectEqual(@as(u32, 700), caps.directory_workspace_bytes);
    try expectEqual(@as(u8, 8), caps.directory_workspace_align);
    try expectEqual(core.fw_path_cap, caps.path_max_bytes);
    try expectEqual(core.fs_max_files, caps.max_open_files);
}

test "capabilities: exFAT lifts the file size and tightens the name" {
    const caps = core.capabilities(true, false, 0, 1, 1);
    try expectEqual(std.math.maxInt(u64), caps.max_file_bytes);
    try expectEqual(core.exfat_name_max_bytes, caps.name_max_bytes);
}

test "capabilities: the advertised flags match the services actually bound" {
    const caps = core.capabilities(false, false, 0, 1, 1);
    const expected = core.cap_namespace | core.cap_stream | core.cap_space_query |
        core.cap_same_volume_rename | core.cap_atomic_noreplace | core.cap_transactions |
        core.cap_rejects_symlink_walk | core.cap_created_time | core.cap_modified_time |
        core.cap_accessed_time;
    try expectEqual(expected, caps.flags);
    try expectEqual(@as(u32, 0), caps.flags & core.cap_atomic_replace);
    try expectEqual(@as(u32, 0), caps.flags & core.cap_file_sync);
    try expectEqual(@as(u32, 0), caps.flags & core.cap_symlinks);
    try expectEqual(@as(u32, 0), caps.flags & core.cap_removable_media);
}

test "capabilities: removable media follows the caller configuration" {
    const caps = core.capabilities(false, true, 0, 1, 1);
    try expect((caps.flags & core.cap_removable_media) != 0);
}

test "capabilities: the workspace sizes are the backend state sizes" {
    const caps = core.capabilities(false, false, 0, 1, 1);
    try expectEqual(@as(u32, @sizeOf(core.FileState)), caps.file_workspace_bytes);
    try expectEqual(@as(u32, @sizeOf(core.TransactionState)), caps.transaction_workspace_bytes);
    try expectEqual(@as(u8, @alignOf(core.FileState)), caps.file_workspace_align);
    try expectEqual(@as(u8, @alignOf(core.TransactionState)), caps.transaction_workspace_align);
}

// --- ListState -------------------------------------------------------------

const Collector = struct {
    seen: u32 = 0,
    stop_after: u32 = 0xFFFF_FFFF,
    fail_at: u32 = 0xFFFF_FFFF,
    last_kind: u8 = 0,
    last_size: u64 = 0,
    last_name_bytes: u16 = 0,

    fn callback(ctx: ?*anyopaque, entry: *const core.Dirent, out_continue: *bool) callconv(.c) u16 {
        const self: *Collector = @ptrCast(@alignCast(ctx.?));
        self.seen += 1;
        self.last_kind = entry.kind;
        self.last_size = entry.size_bytes;
        self.last_name_bytes = entry.name_bytes;
        if (self.seen == self.fail_at) return core.err_invalid_state;
        if (self.seen >= self.stop_after) out_continue.* = false;
        return core.ok;
    }
};

fn bridgeFor(collector: *Collector, max_entries: u32) core.ListState {
    return .{
        .callback = &Collector.callback,
        .callback_ctx = collector,
        .max_entries = max_entries,
    };
}

test "ListState: classifies a file and a directory from the attribute byte" {
    var collector = Collector{};
    var bridge = bridgeFor(&collector, 8);
    bridge.entry("a.cbz", 0x20, 1234);
    try expectEqual(core.node_file, collector.last_kind);
    try expectEqual(@as(u64, 1234), collector.last_size);
    try expectEqual(@as(u16, 5), collector.last_name_bytes);
    bridge.entry("books", core.fs_attr_directory, 0);
    try expectEqual(core.node_directory, collector.last_kind);
    try expectEqual(@as(u32, 2), bridge.count);
    try expect(!bridge.stopped);
}

test "ListState: the budget stops delivery without an error" {
    var collector = Collector{};
    var bridge = bridgeFor(&collector, 2);
    bridge.entry("a", 0, 0);
    bridge.entry("b", 0, 0);
    bridge.entry("c", 0, 0);
    try expectEqual(@as(u32, 2), bridge.count);
    try expectEqual(@as(u32, 2), collector.seen);
    try expect(bridge.stopped);
    try expectEqual(core.ok, bridge.callback_error);
}

test "ListState: a callback stop halts further delivery" {
    var collector = Collector{ .stop_after = 1 };
    var bridge = bridgeFor(&collector, 8);
    bridge.entry("a", 0, 0);
    bridge.entry("b", 0, 0);
    try expectEqual(@as(u32, 1), collector.seen);
    try expect(bridge.stopped);
    try expectEqual(core.ok, bridge.callback_error);
}

test "ListState: a callback error is recorded and stops delivery" {
    var collector = Collector{ .fail_at = 1 };
    var bridge = bridgeFor(&collector, 8);
    bridge.entry("a", 0, 0);
    bridge.entry("b", 0, 0);
    try expectEqual(core.err_invalid_state, bridge.callback_error);
    try expect(bridge.stopped);
    try expectEqual(@as(u32, 1), bridge.count);
}

test "ListState: an overlong native name fails closed without a callback" {
    var collector = Collector{};
    var bridge = bridgeFor(&collector, 8);
    const long = [_]u8{'n'} ** (core.fw_path_cap + 4);
    bridge.entry(@ptrCast(&long), 0, 0);
    try expectEqual(core.err_invalid_size, bridge.callback_error);
    try expect(bridge.stopped);
    try expectEqual(@as(u32, 0), collector.seen);
}

test "ListState: a zero budget delivers nothing" {
    var collector = Collector{};
    var bridge = bridgeFor(&collector, 0);
    bridge.entry("a", 0, 0);
    try expectEqual(@as(u32, 0), collector.seen);
    try expect(bridge.stopped);
}
