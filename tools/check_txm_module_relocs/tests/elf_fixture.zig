//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Small ELF images built in memory, shaped like a linked module: code, a
//! read-only section, data, a GOT, debug information, and a relocation
//! section over each. A test says which relocations go where.
//!
//! Built here rather than checked in: a binary nobody can read is a poor
//! witness, and `*.elf` is ignored by the repository anyway.

const std = @import("std");
const elf = std.elf;

pub const Reloc = struct { at: u32, symbol: u32, kind: u8 };

/// The sections a relocation can be aimed at, by their index in the image.
pub const Target = enum(u32) { text = 1, rodata = 2, data = 3, debug = 4, got = 5 };

/// The symbols every fixture has, by index.
pub const Symbol = struct {
    pub const double: u32 = 1;
    pub const square: u32 = 2;
    /// The section symbol of `.data`: it has no name of its own.
    pub const data_section: u32 = 3;
    pub const counter: u32 = 4;
};

pub const Spec = struct {
    machine: elf.EM = .ARM,
    text: []const Reloc = &.{},
    rodata: []const Reloc = &.{},
    data: []const Reloc = &.{},
    debug: []const Reloc = &.{},
    got: []const Reloc = &.{},
    /// Store relocations with addends (`SHT_RELA`) rather than without.
    with_addends: bool = false,
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
const symtab_index = 11;
const strtab_index = 12;
const shstrtab_index = 13;
const section_count = 14;

fn int(image: *Image, comptime T: type, value: T) void {
    var raw: [@sizeOf(T)]u8 = undefined;
    std.mem.writeInt(T, &raw, value, .little);
    _ = image.append(&raw);
}

/// One relocation section's bytes, appended to the image.
fn relocs(image: *Image, list: []const Reloc, with_addends: bool) struct { u32, u32 } {
    const at: u32 = @intCast(image.len);
    for (list) |reloc| {
        int(image, u32, reloc.at);
        int(image, u32, (reloc.symbol << 8) | reloc.kind);
        if (with_addends) int(image, u32, 0);
    }
    return .{ at, @as(u32, @intCast(image.len)) - at };
}

fn symbol(image: *Image, name: u32, value: u32, info: u8, section: u16) void {
    int(image, u32, name);
    int(image, u32, value);
    int(image, u32, 0);
    int(image, u8, info);
    int(image, u8, 0);
    int(image, u16, section);
}

/// Build the image `spec` describes.
pub fn build(spec: Spec) Image {
    var image: Image = .{};
    @memset(image.buf[0..@sizeOf(elf.Elf32_Ehdr)], 0);
    image.len = @sizeOf(elf.Elf32_Ehdr);

    const words = [_]u8{0} ** 16;
    const strings = "\x00double\x00square\x00counter\x00";
    const rel_kind: u32 = if (spec.with_addends) elf.SHT_RELA else elf.SHT_REL;
    const rel_bytes: u32 = if (spec.with_addends) 12 else 8;

    var headers = [section_count]Header{
        .{ .name = "", .kind = elf.SHT_NULL },
        .{ .name = ".text", .kind = elf.SHT_PROGBITS, .flags = alloc | exec, .address = 0x30080 },
        .{ .name = ".rodata", .kind = elf.SHT_PROGBITS, .flags = alloc, .address = 0x30300 },
        .{
            .name = ".data",
            .kind = elf.SHT_PROGBITS,
            .flags = alloc | write,
            .address = 0x10000018,
        },
        .{ .name = ".debug_info", .kind = elf.SHT_PROGBITS },
        .{ .name = ".got", .kind = elf.SHT_PROGBITS, .flags = alloc | write, .address = 0x30308 },
        .{ .name = ".rel.text", .kind = rel_kind, .info = 1 },
        .{ .name = ".rel.rodata", .kind = rel_kind, .info = 2 },
        .{ .name = ".rel.data", .kind = rel_kind, .info = 3 },
        .{ .name = ".rel.debug_info", .kind = rel_kind, .info = 4 },
        .{ .name = ".rel.got", .kind = rel_kind, .info = 5 },
        .{ .name = ".symtab", .kind = elf.SHT_SYMTAB, .link = strtab_index, .entry_bytes = 16 },
        .{ .name = ".strtab", .kind = elf.SHT_STRTAB },
        .{ .name = ".shstrtab", .kind = elf.SHT_STRTAB },
    };
    for (headers[1..6]) |*header| {
        header.offset = image.append(&words);
        header.size = words.len;
    }
    const lists = [_][]const Reloc{ spec.text, spec.rodata, spec.data, spec.debug, spec.got };
    for (headers[6..11], lists) |*header, list| {
        header.offset, header.size = relocs(&image, list, spec.with_addends);
        header.link = symtab_index;
        header.entry_bytes = rel_bytes;
    }

    headers[symtab_index].offset = @intCast(image.len);
    symbol(&image, 0, 0, 0, 0);
    symbol(&image, 1, 0x30191, elf.STT_FUNC, 1);
    symbol(&image, 8, 0x30195, elf.STT_FUNC, 1);
    symbol(&image, 0, 0x10000018, elf.STT_SECTION, 3);
    symbol(&image, 15, 0x1000001c, elf.STT_OBJECT, 3);
    headers[symtab_index].size = @as(u32, @intCast(image.len)) - headers[symtab_index].offset;
    headers[strtab_index].offset = image.append(strings);
    headers[strtab_index].size = strings.len;

    // Section names, then the header table.
    var name_offsets: [section_count]u32 = undefined;
    headers[shstrtab_index].offset = @intCast(image.len);
    _ = image.append("\x00");
    for (headers, &name_offsets) |header, *offset| {
        offset.* = image.append(header.name) - headers[shstrtab_index].offset;
        _ = image.append("\x00");
    }
    headers[shstrtab_index].size = @as(u32, @intCast(image.len)) - headers[shstrtab_index].offset;

    const table: u32 = @intCast(image.len);
    for (headers, name_offsets) |header, name| {
        for ([_]u32{
            name,          header.kind,        header.flags, header.address,
            header.offset, header.size,        header.link,  header.info,
            4,             header.entry_bytes,
        }) |value| int(&image, u32, value);
    }
    writeFileHeader(&image, spec.machine, table);
    return image;
}

fn writeFileHeader(image: *Image, machine: elf.EM, table: u32) void {
    const header = image.buf[0..@sizeOf(elf.Elf32_Ehdr)];
    @memcpy(header[0..4], elf.MAGIC);
    header[elf.EI_CLASS] = elf.ELFCLASS32;
    header[elf.EI_DATA] = elf.ELFDATA2LSB;
    header[elf.EI_VERSION] = 1;
    put(u16, header, "e_type", @intFromEnum(elf.ET.EXEC));
    put(u16, header, "e_machine", @intFromEnum(machine));
    put(u32, header, "e_version", 1);
    put(u32, header, "e_shoff", table);
    put(u16, header, "e_ehsize", @sizeOf(elf.Elf32_Ehdr));
    put(u16, header, "e_shentsize", @sizeOf(elf.Elf32_Shdr));
    put(u16, header, "e_shnum", section_count);
    put(u16, header, "e_shstrndx", shstrtab_index);
}

fn put(comptime T: type, header: []u8, comptime name: []const u8, value: T) void {
    const at = @offsetOf(elf.Elf32_Ehdr, name);
    std.mem.writeInt(T, header[at..][0..@sizeOf(T)], value, .little);
}
