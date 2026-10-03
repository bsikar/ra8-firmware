//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ELF reader: what it accepts, and that a damaged file is an error and
//! never a quiet pass.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const elf32 = checker.elf32;
const fixture = @import("elf_fixture.zig");

comptime {
    _ = @import("elf_fixture.zig");
}

test "a fixture reads back as the sections and symbols it was built from" {
    const image = fixture.build(.{});
    const file = try elf32.File.init(image.bytes());
    try testing.expectEqual(@as(u16, 14), file.section_count);

    const data = try file.section(@intFromEnum(fixture.Target.data));
    try testing.expectEqualStrings(".data", data.name);
    try testing.expectEqual(@as(u32, 0x10000018), data.address);
    try testing.expectEqual(@as(usize, 16), data.bytes.len);

    const symbols = 11;
    const double = try file.symbol(symbols, fixture.Symbol.double);
    try testing.expectEqualStrings("double", double.name);
    try testing.expectEqual(@as(u32, 0x30191), double.value);
    try testing.expect(!double.is_section);
    const section = try file.symbol(symbols, fixture.Symbol.data_section);
    try testing.expect(section.is_section);
    try testing.expectEqual(@as(u16, 3), section.section);
}

test "a file that is not an ELF, not 32-bit or not ARM is refused" {
    const prose = "not an ELF at all, only prose, but longer than an ELF header is";
    try testing.expectError(error.NotElf, elf32.File.init(prose));
    try testing.expectError(error.NotElf, elf32.File.init("\x7fELF"));

    var wide = fixture.build(.{});
    wide.buf[std.elf.EI_CLASS] = std.elf.ELFCLASS64;
    try testing.expectError(error.NotElf32Le, elf32.File.init(wide.bytes()));

    var big = fixture.build(.{});
    big.buf[std.elf.EI_DATA] = std.elf.ELFDATA2MSB;
    try testing.expectError(error.NotElf32Le, elf32.File.init(big.bytes()));

    const other = fixture.build(.{ .machine = .RISCV });
    try testing.expectError(error.NotArm, elf32.File.init(other.bytes()));
}

test "a file cut short anywhere is an error, never a shorter file that passes" {
    const image = fixture.build(.{ .data = &.{.{ .at = 0x10000020, .symbol = 1, .kind = 2 }} });
    const whole = image.bytes();
    for (0..whole.len) |len| {
        var findings = checker.check.Iterator.init(whole[0..len]) catch continue;
        // The header fitted, so the cut is further in: walking must hit it.
        var failed = false;
        while (findings.next() catch blk: {
            failed = true;
            break :blk null;
        }) |_| {}
        try testing.expect(failed);
    }
}

test "a section or symbol index past the end is an error" {
    const image = fixture.build(.{});
    const file = try elf32.File.init(image.bytes());
    try testing.expectError(error.Truncated, file.section(14));
    try testing.expectError(error.Truncated, file.symbol(11, 5));
}
