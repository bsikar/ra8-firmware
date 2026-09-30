//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_xml/inc/ra8_xml_writer.h`. The emitter itself
//! lives in `internal/writer.zig` over slices; this file owns the published
//! struct layout, the null-pointer guards, the NUL terminator the C callers
//! read, and the `ra8_err_t` mapping.
//!
//! `ra8_xml_writer_t` and `ra8_xml_writer_frame_t` sit on caller stacks, so
//! their layout is part of the ABI rather than an implementation detail. It is
//! pinned below by comptime assertion, pointer-width aware so the same asserts
//! hold for the 64-bit host build and the 32-bit Arm cross build.
//!
//! Guard order is part of the contract: `tests/misc/src/test_ra8_xml_writer.c`
//! tells `invalid_arg` from `invalid_state` by which check fires first, so the
//! checks below run in exactly the order the C wrote them.

const std = @import("std");

const entity = @import("internal/entity.zig");
const name = @import("internal/name.zig");
const writer = @import("internal/writer.zig");

/// Subset of `ra8_err_t` this library returns.
pub const XmlError = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
};

/// One open element (`ra8_xml_writer_frame_t`).
pub const Frame = writer.Frame;

/// Builder state over caller storage (`ra8_xml_writer_t`).
pub const Writer = extern struct {
    out: ?[*]u8,
    frames: ?[*]Frame,
    cap: usize,
    len: usize,
    frame_cap: u32,
    depth: u32,
    status: XmlError,
    tag_open: bool,
    ready: bool,
};

comptime {
    if (@sizeOf(XmlError) != 2) @compileError("ra8_err_t width");
    if (@intFromEnum(XmlError.ok) != 0) @compileError("k_ra8_ok value");
    if (@intFromEnum(XmlError.no_mem) != 0x102) @compileError("k_ra8_err_no_mem");
    if (@intFromEnum(XmlError.invalid_arg) != 0x103) @compileError("k_ra8_err_invalid_arg");
    if (@intFromEnum(XmlError.invalid_state) != 0x104) @compileError("k_ra8_err_invalid_state");

    if (@sizeOf(Frame) != name.limits.name_cap) @compileError("frame size");
    if (@alignOf(Frame) != 1) @compileError("frame alignment");

    const word = @sizeOf(usize);
    if (@offsetOf(Writer, "out") != 0) @compileError("writer out offset");
    if (@offsetOf(Writer, "frames") != word) @compileError("writer frames offset");
    if (@offsetOf(Writer, "cap") != word * 2) @compileError("writer cap offset");
    if (@offsetOf(Writer, "len") != word * 3) @compileError("writer len offset");
    if (@offsetOf(Writer, "frame_cap") != word * 4) @compileError("writer frame_cap offset");
    if (@offsetOf(Writer, "depth") != word * 4 + 4) @compileError("writer depth offset");
    if (@offsetOf(Writer, "status") != word * 4 + 8) @compileError("writer status offset");
    if (@offsetOf(Writer, "tag_open") != word * 4 + 10) @compileError("writer tag_open offset");
    if (@offsetOf(Writer, "ready") != word * 4 + 11) @compileError("writer ready offset");
}

/// `ra8_err_t` for a refusal raised by the emitter core.
fn code(err: writer.Error) XmlError {
    return switch (err) {
        writer.Error.NoSpace => .no_mem,
        writer.Error.InvalidArgument => .invalid_arg,
        writer.Error.InvalidState => .invalid_state,
    };
}

/// The emitter refusal a stored `ra8_err_t` stands for.
fn sticky(status: XmlError) ?writer.Error {
    return switch (status) {
        .ok => null,
        .no_mem => writer.Error.NoSpace,
        .invalid_arg => writer.Error.InvalidArgument,
        .invalid_state => writer.Error.InvalidState,
    };
}

/// A slice view of the caller's struct, for the core to work over.
fn view(w: *Writer) writer.Writer {
    return .{
        .out = w.out.?[0..w.cap],
        .frames = w.frames.?[0..w.frame_cap],
        .len = w.len,
        .depth = w.depth,
        .status = sticky(w.status),
        .tag_open = w.tag_open,
    };
}

/// Publish the core's state back into the caller's struct.
fn commit(w: *Writer, core: writer.Writer) void {
    w.len = core.len;
    w.depth = @intCast(core.depth);
    w.status = if (core.status) |err| code(err) else .ok;
    w.tag_open = core.tag_open;
}

/// Run `operation` over `w`, mapping guards and refusals to `ra8_err_t`.
///
/// The `ready` gate is the one check that cannot live in the core: an
/// uninitialised struct has no slices to build a view from.
fn drive(w: ?*Writer, operation: anytype, arguments: anytype) XmlError {
    const handle = w orelse return .invalid_arg;
    if (!handle.ready) return .invalid_state;
    if (handle.status != .ok) return handle.status;

    var core = view(handle);
    const result = @call(.auto, operation, .{&core} ++ arguments);
    commit(handle, core);
    return if (result) |_| .ok else |err| code(err);
}

/// Bytes of `text` up to its terminator.
fn span(text: [*:0]const u8) []const u8 {
    return std.mem.span(text);
}

/// Escape the five XML predefined entities into a bounded buffer.
export fn ra8_xml_escape(src: ?[*:0]const u8, out: ?[*]u8, cap: usize) callconv(.c) XmlError {
    const buffer = out orelse return .invalid_arg;
    if (cap == 0) return .invalid_arg;
    buffer[0] = 0;

    const source = src orelse return .invalid_arg;

    // One byte held back for the terminator the C callers read.
    const escaped = entity.escape(span(source), buffer[0 .. cap - 1]) catch {
        buffer[0] = 0;
        return .no_mem;
    };
    buffer[escaped.len] = 0;
    return .ok;
}

/// Bind a writer to a caller-owned buffer and element stack.
export fn ra8_xml_writer_init(
    w: ?*Writer,
    out: ?[*]u8,
    cap: usize,
    frames: ?[*]Frame,
    frame_cap: u32,
) callconv(.c) XmlError {
    const handle = w orelse return .invalid_arg;
    const buffer = out orelse return .invalid_arg;
    const stack = frames orelse return .invalid_arg;
    if (cap == 0 or frame_cap < 1) return .invalid_arg;

    handle.* = .{
        .out = buffer,
        .frames = stack,
        .cap = cap,
        .len = 0,
        .frame_cap = frame_cap,
        .depth = 0,
        .status = .ok,
        .tag_open = false,
        .ready = true,
    };
    buffer[0] = 0;
    return .ok;
}

/// Emit the XML declaration.
export fn ra8_xml_writer_declaration(w: ?*Writer) callconv(.c) XmlError {
    return drive(w, writer.Writer.declaration, .{});
}

/// Open an element, leaving its start tag ready to take attributes.
export fn ra8_xml_writer_start_element(w: ?*Writer, tag: ?[*:0]const u8) callconv(.c) XmlError {
    const text = tag orelse return nullArgument(w);
    return drive(w, writer.Writer.startElement, .{span(text)});
}

/// Add an attribute to the start tag still being written.
export fn ra8_xml_writer_attr(
    w: ?*Writer,
    key: ?[*:0]const u8,
    value: ?[*:0]const u8,
) callconv(.c) XmlError {
    const key_text = key orelse return nullArgument(w);
    const value_text = value orelse return nullArgument(w);
    return drive(w, writer.Writer.attr, .{ span(key_text), span(value_text) });
}

/// Append escaped character data inside the open element.
export fn ra8_xml_writer_text(w: ?*Writer, content: ?[*:0]const u8) callconv(.c) XmlError {
    const text = content orelse return nullArgument(w);
    return drive(w, writer.Writer.text, .{span(text)});
}

/// Close the innermost open element.
export fn ra8_xml_writer_end_element(w: ?*Writer) callconv(.c) XmlError {
    return drive(w, writer.Writer.endElement, .{});
}

/// Settle the document and hand back its length.
export fn ra8_xml_writer_finish(w: ?*Writer, out_len: ?*usize) callconv(.c) XmlError {
    if (out_len) |slot| slot.* = 0;

    const handle = w orelse return .invalid_arg;
    if (!handle.ready) return .invalid_state;
    if (handle.status != .ok) return handle.status;

    var core = view(handle);
    const length = core.finish() catch |err| {
        commit(handle, core);
        return code(err);
    };
    commit(handle, core);
    if (out_len) |slot| slot.* = length;
    return .ok;
}

/// A null string argument, recorded as the writer's sticky refusal.
///
/// The C reached its null checks only after the `ready` and `status` gates, so
/// a null name on an unarmed writer still reports `invalid_state`.
fn nullArgument(w: ?*Writer) XmlError {
    const handle = w orelse return .invalid_arg;
    if (!handle.ready) return .invalid_state;
    if (handle.status != .ok) return handle.status;
    handle.status = .invalid_arg;
    return .invalid_arg;
}
