//! Allocation-free line breaking for label text.

pub const Mode = enum(u8) { none = 0, word = 1, clip = 2 };

pub const Line = struct {
    start: usize,
    end: usize,
    next: usize,
    width: u32,
    ellipsis: bool = false,
};

/// Return one explicit or width-wrapped line. `measure` measures one UTF-8 scalar.
pub fn nextLine(bytes: []const u8, start: usize, max_width: u32, mode: Mode, context: anytype, measure: anytype) Line {
    if (mode == .clip) return clippedLine(bytes, start, max_width, context, measure);
    return wordLine(bytes, start, max_width, context, measure);
}

fn wordLine(bytes: []const u8, start: usize, max_width: u32, context: anytype, measure: anytype) Line {
    var offset = start;
    var width: u32 = 0;
    var last_space: ?Line = null;
    while (offset < bytes.len) {
        const len = scalarLength(bytes, offset);
        if (bytes[offset] == 10) return .{ .start = start, .end = offset, .next = offset + 1, .width = width };
        const advance = measure(context, bytes[offset .. offset + len]);
        if (bytes[offset] == 32 and offset > start) {
            last_space = .{ .start = start, .end = offset, .next = offset + len, .width = width };
        }
        if (width +| advance > max_width and offset > start) {
            if (last_space) |line| {
                var next = line.next;
                while (next < bytes.len and bytes[next] == 32) next += 1;
                return .{ .start = line.start, .end = line.end, .next = next, .width = line.width };
            }
            return .{ .start = start, .end = offset, .next = offset, .width = width };
        }
        width +|= advance;
        offset += len;
        if (width > max_width) break;
    }
    return .{ .start = start, .end = offset, .next = offset, .width = width };
}

fn clippedLine(bytes: []const u8, start: usize, max_width: u32, context: anytype, measure: anytype) Line {
    var offset = start;
    var whole_width: u32 = 0;
    while (offset < bytes.len and bytes[offset] != 10) {
        const len = scalarLength(bytes, offset);
        whole_width +|= measure(context, bytes[offset .. offset + len]);
        offset += len;
    }
    const next = if (offset < bytes.len and bytes[offset] == 10) offset + 1 else offset;
    if (whole_width <= max_width) return .{ .start = start, .end = offset, .next = next, .width = whole_width };

    const ellipsis_width = measure(context, "\xe2\x80\xa6");
    const available = if (ellipsis_width <= max_width) max_width - ellipsis_width else 0;
    offset = start;
    var width: u32 = 0;
    while (offset < bytes.len and bytes[offset] != 10) {
        const len = scalarLength(bytes, offset);
        const advance = measure(context, bytes[offset .. offset + len]);
        if (width +| advance > available) break;
        width +|= advance;
        offset += len;
    }
    const fits_ellipsis = ellipsis_width <= max_width;
    return .{ .start = start, .end = offset, .next = next, .width = width +| (if (fits_ellipsis) ellipsis_width else 0), .ellipsis = fits_ellipsis };
}

pub fn scalarLength(bytes: []const u8, offset: usize) usize {
    const first = bytes[offset];
    const wanted: usize = if (first < 0x80) 1 else if (first < 0xE0) 2 else if (first < 0xF0) 3 else 4;
    if (offset + wanted > bytes.len) return 1;
    for (bytes[offset + 1 .. offset + wanted]) |continuation| {
        if (continuation & 0xC0 != 0x80) return 1;
    }
    return wanted;
}
