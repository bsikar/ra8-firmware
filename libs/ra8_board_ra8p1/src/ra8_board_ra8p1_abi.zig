//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI surface of the RA8P1 board-support layer: the exact symbols
//! `ra8_board_ra8p1.h` declares, forwarding to the Zig policy modules and to
//! the HAL.
//!
//! RA8P1 deliberately exports the same substitutable BSP names as EK-RA8D2, so
//! only one board layer is ever linked into an image. The host coverage suite
//! does link both, which is what `-Dabi-prefix` is for: it renames this whole
//! surface (`ra8_board_*` -> `<prefix>board_*`, `k_ra8_board_*` ->
//! `k_<prefix>board_*`) the way the C build's per-symbol `-D` renames used to,
//! so the default EK-RA8D2 objects stay linked without duplicate definitions.
//! The same mechanism `ra8_core` uses for its freestanding archive.
//!
//! Ring 5 / BSP, World S. This layer never touches an MCU register itself;
//! `ra8_gpio_*`, `ra8_pfs_route_peripheral`, `ra8_icu_configure_irq_pin` and
//! `ra8_sci_*` carry every register write, and each of those HAL entry points
//! carries its own RA8P1-identical HUM citation (chip HUM R01UH1064EJ).

const options = @import("build_config");

const console_mod = @import("internal/console.zig");
const identity = @import("internal/identity.zig");
const pins = @import("internal/pins.zig");
const sci_hal = @import("internal/sci_hal.zig");
const vocab = @import("internal/vocab.zig");

const Err = vocab.Err;
const Level = vocab.Level;
const Pull = vocab.Pull;

extern fn ra8_gpio_output_init(pin: u16, level: u8) u32;
extern fn ra8_gpio_input_init(pin: u16, pull: u8) u32;
extern fn ra8_gpio_write(pin: u16, level: u8) u32;
extern fn ra8_gpio_read(pin: u16, out_level: *u8) u32;
extern fn ra8_gpio_toggle(pin: u16) u32;
extern fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: *const IcuIrqCfg) u32;
extern fn ra8_isr_register(
    event: u16,
    handler: ?*const fn (?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
    priority: u8,
    out_slot: ?*u16,
) u32;

/// `ra8_icu_irq_cfg_t` (libs/ra8_hal/inc/ra8_icu.h).
const IcuIrqCfg = extern struct {
    sense: u8,
    filter_div: u8,
    filter_en: bool,
};

/// ICU encodings the switch IRQ uses: falling edge, filter sampled at PCLKB.
const IcuMode = struct {
    const irqmd_falling: u8 = 0;
    const fclksel_pclkb: u8 = 0;
};

/// `k_ra8_isr_prio_default` (libs/ra8_hal/inc/ra8_isr.h).
const isr_prio_default: u8 = 8;

/// `ra8_board_info_t`.
const BoardInfo = extern struct {
    name: [*:0]const u8,
    doc_rev: [*:0]const u8,
    mcu: [*:0]const u8,
};

const board_name: [*:0]const u8 = identity.Board.name;
const board_doc_rev: [*:0]const u8 = identity.Board.doc_rev;
const board_mcu: [*:0]const u8 = identity.Board.mcu;

// Every exported symbol, named as the suffix that follows the ABI prefix. The
// `k_` constants carry their own leading `k_`, so both spellings rename
// together.
comptime {
    @export(&board_name, .{ .name = "k_" ++ options.abi_prefix ++ "board_name", .linkage = .strong });
    @export(&board_doc_rev, .{ .name = "k_" ++ options.abi_prefix ++ "board_doc_rev", .linkage = .strong });
    @export(&board_mcu, .{ .name = "k_" ++ options.abi_prefix ++ "board_mcu", .linkage = .strong });

    const entries = .{
        .{ "board_get_info", &getInfo },
        .{ "board_led_pin", &ledPin },
        .{ "board_led_init", &ledInit },
        .{ "board_led_on", &ledOn },
        .{ "board_led_off", &ledOff },
        .{ "board_led_toggle", &ledToggle },
        .{ "board_sw_pin", &swPin },
        .{ "board_sw_init", &swInit },
        .{ "board_sw_read", &swRead },
        .{ "board_sw_attach_irq", &swAttachIrq },
        .{ "board_uart_console_init", &consoleInit },
        .{ "board_uart_console_write", &consoleWrite },
        .{ "board_uart_console_read", &consoleRead },
        .{ "board_uart_console_flush", &consoleFlush },
    };
    for (entries) |entry| {
        @export(entry[1], .{ .name = options.abi_prefix ++ entry[0], .linkage = .strong });
    }
}

fn getInfo(out: ?*BoardInfo) callconv(.c) u32 {
    const info = out orelse return Err.invalid_arg;
    info.* = .{
        .name = board_name,
        .doc_rev = board_doc_rev,
        .mcu = board_mcu,
    };
    return Err.ok;
}

fn ledPin(led: u8, out_pin: ?*u16) callconv(.c) u32 {
    const slot = out_pin orelse return Err.invalid_arg;
    slot.* = pins.ledPin(led) orelse return Err.invalid_arg;
    return Err.ok;
}

fn ledInit(led: u8) callconv(.c) u32 {
    const pin = pins.ledPin(led) orelse return Err.invalid_arg;
    return ra8_gpio_output_init(pin, Level.low);
}

fn ledOn(led: u8) callconv(.c) u32 {
    const pin = pins.ledPin(led) orelse return Err.invalid_arg;
    return ra8_gpio_write(pin, Level.high);
}

fn ledOff(led: u8) callconv(.c) u32 {
    const pin = pins.ledPin(led) orelse return Err.invalid_arg;
    return ra8_gpio_write(pin, Level.low);
}

fn ledToggle(led: u8) callconv(.c) u32 {
    const pin = pins.ledPin(led) orelse return Err.invalid_arg;
    return ra8_gpio_toggle(pin);
}

fn swPin(sw: u8, out_pin: ?*u16) callconv(.c) u32 {
    const slot = out_pin orelse return Err.invalid_arg;
    slot.* = pins.swPin(sw) orelse return Err.invalid_arg;
    return Err.ok;
}

fn swInit(sw: u8) callconv(.c) u32 {
    const pin = pins.swPin(sw) orelse return Err.invalid_arg;
    return ra8_gpio_input_init(pin, Pull.up);
}

fn swRead(sw: u8, out_pressed: ?*u8) callconv(.c) u32 {
    const slot = out_pressed orelse return Err.invalid_arg;
    const pin = pins.swPin(sw) orelse return Err.invalid_arg;

    var level: u8 = Level.high;
    const err = ra8_gpio_read(pin, &level);
    // GCOVR_EXCL_LINE -- static board pins cannot fail validation
    if (err != Err.ok) return err;

    slot.* = @intFromBool(pins.pressed(level));
    return Err.ok;
}

fn swAttachIrq(
    sw: u8,
    cb: ?*const fn (?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
) callconv(.c) u32 {
    const handler = cb orelse return Err.invalid_arg;
    const irq_num = pins.swIrqNum(sw) orelse return Err.invalid_arg;

    // Step 1: falling-edge detection so a press (active-low) latches the
    // IRQCR channel; the digital filter samples at PCLKB to debounce contact
    // bounce (HUM R01UH1064EJ Ch 14 "Interrupt Controller Unit").
    const cfg = IcuIrqCfg{
        .sense = IcuMode.irqmd_falling,
        .filter_div = IcuMode.fclksel_pclkb,
        .filter_en = true,
    };
    const icu_err = ra8_icu_configure_irq_pin(irq_num, &cfg);
    // GCOVR_EXCL_LINE -- IRQ12/13 are well inside the 32-channel limit
    if (icu_err != Err.ok) return icu_err;

    // Step 2: route the channel's ELC event through an IELSR slot and enable
    // the matching NVIC line. SW1 -> IRQ13, SW2 -> IRQ12 (provisional).
    const event = pins.swEvent(sw) orelse return Err.invalid_arg;
    return ra8_isr_register(event, handler, ctx, isr_prio_default, null);
}

var console: console_mod.Console(sci_hal) = .{};

fn consoleInit(baud: u32) callconv(.c) u32 {
    return console.init(baud);
}

fn consoleWrite(data: ?[*]const u8, len: usize) callconv(.c) u32 {
    return console.write(data, len);
}

fn consoleRead(out: ?[*]u8, cap: usize, out_len: ?*usize) callconv(.c) u32 {
    return console.read(out, cap, out_len);
}

fn consoleFlush() callconv(.c) u32 {
    return console.flush();
}
