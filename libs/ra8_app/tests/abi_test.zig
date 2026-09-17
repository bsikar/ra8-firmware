//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the exported C ABI: guard order and error codes, the focus
//! lifecycle, input / tick / render routing, the navigation back-stack, the
//! derived state, and uninstall. The C MC/DC vector sets in
//! `tests/misc/src/test_ra8_app.c` are mirrored here through the exported
//! symbols, and the log sink below is the same link-time substitution the real
//! build makes against `libs/ra8_core/src/ra8_log.c`.

const std = @import("std");
const abi = @import("abi");

var log_calls: u32 = 0;
var last_message: [*:0]const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    log_calls += 1;
    last_message = message;
}

fn resetLog() void {
    log_calls = 0;
    last_message = "";
}

fn lastMessage() []const u8 {
    return std.mem.span(last_message);
}

const ok = @intFromEnum(abi.AppError.ok);
const null_ptr = @intFromEnum(abi.AppError.null_ptr);
const invalid_arg = @intFromEnum(abi.AppError.invalid_arg);
const not_found = @intFromEnum(abi.AppError.not_found);
const not_supported = @intFromEnum(abi.AppError.not_supported);
const busy = @intFromEnum(abi.AppError.busy);
const no_mem = @intFromEnum(abi.AppError.no_mem);
const conflict = @intFromEnum(abi.AppError.conflict);
const out_of_range = @intFromEnum(abi.AppError.out_of_range);

/// `k_ra8_err_hw_init_failed`, the code the C fixture's failing init returns.
const hw_init_failed: u16 = 0x201;

/// Recording context behind each fixture app, mirroring the C's `app_ctx_t`.
const Recorder = struct {
    init_calls: u32 = 0,
    enter_calls: u32 = 0,
    leave_calls: u32 = 0,
    input_calls: u32 = 0,
    tick_calls: u32 = 0,
    render_calls: u32 = 0,
    deinit_calls: u32 = 0,
    init_fail: bool = false,
    consume: bool = false,
};

fn recorderOf(app: *abi.App) *Recorder {
    return @ptrCast(@alignCast(app.ctx.?));
}

fn recorderOfConst(app: *const abi.App) *Recorder {
    return @ptrCast(@alignCast(app.ctx.?));
}

fn appInit(app: *abi.App) callconv(.c) u16 {
    const rec = recorderOf(app);
    rec.init_calls += 1;
    return if (rec.init_fail) hw_init_failed else ok;
}

fn appEnter(app: *abi.App) callconv(.c) void {
    recorderOf(app).enter_calls += 1;
}

fn appLeave(app: *abi.App) callconv(.c) void {
    recorderOf(app).leave_calls += 1;
}

fn appTick(app: *abi.App) callconv(.c) void {
    recorderOf(app).tick_calls += 1;
}

fn appRender(app: *const abi.App) callconv(.c) void {
    recorderOfConst(app).render_calls += 1;
}

fn appInput(app: *abi.App, event: *const abi.WidgetEvent) callconv(.c) bool {
    _ = event;
    const rec = recorderOf(app);
    rec.input_calls += 1;
    return rec.consume;
}

fn appDeinit(app: *abi.App) callconv(.c) void {
    recorderOf(app).deinit_calls += 1;
}

const full_vtable = abi.Vtable{
    .init = appInit,
    .on_enter = appEnter,
    .tick = appTick,
    .render = appRender,
    .on_input = appInput,
    .on_leave = appLeave,
    .deinit = appDeinit,
};

/// A vtable whose optional callbacks are all NULL (only `init` set).
const bare_vtable = abi.Vtable{ .init = appInit };

fn makeApp(rec: *Recorder, id: u16, vtable: *const abi.Vtable) abi.App {
    return .{ .vt = vtable, .ctx = rec, .id = id, .name = "app" };
}

var event_storage: u32 = 0;

fn anyEvent() *const abi.WidgetEvent {
    return @ptrCast(&event_storage);
}

test "registry_init: binds storage, empties the table and clears the focus" {
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    try std.testing.expectEqual(ok, abi.ra8_app_registry_init(&reg, &slots, 2));
    try std.testing.expectEqual(@as(u16, 0), reg.count);
    try std.testing.expectEqual(@as(u16, 2), reg.cap);
    try std.testing.expectEqual(abi.none_index, reg.active);
}

test "registry_init: guard order is reg, then storage, then a zero capacity" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_app_registry_init(null, &slots, 1));
    try std.testing.expectEqualStrings("reg must not be nullptr", lastMessage());
    try std.testing.expectEqual(null_ptr, abi.ra8_app_registry_init(&reg, null, 1));
    try std.testing.expectEqualStrings("storage must not be nullptr", lastMessage());
    try std.testing.expectEqual(@as(u32, 2), log_calls);
    try std.testing.expectEqual(invalid_arg, abi.ra8_app_registry_init(&reg, &slots, 0));
    try std.testing.expectEqual(@as(u32, 2), log_calls); // a zero cap logs nothing
}

test "register: init fires once per app and marks it initialised" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var a1 = makeApp(&r1, 2, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);

    try std.testing.expectEqual(ok, abi.ra8_app_register(&reg, &a0));
    try std.testing.expectEqual(ok, abi.ra8_app_register(&reg, &a1));
    try std.testing.expectEqual(@as(u32, 1), r0.init_calls);
    try std.testing.expectEqual(@as(u32, 1), r1.init_calls);
    try std.testing.expect(a0.initialized);
    try std.testing.expectEqual(@as(u16, 2), reg.count);
}

test "register: a duplicate id is a conflict even when the registry is full" {
    var r0 = Recorder{};
    var rdup = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var adup = makeApp(&rdup, 1, &full_vtable);
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);

    try std.testing.expectEqual(conflict, abi.ra8_app_register(&reg, &adup));
    try std.testing.expectEqual(@as(u32, 0), rdup.init_calls);
    try std.testing.expectEqual(@as(u16, 1), reg.count);
}

test "register: a full registry reports no_mem and never runs init" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var a1 = makeApp(&r1, 2, &full_vtable);
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);

    try std.testing.expectEqual(no_mem, abi.ra8_app_register(&reg, &a1));
    try std.testing.expectEqual(@as(u32, 0), r1.init_calls);
}

test "register: a failing init is forwarded and leaves the app unregistered" {
    var rfail = Recorder{ .init_fail = true };
    var afail = makeApp(&rfail, 9, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);

    try std.testing.expectEqual(hw_init_failed, abi.ra8_app_register(&reg, &afail));
    var count: u16 = 99;
    try std.testing.expectEqual(ok, abi.ra8_app_count(&reg, &count));
    try std.testing.expectEqual(@as(u16, 0), count);
    try std.testing.expect(!afail.initialized);
}

test "register: guard order is reg, then app, then the vtable" {
    var rec = Recorder{};
    var no_vt = abi.App{ .ctx = &rec, .id = 3 };
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);

    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_app_register(null, null));
    try std.testing.expectEqualStrings("reg must not be nullptr", lastMessage());
    try std.testing.expectEqual(null_ptr, abi.ra8_app_register(&reg, null));
    try std.testing.expectEqualStrings("app must not be nullptr", lastMessage());
    try std.testing.expectEqual(null_ptr, abi.ra8_app_register(&reg, &no_vt));
    try std.testing.expectEqualStrings("app->vt must not be nullptr", lastMessage());
}

test "find: reports the index, none for an absent id, and guards its arguments" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 10, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);

    var index: i16 = 5;
    try std.testing.expectEqual(ok, abi.ra8_app_find(&reg, 10, &index));
    try std.testing.expectEqual(@as(i16, 0), index);
    try std.testing.expectEqual(ok, abi.ra8_app_find(&reg, 11, &index));
    try std.testing.expectEqual(abi.none_index, index);
    try std.testing.expectEqual(null_ptr, abi.ra8_app_find(null, 1, &index));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_find(&reg, 1, null));
}

test "launch: first focus enters, a re-tap is idempotent, a switch leaves then enters" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var a1 = makeApp(&r1, 2, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);

    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 1));
    try std.testing.expectEqual(@as(u32, 1), r0.enter_calls);
    try std.testing.expectEqual(@as(u32, 0), r0.leave_calls);

    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 1));
    try std.testing.expectEqual(@as(u32, 1), r0.enter_calls);
    try std.testing.expectEqual(@as(u32, 0), r0.leave_calls);

    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 2));
    try std.testing.expectEqual(@as(u32, 1), r0.leave_calls);
    try std.testing.expectEqual(@as(u32, 1), r1.enter_calls);

    var active: ?*abi.App = null;
    try std.testing.expectEqual(ok, abi.ra8_app_active(&reg, &active));
    try std.testing.expectEqual(@as(u16, 2), active.?.id);
}

test "launch: an unknown id is not_found and leaves the focus alone" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_launch(&reg, 1);

    try std.testing.expectEqual(not_found, abi.ra8_app_launch(&reg, 99));
    var active: ?*abi.App = null;
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 1), active.?.id);
    try std.testing.expectEqual(null_ptr, abi.ra8_app_launch(null, 1));
}

test "launch: NULL lifecycle callbacks are skipped" {
    var r0 = Recorder{};
    var rbare = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var abare = makeApp(&rbare, 2, &bare_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &abare);

    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 2));
    try std.testing.expectEqual(@as(u32, 0), rbare.enter_calls);
    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 1));
    try std.testing.expectEqual(@as(u32, 0), rbare.leave_calls);
    try std.testing.expectEqual(@as(u32, 1), r0.enter_calls);
}

test "active: reports null when nothing is focused and guards its arguments" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    var active: ?*abi.App = undefined;
    try std.testing.expectEqual(ok, abi.ra8_app_active(&reg, &active));
    try std.testing.expect(active == null);
    try std.testing.expectEqual(null_ptr, abi.ra8_app_active(null, &active));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_active(&reg, null));
}

test "route_input: nothing focused routes nothing, the focused app decides handled" {
    var r0 = Recorder{ .consume = true };
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);

    var handled = true;
    try std.testing.expectEqual(ok, abi.ra8_app_route_input(&reg, anyEvent(), &handled));
    try std.testing.expect(!handled);
    try std.testing.expectEqual(@as(u32, 0), r0.input_calls);

    _ = abi.ra8_app_launch(&reg, 1);
    try std.testing.expectEqual(ok, abi.ra8_app_route_input(&reg, anyEvent(), &handled));
    try std.testing.expect(handled);
    try std.testing.expectEqual(@as(u32, 1), r0.input_calls);
}

test "route_input: a NULL on_input leaves the event unhandled" {
    var rbare = Recorder{};
    var abare = makeApp(&rbare, 2, &bare_vtable);
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &abare);
    _ = abi.ra8_app_launch(&reg, 2);

    var handled = true;
    try std.testing.expectEqual(ok, abi.ra8_app_route_input(&reg, anyEvent(), &handled));
    try std.testing.expect(!handled);
}

test "route_input: guard order is reg, then the event, then out_handled" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    var handled = false;
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_app_route_input(null, anyEvent(), &handled));
    try std.testing.expectEqualStrings("reg must not be nullptr", lastMessage());
    try std.testing.expectEqual(null_ptr, abi.ra8_app_route_input(&reg, null, &handled));
    try std.testing.expectEqualStrings("ev must not be nullptr", lastMessage());
    try std.testing.expectEqual(null_ptr, abi.ra8_app_route_input(&reg, anyEvent(), null));
    try std.testing.expectEqualStrings("out_handled must not be nullptr", lastMessage());
}

test "tick and render: no focus is a no-op, the focused app fires, a NULL hook is skipped" {
    var r0 = Recorder{};
    var rbare = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var abare = makeApp(&rbare, 2, &bare_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &abare);

    try std.testing.expectEqual(ok, abi.ra8_app_tick(&reg));
    try std.testing.expectEqual(ok, abi.ra8_app_render(&reg));
    try std.testing.expectEqual(@as(u32, 0), r0.tick_calls);

    _ = abi.ra8_app_launch(&reg, 1);
    try std.testing.expectEqual(ok, abi.ra8_app_tick(&reg));
    try std.testing.expectEqual(ok, abi.ra8_app_render(&reg));
    try std.testing.expectEqual(@as(u32, 1), r0.tick_calls);
    try std.testing.expectEqual(@as(u32, 1), r0.render_calls);

    _ = abi.ra8_app_launch(&reg, 2);
    try std.testing.expectEqual(ok, abi.ra8_app_tick(&reg));
    try std.testing.expectEqual(ok, abi.ra8_app_render(&reg));
    try std.testing.expectEqual(@as(u32, 0), rbare.tick_calls);
    try std.testing.expectEqual(@as(u32, 0), rbare.render_calls);

    try std.testing.expectEqual(null_ptr, abi.ra8_app_tick(null));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_render(null));
}

test "count and at: enumeration, the range guard and the argument guards" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 10, &full_vtable);
    var a1 = makeApp(&r1, 20, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);

    var count: u16 = 99;
    try std.testing.expectEqual(ok, abi.ra8_app_count(&reg, &count));
    try std.testing.expectEqual(@as(u16, 2), count);

    var app: ?*abi.App = null;
    try std.testing.expectEqual(ok, abi.ra8_app_at(&reg, 1, &app));
    try std.testing.expectEqual(@as(u16, 20), app.?.id);
    try std.testing.expectEqual(out_of_range, abi.ra8_app_at(&reg, 2, &app));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_count(&reg, null));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_at(&reg, 0, null));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_at(null, 0, &app));
}

test "find: a NULL registry slot is skipped rather than dereferenced" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [2]?*abi.App = .{ null, &a0 };
    var reg = abi.Registry{ .apps = &slots, .cap = 2, .count = 2, .active = abi.none_index };

    var index: i16 = 0;
    try std.testing.expectEqual(ok, abi.ra8_app_find(&reg, 1, &index));
    try std.testing.expectEqual(@as(i16, 1), index);
    try std.testing.expectEqual(ok, abi.ra8_app_find(&reg, 7, &index));
    try std.testing.expectEqual(abi.none_index, index);
}

test "launch: a NULL outgoing slot skips on_leave and still enters the target" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [2]?*abi.App = .{ null, &a0 };
    var reg = abi.Registry{ .apps = &slots, .cap = 2, .count = 2, .active = 0 };

    try std.testing.expectEqual(ok, abi.ra8_app_launch(&reg, 1));
    try std.testing.expectEqual(@as(i16, 1), reg.active);
    try std.testing.expectEqual(@as(u32, 1), r0.enter_calls);
}

test "route, tick and render: a NULL focused slot does nothing" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{ .apps = &slots, .cap = 1, .count = 1, .active = 0 };

    var handled = true;
    try std.testing.expectEqual(ok, abi.ra8_app_route_input(&reg, anyEvent(), &handled));
    try std.testing.expect(!handled);
    try std.testing.expectEqual(ok, abi.ra8_app_tick(&reg));
    try std.testing.expectEqual(ok, abi.ra8_app_render(&reg));
}

test "nav_init: binds the trail and rejects a zero capacity" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    var trail: [2]u16 = .{ 0, 0 };
    var nav = abi.Nav{};

    try std.testing.expectEqual(ok, abi.ra8_app_nav_init(&nav, &reg, &trail, 2));
    try std.testing.expectEqual(@as(u16, 0), nav.depth);
    try std.testing.expectEqual(@as(u16, 2), nav.cap);
    try std.testing.expectEqual(invalid_arg, abi.ra8_app_nav_init(&nav, &reg, &trail, 0));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_init(null, &reg, &trail, 1));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_init(&nav, null, &trail, 1));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_init(&nav, &reg, null, 1));
}

test "nav: first go pushes nothing, a switch pushes, back pops, a re-tap pushes nothing" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var r2 = Recorder{};
    var a0 = makeApp(&r0, 10, &full_vtable);
    var a1 = makeApp(&r1, 20, &full_vtable);
    var a2 = makeApp(&r2, 30, &full_vtable);
    var slots: [3]?*abi.App = .{ null, null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 3);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);
    _ = abi.ra8_app_register(&reg, &a2);
    var trail: [4]u16 = .{ 0, 0, 0, 0 };
    var nav = abi.Nav{};
    _ = abi.ra8_app_nav_init(&nav, &reg, &trail, 4);

    var depth: u16 = 99;
    var active: ?*abi.App = null;
    try std.testing.expectEqual(ok, abi.ra8_app_nav_go_index(&nav, 0));
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 10), active.?.id);
    _ = abi.ra8_app_nav_depth(&nav, &depth);
    try std.testing.expectEqual(@as(u16, 0), depth);

    try std.testing.expectEqual(ok, abi.ra8_app_nav_go_index(&nav, 1));
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 20), active.?.id);
    _ = abi.ra8_app_nav_depth(&nav, &depth);
    try std.testing.expectEqual(@as(u16, 1), depth);
    try std.testing.expectEqual(@as(u32, 1), r0.leave_calls);
    try std.testing.expectEqual(@as(u32, 1), r1.enter_calls);

    var popped = false;
    try std.testing.expectEqual(ok, abi.ra8_app_nav_back(&nav, &popped));
    try std.testing.expect(popped);
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 10), active.?.id);
    _ = abi.ra8_app_nav_depth(&nav, &depth);
    try std.testing.expectEqual(@as(u16, 0), depth);
    try std.testing.expectEqual(@as(u32, 2), r0.enter_calls);

    try std.testing.expectEqual(ok, abi.ra8_app_nav_go_index(&nav, 0));
    _ = abi.ra8_app_nav_depth(&nav, &depth);
    try std.testing.expectEqual(@as(u16, 0), depth);
    try std.testing.expectEqual(@as(u32, 2), r0.enter_calls);

    popped = true;
    try std.testing.expectEqual(ok, abi.ra8_app_nav_back(&nav, &popped));
    try std.testing.expect(!popped);

    try std.testing.expectEqual(out_of_range, abi.ra8_app_nav_go_index(&nav, 9));
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 10), active.?.id);
}

test "nav_go_index: a valid index whose slot is NULL is rejected" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{ .apps = &slots, .cap = 1, .count = 1, .active = abi.none_index };
    var trail: [1]u16 = .{0};
    var nav = abi.Nav{};
    _ = abi.ra8_app_nav_init(&nav, &reg, &trail, 1);

    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_go_index(&nav, 0));
    try std.testing.expectEqualStrings("registry slot at idx must not be nullptr", lastMessage());
}

test "nav_go: a full trail refuses the switch and leaves the focus unchanged" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 100, &full_vtable);
    var a1 = makeApp(&r1, 200, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);
    var trail: [1]u16 = .{0};
    var nav = abi.Nav{};
    _ = abi.ra8_app_nav_init(&nav, &reg, &trail, 1);

    try std.testing.expectEqual(ok, abi.ra8_app_nav_go_index(&nav, 0));
    try std.testing.expectEqual(ok, abi.ra8_app_nav_go_index(&nav, 1));
    try std.testing.expectEqual(no_mem, abi.ra8_app_nav_go_index(&nav, 0));
    var active: ?*abi.App = null;
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 200), active.?.id);
}

test "nav: every entry rejects a NULL nav and a NULL registry" {
    var bad = abi.Nav{ .reg = null };
    var popped = false;
    var depth: u16 = 0;
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_go_index(null, 0));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_go_index(&bad, 0));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_go(null, 0));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_go(&bad, 0));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_back(null, &popped));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_back(&bad, &popped));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_depth(null, &depth));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_depth(&bad, null));
}

test "nav_go: a failing launch is forwarded and pushes nothing" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    var trail: [2]u16 = .{ 0, 0 };
    var nav = abi.Nav{};
    _ = abi.ra8_app_nav_init(&nav, &reg, &trail, 2);

    try std.testing.expectEqual(not_found, abi.ra8_app_nav_go(&nav, 99));
    var depth: u16 = 99;
    _ = abi.ra8_app_nav_depth(&nav, &depth);
    try std.testing.expectEqual(@as(u16, 0), depth);
    var active: ?*abi.App = &a0;
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expect(active == null);
}

test "nav_back: a trail entry that no longer resolves forwards not_found intact" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);

    var trail: [1]u16 = .{77}; // never registered
    var nav = abi.Nav{ .reg = &reg, .stack = &trail, .cap = 1, .depth = 1 };
    var popped = true;
    try std.testing.expectEqual(not_found, abi.ra8_app_nav_back(&nav, &popped));
    try std.testing.expect(!popped);
    try std.testing.expectEqual(@as(u16, 1), nav.depth);
}

test "nav_back: out_popped is guarded before the depth is read" {
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    var trail: [1]u16 = .{0};
    var nav = abi.Nav{};
    _ = abi.ra8_app_nav_init(&nav, &reg, &trail, 1);
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_app_nav_back(&nav, null));
    try std.testing.expectEqualStrings("out_popped must not be nullptr", lastMessage());
}

test "state: unmounted, background and foreground are derived from the registry" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var a1 = makeApp(&r1, 2, &full_vtable);
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);

    var state: abi.AppState = .foreground;
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 99, &state));
    try std.testing.expectEqual(abi.AppState.unmounted, state);
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 1, &state));
    try std.testing.expectEqual(abi.AppState.background, state);

    _ = abi.ra8_app_launch(&reg, 1);
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 1, &state));
    try std.testing.expectEqual(abi.AppState.foreground, state);
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 2, &state));
    try std.testing.expectEqual(abi.AppState.background, state);

    _ = abi.ra8_app_launch(&reg, 2);
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 2, &state));
    try std.testing.expectEqual(abi.AppState.foreground, state);
    try std.testing.expectEqual(ok, abi.ra8_app_state(&reg, 1, &state));
    try std.testing.expectEqual(abi.AppState.background, state);

    try std.testing.expectEqual(null_ptr, abi.ra8_app_state(null, 1, &state));
    try std.testing.expectEqual(null_ptr, abi.ra8_app_state(&reg, 1, null));
}

test "uninstall: an unknown id is not_found and a core app is refused" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable); // core: removable stays false
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);

    try std.testing.expectEqual(not_found, abi.ra8_app_uninstall(&reg, 99));
    try std.testing.expectEqual(not_supported, abi.ra8_app_uninstall(&reg, 1));
    try std.testing.expectEqual(@as(u32, 0), r0.deinit_calls);
    try std.testing.expectEqual(@as(u16, 1), reg.count);
    try std.testing.expectEqual(null_ptr, abi.ra8_app_uninstall(null, 1));
}

test "uninstall: the focused app is busy until the chrome navigates away" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 2, &full_vtable);
    a0.removable = true;
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_launch(&reg, 2);

    try std.testing.expectEqual(busy, abi.ra8_app_uninstall(&reg, 2));
    try std.testing.expectEqual(@as(u32, 0), r0.deinit_calls);
    try std.testing.expectEqual(@as(u16, 1), reg.count);
}

test "uninstall: a removable app before the focus compacts and fixes the focus up" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var r2 = Recorder{};
    var r3 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable); // core
    var a1 = makeApp(&r1, 2, &full_vtable);
    var a2 = makeApp(&r2, 3, &full_vtable);
    var a3 = makeApp(&r3, 4, &full_vtable);
    a1.removable = true;
    a2.removable = true;
    a3.removable = true;
    var slots: [4]?*abi.App = .{ null, null, null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 4);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);
    _ = abi.ra8_app_register(&reg, &a2);
    _ = abi.ra8_app_register(&reg, &a3);

    _ = abi.ra8_app_launch(&reg, 4); // active = index 3
    try std.testing.expectEqual(ok, abi.ra8_app_uninstall(&reg, 2));
    try std.testing.expectEqual(@as(u32, 1), r1.deinit_calls);
    try std.testing.expect(!a1.initialized);
    try std.testing.expectEqual(@as(u16, 3), reg.count);

    var state: abi.AppState = .foreground;
    _ = abi.ra8_app_state(&reg, 2, &state);
    try std.testing.expectEqual(abi.AppState.unmounted, state);
    var active: ?*abi.App = null;
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 4), active.?.id);
    _ = abi.ra8_app_state(&reg, 4, &state);
    try std.testing.expectEqual(abi.AppState.foreground, state);
}

test "uninstall: removing the tail leaves an earlier focus untouched" {
    var r0 = Recorder{};
    var r1 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    var a1 = makeApp(&r1, 2, &full_vtable);
    a1.removable = true;
    var slots: [2]?*abi.App = .{ null, null };
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 2);
    _ = abi.ra8_app_register(&reg, &a0);
    _ = abi.ra8_app_register(&reg, &a1);
    _ = abi.ra8_app_launch(&reg, 1); // active = index 0

    try std.testing.expectEqual(ok, abi.ra8_app_uninstall(&reg, 2));
    try std.testing.expectEqual(@as(u16, 1), reg.count);
    var active: ?*abi.App = null;
    _ = abi.ra8_app_active(&reg, &active);
    try std.testing.expectEqual(@as(u16, 1), active.?.id);
}

test "uninstall: a removable app with no deinit unmounts cleanly" {
    var rbare = Recorder{};
    var abare = makeApp(&rbare, 5, &bare_vtable);
    abare.removable = true;
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &abare);

    try std.testing.expectEqual(ok, abi.ra8_app_uninstall(&reg, 5));
    try std.testing.expectEqual(@as(u32, 0), rbare.deinit_calls);
    try std.testing.expectEqual(@as(u16, 0), reg.count);
}

test "uninstall: with no focus at all the active index stays none" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 7, &full_vtable);
    a0.removable = true;
    var slots: [1]?*abi.App = .{null};
    var reg = abi.Registry{};
    _ = abi.ra8_app_registry_init(&reg, &slots, 1);
    _ = abi.ra8_app_register(&reg, &a0);

    try std.testing.expectEqual(ok, abi.ra8_app_uninstall(&reg, 7));
    try std.testing.expectEqual(@as(u16, 0), reg.count);
    try std.testing.expectEqual(abi.none_index, reg.active);
}

test "uninstall: a NULL registry slot is reported rather than dereferenced" {
    var r0 = Recorder{};
    var a0 = makeApp(&r0, 1, &full_vtable);
    a0.removable = true;
    var slots: [2]?*abi.App = .{ &a0, null };
    var reg = abi.Registry{ .apps = &slots, .cap = 2, .count = 2, .active = abi.none_index };

    // The live app still uninstalls; the NULL neighbour is simply never matched.
    try std.testing.expectEqual(ok, abi.ra8_app_uninstall(&reg, 1));
    try std.testing.expectEqual(@as(u16, 1), reg.count);
    try std.testing.expectEqual(not_found, abi.ra8_app_uninstall(&reg, 1));
}

test "ABI layouts match the published C structs" {
    const word = @sizeOf(usize);
    try std.testing.expectEqual(7 * word, @sizeOf(abi.Vtable));
    try std.testing.expectEqual(2 * word, @offsetOf(abi.App, "id"));
    try std.testing.expectEqual(word + 4, @offsetOf(abi.Registry, "active"));
    try std.testing.expectEqual(2 * word + 2, @offsetOf(abi.Nav, "depth"));
    try std.testing.expectEqual(@as(u16, 0x504), @intFromEnum(abi.AppError.null_ptr));
    try std.testing.expectEqual(@as(u16, 0x408), @intFromEnum(abi.AppError.conflict));
    try std.testing.expectEqual(@as(u16, 0x109), @intFromEnum(abi.AppError.busy));
    try std.testing.expectEqual(@as(i16, -1), abi.none_index);
}
