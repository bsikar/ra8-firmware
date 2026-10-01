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
