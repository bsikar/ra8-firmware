//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The rebase records of a linked module: the table of data addresses its
//! start-up walks, adding the load delta to the word at each.
//!
//! The table sits between two symbols and is one little-endian word for
//! each address, as tools/check_txm_module_relocs's generator writes it.

const std = @import("std");
const elf32 = @import("elf32.zig");

pub const Names = struct {
    pub const start = "__txm_rebase_start__";
    pub const end = "__txm_rebase_end__";
};

pub const word_bytes = @sizeOf(u32);

pub const Error = elf32.Error || error{
    /// One bound is there without the other, the end is before the start,
    /// the length is not whole words, or the image holds no such bytes.
    BadRecords,
};

pub const Records = struct {
    /// The table as the image holds it.
    bytes: []const u8,

    pub const none: Records = .{ .bytes = "" };

    /// The records of `file`. A module with neither bound has none.
    pub fn read(file: elf32.File) Error!Records {
        const start = try file.symbolNamed(Names.start);
        const end = try file.symbolNamed(Names.end);
        if (start == null and end == null) return none;
        if (start == null or end == null) return error.BadRecords;
        if (end.?.value < start.?.value) return error.BadRecords;

        const length = end.?.value - start.?.value;
        if (length % word_bytes != 0) return error.BadRecords;
        const bytes = try file.bytesAt(start.?.value, length) orelse return error.BadRecords;
        return .{ .bytes = bytes };
    }

    pub fn count(self: Records) usize {
        return self.bytes.len / word_bytes;
    }

    /// The address record `index` names.
    pub fn at(self: Records, index: usize) u32 {
        return std.mem.readInt(u32, self.bytes[index * word_bytes ..][0..word_bytes], .little);
    }

    /// True when some record names exactly `address`.
    pub fn has(self: Records, address: u32) bool {
        for (0..self.count()) |index| {
            if (self.at(index) == address) return true;
        }
        return false;
    }
};
