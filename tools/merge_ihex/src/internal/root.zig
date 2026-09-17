//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure Intel HEX record algebra for the `merge_ihex` tool (#858).
//!
//! No file system, no process state: text in, merged text out. The merge is
//! defined on RECORDS rather than on file text. Every record of both inputs is
//! kept except the End-Of-File records (type `01`), and exactly one canonical
//! EOF record terminates the result. Concatenating the two files verbatim
//! would leave a terminator in the middle, which most loaders stop at.
//!
//! Extended-Linear-Address records (type `04`) are ordinary records here, so
//! each input keeps positioning its own data and the two address ranges stay
//! where their linker put them.

const std = @import("std");

/// The canonical Intel HEX End-Of-File record, emitted once per merge.
pub const eof_record = ":00000001FF";

/// Whitespace stripped from both ends of an input line, matching the set
/// Python's `str.strip()` removes.
const whitespace = " \t\n\r\x0b\x0c";

/// Line terminators a record may be separated by. The replaced Python tool
/// read its inputs in text mode, so Python's universal newlines split on a
/// lone carriage return as well as on `\n` and `\r\n`. Splitting on `\n`
/// alone would fold a CR-separated file into one oversized "record" and carry
/// the embedded CRs into the merged image.
const line_terminators = "\r\n";

/// Record type field: the two characters at offsets 7 and 8 of `:LLAAAATT`.
const type_field_start = 7;
const type_field_end = 9;

/// True when `line` carries an Intel HEX record rather than blank filler.
///
/// A line that does not open with `:` is not a record. The Python tool skipped
/// those silently and so does this, which keeps hand-annotated hex files and
/// trailing newlines harmless.
pub fn isRecord(line: []const u8) bool {
    return line.len > 0 and line[0] == ':';
}

/// True when `line` is an End-Of-File record (type `01`).
///
/// A line too short to carry a type field is not treated as EOF; it is passed
/// through untouched so malformed input survives the merge visibly instead of
/// being silently dropped.
pub fn isEofRecord(line: []const u8) bool {
    if (line.len < type_field_end) return false;
    return std.ascii.eqlIgnoreCase(line[type_field_start..type_field_end], "01");
}

/// Append every non-EOF record of `text` to `out`, stripped of surrounding
/// whitespace. The appended slices borrow `text`; they do not outlive it.
pub fn appendDataRecords(out: *std.ArrayList([]const u8), text: []const u8) !void {
    var lines = std.mem.splitAny(u8, text, line_terminators);
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, whitespace);
        if (!isRecord(line)) continue;
        if (isEofRecord(line)) continue;
        try out.append(line);
    }
}

/// The result of merging two Intel HEX images.
pub const Merged = struct {
    /// The merged file text, owned by the caller's allocator.
    text: []u8,
    /// Data records carried over from the inputs, excluding the emitted EOF.
    record_count: usize,

    pub fn deinit(self: Merged, allocator: std.mem.Allocator) void {
        allocator.free(self.text);
    }
};

/// Merge the records of `first` and `second` into one Intel HEX image.
///
/// Records keep their input order: every record of `first`, then every record
/// of `second`, then one canonical EOF record. Each line is newline
/// terminated, including the last.
pub fn merge(allocator: std.mem.Allocator, first: []const u8, second: []const u8) !Merged {
    var records = std.ArrayList([]const u8).init(allocator);
    defer records.deinit();

    try appendDataRecords(&records, first);
    try appendDataRecords(&records, second);

    var text = std.ArrayList(u8).init(allocator);
    errdefer text.deinit();

    for (records.items) |record| {
        try text.appendSlice(record);
        try text.append('\n');
    }
    try text.appendSlice(eof_record);
    try text.append('\n');

    return .{ .text = try text.toOwnedSlice(), .record_count = records.items.len };
}
