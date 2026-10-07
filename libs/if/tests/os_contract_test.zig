//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Conformance of the `fw_os` port contract in `inc/fw_os.h`.
//!
//! No binding implements the seam inside this library, so nothing here would
//! ever feed the header to a compiler on its own. Translating it does: clang
//! parses `fw_os.h` and evaluates its own `static_assert`s, and the tests
//! below check what the header cannot say about itself. The header is read
//! twice, once with the default caps and once with the queue block enabled,
//! so the `FW_OS_HAS_QUEUE` half is checked even though no binding sets it.

const std = @import("std");

// Both translations come from build.zig: `os` with the default caps,
// `os_queue` with FW_OS_HAS_QUEUE set.
const os = @import("fw_os_h");
const os_queue = @import("fw_os_queue_h");

const word_bytes = @sizeOf(u64);

fn expectStorage(comptime Handle: type, comptime words: comptime_int) !void {
    try std.testing.expect(@alignOf(Handle) >= @alignOf(u64));
    try std.testing.expectEqual(words * word_bytes, @sizeOf(Handle));
}

test "every handle is 8-byte aligned and exactly as large as its caps constant" {
    try expectStorage(os.fw_os_thread_t, os.K_FW_OS_THREAD_STORAGE_WORDS);
    try expectStorage(os.fw_os_mutex_t, os.K_FW_OS_MUTEX_STORAGE_WORDS);
    try expectStorage(os.fw_os_sem_t, os.K_FW_OS_SEM_STORAGE_WORDS);
}

test "the queue handle obeys the same rule when a binding enables queues" {
    try expectStorage(os_queue.fw_os_queue_t, os_queue.K_FW_OS_QUEUE_STORAGE_WORDS);
}

test "the queue block stays undeclared by default" {
    try std.testing.expectEqual(0, os.FW_OS_HAS_QUEUE);
    try std.testing.expect(!@hasDecl(os, "fw_os_queue_t"));
}

test "the priority band starts at zero and is contiguous" {
    try std.testing.expectEqual(0, os.k_fw_os_priority_idle);
    try std.testing.expectEqual(os.k_fw_os_priority_idle + 1, os.k_fw_os_priority_low);
    try std.testing.expectEqual(os.k_fw_os_priority_low + 1, os.k_fw_os_priority_normal);
    try std.testing.expectEqual(os.k_fw_os_priority_normal + 1, os.k_fw_os_priority_high);
}

test "no-wait is the zero duration and wait-forever tops the millisecond range" {
    try std.testing.expectEqual(0, os.K_FW_OS_NO_WAIT);
    try std.testing.expectEqual(std.math.maxInt(u32), os.K_FW_OS_WAIT_FOREVER);
}
