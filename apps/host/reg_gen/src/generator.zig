//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Production-grade C23 register code generator with MMIO layout verification.

const std = @import("std");

pub const GeneratorError = error{
    EmptyRegisters,
    InvalidBaseAddress,
    InvalidRegisterOffset,
    InvalidRegisterSize,
    UnalignedRegister,
    DuplicateRegisterName,
    DuplicateRegisterOffset,
    OverlappingRegisters,
    UnsortedRegisters,
    InvalidIdentifier,
    InvalidDescription,
    OutOfMemory,
};

pub const RegisterSize = enum(u32) {
    b8 = 8,
    b16 = 16,
    b32 = 32,
    b64 = 64,

    pub fn fromBits(bits: u32) ?RegisterSize {
        return switch (bits) {
            8 => .b8,
            16 => .b16,
            32 => .b32,
            64 => .b64,
            else => null,
        };
    }

    pub fn byteCount(self: RegisterSize) u32 {
        return @intFromEnum(self) / 8;
    }

    pub fn cTypeName(self: RegisterSize) []const u8 {
        return switch (self) {
            .b8 => "uint8_t",
            .b16 => "uint16_t",
            .b32 => "uint32_t",
            .b64 => "uint64_t",
        };
    }
};

pub const Register = struct {
    name: []const u8,
    offset: []const u8,
    size: u32,
    description: []const u8,
};

pub const PeripheralDef = struct {
    peripheral: []const u8,
    base_address: []const u8,
    registers: []const Register,
};

pub const ValidatedRegister = struct {
    name: []const u8,
    offset: u64,
    size: RegisterSize,
    description: []const u8,
};

pub const ValidatedPeripheral = struct {
    base_address: u64,
    registers: []ValidatedRegister,
};

const c_keywords = [_][]const u8{
    "auto",     "break",         "case",      "char",       "const",        "continue",
    "default",  "do",            "double",    "else",       "enum",         "extern",
    "float",    "for",           "goto",      "if",         "inline",       "int",
    "long",     "register",      "restrict",  "return",     "short",        "signed",
    "sizeof",   "static",        "struct",    "switch",     "typedef",      "union",
    "unsigned", "void",          "volatile",  "while",      "alignas",      "alignof",
    "bool",     "complex",       "imaginary", "noreturn",   "thread_local", "static_assert",
    "typeof",   "typeof_unqual", "constexpr", "nullptr",    "true",         "false",
    "_Atomic",  "_BitInt",       "_Generic",  "_Decimal32", "_Decimal64",   "_Decimal128",
};

pub fn isReservedKeyword(name: []const u8) bool {
    for (c_keywords) |kw| {
        if (std.ascii.eqlIgnoreCase(name, kw)) return true;
    }
    return false;
}

pub fn isValidIdentifier(name: []const u8) bool {
    if (name.len == 0 or isReservedKeyword(name)) return false;
    for (name, 0..) |c, i| {
        if (i == 0) {
            if (!std.ascii.isAlphabetic(c) and c != '_') return false;
        } else {
            if (!std.ascii.isAlphanumeric(c) and c != '_') return false;
        }
    }
    return true;
}

// Zero-allocation case transformation formatters
pub fn fmtLower(bytes: []const u8) std.fmt.Formatter(formatLower) {
    return .{ .data = bytes };
}

fn formatLower(bytes: []const u8, comptime fmt: []const u8, options: std.fmt.FormatOptions, writer: anytype) !void {
    _ = fmt;
    _ = options;
    for (bytes) |c| {
        try writer.writeByte(std.ascii.toLower(c));
    }
}

pub fn fmtUpper(bytes: []const u8) std.fmt.Formatter(formatUpper) {
    return .{ .data = bytes };
}

fn formatUpper(bytes: []const u8, comptime fmt: []const u8, options: std.fmt.FormatOptions, writer: anytype) !void {
    _ = fmt;
    _ = options;
    for (bytes) |c| {
        try writer.writeByte(std.ascii.toUpper(c));
    }
}

pub fn determineSmallestType(max_val: u64) []const u8 {
    if (max_val <= 0xFF) {
        return "uint8_t";
    } else if (max_val <= 0xFFFF) {
        return "uint16_t";
    } else if (max_val <= 0xFFFFFFFF) {
        return "uint32_t";
    } else {
        return "uint64_t";
    }
}

pub fn hexWidthForMax(max_offset: u64) usize {
    if (max_offset <= 0xFF) return 2;
    if (max_offset <= 0xFFFF) return 4;
    if (max_offset <= 0xFFFFFFFF) return 8;
    return 16;
}

pub fn printHexLiteral(writer: anytype, val: u64, width: usize) !void {
    try writer.writeAll("0x");
    switch (width) {
        2 => try writer.print("{X:0>2}", .{val}),
        4 => try writer.print("{X:0>4}", .{val}),
        8 => try writer.print("{X:0>8}", .{val}),
        else => try writer.print("{X:0>16}", .{val}),
    }
    try writer.writeByte('U');
}

pub fn parseDefinition(allocator: std.mem.Allocator, json_bytes: []const u8) !std.json.Parsed(PeripheralDef) {
    return std.json.parseFromSlice(
        PeripheralDef,
        allocator,
        json_bytes,
        .{ .ignore_unknown_fields = true },
    );
}

pub fn validateDefinition(def: PeripheralDef, allocator: std.mem.Allocator) GeneratorError!ValidatedPeripheral {
    if (!isValidIdentifier(def.peripheral)) return GeneratorError.InvalidIdentifier;
    if (def.registers.len == 0) return GeneratorError.EmptyRegisters;

    const base_addr = std.fmt.parseInt(u64, def.base_address, 0) catch return GeneratorError.InvalidBaseAddress;
    if (base_addr % 4 != 0) return GeneratorError.InvalidBaseAddress;

    const validated = allocator.alloc(ValidatedRegister, def.registers.len) catch return GeneratorError.OutOfMemory;
    errdefer allocator.free(validated);

    var prev_end: u64 = 0;
    for (def.registers, 0..) |reg, i| {
        if (!isValidIdentifier(reg.name)) return GeneratorError.InvalidIdentifier;
        if (std.mem.indexOf(u8, reg.description, "*/") != null) return GeneratorError.InvalidDescription;
        for (reg.description) |c| {
            if (c < 0x20 or c > 0x7E) return GeneratorError.InvalidDescription;
        }

        for (def.registers[0..i]) |prev| {
            if (std.ascii.eqlIgnoreCase(reg.name, prev.name)) return GeneratorError.DuplicateRegisterName;
        }

        const offset = std.fmt.parseInt(u64, reg.offset, 0) catch return GeneratorError.InvalidRegisterOffset;
        const size = RegisterSize.fromBits(reg.size) orelse return GeneratorError.InvalidRegisterSize;
        if (offset % size.byteCount() != 0) return GeneratorError.UnalignedRegister;
        const reg_end = std.math.add(u64, offset, size.byteCount()) catch return GeneratorError.InvalidRegisterOffset;

        if (i > 0) {
            const prev_offset = validated[i - 1].offset;
            if (offset < prev_offset) {
                return GeneratorError.UnsortedRegisters;
            } else if (offset == prev_offset) {
                return GeneratorError.DuplicateRegisterOffset;
            } else if (offset < prev_end) {
                return GeneratorError.OverlappingRegisters;
            }
        }

        validated[i] = .{
            .name = reg.name,
            .offset = offset,
            .size = size,
            .description = reg.description,
        };
        prev_end = reg_end;
    }

    return .{
        .base_address = base_addr,
        .registers = validated,
    };
}

pub fn generateC23Header(def: PeripheralDef, allocator: std.mem.Allocator, writer: anytype) !void {
    const validated = try validateDefinition(def, allocator);
    defer allocator.free(validated.registers);

    const peri_l = fmtLower(def.peripheral);
    const peri_u = fmtUpper(def.peripheral);

    var max_offset: u64 = 0;
    for (validated.registers) |r| {
        if (r.offset > max_offset) max_offset = r.offset;
    }
    const offset_type = determineSmallestType(max_offset);
    const offset_width = hexWidthForMax(max_offset);
    const is_64bit_base = validated.base_address > 0xFFFFFFFF;
    const base_suffix: []const u8 = if (is_64bit_base) "ULL" else "UL";

    try writer.print(
        \\/**
        \\ * @file {s}_regs.h
        \\ * @brief {s} Register Definitions
        \\ *
        \\ * @copyright Copyright (c) 2026 Brighton Sikarskie
        \\ * SPDX-License-Identifier: MIT
        \\ */
        \\
        \\#pragma once
        \\
        \\#include <stddef.h>
        \\#include <stdint.h>
        \\
        \\#ifdef __cplusplus
        \\extern "C" {{
        \\#endif
        \\
        \\/**
        \\ * @brief {s} Base Address.
        \\ */
        \\typedef enum : uintptr_t {{
        \\
    , .{
        peri_l,
        peri_u,
        peri_u,
    });

    if (is_64bit_base) {
        try writer.print("  k_{s}_base_addr = 0x{X:0>16}{s}, /**< Base address for {s}. */\n", .{
            peri_l,
            validated.base_address,
            base_suffix,
            peri_u,
        });
    } else {
        try writer.print("  k_{s}_base_addr = 0x{X:0>8}{s}, /**< Base address for {s}. */\n", .{
            peri_l,
            validated.base_address,
            base_suffix,
            peri_u,
        });
    }

    try writer.print(
        \\}} {s}_addr_t;
        \\
        \\/**
        \\ * @brief {s} Register Offsets.
        \\ */
        \\typedef enum : {s} {{
        \\
    , .{
        peri_l,
        peri_u,
        offset_type,
    });

    for (validated.registers) |r| {
        try writer.print("  k_{s}_{s}_offset = ", .{ peri_l, fmtLower(r.name) });
        try printHexLiteral(writer, r.offset, offset_width);
        try writer.print(", /**< {s} */\n", .{r.description});
    }

    try writer.print(
        \\}} {s}_offset_t;
        \\
        \\/**
        \\ * @struct {s}_regs
        \\ * @brief {s} register block layout.
        \\ */
        \\typedef struct {s}_regs {{
        \\
    , .{
        peri_l,
        peri_l,
        peri_u,
        peri_l,
    });

    var current_offset: u64 = 0;
    var reserved_idx: usize = 0;
    var max_align: u32 = 1;
    for (validated.registers) |r| {
        const align_bytes = r.size.byteCount();
        if (align_bytes > max_align) max_align = align_bytes;
        if (r.offset > current_offset) {
            const gap = r.offset - current_offset;
            try writer.print("  uint8_t _reserved{d}[{d}]; /**< Reserved padding */\n", .{ reserved_idx, gap });
            reserved_idx += 1;
            current_offset = r.offset;
        }
        try writer.print("  volatile {s} {s}; /**< +", .{
            r.size.cTypeName(),
            fmtUpper(r.name),
        });
        try printHexLiteral(writer, r.offset, offset_width);
        try writer.print(" {s} */\n", .{r.description});
        current_offset += r.size.byteCount();
    }

    const remainder = current_offset % max_align;
    if (remainder != 0) {
        const tail_padding = max_align - remainder;
        try writer.print("  uint8_t _reserved{d}[{d}]; /**< Reserved trailing padding */\n", .{ reserved_idx, tail_padding });
        current_offset += tail_padding;
    }

    try writer.print(
        \\}} {s}_regs_t;
        \\
        \\
    , .{peri_l});

    for (validated.registers) |r| {
        try writer.print("static_assert(offsetof({s}_regs_t, {s}) == ", .{
            peri_l,
            fmtUpper(r.name),
        });
        try printHexLiteral(writer, r.offset, offset_width);
        try writer.print(", \"{s} offset mismatch\");\n", .{fmtUpper(r.name)});
    }

    try writer.print("static_assert(sizeof({s}_regs_t) == ", .{peri_l});
    try printHexLiteral(writer, current_offset, offset_width);
    try writer.print(
        \\, "{s}_regs_t size mismatch");
        \\
        \\/**
        \\ * @brief Get pointer to {s} hardware registers.
        \\ * @return Pointer to volatile register structure.
        \\ */
        \\static inline volatile {s}_regs_t *{s}_get_regs(void)
        \\{{
        \\  return (volatile {s}_regs_t *)k_{s}_base_addr;
        \\}}
        \\
        \\#ifdef __cplusplus
        \\}}
        \\#endif
        \\
    , .{
        peri_l,
        peri_u,
        peri_l,
        peri_l,
        peri_l,
        peri_l,
    });
}
