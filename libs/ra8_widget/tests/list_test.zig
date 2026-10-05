const std = @import("std");
const abi = @import("abi");

var invalidations: u32 = 0;
var last_refresh: u8 = 0;
export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}
export fn ra8_widget_invalidate(w: *abi.Widget, refresh: u8) callconv(.c) u16 {
    invalidations += 1;
    last_refresh = refresh;
    w.dirty = true;
    w.refresh = refresh;
    return abi.err.ok;
}
var routed_action: u16 = 0;
fn selected(_: *abi.Widget, action_id: u16) callconv(.c) void {
    routed_action = action_id;
}
fn widget() abi.Widget {
    return .{ .vt = null, .ctx = null, .rect = .{ .x = 10, .y = 20, .w = 120, .h = 100 }, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}
const rows = [_]abi.Row{
    .{ .title = "Settings", .subtitle = "Preferences", .trailing_text = null, .action_id = 11, .trailing = .chevron },
    .{ .title = "Listen", .subtitle = "Continue audio", .trailing_text = "12 min", .action_id = 22, .trailing = .value },
    .{ .title = "Activity", .subtitle = "Reading history", .trailing_text = null, .action_id = 33, .trailing = .chevron },
};

fn listDescriptor() abi.List {
    return .{ .paint = null, .rows = &rows, .count = 3, .on_select = selected, .bg = 0xFFFFFF, .title_fg = 0x111111, .subtitle_fg = 0x666666, .trailing_fg = 0x333333, .divider = 0xDDDDDD, .row_height = 40, .pad = 8, .selected = 0, .has_selection = false, .damage = .{ .x = 0, .y = 0, .w = 0, .h = 0 } };
}
test "row geometry tiles fixed-height rows and clips the last visible row" {
    const r = abi.Rect{ .x = 10, .y = 20, .w = 120, .h = 100 };
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 20, .w = 120, .h = 40 }, abi.rowRect(r, 40, 0));
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 60, .w = 120, .h = 40 }, abi.rowRect(r, 40, 1));
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 100, .w = 120, .h = 20 }, abi.rowRect(r, 40, 2));
}
test "hit testing routes only points inside a real row" {
    const r = abi.Rect{ .x = 10, .y = 20, .w = 120, .h = 100 };
    try std.testing.expectEqual(@as(?u16, 0), abi.hitRow(r, 40, 2, 20));
    try std.testing.expectEqual(@as(?u16, 1), abi.hitRow(r, 40, 2, 60));
    try std.testing.expectEqual(@as(?u16, null), abi.hitRow(r, 40, 2, 100));
    try std.testing.expectEqual(@as(?u16, null), abi.hitRow(r, 0, 2, 20));
}
test "tap selects row action and reports exactly that row as damage" {
    invalidations = 0;
    routed_action = 0;
    var w = widget();
    var list = listDescriptor();
    list.on_select = selected;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_list_init(&w, &list));
    const event = abi.Event{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 50, .y = 73 };
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expectEqual(@as(u16, 1), list.selected);
    try std.testing.expect(list.has_selection);
    try std.testing.expectEqual(@as(u16, 22), routed_action);
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 60, .w = 120, .h = 40 }, list.damage);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expectEqual(@as(u8, 1), last_refresh);
}
test "Settings and Activity rows route their own action ids" {
    var w = widget();
    var list = listDescriptor();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_list_init(&w, &list));
    const events = [_]abi.Event{
        .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 30, .y = 30 },
        .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 30, .y = 103 },
    };
    for (events, 0..) |event, i| {
        routed_action = 0;
        try std.testing.expect(w.vt.?.on_input.?(&w, &event));
        try std.testing.expectEqual(if (i == 0) @as(u16, 11) else @as(u16, 33), routed_action);
        try std.testing.expectEqual(@as(u16, @intCast(i * 2)), list.selected);
        try std.testing.expectEqual(if (i == 0) @as(i32, 20) else @as(i32, 100), list.damage.y);
    }
}
test "tap outside rows is declined without changing state or damage" {
    var w = widget();
    var list = listDescriptor();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_list_init(&w, &list));
    const event = abi.Event{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 50, .y = 130 };
    try std.testing.expect(!w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(!list.has_selection);
    try std.testing.expectEqual(abi.Rect{ .x = 0, .y = 0, .w = 0, .h = 0 }, list.damage);
}

var routed_row: u16 = 0;
var routed_element: u8 = 0;
var routed_element_action: u16 = 0;

fn selectedElement(_: *abi.Widget, row: u16, element: u8, action_id: u16) callconv(.c) void {
    routed_row = row;
    routed_element = element;
    routed_element_action = action_id;
}

test "two-button rows report each element and limit damage to that button" {
    const variant_rows = [_]abi.Row{.{
        .title = "Apps",
        .subtitle = null,
        .trailing_text = null,
        .action_id = 30,
        .trailing = .none,
        .variant = .two_buttons,
        .button_1_text = "Open",
        .button_1_action_id = 31,
        .button_2_text = "Remove",
        .button_2_action_id = 32,
    }};
    var w = widget();
    var list = listDescriptor();
    list.rows = &variant_rows;
    list.count = variant_rows.len;
    list.on_select_element = selectedElement;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_list_init(&w, &list));

    const first = abi.Event{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 85, .y = 35 };
    try std.testing.expect(w.vt.?.on_input.?(&w, &first));
    try std.testing.expectEqual(@as(u16, 0), routed_row);
    try std.testing.expectEqual(@intFromEnum(abi.Element.button_1), routed_element);
    try std.testing.expectEqual(@as(u16, 31), routed_element_action);
    try std.testing.expectEqual(abi.Rect{ .x = 70, .y = 20, .w = 30, .h = 40 }, list.damage);

    const second = abi.Event{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 110, .y = 35 };
    try std.testing.expect(w.vt.?.on_input.?(&w, &second));
    try std.testing.expectEqual(@intFromEnum(abi.Element.button_2), routed_element);
    try std.testing.expectEqual(@as(u16, 32), routed_element_action);
    try std.testing.expectEqual(abi.Rect{ .x = 100, .y = 20, .w = 30, .h = 40 }, list.damage);
}

test "toggle-help rows flip the value and damage only the checkbox" {
    var checked = false;
    const variant_rows = [_]abi.Row{.{
        .title = "Airplane mode",
        .subtitle = null,
        .trailing_text = null,
        .action_id = 41,
        .trailing = .none,
        .variant = .toggle_help,
        .help_text = "Disable wireless radios",
        .toggle_value = &checked,
    }};
    var w = widget();
    var list = listDescriptor();
    list.rows = &variant_rows;
    list.count = variant_rows.len;
    list.on_select_element = selectedElement;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_list_init(&w, &list));

    const event = abi.Event{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 94, .y = 30 };
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(checked);
    try std.testing.expectEqual(@as(u16, 0), routed_row);
    try std.testing.expectEqual(@intFromEnum(abi.Element.toggle), routed_element);
    try std.testing.expectEqual(@as(u16, 41), routed_element_action);
    try std.testing.expectEqual(abi.Rect{ .x = 90, .y = 26, .w = 28, .h = 28 }, list.damage);
}
