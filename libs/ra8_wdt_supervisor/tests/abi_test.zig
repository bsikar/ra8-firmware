//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI-membrane tests: the nine exported symbols, their guard order and their
//! `ra8_err_t` codes, driven exactly as the unchanged C suites drive them.
//!
//! The image exports its own `ra8_wdt_refresh_deferred`, which is the same
//! link-time substitution the real build performs against the HAL, so the
//! default refresh hook is exercised rather than stubbed out of the picture.

const std = @import("std");
const abi = @import("abi");

const ok: u16 = 0x000;
const no_mem: u16 = 0x102;
const invalid_arg: u16 = 0x103;
const not_found: u16 = 0x106;
const busy: u16 = 0x109;
const not_initialized: u16 = 0x10F;
const null_ptr: u16 = 0x504;
const handle_invalid: u8 = 0xFF;

var deferred_kicks: u32 = 0;
var hook_kicks: u32 = 0;
var fake_clock: u32 = 0;

export fn ra8_wdt_refresh_deferred() void {
    deferred_kicks += 1;
}

fn fakeNow() callconv(.c) u32 {
    return fake_clock;
}

fn countingRefresh() callconv(.c) void {
    hook_kicks += 1;
}

var stack_area: [1024]u8 = undefined;

fn cfg() abi.Cfg {
    return .{
        .stack = @ptrCast(&stack_area),
        .stack_size_bytes = stack_area.len,
        .priority = 4,
        .refresh_period_ms = 50,
    };
}

/// Fresh supervisor on a deterministic clock, the posture every C case sets up.
fn freshInit() !void {
    _ = abi.ra8_wdt_supervisor_deinit();
    deferred_kicks = 0;
    hook_kicks = 0;
    fake_clock = 1000;
    const block = cfg();
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_init(&block));
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_now_hook(fakeNow));
}

fn registerOne(name: [*:0]const u8, deadline_ms: u32) !u8 {
    var handle: u8 = handle_invalid;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_register_thread(name, deadline_ms, &handle));
    return handle;
}

test "init rejects a null config" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_init(null));
}

test "init rejects a null stack" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var block = cfg();
    block.stack = null;
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_init(&block));
}

test "init rejects a stack below the floor" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var block = cfg();
    block.stack_size_bytes = 511;
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_init(&block));
}

test "init rejects a zero refresh period" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var block = cfg();
    block.refresh_period_ms = 0;
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_init(&block));
}

test "init rejects an illegal priority" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var block = cfg();
    block.priority = 32;
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_init(&block));
}

test "init is single-shot and reports busy on the second call" {
    try freshInit();
    const block = cfg();
    try std.testing.expectEqual(busy, abi.ra8_wdt_supervisor_init(&block));
}

test "init validates the config before reporting busy" {
    try freshInit();
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_init(null));
}

test "deinit is safe before any init" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_deinit());
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_wdt_supervisor_thread_count());
}

test "deinit clears the registry" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(@as(u8, 1), abi.ra8_wdt_supervisor_thread_count());
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_deinit());
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_wdt_supervisor_thread_count());
}

test "deinit after start tears the thread down too" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_start());
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_deinit());
    try std.testing.expectEqual(not_initialized, abi.ra8_wdt_supervisor_tick(null));
}

test "register_thread rejects a null out_handle" {
    try freshInit();
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_register_thread("worker", 100, null));
}

test "register_thread rejects a null name and stamps the invalid handle" {
    try freshInit();
    var handle: u8 = 3;
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_register_thread(null, 100, &handle));
    try std.testing.expectEqual(handle_invalid, handle);
}

test "register_thread rejects a zero deadline" {
    try freshInit();
    var handle: u8 = 3;
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_register_thread("worker", 0, &handle));
    try std.testing.expectEqual(handle_invalid, handle);
}

test "register_thread reports not_initialized before init" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var handle: u8 = 3;
    try std.testing.expectEqual(not_initialized, abi.ra8_wdt_supervisor_register_thread("worker", 100, &handle));
    try std.testing.expectEqual(handle_invalid, handle);
}

test "register_thread checks out_handle before the name" {
    try freshInit();
    try std.testing.expectEqual(null_ptr, abi.ra8_wdt_supervisor_register_thread(null, 0, null));
}

test "register_thread checks the deadline before init state" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var handle: u8 = 3;
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_register_thread("worker", 0, &handle));
}

test "register_thread hands out ascending handles" {
    try freshInit();
    try std.testing.expectEqual(@as(u8, 0), try registerOne("a", 100));
    try std.testing.expectEqual(@as(u8, 1), try registerOne("b", 100));
    try std.testing.expectEqual(@as(u8, 2), abi.ra8_wdt_supervisor_thread_count());
}

test "register_thread fills every slot then reports no_mem" {
    try freshInit();
    var i: u8 = 0;
    while (i < 8) : (i += 1) _ = try registerOne("w", 100);
    var handle: u8 = 3;
    try std.testing.expectEqual(no_mem, abi.ra8_wdt_supervisor_register_thread("extra", 100, &handle));
    try std.testing.expectEqual(handle_invalid, handle);
    try std.testing.expectEqual(@as(u8, 8), abi.ra8_wdt_supervisor_thread_count());
}

test "registration primes the check-in stamp so the first tick refreshes" {
    try freshInit();
    fake_clock = 5000;
    _ = try registerOne("worker", 10);
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
}

test "checkin rejects an out-of-range handle" {
    try freshInit();
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_checkin(8));
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_checkin(handle_invalid));
}

test "checkin reports not_initialized before init" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(not_initialized, abi.ra8_wdt_supervisor_checkin(0));
}

test "checkin checks the handle range before init state" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(invalid_arg, abi.ra8_wdt_supervisor_checkin(8));
}

test "checkin reports not_found for a free slot" {
    try freshInit();
    try std.testing.expectEqual(not_found, abi.ra8_wdt_supervisor_checkin(0));
}

test "checkin resets the deadline window" {
    try freshInit();
    const handle = try registerOne("worker", 50);
    fake_clock = 1100;
    var refreshed: u8 = 1;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_checkin(handle));
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
}

test "start reports not_initialized before init" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(not_initialized, abi.ra8_wdt_supervisor_start());
}

test "start is single-shot" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_start());
    try std.testing.expectEqual(busy, abi.ra8_wdt_supervisor_start());
}

test "tick reports not_initialized and clears the output" {
    _ = abi.ra8_wdt_supervisor_deinit();
    var refreshed: u8 = 1;
    try std.testing.expectEqual(not_initialized, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
}

test "tick tolerates a null output" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(null));
}

test "an empty registry never refreshes" {
    try freshInit();
    var refreshed: u8 = 1;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
    try std.testing.expectEqual(@as(u32, 0), deferred_kicks);
}

test "the default refresh hook calls ra8_wdt_refresh_deferred" {
    try freshInit();
    _ = try registerOne("worker", 100);
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
    try std.testing.expectEqual(@as(u32, 1), deferred_kicks);
}

test "an overdue thread stops the kick" {
    try freshInit();
    _ = try registerOne("worker", 50);
    fake_clock = 1051;
    var refreshed: u8 = 1;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
    try std.testing.expectEqual(@as(u32, 0), deferred_kicks);
}

test "one wedged worker among healthy ones stops the kick" {
    try freshInit();
    const fast = try registerOne("fast", 10);
    _ = try registerOne("slow", 10_000);
    fake_clock = 1020;
    var refreshed: u8 = 1;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_checkin(fast));
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
}

test "the refresh hook replaces the default" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_refresh_hook(countingRefresh));
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
    try std.testing.expectEqual(@as(u32, 1), hook_kicks);
    try std.testing.expectEqual(@as(u32, 0), deferred_kicks);
}

test "a null refresh hook restores the default" {
    try freshInit();
    _ = try registerOne("worker", 100);
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_refresh_hook(countingRefresh));
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_refresh_hook(null));
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
    try std.testing.expectEqual(@as(u32, 0), hook_kicks);
    try std.testing.expectEqual(@as(u32, 1), deferred_kicks);
}

test "a null now hook restores the default clock" {
    try freshInit();
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_now_hook(null));
    _ = try registerOne("worker", 100);
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
}

test "the hooks are settable before init" {
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_now_hook(fakeNow));
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_set_refresh_hook(countingRefresh));
}

test "thread_count tracks registrations and survives deinit" {
    try freshInit();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_wdt_supervisor_thread_count());
    _ = try registerOne("a", 100);
    _ = try registerOne("b", 100);
    try std.testing.expectEqual(@as(u8, 2), abi.ra8_wdt_supervisor_thread_count());
    _ = abi.ra8_wdt_supervisor_deinit();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_wdt_supervisor_thread_count());
}

test "the deadline boundary is inclusive through the ABI" {
    try freshInit();
    _ = try registerOne("worker", 50);
    fake_clock = 1050;
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
    fake_clock = 1051;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 0), refreshed);
}

test "the supervisor keeps kicking across a clock wrap" {
    try freshInit();
    fake_clock = 0xFFFF_FF00;
    _ = try registerOne("worker", 500);
    // 0x100 -% 0xFFFF_FF00 is a 512 ms gap, so the wrap itself must not read as
    // a 4-billion-millisecond one.
    fake_clock = 0x0000_0050;
    var refreshed: u8 = 0;
    try std.testing.expectEqual(ok, abi.ra8_wdt_supervisor_tick(&refreshed));
    try std.testing.expectEqual(@as(u8, 1), refreshed);
}

test "the config block matches the C layout" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(abi.Cfg, "stack"));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(abi.Cfg, "stack_size_bytes"));
    try std.testing.expectEqual(@sizeOf(usize) + 4, @offsetOf(abi.Cfg, "priority"));
    try std.testing.expectEqual(@sizeOf(usize) + 8, @offsetOf(abi.Cfg, "refresh_period_ms"));
}
