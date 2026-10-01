//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `if_ra8_vfs` namespace half of the adapter: the
//! init handshake, the namespace operations, the bounded listdir walk and the
//! directory cursors, all driven over the fake medium in `vfs_fake.zig`.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;
const fake = @import("vfs_fake.zig");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

const Fixture = fake.Fixture;
const addNode = fake.addNode;
const findNode = fake.findNode;
const mountedFat = fake.mountedFat;
const names = fake.names;
const pathEq = fake.pathEq;
const resetMedium = fake.resetMedium;
const streams = fake.streams;
const txns = fake.txns;

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

test "init: every required pointer is checked before anything is touched" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = 16 };
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(null, &fixture.state, &cfg));
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(&fixture.fs, null, &cfg));
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, null));
    try expectEqual(@as(u32, 0), fake.g_bind_calls);
}

test "init: a missing mount name or mount is null_ptr" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    var cfg = abi.Config{ .mount_name = null, .mount = &fixture.mount };
    try expectEqual(
        core.err_null_ptr,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
    cfg = .{ .mount_name = "ram", .mount = null };
    try expectEqual(
        core.err_null_ptr,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: an unmounted volume is not_initialized" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 0 };
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_not_initialized,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: a mount name with a separator is invalid_arg" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    const cfg = abi.Config{ .mount_name = "ra/m", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
    try expectEqual(@as(u32, 0), fake.g_bind_calls);
}

test "init: a directory-requirements failure is returned verbatim" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    fake.g_requirements_rc = core.err_not_supported;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_not_supported,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: an unrepresentable workspace total is invalid_size" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    fake.g_req_bytes = 0xFFFF_FFFF;
    fake.g_req_align = 8;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_size,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: requirements are queried against the bound root" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expect(pathEq(&fake.g_last_path, "ram:/"));
    try expectEqual(@as(u32, 640), fixture.state.directory_workspace_bytes);
    try expectEqual(@as(u8, 8), fixture.state.directory_workspace_align);
    try expectEqual(@as(u16, 2), fixture.state.max_open_directories);
}

test "init: FAT capabilities reach fw_fs_bind with the adapter as context" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(@as(u32, 1), fake.g_bind_calls);
    try expectEqual(@as(?*anyopaque, @ptrCast(&fixture.state)), fake.g_bound_ctx);
    try expectEqual(core.fs_fat_max_file_bytes, fake.g_bound_caps.max_file_bytes);
    try expectEqual(core.fat_name_max_bytes, fake.g_bound_caps.name_max_bytes);
    try expectEqual(@as(u32, @sizeOf(core.DirectoryState) + 8 - 1 + 640), fake.g_bound_caps.directory_workspace_bytes);
    try expectEqual(@as(u32, 0), fake.g_bound_caps.flags & core.cap_removable_media);
}

test "init: exFAT and removable media change only what they should" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = core.fs_type_exfat };
    const cfg = abi.Config{
        .mount_name = "sd0",
        .mount = &fixture.mount,
        .removable_media = true,
    };
    try expectEqual(core.ok, abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg));
    try expectEqual(std.math.maxInt(u64), fake.g_bound_caps.max_file_bytes);
    try expectEqual(core.exfat_name_max_bytes, fake.g_bound_caps.name_max_bytes);
    try expect((fake.g_bound_caps.flags & core.cap_removable_media) != 0);
    try expect(fixture.state.removable_media);
}

test "init: a bind failure is returned verbatim" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    fake.g_bind_rc = core.err_invalid_arg;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

// ---------------------------------------------------------------------------
// namespace
// ---------------------------------------------------------------------------

test "stat: a hit reports size, type and translated timestamps" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books/a.cbz", false, "hello");
    var out: core.Stat = .{};
    try expectEqual(core.ok, names().stat.?(fake.g_bound_ctx, "/books/a.cbz", &out));
    try expect(pathEq(&fake.g_last_path, "ram:/books/a.cbz"));
    try expect(out.exists);
    try expectEqual(core.node_file, out.kind);
    try expectEqual(@as(u64, 5), out.size_bytes);
    try expect(out.created.valid);
    try expectEqual(@as(u32, 250_000_000), out.created.value.nanosecond);
    try expect(!out.modified.valid);
}

test "stat: a directory is classified as a directory" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books", true, "");
    var out: core.Stat = .{};
    try expectEqual(core.ok, names().stat.?(fake.g_bound_ctx, "/books", &out));
    try expectEqual(core.node_directory, out.kind);
}

test "stat: a clean miss is success with no node kind" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var out: core.Stat = .{ .kind = core.node_file, .exists = true };
    try expectEqual(core.ok, names().stat.?(fake.g_bound_ctx, "/nope", &out));
    try expect(!out.exists);
    try expectEqual(core.node_none, out.kind);
}

test "stat: a native failure is returned without writing the output" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    fake.g_stat_rc = core.err_invalid_state;
    var out: core.Stat = .{ .size_bytes = 77 };
    try expectEqual(core.err_invalid_state, names().stat.?(fake.g_bound_ctx, "/x", &out));
    try expectEqual(@as(u64, 77), out.size_bytes);
}

test "mkdir, unlink and rmdir all dispatch on the prefixed path" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(core.ok, names().mkdir.?(fake.g_bound_ctx, "/books"));
    try expect(pathEq(&fake.g_last_path, "ram:/books"));
    _ = addNode("ram:/books/a.cbz", false, "x");
    try expectEqual(core.ok, names().unlink.?(fake.g_bound_ctx, "/books/a.cbz"));
    try expect(pathEq(&fake.g_last_path, "ram:/books/a.cbz"));
    try expectEqual(core.ok, names().rmdir.?(fake.g_bound_ctx, "/books"));
    try expect(pathEq(&fake.g_last_path, "ram:/books"));
    try expectEqual(@as(u32, 1), fake.g_mkdir_calls);
    try expectEqual(@as(u32, 1), fake.g_unlink_calls);
    try expectEqual(@as(u32, 1), fake.g_rmdir_calls);
}

test "rename: a replacement request is refused without touching the volume" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(
        core.err_not_supported,
        names().rename.?(fake.g_bound_ctx, "/a", "/b", true),
    );
    try expectEqual(@as(u32, 0), fake.g_rename_calls);
}

test "rename: both paths are built in separate scratch buffers" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "x");
    try expectEqual(core.ok, names().rename.?(fake.g_bound_ctx, "/a", "/b", false));
    try expect(pathEq(&fake.g_last_path, "ram:/a"));
    try expect(pathEq(&fake.g_last_path_b, "ram:/b"));
}

test "space: the three portable fields are copied from the live mount" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var out: core.Space = .{};
    try expectEqual(core.ok, names().space.?(fake.g_bound_ctx, &out));
    try expect(pathEq(&fake.g_last_path, "ram"));
    try expectEqual(@as(u64, 4096), out.total_bytes);
    try expectEqual(@as(u64, 1024), out.free_bytes);
    try expectEqual(@as(u64, 3072), out.used_bytes);
}

test "space: a native failure leaves the output alone" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    fake.g_space_rc = core.err_invalid_state;
    var out: core.Space = .{ .total_bytes = 9 };
    try expectEqual(core.err_invalid_state, names().space.?(fake.g_bound_ctx, &out));
    try expectEqual(@as(u64, 9), out.total_bytes);
}

// ---------------------------------------------------------------------------
// listdir
// ---------------------------------------------------------------------------

const Collector = struct {
    seen: u32 = 0,
    stop_after: u32 = 0xFFFF_FFFF,
    fail_at: u32 = 0xFFFF_FFFF,

    fn callback(ctx: ?*anyopaque, entry: *const core.Dirent, out_continue: *bool) callconv(.c) u16 {
        const self: *Collector = @ptrCast(@alignCast(ctx.?));
        _ = entry;
        self.seen += 1;
        if (self.seen == self.fail_at) return core.err_invalid_state;
        if (self.seen >= self.stop_after) out_continue.* = false;
        return core.ok;
    }
};

test "listdir: the budget bounds delivery and reports an incomplete walk" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    _ = addNode("ram:/b", false, "22");
    _ = addNode("ram:/c", false, "333");
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = true;
    try expectEqual(core.ok, names().listdir.?(
        fake.g_bound_ctx,
        "/",
        2,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 2), count);
    try expectEqual(@as(u32, 2), collector.seen);
    try expect(!complete);
}

test "listdir: a complete walk within budget reports complete" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = false;
    try expectEqual(core.ok, names().listdir.?(
        fake.g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 1), count);
    try expect(complete);
}

test "listdir: a callback error outranks the native result" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    fake.g_listdir_rc = core.err_not_supported;
    var collector = Collector{ .fail_at = 1 };
    var count: u32 = 0;
    var complete: bool = true;
    try expectEqual(core.err_invalid_state, names().listdir.?(
        fake.g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expect(!complete);
}

test "listdir: the native result stands when the callback is clean" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    fake.g_listdir_rc = core.err_not_supported;
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = false;
    try expectEqual(core.err_not_supported, names().listdir.?(
        fake.g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 0), count);
    try expect(complete);
}

// ---------------------------------------------------------------------------
// directory cursors
// ---------------------------------------------------------------------------

test "dir_open: a span too small for the cursor is no_mem" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.err_no_mem, names().dir_open.?(fake.g_bound_ctx, "/", &span, 4));
}

test "dir_open: a span too small for the format workspace is no_mem" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    const just_the_cursor: u32 = @sizeOf(core.DirectoryState) + 4;
    try expectEqual(core.err_no_mem, names().dir_open.?(fake.g_bound_ctx, "/", &span, just_the_cursor));
}

test "dir_open: the format workspace lands past the cursor, correctly aligned" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(fake.g_bound_ctx, "/", &span, span.len));
    try expect(pathEq(&fake.g_last_path, "ram:/"));
    try expect(fake.g_last_dir_workspace >= @intFromPtr(&span) + @sizeOf(core.DirectoryState));
    try expectEqual(@as(usize, 0), fake.g_last_dir_workspace % 8);
    try expectEqual(@as(u32, span.len) - @as(u32, @intCast(fake.g_last_dir_workspace - @intFromPtr(&span))), fake.g_last_dir_bytes);
}

test "dir_next: an entry is copied and classified, then the walk ends cleanly" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books", true, "");
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(fake.g_bound_ctx, "/", &span, span.len));
    var value: core.DirentValue = std.mem.zeroes(core.DirentValue);
    var have: bool = false;
    try expectEqual(core.ok, names().dir_next.?(fake.g_bound_ctx, &span, &value, &have));
    try expect(have);
    try expectEqual(core.node_directory, value.kind);
    try expectEqual(@as(u16, 10), value.name_bytes);
    try expect(std.mem.eql(u8, value.name[0..10], "ram:/books"));
    try expectEqual(core.ok, names().dir_next.?(fake.g_bound_ctx, &span, &value, &have));
    try expect(!have);
}

test "dir_close: the cursor is consumed through the same span" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(fake.g_bound_ctx, "/", &span, span.len));
    try expectEqual(core.ok, names().dir_close.?(fake.g_bound_ctx, &span));
    const cursor: *core.DirectoryState = @ptrFromInt(core.dirCursorBase(@intFromPtr(&span)));
    try expect(!cursor.native.is_open);
}
