//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The RA8 module-to-clock table, and nothing else.
//!
//! This unit includes no register header and reads no hardware, so the whole
//! mapping is a pure function of its arguments and every row can be driven on
//! the host without a fake peripheral block. The ops in `clock_ops_abi.zig`
//! all funnel through `resolve`, so a row proven here is the row they use.
//!
//! Where the rows come from: a row exists only where the tree already
//! establishes the pairing (UART/SPI/I2C/CAN/SD host read PCLKA, the camera
//! reads PCLKD, the core is CPUCLK0 and memory is FCLK per `ra8_cgc.h`).
//! Gate-only rows take their module-stop bit from `ra8_mstp_regs.h`; their
//! feed domain is not established anywhere in the tree, so they have none
//! here. Timer, PWM, DMA, USB, RTC and watchdog are deliberately absent: no
//! flat instance index addresses their module-stop bits correctly.

const std = @import("std");

/// `ra8_clock_id_t`, the clock-tree domains queryable at runtime.
pub const Domain = enum(u8) {
    cpuclk0 = 0,
    cpuclk1 = 1,
    iclk = 2,
    pclka = 3,
    pclkb = 4,
    pclkc = 5,
    pclkd = 6,
    pclke = 7,
    fclk = 8,
    mriclk = 9,
};

/// `ra8_mstp_t`, a packed `(register << 8) | bit` module-stop identifier.
/// Only the bases this table names are spelled out; the untouched C suite
/// compares them against `ra8_mstp_regs.h` itself, which is what keeps the
/// two transcriptions from drifting apart.
pub const Mstp = struct {
    pub const Reg = enum(u16) { a = 0, b = 1, c = 2, d = 3, e = 4 };

    fn id(reg: Reg, bit: u8) u16 {
        return (@as(u16, @intFromEnum(reg)) << 8) | bit;
    }

    pub const sram0: u16 = id(.a, 0);
    pub const sci0: u16 = id(.b, 31);
    pub const spi0: u16 = id(.b, 19);
    pub const iic0: u16 = id(.b, 9);
    pub const canfd0: u16 = id(.c, 27);
    pub const sdhi0: u16 = id(.c, 12);
    pub const glcdc: u16 = id(.c, 4);
    pub const ceu: u16 = id(.c, 16);
    pub const eswm: u16 = id(.c, 30);
    pub const rsip: u16 = id(.c, 31);
    pub const dac12_0: u16 = id(.d, 20);
    pub const adc16h: u16 = id(.d, 21);
};

/// `fw_clock_module_kind_t`. The count is the table's length, so a kind added
/// to the port without a row here fails to compile rather than resolving to
/// whatever follows the array.
pub const Kind = enum(u8) {
    none = 0,
    core = 1,
    uart = 2,
    spi = 3,
    i2c = 4,
    can = 5,
    timer = 6,
    pwm = 7,
    adc = 8,
    dac = 9,
    dma = 10,
    display = 11,
    camera = 12,
    usb = 13,
    ethernet = 14,
    sdhost = 15,
    crypto = 16,
    rtc = 17,
    watchdog = 18,
    memory = 19,
};

pub const kind_count: u8 = 20;

/// What this adapter knows about one module instance. Both fields are absent
/// for a kind it carries no row for, which `resolve` reports as `NotFound`
/// rather than handing back a row of nothing.
pub const Row = struct {
    domain: ?Domain = null,
    gate: ?u16 = null,
};

pub const Fault = error{
    /// Kind outside the enumeration.
    BadKind,
    /// No row for that kind and index.
    NotFound,
};

/// One row of the per-kind table.
///
/// `gate_base` is instance zero's module-stop id. Every multi-instance run
/// this adapter carries descends by one bit position within a single MSTPCRx
/// word as the instance number rises (SCI0 is MSTPB31 and SCI9 is MSTPB22;
/// IIC0 is MSTPB9 and IIC2 is MSTPB7; SPI, CANFD, SDHI and DAC12 do the same),
/// so instance N's id is `gate_base - N`. That is a property of the rows
/// present, not a general rule about the chip, which is exactly why the runs
/// that break it are absent rather than approximated.
const KindRow = struct {
    domain: ?Domain = null,
    gate_base: ?u16 = null,
    instances: u8 = 0,
};

/// Instance counts, named so the table reads as hardware rather than as a
/// column of integers.
const Instances = struct {
    pub const one: u8 = 1;
    pub const two: u8 = 2;
    pub const three: u8 = 3;
    pub const sci: u8 = 10;
};

const kinds = blk: {
    var table = [_]KindRow{.{}} ** kind_count;
    table[@intFromEnum(Kind.core)] = .{ .domain = .cpuclk0, .instances = Instances.one };
    table[@intFromEnum(Kind.uart)] = .{ .domain = .pclka, .gate_base = Mstp.sci0, .instances = Instances.sci };
    table[@intFromEnum(Kind.spi)] = .{ .domain = .pclka, .gate_base = Mstp.spi0, .instances = Instances.two };
    table[@intFromEnum(Kind.i2c)] = .{ .domain = .pclka, .gate_base = Mstp.iic0, .instances = Instances.three };
    table[@intFromEnum(Kind.can)] = .{ .domain = .pclka, .gate_base = Mstp.canfd0, .instances = Instances.two };
    table[@intFromEnum(Kind.adc)] = .{ .gate_base = Mstp.adc16h, .instances = Instances.one };
    table[@intFromEnum(Kind.dac)] = .{ .gate_base = Mstp.dac12_0, .instances = Instances.two };
    table[@intFromEnum(Kind.display)] = .{ .gate_base = Mstp.glcdc, .instances = Instances.one };
    table[@intFromEnum(Kind.camera)] = .{ .domain = .pclkd, .gate_base = Mstp.ceu, .instances = Instances.one };
    table[@intFromEnum(Kind.ethernet)] = .{ .gate_base = Mstp.eswm, .instances = Instances.one };
    table[@intFromEnum(Kind.sdhost)] = .{ .domain = .pclka, .gate_base = Mstp.sdhi0, .instances = Instances.two };
    table[@intFromEnum(Kind.crypto)] = .{ .gate_base = Mstp.rsip, .instances = Instances.one };
    table[@intFromEnum(Kind.memory)] = .{ .domain = .fclk, .instances = Instances.one };
    break :blk table;
};

/// Resolve a neutral module onto its RA8 domain and module-stop bit.
pub fn resolve(kind_raw: u8, index: u8) Fault!Row {
    if (kind_raw >= kind_count) return error.BadKind;

    const row = kinds[kind_raw];
    if (index >= row.instances) return error.NotFound;

    return .{
        .domain = row.domain,
        // Descending within one MSTPCRx word; see KindRow.
        .gate = if (row.gate_base) |base| base - index else null,
    };
}

test "the table length is the port's kind count" {
    try std.testing.expectEqual(kind_count, kinds.len);
    try std.testing.expectEqual(kind_count, @intFromEnum(Kind.memory) + 1);
}
