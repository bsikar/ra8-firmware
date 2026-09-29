//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane: the module-static handle, the
//! guard order on every dispatcher entry point, the log lines each refusal
//! emits and the two different log tags the dispatcher and the policy use.

const std = @import("std");
const abi = @import("abi");

var log_calls: usize = 0;
var last_tag: [64]u8 = undefined;
var last_tag_len: usize = 0;
var last_message: [96]u8 = undefined;
var last_message_len: usize = 0;

fn capture(destination: []u8, length: *usize, text: [*:0]const u8) void {
    const slice = std.mem.span(text);
    const take = @min(slice.len, destination.len);
    @memcpy(destination[0..take], slice[0..take]);
    length.* = take;
}

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    log_calls += 1;
    capture(&last_tag, &last_tag_len, tag);
    capture(&last_message, &last_message_len, message);
}

export fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void {
    log_calls += 1;
    capture(&last_tag, &last_tag_len, tag);
    capture(&last_message, &last_message_len, message);
}

fn resetLog() void {
    log_calls = 0;
    last_tag_len = 0;
    last_message_len = 0;
}

fn taggedLast() []const u8 {
    return last_tag[0..last_tag_len];
}

fn messagedLast() []const u8 {
    return last_message[0..last_message_len];
}

/// A backend whose every row is scripted, so the suite can drive the
/// dispatcher without a panel.
const Fake = struct {
    var init_result: u16 = 0;
    var caps_result: u16 = 0;
    var deinit_result: u16 = 0;
    var flush_calls: usize = 0;
    var clear_calls: usize = 0;
    var last_hint: u8 = 0xFF;
    var last_color: u32 = 0;
    var context_marker: u32 = 0xA5A5A5A5;

    fn reset() void {
        init_result = 0;
        caps_result = 0;
        deinit_result = 0;
        flush_calls = 0;
        clear_calls = 0;
        last_hint = 0xFF;
        last_color = 0;
    }

    fn init(cfg: ?*const abi.Config, out_ctx: ?*?*anyopaque) callconv(.c) u16 {
        _ = cfg;
        if (init_result != 0) return init_result;
        out_ctx.?.* = @ptrCast(&context_marker);
        return 0;
    }

    fn getCaps(ctx: ?*const anyopaque, out: ?*abi.Caps) callconv(.c) u16 {
        _ = ctx;
        if (caps_result != 0) return caps_result;
        out.?.* = .{ .width_px = 1024, .height_px = 600, .stride_bytes = 2048 };
        return 0;
    }

    fn getFramebuffer(ctx: ?*anyopaque, out: ?*abi.Fb) callconv(.c) u16 {
        _ = ctx;
        out.?.* = .{ .width_px = 1024, .height_px = 600, .stride_bytes = 2048 };
        return 0;
    }

    fn flush(ctx: ?*anyopaque, rect: abi.Rect, hint: u8) callconv(.c) u16 {
        _ = ctx;
        _ = rect;
        flush_calls += 1;
        last_hint = hint;
        return 0;
    }

    fn clear(ctx: ?*anyopaque, color: u32) callconv(.c) u16 {
        _ = ctx;
        clear_calls += 1;
        last_color = color;
        return 0;
    }

    fn deinitFn(ctx: ?*anyopaque) callconv(.c) u16 {
        _ = ctx;
        return deinit_result;
    }

    const iface: abi.BackendIface = .{
        .init = init,
        .get_caps = getCaps,
        .get_framebuffer = getFramebuffer,
        .flush = flush,
        .clear = clear,
        .deinit = deinitFn,
    };
};

var framebuffer: [16]u8 = undefined;

fn goodConfig() abi.Config {
    return .{
        .iface = &Fake.iface,
        .framebuffer = @ptrCast(&framebuffer),
        .framebuffer_bytes = framebuffer.len,
        .width_px = 1024,
        .height_px = 600,
        .pixfmt = 0,
    };
}

fn freshStart() void {
    abi.testResetState();
    Fake.reset();
    resetLog();
}

test "init rejects its arguments in the C's order, each with its own message" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();

    try std.testing.expectEqual(@as(u16, 0x504), abi.display_init(null, &handle));
    try std.testing.expectEqualStrings("cfg must not be nullptr", messagedLast());
    try std.testing.expectEqualStrings("ra8_display_pal", taggedLast());

    try std.testing.expectEqual(@as(u16, 0x504), abi.display_init(&cfg, null));
    try std.testing.expectEqualStrings("out_handle must not be nullptr", messagedLast());

    var no_iface = cfg;
    no_iface.iface = null;
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_init(&no_iface, &handle));
    try std.testing.expectEqualStrings("cfg->iface must not be nullptr", messagedLast());

    var empty_iface: abi.BackendIface = .{};
    var no_init = cfg;
    no_init.iface = &empty_iface;
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_init(&no_init, &handle));
    try std.testing.expectEqualStrings("iface->init must not be nullptr", messagedLast());

    try std.testing.expect(!abi.testInitialized());
}

test "a successful init binds the handle and logs the bind" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));
    try std.testing.expect(handle == abi.testLiveHandle());
    try std.testing.expect(abi.testInitialized());
    try std.testing.expectEqualStrings("display_init: backend bound", messagedLast());
}

test "a backend that refuses init leaves the PAL closed and the out parameter untouched" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    Fake.init_result = 0x201;
    try std.testing.expectEqual(@as(u16, 0x201), abi.display_init(&cfg, &handle));
    try std.testing.expect(handle == null);
    try std.testing.expect(!abi.testInitialized());
}

test "a second init while a handle is live answers busy" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));
    var second: ?*abi.Handle = null;
    try std.testing.expectEqual(@as(u16, 0x109), abi.display_init(&cfg, &second));
    try std.testing.expectEqualStrings("display_init: PAL already initialised", messagedLast());
    try std.testing.expect(second == null);
}

test "handle judgement runs before the out-parameter check and emits no log line" {
    freshStart();
    var caps: abi.Caps = .{};
    resetLog();
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_get_caps(null, &caps));
    try std.testing.expectEqual(@as(usize, 0), log_calls);

    // Uninitialised PAL with a non-null handle: invalid_arg, still silent.
    var stray: abi.Handle = .{};
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_get_caps(&stray, &caps));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "a foreign handle is refused once the PAL is live" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));
    var stray: abi.Handle = .{};
    var caps: abi.Caps = .{};
    resetLog();
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_get_caps(&stray, &caps));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "a live handle with a null out parameter answers null_ptr and logs it" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));

    resetLog();
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_get_caps(handle, null));
    try std.testing.expectEqualStrings("out must not be nullptr", messagedLast());

    resetLog();
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_get_framebuffer(handle, null));
    try std.testing.expectEqualStrings("out must not be nullptr", messagedLast());
}

test "flush and clear forward their arguments to the bound backend" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));

    const rect: abi.Rect = .{ .x = 0, .y = 0, .w = 32, .h = 16 };
    try std.testing.expectEqual(@as(u16, 0), abi.display_flush(handle, rect, 1));
    try std.testing.expectEqual(@as(usize, 1), Fake.flush_calls);
    try std.testing.expectEqual(@as(u8, 1), Fake.last_hint);

    try std.testing.expectEqual(@as(u16, 0), abi.display_clear(handle, 0xDEADBEEF));
    try std.testing.expectEqual(@as(usize, 1), Fake.clear_calls);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), Fake.last_color);
}

test "deinit drops the handle even when the backend reports a tear-down error" {
    freshStart();
    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));
    Fake.deinit_result = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), abi.display_deinit(handle));
    try std.testing.expect(!abi.testInitialized());

    // Every later call is refused, and a fresh init is allowed again.
    var caps: abi.Caps = .{};
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_get_caps(handle, &caps));
    Fake.deinit_result = 0;
    var again: ?*abi.Handle = null;
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &again));
}

test "full_rect answers the empty rectangle on both failure paths" {
    freshStart();
    try std.testing.expectEqual(@as(u16, 0), abi.display_full_rect(null).w);

    var handle: ?*abi.Handle = null;
    const cfg = goodConfig();
    try std.testing.expectEqual(@as(u16, 0), abi.display_init(&cfg, &handle));

    const full = abi.display_full_rect(handle);
    try std.testing.expectEqual(@as(u16, 1024), full.w);
    try std.testing.expectEqual(@as(u16, 600), full.h);

    Fake.caps_result = 0x107;
    const refused = abi.display_full_rect(handle);
    try std.testing.expectEqual(@as(u16, 0), refused.w);
    try std.testing.expectEqual(@as(u16, 0), refused.h);
}

test "policy guards log under their own tag" {
    freshStart();
    var policy: abi.Policy = .{};
    var decision: abi.Decision = .{};

    try std.testing.expectEqual(@as(u16, 0x504), abi.display_policy_init(null, 2, 8));
    try std.testing.expectEqualStrings("disp_policy", taggedLast());
    try std.testing.expectEqualStrings("init: null policy", messagedLast());

    try std.testing.expectEqual(@as(u16, 0x103), abi.display_policy_init(&policy, 99, 8));
    try std.testing.expectEqualStrings("init: kind out of range", messagedLast());

    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_init(&policy, 2, 0));
    try std.testing.expectEqual(@as(u16, 1), policy.clean_every);
    try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_init(&policy, 2, 9999));
    try std.testing.expectEqual(@as(u16, 256), policy.clean_every);

    try std.testing.expectEqual(@as(u16, 0x504), abi.display_policy_decide(null, 1, &decision));
    try std.testing.expectEqualStrings("decide: null policy", messagedLast());
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_policy_decide(&policy, 1, null));
    try std.testing.expectEqualStrings("decide: null out", messagedLast());
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_policy_decide(&policy, 9, &decision));
    try std.testing.expectEqualStrings("decide: event out of range", messagedLast());
}

test "policy full_rect fills the rectangle or refuses a zero dimension" {
    freshStart();
    var rect: abi.Rect = .{};
    try std.testing.expectEqual(@as(u16, 0x504), abi.display_policy_full_rect(4, 4, null));
    try std.testing.expectEqualStrings("full_rect: null out", messagedLast());
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_policy_full_rect(0, 4, &rect));
    try std.testing.expectEqualStrings("full_rect: zero dimension", messagedLast());
    try std.testing.expectEqual(@as(u16, 0x103), abi.display_policy_full_rect(4, 0, &rect));
    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_full_rect(800, 480, &rect));
    try std.testing.expectEqual(@as(u16, 0), rect.x);
    try std.testing.expectEqual(@as(u16, 800), rect.w);
    try std.testing.expectEqual(@as(u16, 480), rect.h);
}

test "the cadence drives a whole page-turn sequence through the membrane" {
    freshStart();
    var policy: abi.Policy = .{};
    var decision: abi.Decision = .{};
    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_init(&policy, 2, 3));

    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_decide(&policy, 0, &decision));
    try std.testing.expectEqual(@as(u8, 2), decision.hint);
    try std.testing.expect(decision.full_update);

    var index: usize = 0;
    while (index < 2) : (index += 1) {
        try std.testing.expectEqual(@as(u16, 0), abi.display_policy_decide(&policy, 1, &decision));
        try std.testing.expectEqual(@as(u8, 0), decision.hint);
        try std.testing.expect(!decision.full_update);
    }
    try std.testing.expectEqual(@as(u16, 0), abi.display_policy_decide(&policy, 1, &decision));
    try std.testing.expectEqual(@as(u8, 1), decision.hint);
    try std.testing.expect(decision.full_update);
    try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
}
