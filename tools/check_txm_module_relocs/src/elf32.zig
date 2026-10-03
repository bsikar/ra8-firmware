//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Just enough of a 32-bit little-endian ELF to walk its relocations: the
//! section headers, their names, and the symbols a relocation points at.
//!
//! Nothing is trusted. Every offset and count comes from the file, so every
//! read is checked against the file's length and reported as `Truncated`.

const std = @import("std");
const elf = std.elf;

pub const Error = error{
    /// Not an ELF file at all.
    NotElf,
    /// An ELF, but not the 32-bit little-endian kind a Cortex-M image is.
    NotElf32Le,
    /// Not built for ARM.
    NotArm,
    /// A header points outside the file, or the file ends mid-record.
    Truncated,
};

/// One section header, in host terms.
pub const Section = struct {
    name: []const u8,
    kind: u32,
    flags: u32,
    address: u32,
    bytes: []const u8,
    link: u32,
    info: u32,
    entry_bytes: u32,
};

/// The symbol a relocation refers to.
pub const Symbol = struct {
    name: []const u8,
    value: u32,
    /// The section it is defined in, or a reserved index such as `SHN_ABS`.
    section: u16,
    is_section: bool,
};

pub const File = struct {
    bytes: []const u8,
    section_table: u32,
    section_count: u16,
    names_index: u16,

    pub fn init(bytes: []const u8) Error!File {
        if (bytes.len < @sizeOf(elf.Elf32_Ehdr)) return error.NotElf;
        if (!std.mem.eql(u8, bytes[0..4], elf.MAGIC)) return error.NotElf;
        if (bytes[elf.EI_CLASS] != elf.ELFCLASS32) return error.NotElf32Le;
        if (bytes[elf.EI_DATA] != elf.ELFDATA2LSB) return error.NotElf32Le;

        const header = bytes[0..@sizeOf(elf.Elf32_Ehdr)];
        const machine = field(u16, header, elf.Elf32_Ehdr, "e_machine");
        if (machine != @intFromEnum(elf.EM.ARM)) return error.NotArm;
        if (field(u16, header, elf.Elf32_Ehdr, "e_shentsize") != @sizeOf(elf.Elf32_Shdr)) {
            return error.Truncated;
        }
        const self: File = .{
            .bytes = bytes,
            .section_table = field(u32, header, elf.Elf32_Ehdr, "e_shoff"),
            .section_count = field(u16, header, elf.Elf32_Ehdr, "e_shnum"),
            .names_index = field(u16, header, elf.Elf32_Ehdr, "e_shstrndx"),
        };
        _ = try self.span(self.section_table, self.tableBytes());
        return self;
    }

    fn tableBytes(self: File) u64 {
        return @as(u64, self.section_count) * @sizeOf(elf.Elf32_Shdr);
    }

    /// Section `index`, with its name looked up.
    pub fn section(self: File, index: u32) Error!Section {
        var found = try self.raw(index);
        const names = (try self.raw(self.names_index)).section.bytes;
        found.section.name = try string(names, found.name_offset);
        return found.section;
    }

    /// Symbol `index` of the symbol table in section `table`.
    pub fn symbol(self: File, table: u32, index: u32) Error!Symbol {
        const symbols = try self.section(table);
        const size = @sizeOf(elf.Elf32_Sym);
        const at = @as(u64, index) * size;
        if (at + size > symbols.bytes.len) return error.Truncated;
        const entry = symbols.bytes[@intCast(at)..][0..size];
        const strings = try self.section(symbols.link);
        const info = field(u8, entry, elf.Elf32_Sym, "st_info");
        return .{
            .name = try string(strings.bytes, field(u32, entry, elf.Elf32_Sym, "st_name")),
            .value = field(u32, entry, elf.Elf32_Sym, "st_value"),
            .section = field(u16, entry, elf.Elf32_Sym, "st_shndx"),
            .is_section = info & 0xF == elf.STT_SECTION,
        };
    }

    /// A section header as read, before its name is resolved.
    const Raw = struct { section: Section, name_offset: u32 };

    fn raw(self: File, index: u32) Error!Raw {
        if (index >= self.section_count) return error.Truncated;
        const size = @sizeOf(elf.Elf32_Shdr);
        const at = self.section_table + @as(u64, index) * size;
        const header = (try self.span(at, size))[0..size];
        const kind = field(u32, header, elf.Elf32_Shdr, "sh_type");
        const offset = field(u32, header, elf.Elf32_Shdr, "sh_offset");
        const length = field(u32, header, elf.Elf32_Shdr, "sh_size");
        return .{
            .name_offset = field(u32, header, elf.Elf32_Shdr, "sh_name"),
            .section = .{
                .name = "",
                .kind = kind,
                .flags = field(u32, header, elf.Elf32_Shdr, "sh_flags"),
                .address = field(u32, header, elf.Elf32_Shdr, "sh_addr"),
                // A NOBITS section takes no room in the file.
                .bytes = if (kind == elf.SHT_NOBITS) "" else try self.span(offset, length),
                .link = field(u32, header, elf.Elf32_Shdr, "sh_link"),
                .info = field(u32, header, elf.Elf32_Shdr, "sh_info"),
                .entry_bytes = field(u32, header, elf.Elf32_Shdr, "sh_entsize"),
            },
        };
    }

    fn span(self: File, offset: u64, length: u64) Error![]const u8 {
        if (offset > self.bytes.len or length > self.bytes.len - offset) return error.Truncated;
        return self.bytes[@intCast(offset)..][0..@intCast(length)];
    }
};

/// The little-endian field `name` of the record `T` at the front of `bytes`.
pub fn field(
    comptime Int: type,
    bytes: []const u8,
    comptime T: type,
    comptime name: []const u8,
) Int {
    return std.mem.readInt(Int, bytes[@offsetOf(T, name)..][0..@sizeOf(Int)], .little);
}

/// The NUL-terminated string at `offset` of a string table.
fn string(table: []const u8, offset: u32) Error![]const u8 {
    if (offset >= table.len) return error.Truncated;
    const end = std.mem.indexOfScalarPos(u8, table, offset, 0) orelse return error.Truncated;
    return table[offset..end];
}
