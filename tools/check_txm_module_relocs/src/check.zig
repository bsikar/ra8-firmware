//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The rule a linked ThreadX module is held to.
//!
//! A module is loaded wherever the manager finds room. Its start-up code
//! rebases the GOT against the load addresses, and copies initialised data
//! byte for byte. So a word in data that holds an absolute address keeps its
//! link-time value, and is wrong the moment the module runs.
//!
//! The link keeps its relocations (`--emit-relocs`), and this walks them. A
//! relocation is a site when the section it patches is data and its type
//! does not move with the module. Whether a site is a failure is for
//! `coverage.zig`: one the module's rebase records name is put right at
//! start-up, and one they miss is not.
//!
//! Data here means loaded and not executable. Three kinds of section are
//! left alone: code, which is never copied and may hold link addresses on
//! purpose (the start-up's own literals do); the GOT, which is what the
//! start-up rebases; and anything not loaded, which is debug information.

const std = @import("std");
const elf = std.elf;
const arm = @import("arm.zig");
const elf32 = @import("elf32.zig");

pub const Error = elf32.Error;

/// One word the loader would leave wrong.
pub const Finding = struct {
    /// The data section the word is in.
    section: []const u8,
    /// Where the word is: an address in a linked image.
    address: u32,
    kind: arm.Kind,
    /// What it points at. A section's own name when the symbol is a section.
    symbol: []const u8,
    /// The symbol's link-time value.
    value: u32,
};

/// True for a section whose relocations are checked.
pub fn isData(section: elf32.Section) bool {
    if (section.flags & elf.SHF_ALLOC == 0) return false;
    if (section.flags & elf.SHF_EXECINSTR != 0) return false;
    return !std.mem.startsWith(u8, section.name, ".got");
}

/// Walks every relocation of every data section, yielding the findings.
pub const Iterator = struct {
    file: elf32.File,
    /// The relocation section being walked, and the next entry in it.
    section: u32 = 0,
    entry: u32 = 0,

    pub fn init(bytes: []const u8) Error!Iterator {
        return .{ .file = try elf32.File.init(bytes) };
    }

    pub fn next(self: *Iterator) Error!?Finding {
        while (self.section < self.file.section_count) {
            if (try self.nextIn(try self.file.section(self.section))) |finding| return finding;
            self.section += 1;
            self.entry = 0;
        }
        return null;
    }

    /// The next finding among the entries of `relocs`, if it is a
    /// relocation section over data and has one left.
    fn nextIn(self: *Iterator, relocs: elf32.Section) Error!?Finding {
        const size: u32 = switch (relocs.kind) {
            elf.SHT_REL => @sizeOf(elf.Elf32_Rel),
            elf.SHT_RELA => @sizeOf(elf.Elf32_Rela),
            else => return null,
        };
        const target = try self.file.section(relocs.info);
        if (!isData(target)) return null;
        if (relocs.bytes.len % size != 0) return error.Truncated;

        while (self.entry < relocs.bytes.len / size) {
            const bytes = relocs.bytes[self.entry * size ..][0..size];
            self.entry += 1;
            const info = elf32.field(u32, bytes, elf.Elf32_Rel, "r_info");
            const kind: arm.Kind = @truncate(info);
            if (arm.movesWithTheModule(kind)) continue;

            const symbol = try self.file.symbol(relocs.link, info >> 8);
            return .{
                .section = target.name,
                .address = elf32.field(u32, bytes, elf.Elf32_Rel, "r_offset"),
                .kind = kind,
                .symbol = try self.symbolName(symbol),
                .value = symbol.value,
            };
        }
        return null;
    }

    fn symbolName(self: *Iterator, symbol: elf32.Symbol) Error![]const u8 {
        if (!symbol.is_section) return symbol.name;
        return (try self.file.section(symbol.section)).name;
    }
};
