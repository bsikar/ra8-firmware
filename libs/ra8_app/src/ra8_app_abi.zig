//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_app/inc/ra8_app.h`. The table arithmetic and
//! the back-stack rule live in `internal/root.zig`; this file owns the
//! exported symbols, the caller-owned struct layouts, the pointer guards in
//! their original order, the `ra8_err_t` mapping, and the diagnostic lines the
//! C emitted through `RA8_CHECK_NULL_PTR`.
//!
//! Guard order is part of the contract. `ra8_app_register` reports a duplicate
//! id before a full registry, `ra8_app_uninstall` refuses a core app before it
//! refuses the focused one, and `ra8_app_nav_go_index` resolves the index
//! before it rejects a NULL slot, so the host suite can tell each rejection
//! apart by the code it gets back.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Lifecycle state of one app (`ra8_app_state_t`).
pub const AppState = implementation.AppState;
/// `k_ra8_app_none`.
pub const none_index = implementation.none_index;

/// Subset of `ra8_err_t` this library returns.
pub const AppError = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    not_found = 0x106,
    not_supported = 0x107,
    busy = 0x109,
    out_of_range = 0x208,
    conflict = 0x408,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_app";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn rejectNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(AppError.null_ptr);
}

fn code(err: AppError) u16 {
    return @intFromEnum(err);
}

/// `ra8_widget_event_t`, forwarded to the focused app untouched. The framework
/// never reads it, so it stays opaque here and `ra8_widget` keeps owning the
/// layout.
pub const WidgetEvent = opaque {};

/// App lifecycle callbacks (`ra8_app_vtable_t`).
pub const Vtable = extern struct {
    init: ?*const fn (app: *App) callconv(.c) u16 = null,
    on_enter: ?*const fn (app: *App) callconv(.c) void = null,
    tick: ?*const fn (app: *App) callconv(.c) void = null,
    render: ?*const fn (app: *const App) callconv(.c) void = null,
    on_input: ?*const fn (app: *App, event: *const WidgetEvent) callconv(.c) bool = null,
    on_leave: ?*const fn (app: *App) callconv(.c) void = null,
    deinit: ?*const fn (app: *App) callconv(.c) void = null,
};

/// One app instance and its metadata (`ra8_app_t`, caller-owned static).
pub const App = extern struct {
    vt: ?*const Vtable = null,
    ctx: ?*anyopaque = null,
    id: u16 = 0,
    name: ?[*:0]const u8 = null,
    removable: bool = false,
    initialized: bool = false,
};

/// Fixed table of registered apps plus the focused index (`ra8_app_registry_t`).
pub const Registry = extern struct {
    apps: ?[*]?*App = null,
    cap: u16 = 0,
    count: u16 = 0,
    active: i16 = none_index,
};

/// Bounded back-stack of app ids over a registry (`ra8_app_nav_t`).
pub const Nav = extern struct {
    reg: ?*Registry = null,
    stack: ?[*]u16 = null,
    cap: u16 = 0,
    depth: u16 = 0,
};

// The four layouts above are the published C ABI. Offsets are asserted in
// pointer-width multiples so they hold on the 64-bit host and on 32-bit Arm.
comptime {
    const word = @sizeOf(usize);

    std.debug.assert(@sizeOf(Vtable) == 7 * word);
    std.debug.assert(@offsetOf(Vtable, "on_input") == 4 * word);
    std.debug.assert(@offsetOf(Vtable, "deinit") == 6 * word);

    std.debug.assert(@offsetOf(App, "ctx") == word);
    std.debug.assert(@offsetOf(App, "id") == 2 * word);
    std.debug.assert(@offsetOf(App, "name") == 3 * word);
    std.debug.assert(@offsetOf(App, "removable") == 4 * word);
    std.debug.assert(@offsetOf(App, "initialized") == 4 * word + 1);
    std.debug.assert(@sizeOf(App) == std.mem.alignForward(usize, 4 * word + 2, word));

    std.debug.assert(@offsetOf(Registry, "cap") == word);
    std.debug.assert(@offsetOf(Registry, "count") == word + 2);
    std.debug.assert(@offsetOf(Registry, "active") == word + 4);
    std.debug.assert(@sizeOf(Registry) == std.mem.alignForward(usize, word + 6, word));

    std.debug.assert(@offsetOf(Nav, "stack") == word);
    std.debug.assert(@offsetOf(Nav, "cap") == 2 * word);
    std.debug.assert(@offsetOf(Nav, "depth") == 2 * word + 2);
    std.debug.assert(@sizeOf(Nav) == std.mem.alignForward(usize, 2 * word + 4, word));

    std.debug.assert(@sizeOf(AppState) == 1);
}

const Table = implementation.Table(App);

const empty_slots: []?*App = &[_]?*App{};

/// The live slice of the registry table.
///
/// A registry whose `apps` is NULL holds nothing: `ra8_app_registry_init`
/// rejects NULL storage, so this only shields a hand-built registry from the
/// dereference the C would have made.
fn liveSlots(reg: *const Registry) []?*App {
    const base = reg.apps orelse return empty_slots;
    return base[0..reg.count];
}

/// Slot at a registry index, or null when the index is out of the live range.
fn slotAt(reg: *const Registry, index: i16) ?*App {
    if (index < 0) return null;
    const position: u16 = @bitCast(index);
    const slots = liveSlots(reg);
    if (position >= slots.len) return null;
    return slots[position];
}

/// Focused app, or null when nothing is focused.
fn activeSlot(reg: *const Registry) ?*App {
    if (reg.active == none_index) return null;
    return slotAt(reg, reg.active);
}

// ============================================================================
// Registry lifecycle
// ============================================================================

/// Bind a registry to caller-owned pointer storage (empty, no focus).
pub export fn ra8_app_registry_init(
    reg: ?*Registry,
    storage: ?[*]?*App,
    cap: u16,
) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const slots = storage orelse return rejectNull("storage must not be nullptr");
    if (cap == 0) return code(.invalid_arg);
    registry.apps = slots;
    registry.cap = cap;
    registry.count = 0;
    registry.active = none_index;
    return code(.ok);
}

/// Find a registered app's index by id.
pub export fn ra8_app_find(
    reg: ?*const Registry,
    id: u16,
    out_idx: ?*i16,
) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const out = out_idx orelse return rejectNull("out_idx must not be nullptr");
    out.* = Table.find(liveSlots(registry), id);
    return code(.ok);
}

/// Register an app (running its `init` once) and add it to the registry.
pub export fn ra8_app_register(reg: ?*Registry, app: ?*App) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const instance = app orelse return rejectNull("app must not be nullptr");
    const vtable = instance.vt orelse return rejectNull("app->vt must not be nullptr");

    // A duplicate id is a conflict regardless of capacity (checked first so a
    // full registry still reports the more precise error).
    if (Table.find(liveSlots(registry), instance.id) != none_index) return code(.conflict);
    if (registry.count >= registry.cap) return code(.no_mem);
    const base = registry.apps orelse return rejectNull("reg->apps must not be nullptr");

    if (vtable.init) |init_fn| {
        const init_err = init_fn(instance);
        if (init_err != code(.ok)) return init_err; // leave unregistered
    }
    instance.initialized = true;
    base[registry.count] = instance;
    registry.count +%= 1;
    return code(.ok);
}

// ============================================================================
// Focus and routing
// ============================================================================

/// Focus the app with `id`, running the focus lifecycle.
pub export fn ra8_app_launch(reg: ?*Registry, id: u16) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const target = Table.find(liveSlots(registry), id);
    if (target == none_index) return code(.not_found);
    if (target == registry.active) return code(.ok); // already focused: idempotent

    if (registry.active != none_index) {
        if (activeSlot(registry)) |current| {
            if (current.vt) |vtable| {
                if (vtable.on_leave) |leave_fn| leave_fn(current);
            }
        }
    }
    registry.active = target;
    if (slotAt(registry, target)) |next| {
        if (next.vt) |vtable| {
            if (vtable.on_enter) |enter_fn| enter_fn(next);
        }
    }
    return code(.ok);
}

/// Report the currently focused app (null when none).
pub export fn ra8_app_active(reg: ?*const Registry, out_app: ?*?*App) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const out = out_app orelse return rejectNull("out_app must not be nullptr");
    out.* = activeSlot(registry);
    return code(.ok);
}

/// Route an input event to the focused app.
pub export fn ra8_app_route_input(
    reg: ?*Registry,
    event: ?*const WidgetEvent,
    out_handled: ?*bool,
) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const input = event orelse return rejectNull("ev must not be nullptr");
    const handled = out_handled orelse return rejectNull("out_handled must not be nullptr");
    handled.* = false;
    if (registry.active == none_index) return code(.ok);
    if (activeSlot(registry)) |app| {
        if (app.vt) |vtable| {
            if (vtable.on_input) |input_fn| handled.* = input_fn(app, input);
        }
    }
    return code(.ok);
}

/// Run the focused app's per-frame `tick` (no-op when there is none).
pub export fn ra8_app_tick(reg: ?*Registry) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    if (registry.active == none_index) return code(.ok);
    if (activeSlot(registry)) |app| {
        if (app.vt) |vtable| {
            if (vtable.tick) |tick_fn| tick_fn(app);
        }
    }
    return code(.ok);
}

/// Run the focused app's `render` (on-target; no-op when there is none).
pub export fn ra8_app_render(reg: ?*Registry) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    if (registry.active == none_index) return code(.ok);
    if (activeSlot(registry)) |app| {
        if (app.vt) |vtable| {
            if (vtable.render) |render_fn| render_fn(app);
        }
    }
    return code(.ok);
}

/// Number of registered apps (for the launcher to list).
pub export fn ra8_app_count(reg: ?*const Registry, out_count: ?*u16) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const out = out_count orelse return rejectNull("out_count must not be nullptr");
    out.* = registry.count;
    return code(.ok);
}

/// App at registry index `idx` (launcher enumeration).
pub export fn ra8_app_at(
    reg: ?*const Registry,
    idx: u16,
    out_app: ?*?*App,
) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const out = out_app orelse return rejectNull("out_app must not be nullptr");
    if (!implementation.inRange(idx, registry.count)) return code(.out_of_range);
    out.* = liveSlots(registry)[idx];
    return code(.ok);
}

// ============================================================================
// Navigation back-stack
// ============================================================================

/// Bind a navigation back-stack to a registry and caller-owned storage.
pub export fn ra8_app_nav_init(
    nav: ?*Nav,
    reg: ?*Registry,
    storage: ?[*]u16,
    cap: u16,
) callconv(.c) u16 {
    const navigation = nav orelse return rejectNull("nav must not be nullptr");
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const trail = storage orelse return rejectNull("storage must not be nullptr");
    if (cap == 0) return code(.invalid_arg);
    navigation.reg = registry;
    navigation.stack = trail;
    navigation.cap = cap;
    navigation.depth = 0;
    return code(.ok);
}

/// Focus app `id`, pushing the outgoing app onto the back-stack.
pub export fn ra8_app_nav_go(nav: ?*Nav, id: u16) callconv(.c) u16 {
    const navigation = nav orelse return rejectNull("nav must not be nullptr");
    const registry = navigation.reg orelse return rejectNull("nav->reg must not be nullptr");

    const current = activeSlot(registry);
    const plan = implementation.pushPlan(if (current) |app| app.id else null, id);
    if (plan.push and implementation.stackFull(navigation.depth, navigation.cap)) {
        return code(.no_mem);
    }

    const launch_err = ra8_app_launch(registry, id);
    if (launch_err != code(.ok)) return launch_err;

    if (plan.push) {
        const trail = navigation.stack orelse return rejectNull("nav->stack must not be nullptr");
        trail[navigation.depth] = plan.prev_id;
        navigation.depth +%= 1;
    }
    return code(.ok);
}

/// Focus the app at registry index `idx` (launcher select-by-position).
pub export fn ra8_app_nav_go_index(nav: ?*Nav, idx: u16) callconv(.c) u16 {
    const navigation = nav orelse return rejectNull("nav must not be nullptr");
    const registry = navigation.reg orelse return rejectNull("nav->reg must not be nullptr");

    var app: ?*App = null;
    const at_err = ra8_app_at(registry, idx, &app);
    if (at_err != code(.ok)) return at_err; // out_of_range for idx >= count
    const resolved = app orelse return rejectNull("registry slot at idx must not be nullptr");
    return ra8_app_nav_go(navigation, resolved.id);
}

/// Return to the most recently pushed app (pop the back-stack).
pub export fn ra8_app_nav_back(nav: ?*Nav, out_popped: ?*bool) callconv(.c) u16 {
    const navigation = nav orelse return rejectNull("nav must not be nullptr");
    const registry = navigation.reg orelse return rejectNull("nav->reg must not be nullptr");
    const popped = out_popped orelse return rejectNull("out_popped must not be nullptr");
    popped.* = false;
    if (navigation.depth == 0) return code(.ok); // at the root: nothing to go back to

    const trail = navigation.stack orelse return rejectNull("nav->stack must not be nullptr");
    const prev_id = trail[navigation.depth - 1];
    const launch_err = ra8_app_launch(registry, prev_id);
    if (launch_err != code(.ok)) return launch_err;

    navigation.depth -%= 1;
    popped.* = true;
    return code(.ok);
}

/// Current back-stack depth.
pub export fn ra8_app_nav_depth(nav: ?*const Nav, out_depth: ?*u16) callconv(.c) u16 {
    const navigation = nav orelse return rejectNull("nav must not be nullptr");
    const out = out_depth orelse return rejectNull("out_depth must not be nullptr");
    out.* = navigation.depth;
    return code(.ok);
}

// ============================================================================
// Lifecycle state and uninstall
// ============================================================================

/// Report an app's lifecycle state, derived from the registry.
pub export fn ra8_app_state(
    reg: ?*const Registry,
    id: u16,
    out_state: ?*AppState,
) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const out = out_state orelse return rejectNull("out_state must not be nullptr");
    const index = Table.find(liveSlots(registry), id);
    out.* = implementation.stateFor(index, registry.active);
    return code(.ok);
}

/// Uninstall (unmount) a removable, non-focused app from the registry.
pub export fn ra8_app_uninstall(reg: ?*Registry, id: u16) callconv(.c) u16 {
    const registry = reg orelse return rejectNull("reg must not be nullptr");
    const index = Table.find(liveSlots(registry), id);
    if (index == none_index) return code(.not_found);

    const app = slotAt(registry, index) orelse
        return rejectNull("registry slot must not be nullptr");
    if (!app.removable) return code(.not_supported); // core app: uninstall refused
    if (index == registry.active) return code(.busy); // the focused app cannot be unmounted

    if (app.vt) |vtable| {
        if (vtable.deinit) |deinit_fn| deinit_fn(app);
    }
    app.initialized = false;

    const position: u16 = @bitCast(index);
    Table.compactAt(liveSlots(registry), position);
    registry.count -%= 1;
    registry.active = implementation.adjustActive(registry.active, position);
    return code(.ok);
}
