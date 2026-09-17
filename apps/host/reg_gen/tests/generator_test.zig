//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Dedicated native Zig tests for the register generator.

const std = @import("std");
const build_options = @import("build_options");
const c23_cc = @import("c23_cc.zig");
const generator = @import("application").generator;
const GeneratorError = generator.GeneratorError;
const PeripheralDef = generator.PeripheralDef;
const Register = generator.Register;
const determineSmallestType = generator.determineSmallestType;
const generateC23Header = generator.generateC23Header;
const validateDefinition = generator.validateDefinition;

test "determineSmallestType boundaries" {
    try std.testing.expectEqualStrings("uint8_t", determineSmallestType(0));
    try std.testing.expectEqualStrings("uint8_t", determineSmallestType(0xFF));
    try std.testing.expectEqualStrings("uint16_t", determineSmallestType(0x100));
    try std.testing.expectEqualStrings("uint16_t", determineSmallestType(0xFFFF));
    try std.testing.expectEqualStrings("uint32_t", determineSmallestType(0x10000));
    try std.testing.expectEqualStrings("uint32_t", determineSmallestType(0xFFFFFFFF));
    try std.testing.expectEqualStrings("uint64_t", determineSmallestType(0x100000000));
}

test "validation: empty registers" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{},
    };
    try std.testing.expectError(GeneratorError.EmptyRegisters, validateDefinition(def, std.testing.allocator));
}

test "validation: invalid identifier" {
    const def1 = PeripheralDef{
        .peripheral = "123TIMER",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidIdentifier, validateDefinition(def1, std.testing.allocator));

    const def2 = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "volatile", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidIdentifier, validateDefinition(def2, std.testing.allocator));
}

test "validation: invalid description injection" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Bad comment */ evil" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidDescription, validateDefinition(def, std.testing.allocator));
}

test "validation: invalid register size" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 24, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidRegisterSize, validateDefinition(def, std.testing.allocator));
}

test "validation: duplicate register name case-insensitive" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "ctrl", .offset = "0x00", .size = 32, .description = "Control" },
            .{ .name = "CTRL", .offset = "0x04", .size = 32, .description = "Duplicate Control" },
        },
    };
    try std.testing.expectError(GeneratorError.DuplicateRegisterName, validateDefinition(def, std.testing.allocator));
}

test "validation: duplicate register offset" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control" },
            .{ .name = "STATUS", .offset = "0x00", .size = 32, .description = "Status" },
        },
    };
    try std.testing.expectError(GeneratorError.DuplicateRegisterOffset, validateDefinition(def, std.testing.allocator));
}

test "validation: overlapping registers" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 64, .description = "Control" },
            .{ .name = "STATUS", .offset = "0x04", .size = 32, .description = "Status" },
        },
    };
    try std.testing.expectError(GeneratorError.OverlappingRegisters, validateDefinition(def, std.testing.allocator));
}

test "validation: unsorted registers" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "STATUS", .offset = "0x08", .size = 32, .description = "Status" },
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.UnsortedRegisters, validateDefinition(def, std.testing.allocator));
}

test "validation: unaligned register offset" {
    const def32 = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x01", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.UnalignedRegister, validateDefinition(def32, std.testing.allocator));

    const def16 = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x03", .size = 16, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.UnalignedRegister, validateDefinition(def16, std.testing.allocator));
}

test "validation: unaligned or non-numeric base address" {
    const def_unaligned = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001001",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidBaseAddress, validateDefinition(def_unaligned, std.testing.allocator));

    const def_invalid = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "invalid_hex",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidBaseAddress, validateDefinition(def_invalid, std.testing.allocator));
}

test "validation: non-numeric offset" {
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0xNOT_HEX", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidRegisterOffset, validateDefinition(def, std.testing.allocator));
}

test "validation: description with newline or non-ascii" {
    const def_nl = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Line 1\nLine 2" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidDescription, validateDefinition(def_nl, std.testing.allocator));

    const def_non_ascii = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control \xFF bad" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidDescription, validateDefinition(def_non_ascii, std.testing.allocator));
}

test "validation: c23 reserved keywords as name" {
    const def_nullptr = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "nullptr", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidIdentifier, validateDefinition(def_nullptr, std.testing.allocator));

    const def_constexpr = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "constexpr", .offset = "0x00", .size = 32, .description = "Control" },
        },
    };
    try std.testing.expectError(GeneratorError.InvalidIdentifier, validateDefinition(def_constexpr, std.testing.allocator));
}

test "generateC23Header with padding holes and C23 static_assert" {
    const allocator = std.testing.allocator;
    const def = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control Register" },
            .{ .name = "STATUS", .offset = "0x08", .size = 32, .description = "Status Register" },
        },
    };

    var buffer = std.ArrayList(u8).init(allocator);
    defer buffer.deinit();

    try generateC23Header(def, allocator, buffer.writer());
    const output = buffer.items;

    try std.testing.expect(std.mem.indexOf(u8, output, "#pragma once") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "#ifndef TIMER0_REGS_H_") == null);
    try std.testing.expect(std.mem.indexOf(u8, output, "typedef enum : uintptr_t {") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "k_timer0_base_addr = 0x40001000UL") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "k_timer0_ctrl_offset = 0x00U") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "k_timer0_status_offset = 0x08U") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "uint8_t _reserved0[4];") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "volatile uint32_t STATUS;") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static_assert(offsetof(timer0_regs_t, CTRL) == 0x00U") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static_assert(offsetof(timer0_regs_t, STATUS) == 0x08U") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static_assert(sizeof(timer0_regs_t) == 0x0CU") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static inline volatile timer0_regs_t *timer0_get_regs(void)") != null);
}

fn expectC23HeaderCompiles(compiler: []const u8, def: PeripheralDef) !void {
    const allocator = std.testing.allocator;
    var header = std.ArrayList(u8).init(allocator);
    defer header.deinit();

    try generateC23Header(def, allocator, header.writer());

    var argv = std.ArrayList([]const u8).init(allocator);
    defer argv.deinit();
    var words = std.mem.tokenizeAny(u8, compiler, " ");
    while (words.next()) |word| try argv.append(word);
    try argv.appendSlice(&c23_cc.compile_args);

    var child = std.process.Child.init(argv.items, allocator);
    child.stdin_behavior = .Pipe;
    child.stdout_behavior = .Ignore;
    child.stderr_behavior = .Inherit;
    try child.spawn();
    try child.stdin.?.writeAll(header.items);
    child.stdin.?.close();
    child.stdin = null;

    switch (try child.wait()) {
        .Exited => |code| {
            if (code != 0) {
                return error.GeneratedHeaderDoesNotCompile;
            }
        },
        else => return error.GeneratedHeaderCompilerDidNotExit,
    }
}

test "generated headers compile under the pinned C23 compiler" {
    const timer = PeripheralDef{
        .peripheral = "TIMER0",
        .base_address = "0x40001000",
        .registers = &[_]Register{
            .{ .name = "CTRL", .offset = "0x00", .size = 32, .description = "Control Register" },
            .{ .name = "STATUS", .offset = "0x08", .size = 32, .description = "Status Register" },
        },
    };
    const pcie = PeripheralDef{
        .peripheral = "PCIE0",
        .base_address = "0x100000000",
        .registers = &[_]Register{
            .{ .name = "BAR0", .offset = "0x00", .size = 64, .description = "Base Address Register 0" },
        },
    };

    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const override = std.process.getEnvVarOwned(allocator, "RA8_C23_CC") catch null;
    const argv = try c23_cc.resolve(allocator, .{
        .override = override,
        .zig_exe = build_options.zig_exe,
    });
    const compiler = try std.mem.join(allocator, " ", argv);

    try expectC23HeaderCompiles(compiler, timer);
    try expectC23HeaderCompiles(compiler, pcie);
}

test "generateC23Header with trailing padding for struct alignment" {
    const allocator = std.testing.allocator;
    const def = PeripheralDef{
        .peripheral = "UART0",
        .base_address = "0x40002000",
        .registers = &[_]Register{
            .{ .name = "CR", .offset = "0x00", .size = 32, .description = "Control Register" },
            .{ .name = "DATA", .offset = "0x04", .size = 8, .description = "Data Byte" },
        },
    };

    var buffer = std.ArrayList(u8).init(allocator);
    defer buffer.deinit();

    try generateC23Header(def, allocator, buffer.writer());
    const output = buffer.items;

    try std.testing.expect(std.mem.indexOf(u8, output, "volatile uint8_t DATA;") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "uint8_t _reserved0[3]; /**< Reserved trailing padding */") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static_assert(sizeof(uart0_regs_t) == 0x08U") != null);
}

test "generateC23Header 64-bit base address" {
    const allocator = std.testing.allocator;
    const def = PeripheralDef{
        .peripheral = "PCIE0",
        .base_address = "0x100000000",
        .registers = &[_]Register{
            .{ .name = "BAR0", .offset = "0x00", .size = 64, .description = "Base Address Register 0" },
        },
    };

    var buffer = std.ArrayList(u8).init(allocator);
    defer buffer.deinit();

    try generateC23Header(def, allocator, buffer.writer());
    const output = buffer.items;

    try std.testing.expect(std.mem.indexOf(u8, output, "k_pcie0_base_addr = 0x0000000100000000ULL") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "volatile uint64_t BAR0;") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "static_assert(sizeof(pcie0_regs_t) == 0x08U") != null);
}

test "generateC23Header 16-bit offset width alignment" {
    const allocator = std.testing.allocator;
    const def = PeripheralDef{
        .peripheral = "DMA0",
        .base_address = "0x40003000",
        .registers = &[_]Register{
            .{ .name = "CFG", .offset = "0x0100", .size = 32, .description = "Config Register" },
        },
    };

    var buffer = std.ArrayList(u8).init(allocator);
    defer buffer.deinit();

    try generateC23Header(def, allocator, buffer.writer());
    const output = buffer.items;

    try std.testing.expect(std.mem.indexOf(u8, output, "typedef enum : uint16_t {") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "k_dma0_cfg_offset = 0x0100U") != null);
}
