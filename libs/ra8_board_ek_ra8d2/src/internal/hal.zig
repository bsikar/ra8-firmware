//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam between this board layer and everything it calls: the HAL, the
//! chip clock binding, the io-stream sink, and the board's own remaining C
//! translation units. Declared in one place so a host suite can stand a fake
//! behind any of them by defining the symbol.

const stream = @import("stream.zig");
const clock = @import("clock_types.zig");

/// `ra8_board_clock_rates_t`.
pub const ClockRates = extern struct {
    cpuclk0_hz: u32,
    pclka_hz: u32,
};

/// `ra8_sci_cfg_t`. The three mode fields are `enum : uint8_t` in the header.
pub const SciCfg = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    pclk_hz: u32,
};

/// `ra8_sci_cfg_t` field values this layer asks for.
pub const Sci = struct {
    pub const data_8: u8 = 8;
    pub const parity_none: u8 = 0;
    pub const stop_1: u8 = 0;
};

pub extern fn ra8_mstp_init() u32;
pub extern fn ra8_time_init(cpu_hz: u32) u32;
pub extern fn ra8_cgc_init() u32;
pub extern fn ra8_cgc_get_clock_hz(id: u32, out_hz: *u32) u32;
pub extern fn ra8_isr_globals_enable() void;

/// `clocks.zig` and `uart_console.zig` define these two; they stay on the
/// extern seam rather than being imported directly so a suite rooted at
/// `bringup.zig` can stand a fake behind either without dragging the real
/// CGC and SCI calls into its link.
pub extern fn ra8_board_clocks_init(out_rates: *ClockRates) u32;
pub extern fn ra8_board_uart_console_init(baud: u32) u32;
pub extern fn ra8_board_led_init(led: u32) u32;

pub extern fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32;
pub extern fn ra8_pfs_set_drive_strength(pin: u16, dscr: u8) u32;
pub extern fn ra8_gpio_output_init(pin: u16, init_level: u32) u32;
pub extern fn ra8_gpio_write(pin: u16, level: u32) u32;

pub extern fn ra8_sci_init(channel: u8, cfg: *const SciCfg) u32;
pub extern fn ra8_sci_write_polling(channel: u8, data: [*]const u8, len: u32) u32;
pub extern fn ra8_sci_getc_polling(channel: u8, out_byte: *u8) u32;
pub extern fn ra8_sci_flush(channel: u8) u32;

/// `ra8_etha_config_t`. `initial_mode` is `enum : uint8_t` in the header.
pub const EthaConfig = extern struct {
    initial_mode: u8,
    eaeie0_mask: u32,
    eaeie1_mask: u32,
    eaeie2_mask: u32,
};

/// `ra8_rmac_config_t`. `rx_filter` is `enum : uint32_t`; the three interface
/// fields are `enum : uint8_t`.
pub const RmacConfig = extern struct {
    rx_filter: u32,
    err_irq_enable: u32,
    mon0_irq_enable: u32,
    mon1_irq_enable: u32,
    mon2_irq_enable: u32,
    phy_interface: u8,
    link_speed: u8,
    duplex: u8,
    eswclk_hz: u32,
    mdc_hz: u32,
};

pub extern fn ra8_mstp_enable(id: u16) u32;
pub extern fn ra8_cgc_eswclk_init() u32;
pub extern fn ra8_cgc_eswclk_hz(out_hz: *u32) u32;
pub extern fn ra8_eth_coma_bringup() u32;
pub extern fn ra8_eth_rgmii_select(port: u8) u32;
pub extern fn ra8_etha_init(port: u8, cfg: *const EthaConfig) u32;
pub extern fn ra8_etha_set_mode(port: u8, mode: u8) u32;
pub extern fn ra8_rmac_init(port: u8, cfg: *const RmacConfig) u32;
pub extern fn ra8_rmac_mdio_c22_read(port: u8, phy_addr: u8, reg_addr: u8, out_value: *u16) u32;
pub extern fn ra8_rmac_mdio_c22_write(port: u8, phy_addr: u8, reg_addr: u8, value: u16) u32;

pub extern fn ra8_board_usbhs_device_init() u32;
pub extern fn ra8_board_usbhs_host_init() u32;

/// `uart_console.zig` defines this; same extern seam, same reason, so the
/// `console_stream.zig` suite can answer it without an SCI behind it.
pub extern fn priv_ra8_board_uart_console_is_up() bool;

/// `ra8_io_stream_uart_init`, the one symbol in this seam that an app may
/// legitimately not link.
///
/// The C build dropped `..._console_stream.c` from the board glob unless the
/// app named `ra8_io` in LIBS, because the sink lives in that library. A Zig
/// static archive is a single compilation unit, so there is no per-file gate
/// left to apply: the console code is in the archive whether the app asked for
/// it or not. Declaring the sink weak keeps the link honest instead -- an app
/// without `ra8_io` resolves it to null and `console_stream.bind` refuses,
/// rather than failing to link over a facility it never asked for. Same shape
/// `src/boot/vector_table.c` already uses for the optional anti-rollback hook.
pub const UartStreamInit = fn (
    s: *stream.IoStream,
    state: *stream.UartState,
    channel: u8,
) callconv(.c) u32;

pub const ra8_io_stream_uart_init: ?*const UartStreamInit = @extern(
    ?*const UartStreamInit,
    .{ .name = "ra8_io_stream_uart_init", .linkage = .weak },
);

pub extern fn fw_clock_bind(
    clk: *clock.FwClock,
    iface: *const clock.FwClockIface,
    ctx: ?*anyopaque,
) u32;
pub extern fn fw_clock_ra8_iface() *const clock.FwClockIface;

/// `ra8_gpt_cfg_t`. `mode` and `prescaler` are each `enum : uint8_t`.
pub const GptCfg = extern struct {
    mode: u8,
    prescaler: u8,
    period: u32,
    duty_a: u32,
    duty_b: u32,
    auto_start: bool,
};

/// `ra8_gpt_pwm_pin_cfg_t`. The three encodings are each `enum : uint8_t`.
pub const GptPwmPinCfg = extern struct {
    output_enable: bool,
    polarity: u8,
    stop_level: u8,
    disable_on_fault: u8,
};

pub extern fn ra8_gpt_init(channel: u8, cfg: *const GptCfg) u32;
pub extern fn ra8_gpt_pwm_pin_configure(channel: u8, pin: u8, cfg: *const GptPwmPinCfg) u32;
pub extern fn ra8_delay_ms(milliseconds: u32) void;
pub extern fn ra8_board_io_expander_apply_sw4_mask(output_byte: u8, output_mask: u8) u32;

/// `ra8_i2c_bus_ops_t`, the Ring-3 facade the sensor driver is handed.
pub const I2cBusOps = extern struct {
    write: ?*const fn (
        ctx: ?*anyopaque,
        addr: u8,
        data: [*]const u8,
        len: u32,
        send_stop: bool,
    ) callconv(.c) u32,
    read: ?*const fn (ctx: ?*anyopaque, addr: u8, data: [*]u8, len: u32) callconv(.c) u32,
    transfer: ?*const fn (
        ctx: ?*anyopaque,
        addr: u8,
        wr: [*]const u8,
        wr_len: u32,
        rd: [*]u8,
        rd_len: u32,
    ) callconv(.c) u32,
    ctx: ?*anyopaque,
};

/// `ra8_io_i2c_bus_t`. Both members are private to `ra8_io`; this side only
/// ever hands the address back, never reads through it.
pub const IoI2cBus = extern struct {
    iface: ?*const anyopaque,
    ctx: ?*anyopaque,
};

/// The two `ra8_io` entry points the camera adapter needs, both weak.
///
/// The C build dropped `..._camera.c` from the board glob unless the app
/// named `ra8_io`, `ra8_io_bus` or `ra8_camera` in LIBS, because the bus
/// backend lives there. A Zig static archive is a single compilation unit, so
/// that per-file gate has nowhere left to attach: the camera code is in the
/// archive whether the app asked for it or not. Declaring both sinks weak
/// keeps the link honest instead. An app without `ra8_io` resolves them to
/// null and `camera.i2cOps` refuses, rather than failing to link over a
/// facility it never asked for. Same treatment `ra8_io_stream_uart_init` got
/// above.
pub const IoI2cBindRiic = fn (bus: *IoI2cBus, channel: u8) callconv(.c) u32;
pub const IoI2cAsOps = fn (bus: *const IoI2cBus, out: *I2cBusOps) callconv(.c) u32;

pub const ra8_io_i2c_bus_bind_riic: ?*const IoI2cBindRiic = @extern(
    ?*const IoI2cBindRiic,
    .{ .name = "ra8_io_i2c_bus_bind_riic", .linkage = .weak },
);

pub const ra8_io_i2c_bus_as_ops: ?*const IoI2cAsOps = @extern(
    ?*const IoI2cAsOps,
    .{ .name = "ra8_io_i2c_bus_as_ops", .linkage = .weak },
);

/// `ra8_i3c_cfg_t`. The bit-rate divider is solved inside `ra8_i3c` against
/// the `pclka_hz` handed in, which is why this side reads the live clock.
pub const I3cCfg = extern struct {
    mode: u8,
    bus_hz: u32,
    pclka_hz: u32,
};

/// `ra8_touch_cfg_t`, the GT911 driver's open config.
pub const TouchCfg = extern struct {
    bus: I2cBusOps,
    target_7b: u8,
    irq_pin: u8,
    max_points: u8,
};

pub extern fn ra8_i3c_init(channel: u8, cfg: *const I3cCfg) u32;
pub extern fn ra8_touch_open(cfg: *const TouchCfg) u32;

/// The third `ra8_io` sink, weak for the same reason as the two above: the C
/// build dropped `..._touch.c` from the board glob unless the app named
/// `ra8_io` or `ra8_io_bus` in LIBS, and a Zig archive has nowhere to hang
/// that per-file gate.
pub const IoI2cBindI3cCompat = fn (bus: *IoI2cBus, channel: u8) callconv(.c) u32;

pub const ra8_io_i2c_bus_bind_i3c_compat: ?*const IoI2cBindI3cCompat = @extern(
    ?*const IoI2cBindI3cCompat,
    .{ .name = "ra8_io_i2c_bus_bind_i3c_compat", .linkage = .weak },
);
