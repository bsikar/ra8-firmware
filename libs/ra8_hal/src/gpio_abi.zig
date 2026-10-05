//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the rest of the GPIO driver (RA8FW-767), which replaces
//! gpio.c: pin init, peripheral routing, external IRQ attach/detach and
//! the default pin interface. Pin-level ops are in gpio_pins_abi.zig and
//! ra8_gpio_release in forwarders_abi.zig. Logic is in internal/gpio_pins.zig.

const common = @import("abi_common.zig");
const gp = @import("internal/gpio_pins.zig");

const tag = "GPIO";
const ok = common.k_ra8_ok;

const IcuCfg = extern struct { sense: u8, filter_div: u8, filter_en: bool };

/// Mirror of ra8_gpio_irq_cfg_t.
const IrqCfg = extern struct { pull: u8, sense: u8, filter_div: u8, filter_en: bool, priority: u8 };

const Handler = *const fn (?*anyopaque) callconv(.c) void;

extern fn ra8_pin_validator_claim(pin: u16, owner: [*:0]const u8) u16;
extern fn ra8_pin_validator_release(pin: u16) u16;
extern fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: *const IcuCfg) u16;
extern fn ra8_isr_register(event: u16, handler: Handler, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16;
extern fn ra8_isr_unregister(event: u16) u16;
extern fn ra8_gpio_write(pin: u16, level: u8) u16;
extern fn ra8_gpio_read(pin: u16, out_level: ?*u8) u16;
extern fn ra8_gpio_toggle(pin: u16) u16;
extern fn ra8_gpio_release(pin: u16) u16;

const icu_off = IcuCfg{ .sense = 0, .filter_div = 0, .filter_en = false };

const Hw = struct {
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    pub fn write8(_: Hw, addr: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(addr)).* = value;
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

const Decoded = union(enum) { pin: gp.Pin, err: u16 };

fn decode(pin: u16) Decoded {
    return switch (gp.decode(pin)) {
        .pin => |p| .{ .pin = p },
        .err => |e| .{ .err = if (e == .invalid_port) 0x206 else 0x207 },
    };
}

/// Range check, then claim for `owner`; returns the decoded pin or an error.
fn claim(pin: u16, owner: [*:0]const u8) Decoded {
    const d = decode(pin);
    if (d == .err) return d;
    const err = ra8_pin_validator_claim(pin, owner);
    if (err != ok) return .{ .err = err };
    return d;
}

fn initPin(pin: u16, value: u32, msg: [*:0]const u8) u16 {
    const p = switch (claim(pin, tag)) {
        .pin => |v| v,
        .err => |e| {
            common.ra8_log_emit_error_val(tag, "claim failed", e);
            return e;
        },
    };
    gp.program(Hw{}, gp.pfsAddr(p), &.{value});
    common.ra8_log_emit_info_val(tag, msg, pin);
    return ok;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u8) u16 {
    return initPin(pin, gp.outputValue(init_level == 1), "output init pin");
}

export fn ra8_gpio_input_init(pin: u16, pull: u8) u16 {
    return initPin(pin, gp.inputValue(pull), "input init pin");
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: ?[*:0]const u8) u16 {
    const who = owner orelse return nullPtr("owner must not be nullptr");
    const d = decode(pin);
    if (d == .err) return d.err;
    const err = ra8_pin_validator_claim(pin, who);
    if (err != ok) {
        common.ra8_log_emit_error_val(tag, "peripheral claim failed", err);
        return err;
    }
    const steps = gp.routeSteps(psel);
    gp.program(Hw{}, gp.pfsAddr(d.pin), &steps);
    common.ra8_log_emit_info_val(tag, "peripheral route pin", pin);
    return ok;
}

export fn ra8_gpio_attach_irq(pin: u16, irq_num: u8, cfg: ?*const IrqCfg, handler: ?Handler, ctx: ?*anyopaque) u16 {
    const c = cfg orelse return nullPtr("cfg must not be nullptr");
    const h = handler orelse return nullPtr("handler must not be nullptr");
    if (irq_num > gp.irq_num_max) return common.k_ra8_err_invalid_arg;
    var err = ra8_gpio_input_init(pin, c.pull);
    if (err != ok) return err;
    const icu = IcuCfg{ .sense = c.sense, .filter_div = c.filter_div, .filter_en = c.filter_en };
    err = ra8_icu_configure_irq_pin(irq_num, &icu);
    if (err != ok) {
        _ = ra8_pin_validator_release(pin);
        return err;
    }
    err = ra8_isr_register(gp.irqEvent(irq_num), h, ctx, c.priority, null);
    if (err != ok) {
        _ = ra8_icu_configure_irq_pin(irq_num, &icu_off);
        _ = ra8_pin_validator_release(pin);
        return err;
    }
    common.ra8_log_emit_info_val(tag, "attach irq pin", pin);
    return ok;
}

export fn ra8_gpio_detach_irq(pin: u16, irq_num: u8) u16 {
    if (irq_num > gp.irq_num_max) return common.k_ra8_err_invalid_arg;
    const err = ra8_isr_unregister(gp.irqEvent(irq_num));
    if (err != ok) return err;
    _ = ra8_icu_configure_irq_pin(irq_num, &icu_off);
    _ = ra8_pin_validator_release(pin);
    common.ra8_log_emit_info_val(tag, "detach irq pin", pin);
    return ok;
}

// ---- Default pin interface (ra8_pin_interface_t) -----------------------

const PinInterface = extern struct {
    output_init: *const fn (?*anyopaque, u16, u8) callconv(.c) u16,
    input_init: *const fn (?*anyopaque, u16, u8) callconv(.c) u16,
    write: *const fn (?*anyopaque, u16, u8) callconv(.c) u16,
    read: *const fn (?*anyopaque, u16, ?*u8) callconv(.c) u16,
    toggle: *const fn (?*anyopaque, u16) callconv(.c) u16,
    release: *const fn (?*anyopaque, u16) callconv(.c) u16,
    ctx: ?*anyopaque,
};

fn ifOutputInit(_: ?*anyopaque, pin: u16, level: u8) callconv(.c) u16 {
    return ra8_gpio_output_init(pin, level);
}
fn ifInputInit(_: ?*anyopaque, pin: u16, pull: u8) callconv(.c) u16 {
    return ra8_gpio_input_init(pin, pull);
}
fn ifWrite(_: ?*anyopaque, pin: u16, level: u8) callconv(.c) u16 {
    return ra8_gpio_write(pin, level);
}
fn ifRead(_: ?*anyopaque, pin: u16, out: ?*u8) callconv(.c) u16 {
    return ra8_gpio_read(pin, out);
}
fn ifToggle(_: ?*anyopaque, pin: u16) callconv(.c) u16 {
    return ra8_gpio_toggle(pin);
}
fn ifRelease(_: ?*anyopaque, pin: u16) callconv(.c) u16 {
    return ra8_gpio_release(pin);
}

export const g_ra8_gpio_pin_interface: PinInterface = .{
    .output_init = ifOutputInit,
    .input_init = ifInputInit,
    .write = ifWrite,
    .read = ifRead,
    .toggle = ifToggle,
    .release = ifRelease,
    .ctx = null,
};

export fn ra8_pin_interface_default() *const PinInterface {
    return &g_ra8_gpio_pin_interface;
}
