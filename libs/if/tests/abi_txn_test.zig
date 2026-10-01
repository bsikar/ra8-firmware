//! ABI-membrane tests for the transaction facade: fw_fs_transaction_* driven
//! over the shared fake backend, pinning the argument guards, the capability
//! and policy checks, staged-write bounds, and what commit and abort publish.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;
const fake = @import("abi_fake.zig");

// ---------------------------------------------------------------------------
// Transactions.
// ---------------------------------------------------------------------------

test "begin guards its arguments, capability, policy and workspace" {
    var rig = fake.Rig{};
    var port = rig.transactions();
    var transaction: abi.Transaction = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_transaction_begin(
        null,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        null,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        7,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_transaction_begin(
        &port,
        "/",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_no_mem, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        2,
    ));
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(transaction.active);
    try std.testing.expect(!transaction.validated);
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
}

test "an unbound transaction port answers by its transactions capability" {
    var rig = fake.Rig{};
    var transaction: abi.Transaction = .{};
    var plain = abi.TransactionPort{ .iface = null, .ctx = &rig, .caps = fake.allCaps() };
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_transaction_begin(
        &plain,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    plain.caps.flags &= ~core.cap_transactions;
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_transaction_begin(
        &plain,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
}

test "a failed begin leaves the transaction inactive" {
    var rig = fake.Rig{};
    var port = rig.transactions();
    var transaction: abi.Transaction = .{};
    rig.fake.begin_result = core.err_access_denied;
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(!transaction.active);
}

test "the staged write path closes once validation is accepted" {
    var rig = fake.Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);

    var payload: [4]u8 = @splat(7);
    var written: u32 = 0;
    rig.fake.txn_write_count = 4;
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(@as(u32, 4), written);
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_seek(&transaction, 0));

    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_validate(&transaction, null, null),
    );
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_validate(&transaction, fake.fakeValidator, null),
    );
    try std.testing.expect(transaction.validated);

    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_seek(&transaction, 8),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_validate(&transaction, fake.fakeValidator, null),
    );
}

test "a staged write may not exceed the caller length" {
    var rig = fake.Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    var payload: [4]u8 = @splat(1);
    var written: u32 = 0;
    rig.fake.txn_write_count = 5;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(@as(u32, 0), written);
}

// @par MC/DC:
// Covers ``result == ok and !published_value``:
// - backend error + unpublished -> false from the left condition alone
// - success + published -> false from the right condition alone
// - success + unpublished -> true; transaction remains active
test "commit demands validation and a published answer" {
    var rig = fake.Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    var published: u8 = 0;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_validate(&transaction, fake.fakeValidator, null),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_commit(&transaction, null),
    );

    rig.fake.commit_published = false;
    rig.fake.commit_result = core.err_busy;
    try std.testing.expectEqual(
        core.err_busy,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(@as(u8, 0), published);
    try std.testing.expect(transaction.active);
    try std.testing.expect(transaction.validated);

    rig.fake.commit_result = core.ok;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expect(transaction.active);

    rig.fake.commit_published = true;
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(@as(u8, 1), published);
    try std.testing.expect(!transaction.active);
    try std.testing.expect(!transaction.validated);
}

test "abort clears the transaction only when the backend agrees" {
    var rig = fake.Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    rig.fake.abort_result = core.err_busy;
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_transaction_abort(&transaction));
    try std.testing.expect(transaction.active);
    rig.fake.abort_result = core.ok;
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_abort(&transaction));
    try std.testing.expect(!transaction.active);
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_abort(&transaction),
    );
}

test "every transaction entry point refuses an inactive handle" {
    var transaction: abi.Transaction = .{};
    var payload: [2]u8 = @splat(0);
    var written: u32 = 0;
    var published: u8 = 0;
    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_write(null, &payload, 2, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 2, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_seek(&transaction, 0),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_validate(&transaction, fake.fakeValidator, null),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_abort(&transaction),
    );

    transaction.active = true;
    try std.testing.expectEqual(
        core.err_not_initialized,
        abi.fw_fs_transaction_abort(&transaction),
    );
}
