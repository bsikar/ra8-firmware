//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Debug-only widget tree channel. Applications register stable names and
//! readable state strings against caller-owned widgets. A successful panel
//! compose publishes a bounded, flat snapshot that an emulator can read from
//! the exported `ra8_widget_debug_tree` symbol.

/// Widget layout mirrored for this standalone channel module. The panel
/// membrane verifies its size and offsets against the library's shared type.
pub const SnapshotWidget = extern struct {
    vtable: ?*const anyopaque,
    context: ?*anyopaque,
    rect: Rect,
    fixed: i16,
    flex: u16,
    action_id: u16,
    refresh: u8,
    visible: bool,
    dirty: bool,
};

/// Geometry matching the published `ra8_ui_rect_t` layout.
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// Errors returned by this optional channel.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};

/// Fixed protocol dimensions. Names and state values are copied at registration
/// time so the emulator never follows a borrowed string pointer.
pub const limits = struct {
    /// Maximum number of registered widget identities.
    pub const registrations: usize = 64;
    /// Maximum records in one published screen.
    pub const records: usize = 64;
    /// Bytes including the terminator in a widget name.
    pub const name_bytes: usize = 32;
    /// Bytes including the terminator in a widget kind.
    pub const kind_bytes: usize = 16;
    /// Bytes including the terminator in a widget state.
    pub const state_bytes: usize = 24;
    /// Maximum nested panel depth visited in one snapshot.
    pub const panel_depth: usize = 16;
};

/// Record encoding published to the emulator as one row in the current tree.
pub const Record = extern struct {
    name: [limits.name_bytes]u8,
    kind: [limits.kind_bytes]u8,
    state: [limits.state_bytes]u8,
    rect: Rect,
};

/// Exported channel header and the latest complete compose snapshot.
pub const Channel = extern struct {
    magic: u32,
    version: u16,
    count: u16,
    generation: u32,
    truncated: bool,
    reserved: [3]u8,
    records: [limits.records]Record,
};

/// Magic identifying a valid widget-tree snapshot (`R8WT`).
pub const protocol = struct {
    pub const magic: u32 = 0x52385754;
    pub const version: u16 = 1;
};

const Registration = struct {
    widget: *SnapshotWidget,
    name: [limits.name_bytes]u8,
    kind: [limits.kind_bytes]u8,
    state: [limits.state_bytes]u8,
};

var registered: [limits.registrations]Registration = undefined;
var registered_count: usize = 0;

/// Stable symbol for emulator memory inspection. Debug builds keep the
/// snapshot in zero-initialized SRAM so its generation starts deterministically.
pub export var ra8_widget_debug_tree: Channel = .{
    .magic = 0,
    .version = 0,
    .count = 0,
    .generation = 0,
    .truncated = false,
    .reserved = @splat(0),
    .records = @splat(.{
        .name = @splat(0),
        .kind = @splat(0),
        .state = @splat(0),
        .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
    }),
};

/// Bounded child lookup supplied by the panel module, avoiding a dependency on
/// panel internals from this channel's protocol module.
pub const ChildrenFn = *const fn (*SnapshotWidget) ?[]SnapshotWidget;

/// Copy a bounded NUL-terminated string, rejecting values without room for a
/// terminator. All input reads stop at the destination's fixed capacity.
fn copyText(comptime size: usize, out: *[size]u8, text: [*:0]const u8) bool {
    for (0..size) |index| {
        const byte = text[index];
        out[index] = byte;
        if (byte == 0) return true;
    }
    out[size - 1] = 0;
    return false;
}

fn find(widget: *SnapshotWidget) ?usize {
    for (registered[0..registered_count], 0..) |entry, index| {
        if (entry.widget == widget) return index;
    }
    return null;
}

/// Register or replace the stable name, kind, and initial state for a widget.
pub export fn ra8_widget_debug_register(
    widget: ?*SnapshotWidget,
    name: ?[*:0]const u8,
    kind: ?[*:0]const u8,
    state: ?[*:0]const u8,
) callconv(.c) u16 {
    const instance = widget orelse return err.null_ptr;
    const widget_name = name orelse return err.null_ptr;
    const widget_kind = kind orelse return err.null_ptr;
    const widget_state = state orelse return err.null_ptr;

    var entry = Registration{
        .widget = instance,
        .name = undefined,
        .kind = undefined,
        .state = undefined,
    };
    if (!copyText(limits.name_bytes, &entry.name, widget_name)) return err.invalid_arg;
    if (!copyText(limits.kind_bytes, &entry.kind, widget_kind)) return err.invalid_arg;
    if (!copyText(limits.state_bytes, &entry.state, widget_state)) return err.invalid_arg;

    if (find(instance)) |index| {
        registered[index] = entry;
        return err.ok;
    }
    if (registered_count == limits.registrations) return err.invalid_arg;
    registered[registered_count] = entry;
    registered_count += 1;
    return err.ok;
}

/// Replace a registered widget's current readable state (for example,
/// `pressed` or `checked`) before its next compose snapshot.
pub export fn ra8_widget_debug_set_state(
    widget: ?*SnapshotWidget,
    state: ?[*:0]const u8,
) callconv(.c) u16 {
    const instance = widget orelse return err.null_ptr;
    const value = state orelse return err.null_ptr;
    const index = find(instance) orelse return err.invalid_arg;
    if (!copyText(limits.state_bytes, &registered[index].state, value)) return err.invalid_arg;
    return err.ok;
}

/// Forget one widget identity, preserving registration order for the rest.
pub export fn ra8_widget_debug_unregister(widget: ?*SnapshotWidget) callconv(.c) u16 {
    const instance = widget orelse return err.null_ptr;
    const index = find(instance) orelse return err.invalid_arg;
    for (index..registered_count - 1) |move| {
        registered[move] = registered[move + 1];
    }
    registered_count -= 1;
    return err.ok;
}

const Frame = struct {
    widgets: []SnapshotWidget,
    index: usize,
};

fn append(widget: *SnapshotWidget, count: *usize, truncated: *bool) void {
    if (count.* == limits.records) {
        truncated.* = true;
        return;
    }

    const default: [limits.name_bytes]u8 = @splat(0);
    var record: Record = .{
        .name = default,
        .kind = @splat(0),
        .state = @splat(0),
        .rect = widget.rect,
    };
    if (find(widget)) |index| {
        const entry = &registered[index];
        record.name = entry.name;
        record.kind = entry.kind;
        record.state = entry.state;
    }
    ra8_widget_debug_tree.records[count.*] = record;
    count.* += 1;
}

/// Publish the visible tree after a compose. Child frames and the output are
/// fixed-capacity; overflow sets `truncated` instead of allocating or walking
/// beyond a known bound.
pub fn publish(root: *SnapshotWidget, child_fn: ChildrenFn) void {
    var count: usize = 0;
    var truncated = false;
    var frames: [limits.panel_depth]Frame = undefined;

    ra8_widget_debug_tree.magic = 0;
    ra8_widget_debug_tree.version = protocol.version;
    ra8_widget_debug_tree.count = 0;
    ra8_widget_debug_tree.generation +%= 1;
    ra8_widget_debug_tree.truncated = false;
    ra8_widget_debug_tree.reserved = @splat(0);

    if (root.visible) {
        append(root, &count, &truncated);
        if (child_fn(root)) |kids| {
            if (kids.len > 0) {
                frames[0] = .{ .widgets = kids, .index = 0 };
                var depth: usize = 1;
                while (depth > 0) {
                    if (count == limits.records) {
                        truncated = true;
                        break;
                    }
                    const frame = &frames[depth - 1];
                    if (frame.index == frame.widgets.len) {
                        depth -= 1;
                        continue;
                    }
                    const child = &frame.widgets[frame.index];
                    frame.index += 1;
                    if (!child.visible) continue;

                    append(child, &count, &truncated);
                    if (child_fn(child)) |grandchildren| {
                        if (grandchildren.len == 0) continue;
                        if (depth == limits.panel_depth) {
                            truncated = true;
                            continue;
                        }
                        frames[depth] = .{ .widgets = grandchildren, .index = 0 };
                        depth += 1;
                    }
                }
            }
        }
    }

    ra8_widget_debug_tree.count = @intCast(count);
    ra8_widget_debug_tree.truncated = truncated;
    ra8_widget_debug_tree.magic = protocol.magic;
}

comptime {
    if (@sizeOf(Record) != limits.name_bytes + limits.kind_bytes + limits.state_bytes + @sizeOf(Rect)) {
        @compileError("ra8_widget_debug_record_t layout");
    }
    if (@offsetOf(Channel, "records") != 16) @compileError("ra8_widget_debug_tree_t records offset");
}
