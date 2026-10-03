//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Small ELF images built in memory, shaped like a linked module: code, a
//! read-only section, data, a GOT, debug information, and a relocation
//! section over each. A test says which relocations go where, what the data
//! words hold, and what the rebase table lists.
//!
//! Built here rather than checked in: a binary nobody can read is a poor
//! witness, and `*.elf` is ignored by the repository anyway.

const std = @import("std");
const elf = std.elf;

pub const Reloc = struct { at: u32, symbol: u32, kind: u8 };

/// The symbols every fixture has, by index.
pub const Symbol = struct {
    pub const double: u32 = 1;
    pub const square: u32 = 2;
    /// The section symbol of `.data`: it has no name of its own.
    pub const data_section: u32 = 3;
    pub const counter: u32 = 4;
};

/// Where things are, as the module linker script would have put them.
pub const Address = struct {
    pub const code_start: u32 = 0x00030000;
    pub const code_end: u32 = 0x00040000;
    pub const data_start: u32 = 0x10000000;
    pub const data_end: u32 = 0x10008000;
    pub const text: u32 = 0x00030080;
    /// Where the rebase table goes when a fixture has one.
    pub const rodata: u32 = 0x00030300;
    pub const got: u32 = 0x00030308;
    /// The first of `.data`'s four words.
    pub const data: u32 = 0x10000018;
    pub const double: u32 = 0x00030191;
    pub const square: u32 = 0x00030195;
    pub const counter: u32 = 0x1000001c;
};

pub const data_words = 4;
pub const section_count = 15;
pub const symtab_index = 12;
const strtab_index = 13;
const shstrtab_index = 14;

pub const Spec = struct {
    machine: elf.EM = .ARM,
    text: []const Reloc = &.{},
    rodata: []const Reloc = &.{},
    data: []const Reloc = &.{},
    debug: []const Reloc = &.{},
    got: []const Reloc = &.{},
    /// Dynamic relocations, in a loaded `.rel.dyn`.
    dynamic: []const Reloc = &.{},
    /// Store relocations with addends (`SHT_RELA`) rather than without.
    with_addends: bool = false,
    /// What the four words of `.data` hold.
    words: [data_words]u32 = @splat(0),
    /// The rebase table, at the start of `.rodata`. Null for a module with
    /// no table and no table symbols.
    records: ?[]const u32 = null,
    /// Leave out the symbol that ends the table.
    without_records_end: bool = false,
    /// Leave out the four symbols that bound the link ranges.
    without_ranges: bool = false,
};

pub const Image = struct {
    buf: [4096]u8 = undefined,
    len: usize = 0,

    pub fn bytes(self: *const Image) []const u8 {
        return self.buf[0..self.len];
    }

    fn append(self: *Image, data: []const u8) u32 {
        const at: u32 = @intCast(self.len);
        @memcpy(self.buf[self.len..][0..data.len], data);
        self.len += data.len;
        return at;
    }

    fn int(self: *Image, comptime T: type, value: T) void {
        var raw: [@sizeOf(T)]u8 = undefined;
        std.mem.writeInt(T, &raw, value, .little);
        _ = self.append(&raw);
    }
};

const Header = struct {
    name: []const u8,
    kind: u32,
    flags: u32 = 0,
    address: u32 = 0,
    offset: u32 = 0,
    size: u32 = 0,
    link: u32 = 0,
    info: u32 = 0,
    entry_bytes: u32 = 0,
};

const alloc = elf.SHF_ALLOC;
const write = elf.SHF_WRITE;
const exec = elf.SHF_EXECINSTR;
const absolute = elf.SHN_ABS;

/// A symbol to be: its name is appended to the string table as it is added.
const Entry = struct { name: []const u8, value: u32, info: u8 = elf.STT_NOTYPE, section: u16 };

/// One relocation section's bytes, appended to the image.
fn relocs(image: *Image, list: []const Reloc, with_addends: bool) struct { u32, u32 } {
    const at: u32 = @intCast(image.len);
    for (list) |reloc| {
        image.int(u32, reloc.at);
        image.int(u32, (reloc.symbol << 8) | reloc.kind);
        if (with_addends) image.int(u32, 0);
    }
    return .{ at, @as(u32, @intCast(image.len)) - at };
}

/// Four little-endian words as a section's sixteen bytes.
fn wordBytes(words: []const u32) [4 * data_words]u8 {
    var out: [4 * data_words]u8 = @splat(0);
    for (words, 0..) |word, index| std.mem.writeInt(u32, out[4 * index ..][0..4], word, .little);
    return out;
}

/// The symbol table and its strings, appended to the image.
fn symbols(image: *Image, spec: Spec, headers: *[section_count]Header) void {
    const record_bytes: u32 = if (spec.records) |list| @intCast(4 * list.len) else 0;
    const fixed = [_]Entry{
        .{ .name = "", .value = 0, .section = 0 },
        .{ .name = "double", .value = Address.double, .info = elf.STT_FUNC, .section = 1 },
        .{ .name = "square", .value = Address.square, .info = elf.STT_FUNC, .section = 1 },
        .{ .name = "", .value = Address.data, .info = elf.STT_SECTION, .section = 3 },
        .{ .name = "counter", .value = Address.counter, .info = elf.STT_OBJECT, .section = 3 },
    };
    const ranges = [_]Entry{
        .{ .name = "__FLASH_segment_start__", .value = Address.code_start, .section = absolute },
        .{ .name = "__FLASH_segment_end__", .value = Address.code_end, .section = absolute },
        .{ .name = "__RAM_segment_start__", .value = Address.data_start, .section = absolute },
        .{ .name = "__RAM_segment_end__", .value = Address.data_end, .section = absolute },
    };
    const table = [_]Entry{
        .{ .name = "__txm_rebase_start__", .value = Address.rodata, .section = 2 },
        .{ .name = "__txm_rebase_end__", .value = Address.rodata + record_bytes, .section = 2 },
    };

    var strings: [256]u8 = undefined;
    var strings_len: usize = 1;
    strings[0] = 0;
    headers[symtab_index].offset = @intCast(image.len);
    const groups = [_][]const Entry{
        &fixed,
        if (spec.without_ranges) &.{} else &ranges,
        if (spec.records == null) &.{} else if (spec.without_records_end) table[0..1] else &table,
    };
    for (groups) |group| for (group) |entry| {
        image.int(u32, if (entry.name.len == 0) 0 else @intCast(strings_len));
        image.int(u32, entry.value);
        image.int(u32, 0);
        image.int(u8, entry.info);
        image.int(u8, 0);
        image.int(u16, entry.section);
        if (entry.name.len == 0) continue;
        @memcpy(strings[strings_len..][0..entry.name.len], entry.name);
        strings[strings_len + entry.name.len] = 0;
        strings_len += entry.name.len + 1;
    };
    headers[symtab_index].size = @as(u32, @intCast(image.len)) - headers[symtab_index].offset;
    headers[strtab_index].offset = image.append(strings[0..strings_len]);
    headers[strtab_index].size = @intCast(strings_len);
}

/// Build the image `spec` describes.
pub fn build(spec: Spec) Image {
    var image: Image = .{};
    @memset(image.buf[0..@sizeOf(elf.Elf32_Ehdr)], 0);
    image.len = @sizeOf(elf.Elf32_Ehdr);

    const rel_kind: u32 = if (spec.with_addends) elf.SHT_RELA else elf.SHT_REL;
    const rel_bytes: u32 = if (spec.with_addends) 12 else 8;
    const bits = elf.SHT_PROGBITS;
    var headers = [section_count]Header{
        .{ .name = "", .kind = elf.SHT_NULL },
        .{ .name = ".text", .kind = bits, .flags = alloc | exec, .address = Address.text },
        .{ .name = ".rodata", .kind = bits, .flags = alloc, .address = Address.rodata },
        .{ .name = ".data", .kind = bits, .flags = alloc | write, .address = Address.data },
        .{ .name = ".debug_info", .kind = bits },
        .{ .name = ".got", .kind = bits, .flags = alloc | write, .address = Address.got },
        .{ .name = ".rel.text", .kind = rel_kind, .info = 1 },
        .{ .name = ".rel.rodata", .kind = rel_kind, .info = 2 },
        .{ .name = ".rel.data", .kind = rel_kind, .info = 3 },
        .{ .name = ".rel.debug_info", .kind = rel_kind, .info = 4 },
        .{ .name = ".rel.got", .kind = rel_kind, .info = 5 },
        .{ .name = ".rel.dyn", .kind = rel_kind, .flags = alloc },
        .{ .name = ".symtab", .kind = elf.SHT_SYMTAB, .link = strtab_index, .entry_bytes = 16 },
        .{ .name = ".strtab", .kind = elf.SHT_STRTAB },
        .{ .name = ".shstrtab", .kind = elf.SHT_STRTAB },
    };

    const contents = [_][4 * data_words]u8{
        @splat(0),
        wordBytes(spec.records orelse &.{}),
        wordBytes(&spec.words),
        @splat(0),
        @splat(0),
    };
    for (headers[1..6], contents) |*header, bytes| {
        header.offset = image.append(&bytes);
        header.size = bytes.len;
    }
    const lists = [_][]const Reloc{
        spec.text, spec.rodata, spec.data, spec.debug, spec.got, spec.dynamic,
    };
    for (headers[6..12], lists) |*header, list| {
        header.offset, header.size = relocs(&image, list, spec.with_addends);
        header.link = symtab_index;
        header.entry_bytes = rel_bytes;
    }
    symbols(&image, spec, &headers);
    sectionTable(&image, &headers, spec.machine);
    return image;
}

/// The section names, the header table, and then the file header that
/// points at it.
fn sectionTable(image: *Image, headers: *[section_count]Header, machine: elf.EM) void {
    var name_offsets: [section_count]u32 = undefined;
    const names_at: u32 = @intCast(image.len);
    headers[shstrtab_index].offset = names_at;
    _ = image.append("\x00");
    for (headers, &name_offsets) |header, *offset| {
        offset.* = image.append(header.name) - names_at;
        _ = image.append("\x00");
    }
    headers[shstrtab_index].size = @as(u32, @intCast(image.len)) - names_at;

    const table: u32 = @intCast(image.len);
    for (headers, name_offsets) |header, name| {
        for ([_]u32{
            name,          header.kind,        header.flags, header.address,
            header.offset, header.size,        header.link,  header.info,
            4,             header.entry_bytes,
        }) |value| image.int(u32, value);
    }
    const file = image.buf[0..@sizeOf(elf.Elf32_Ehdr)];
    @memcpy(file[0..4], elf.MAGIC);
    file[elf.EI_CLASS] = elf.ELFCLASS32;
    file[elf.EI_DATA] = elf.ELFDATA2LSB;
    file[elf.EI_VERSION] = 1;
    put(u16, file, "e_type", @intFromEnum(elf.ET.EXEC));
    put(u16, file, "e_machine", @intFromEnum(machine));
    put(u32, file, "e_version", 1);
    put(u32, file, "e_shoff", table);
    put(u16, file, "e_ehsize", @sizeOf(elf.Elf32_Ehdr));
    put(u16, file, "e_shentsize", @sizeOf(elf.Elf32_Shdr));
    put(u16, file, "e_shnum", section_count);
    put(u16, file, "e_shstrndx", shstrtab_index);
}

fn put(comptime T: type, header: []u8, comptime name: []const u8, value: T) void {
    const at = @offsetOf(elf.Elf32_Ehdr, name);
    std.mem.writeInt(T, header[at..][0..@sizeOf(T)], value, .little);
}
