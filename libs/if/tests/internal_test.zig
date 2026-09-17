//! Unit tests for the pure filesystem-interface core: workspace arithmetic,
//! portable path syntax, backend-answer coherence and bind-time validation.

const std = @import("std");
const core = @import("implementation");

fn baseCaps() core.Caps {
    return .{
        .max_file_bytes = 1 << 30,
        .flags = core.cap_namespace | core.cap_stream,
        .file_workspace_bytes = 32,
        .directory_workspace_bytes = 64,
        .transaction_workspace_bytes = 48,
        .path_max_bytes = 256,
        .name_max_bytes = 64,
        .max_open_files = 4,
        .max_open_directories = 2,
        .file_workspace_align = 4,
        .directory_workspace_align = 4,
        .transaction_workspace_align = 4,
    };
}

fn validate(caps: *const core.Caps, path: []const u8) core.Err {
    return core.pathValidate(caps, path.ptr);
}

test "power of two rejects zero and non powers" {
    try std.testing.expect(!core.powerOfTwo(0));
    try std.testing.expect(core.powerOfTwo(1));
    try std.testing.expect(core.powerOfTwo(2));
    try std.testing.expect(!core.powerOfTwo(3));
    try std.testing.expect(core.powerOfTwo(256));
    try std.testing.expect(!core.powerOfTwo(0x8000_0001));
    try std.testing.expect(core.powerOfTwo(0x8000_0000));
}

test "workspace guard order is null, size, alignment metadata, address" {
    try std.testing.expectEqual(core.err_null_ptr, core.workspace(0, 64, 32, 4));
    try std.testing.expectEqual(core.err_no_mem, core.workspace(0x1000, 16, 32, 4));
    try std.testing.expectEqual(core.err_invalid_state, core.workspace(0x1000, 64, 32, 0));
    try std.testing.expectEqual(core.err_invalid_state, core.workspace(0x1000, 64, 32, 6));
    try std.testing.expectEqual(core.err_invalid_arg, core.workspace(0x1002, 64, 32, 4));
    try std.testing.expectEqual(core.ok, core.workspace(0x1000, 64, 32, 4));
}

test "workspace accepts an exactly sized region" {
    try std.testing.expectEqual(core.ok, core.workspace(0x2000, 32, 32, 8));
    try std.testing.expectEqual(core.err_no_mem, core.workspace(0x2000, 31, 32, 8));
}

test "component guard rejects empty, dot and dotdot" {
    const path = "/a/./../ab";
    try std.testing.expectEqual(core.err_invalid_arg, core.componentStatus(path, 1, 0));
    try std.testing.expectEqual(core.err_access_denied, core.componentStatus(path, 3, 1));
    try std.testing.expectEqual(core.err_access_denied, core.componentStatus(path, 5, 2));
    try std.testing.expectEqual(core.ok, core.componentStatus(path, 1, 1));
    try std.testing.expectEqual(core.ok, core.componentStatus(path, 8, 2));
}

test "a single dot only trips at length one" {
    const path = "/.x";
    try std.testing.expectEqual(core.ok, core.componentStatus(path, 1, 2));
}

test "caps sanity is judged before the path itself" {
    var caps = baseCaps();
    caps.path_max_bytes = 1;
    try std.testing.expectEqual(core.err_invalid_state, validate(&caps, "/a"));
    caps.path_max_bytes = core.path_cap + 1;
    try std.testing.expectEqual(core.err_invalid_state, validate(&caps, "/a"));
    caps = baseCaps();
    caps.name_max_bytes = 0;
    try std.testing.expectEqual(core.err_invalid_state, validate(&caps, "/a"));
}

test "root and ordinary paths validate" {
    const caps = baseCaps();
    try std.testing.expectEqual(core.ok, validate(&caps, "/"));
    try std.testing.expectEqual(core.ok, validate(&caps, "/a"));
    try std.testing.expectEqual(core.ok, validate(&caps, "/dir/file.bin"));
    try std.testing.expectEqual(core.err_invalid_arg, validate(&caps, "relative"));
}

test "path syntax rejects the portable hazards" {
    const caps = baseCaps();
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/a/../b"));
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/a/./b"));
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/c:/b"));
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/a\\b"));
    try std.testing.expectEqual(core.err_invalid_arg, validate(&caps, "/a\tb"));
    try std.testing.expectEqual(core.err_invalid_arg, validate(&caps, "/a\x7fb"));
    try std.testing.expectEqual(core.err_invalid_arg, validate(&caps, "/a//b"));
    try std.testing.expectEqual(core.err_invalid_arg, validate(&caps, "/a/"));
}

test "a trailing dot component is rejected at the NUL" {
    const caps = baseCaps();
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/a/."));
    try std.testing.expectEqual(core.err_access_denied, validate(&caps, "/a/.."));
}

test "component length is capped by name_max_bytes" {
    var caps = baseCaps();
    caps.name_max_bytes = 3;
    try std.testing.expectEqual(core.ok, validate(&caps, "/abc"));
    try std.testing.expectEqual(core.err_invalid_size, validate(&caps, "/abcd"));
    try std.testing.expectEqual(core.ok, validate(&caps, "/abc/abc"));
    try std.testing.expectEqual(core.err_invalid_size, validate(&caps, "/abc/abcd"));
}

test "an unterminated path runs out at path_max_bytes" {
    var caps = baseCaps();
    caps.path_max_bytes = 8;
    caps.name_max_bytes = 64;
    try std.testing.expectEqual(core.err_invalid_size, validate(&caps, "/abcdefghij"));
    try std.testing.expectEqual(core.ok, validate(&caps, "/abcdef"));
}

test "stat coherence predicates are individually reachable" {
    var value: core.Stat = .{ .exists = true, .node_type = 9 };
    try std.testing.expect(core.statTypeInvalid(&value));
    value = .{ .exists = false, .node_type = core.node_file };
    try std.testing.expect(core.statMissingInvalid(&value));
    value = .{ .exists = false, .node_type = core.node_none, .size_bytes = 7 };
    try std.testing.expect(core.statMissingInvalid(&value));
    value = .{ .exists = true, .node_type = core.node_none };
    try std.testing.expect(core.statPresentInvalid(&value));
    value = .{ .exists = true, .node_type = core.node_directory, .size_bytes = 1 };
    try std.testing.expect(core.statDirectoryInvalid(&value));
}

test "a coherent stat answer survives" {
    var value: core.Stat = .{ .exists = true, .node_type = core.node_file, .size_bytes = 12 };
    try std.testing.expect(!core.statIncoherent(&value));
    value = .{ .exists = false, .node_type = core.node_none, .size_bytes = 0 };
    try std.testing.expect(!core.statIncoherent(&value));
    value = .{ .exists = true, .node_type = core.node_directory, .size_bytes = 0 };
    try std.testing.expect(!core.statIncoherent(&value));
}

test "space coherence compares both halves against the total" {
    var value: core.Space = .{ .total_bytes = 100, .free_bytes = 40, .used_bytes = 60 };
    try std.testing.expect(!core.spaceIncoherent(&value));
    value.free_bytes = 101;
    try std.testing.expect(core.spaceIncoherent(&value));
    value = .{ .total_bytes = 100, .used_bytes = 101 };
    try std.testing.expect(core.spaceIncoherent(&value));
}

fn entryNamed(name: []const u8, node_type: u8, size: u64) core.DirentValue {
    var entry: core.DirentValue = .{ .node_type = node_type, .size_bytes = size };
    @memcpy(entry.name[0..name.len], name);
    entry.name_bytes = @intCast(name.len);
    return entry;
}

test "a well formed directory entry is accepted" {
    const caps = baseCaps();
    const entry = entryNamed("report.txt", core.node_file, 42);
    try std.testing.expectEqual(core.ok, core.entryStatus(&caps, &entry));
}

test "entry guards catch every cursor contract break" {
    const caps = baseCaps();
    var entry = entryNamed("ok", core.node_file, 1);
    entry.name_bytes = 0;
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));

    entry = entryNamed("ok", core.node_file, 1);
    entry.name_bytes = caps.name_max_bytes + 1;
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));

    entry = entryNamed("ok", core.node_file, 1);
    entry.name_bytes = 3;
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));

    entry = entryNamed("ok", core.node_none, 0);
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));

    entry = entryNamed("ok", 5, 0);
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));

    entry = entryNamed("sub", core.node_directory, 8);
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));
}

test "an embedded NUL makes the copied name incoherent" {
    const caps = baseCaps();
    var entry = entryNamed("ab", core.node_file, 1);
    entry.name[1] = 0;
    entry.name_bytes = 2;
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));
}

test "a leaf that fails path syntax is refused" {
    const caps = baseCaps();
    var entry = entryNamed("..", core.node_directory, 0);
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));
    // A separator inside a leaf is NOT caught: the C composes "/" + name and
    // validates that, so "a/b" becomes the perfectly legal path "/a/b". Pinned
    // deliberately, because it is the C's behaviour.
    entry = entryNamed("a/b", core.node_file, 1);
    try std.testing.expectEqual(core.ok, core.entryStatus(&caps, &entry));
    entry = entryNamed("c:x", core.node_file, 1);
    try std.testing.expectEqual(core.err_invalid_state, core.entryStatus(&caps, &entry));
}

test "bounded name length matches strnlen" {
    var buffer: [core.path_cap]u8 = @splat(0);
    try std.testing.expectEqual(@as(u16, 0), core.nulBoundedLen(&buffer));
    @memcpy(buffer[0..3], "abc");
    try std.testing.expectEqual(@as(u16, 3), core.nulBoundedLen(&buffer));
    buffer = @splat('x');
    try std.testing.expectEqual(core.path_cap, core.nulBoundedLen(&buffer));
}

fn fullNames() core.NamespacePresence {
    return .{
        .stat = true,
        .listdir = true,
        .dir_open = true,
        .dir_next = true,
        .dir_close = true,
        .mkdir = true,
        .unlink = true,
        .rmdir = true,
        .rename = true,
        .space = true,
    };
}

fn fullStreams() core.StreamPresence {
    return .{
        .open = true,
        .read = true,
        .write = true,
        .seek = true,
        .tell = true,
        .size = true,
        .sync = true,
        .close = true,
    };
}

fn fullTransactions() core.TransactionPresence {
    return .{
        .begin = true,
        .write = true,
        .seek = true,
        .validate = true,
        .commit = true,
        .abort = true,
    };
}

test "a complete vtable set binds" {
    try std.testing.expectEqual(
        core.ok,
        core.interfacesStatus(fullNames(), fullStreams(), fullTransactions()),
    );
    try std.testing.expectEqual(core.ok, core.interfacesStatus(fullNames(), fullStreams(), null));
}

test "space and sync stay optional for binding" {
    var names = fullNames();
    names.space = false;
    var streams = fullStreams();
    streams.sync = false;
    try std.testing.expectEqual(core.ok, core.interfacesStatus(names, streams, null));
}

test "every required namespace slot is checked" {
    const required = [_][]const u8{
        "stat",      "listdir", "dir_open", "dir_next",
        "mkdir",     "unlink",  "rmdir",    "rename",
        "dir_close",
    };
    inline for (required) |field| {
        var names = fullNames();
        @field(names, field) = false;
        try std.testing.expectEqual(
            core.err_invalid_arg,
            core.interfacesStatus(names, fullStreams(), null),
        );
    }
}

test "every required stream slot is checked" {
    const required = [_][]const u8{ "open", "read", "write", "close", "seek", "tell", "size" };
    inline for (required) |field| {
        var streams = fullStreams();
        @field(streams, field) = false;
        try std.testing.expectEqual(
            core.err_invalid_arg,
            core.interfacesStatus(fullNames(), streams, null),
        );
    }
}

test "every transaction slot is checked once a table is offered" {
    const required = [_][]const u8{ "begin", "write", "seek", "validate", "commit", "abort" };
    inline for (required) |field| {
        var transactions = fullTransactions();
        @field(transactions, field) = false;
        try std.testing.expectEqual(
            core.err_invalid_arg,
            core.interfacesStatus(fullNames(), fullStreams(), transactions),
        );
    }
}

test "caps validation demands the two mandatory capabilities" {
    var caps = baseCaps();
    caps.flags = core.cap_namespace;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));
    caps.flags = core.cap_stream;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));
    caps = baseCaps();
    try std.testing.expectEqual(core.ok, core.capsStatus(&caps, false, false, false));
}

test "a claimed capability must have its operation behind it" {
    var caps = baseCaps();
    caps.flags |= core.cap_space_query;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, false, true, true));
    try std.testing.expectEqual(core.ok, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.flags |= core.cap_file_sync;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, false, true));

    caps = baseCaps();
    caps.flags |= core.cap_durable_file_sync;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));
    caps.flags |= core.cap_file_sync;
    try std.testing.expectEqual(core.ok, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.flags |= core.cap_transactions;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, false));
    try std.testing.expectEqual(core.ok, core.capsStatus(&caps, true, true, true));
}

test "workspace metadata must be coherent to bind" {
    var caps = baseCaps();
    caps.file_workspace_align = 0;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.directory_workspace_bytes = 0;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.max_open_directories = 0;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.directory_workspace_align = 3;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));

    caps = baseCaps();
    caps.transaction_workspace_align = 0;
    try std.testing.expectEqual(core.err_invalid_arg, core.capsStatus(&caps, true, true, true));
}

test "open mode range and the exclusive create capability" {
    const plain = core.cap_namespace | core.cap_stream;
    try std.testing.expectEqual(core.ok, core.openModeStatus(core.open_read, plain));
    try std.testing.expectEqual(core.ok, core.openModeStatus(core.open_append, plain));
    try std.testing.expectEqual(core.err_invalid_arg, core.openModeStatus(4, plain));
    try std.testing.expectEqual(
        core.err_not_supported,
        core.openModeStatus(core.open_create_new, plain),
    );
    try std.testing.expectEqual(
        core.ok,
        core.openModeStatus(core.open_create_new, plain | core.cap_create_exclusive),
    );
}

test "rename picks the capability matching the replace flag" {
    try std.testing.expectEqual(
        core.err_not_supported,
        core.renameCapabilityStatus(0, true),
    );
    try std.testing.expectEqual(
        core.ok,
        core.renameCapabilityStatus(core.cap_atomic_replace, true),
    );
    try std.testing.expectEqual(
        core.err_not_supported,
        core.renameCapabilityStatus(core.cap_atomic_replace, false),
    );
    try std.testing.expectEqual(
        core.ok,
        core.renameCapabilityStatus(core.cap_atomic_noreplace, false),
    );
}

test "a missing transaction vtable answers by whether the port claimed one" {
    try std.testing.expectEqual(
        core.err_not_supported,
        core.transactionPreamble(0, false, false, core.txn_create_new),
    );
    try std.testing.expectEqual(
        core.err_not_initialized,
        core.transactionPreamble(core.cap_transactions, false, false, core.txn_create_new),
    );
}

test "transaction preamble order is capability, busy, policy range, policy support" {
    const noreplace = core.cap_transactions | core.cap_atomic_noreplace;
    try std.testing.expectEqual(
        core.err_not_supported,
        core.transactionPreamble(core.cap_atomic_noreplace, true, false, core.txn_create_new),
    );
    try std.testing.expectEqual(
        core.err_busy,
        core.transactionPreamble(noreplace, true, true, core.txn_create_new),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        core.transactionPreamble(noreplace, true, false, 2),
    );
    try std.testing.expectEqual(
        core.err_not_supported,
        core.transactionPreamble(noreplace, true, false, core.txn_replace_atomic),
    );
    try std.testing.expectEqual(
        core.ok,
        core.transactionPreamble(noreplace, true, false, core.txn_create_new),
    );
    try std.testing.expectEqual(
        core.ok,
        core.transactionPreamble(
            core.cap_transactions | core.cap_atomic_replace,
            true,
            false,
            core.txn_replace_atomic,
        ),
    );
}

test "root detection reads the second byte" {
    try std.testing.expect(core.isRoot("/"));
    try std.testing.expect(!core.isRoot("/a"));
}
