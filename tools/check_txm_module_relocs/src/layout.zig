//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where a module was linked, and which side of it an address is on.
//!
//! A module is linked as two ranges, code and data, and loaded with each
//! one somewhere else. The module's start-up tells a stored address's range
//! by its value and adds that range's load delta. This is the same rule,
//! used at build time to refuse a value the start-up could not place.

const elf32 = @import("elf32.zig");

/// The linker script's own names for the two ranges.
pub const Names = struct {
    pub const code_start = "__FLASH_segment_start__";
    pub const code_end = "__FLASH_segment_end__";
    pub const data_start = "__RAM_segment_start__";
    pub const data_end = "__RAM_segment_end__";
};

pub const Region = enum { code, data, outside };

pub const Range = struct {
    start: u32,
    end: u32,

    pub fn holds(self: Range, address: u32) bool {
        return address >= self.start and address < self.end;
    }
};

pub const Layout = struct {
    code: Range,
    data: Range,

    /// The link ranges of `file`, or null when it does not name all four
    /// bounds and so is not a module image.
    pub fn read(file: elf32.File) elf32.Error!?Layout {
        const code_start = try file.symbolNamed(Names.code_start) orelse return null;
        const code_end = try file.symbolNamed(Names.code_end) orelse return null;
        const data_start = try file.symbolNamed(Names.data_start) orelse return null;
        const data_end = try file.symbolNamed(Names.data_end) orelse return null;
        return .{
            .code = .{ .start = code_start.value, .end = code_end.value },
            .data = .{ .start = data_start.value, .end = data_end.value },
        };
    }

    /// Which range `address` was linked in. A Thumb function's address has
    /// its low bit set and is still inside the code range.
    pub fn region(self: Layout, address: u32) Region {
        if (self.code.holds(address)) return .code;
        if (self.data.holds(address)) return .data;
        return .outside;
    }
};
