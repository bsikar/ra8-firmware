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
pub extern fn ra8_board_led_init(led: u8) u32;

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

/// `ra8_pdm_channel_cfg_t`. Mirrored field for field because the board's host
/// suite reads the coefficients back off the config it is handed.
pub const PdmChannelCfg = extern struct {
    pub const hpf_h_count = 2;
    pub const comp_h_count = 11;
    pub const lpf_h1_count = 20;

    sinc_order: u8,
    clock_div: u8,
    sinc_dec: u8,
    sinc_range: u8,
    data_shift: u8,
    edge: u8,
    hpf_shift: u8,
    cf_shift: u8,
    lpf_shift: u8,
    rx_threshold: u8,
    hpf_s0: u16,
    hpf_k1: u16,
    hpf_h: [hpf_h_count]u16,
    comp_h: [comp_h_count]u16,
    lpf_h0: u16,
    lpf_h1: [lpf_h1_count]u16,
};

/// `ra8_board_pdm_mic_config_t`, the whole microphone story in one struct.
pub const PdmMicConfig = extern struct {
    pdm: PdmChannelCfg,
    sample_rate_hz: u32,
    channel: u8,
    valid_bits: u8,
};

/// `ra8_ssie_cfg_t`. Mirrored because the board builds one and hands it over;
/// the SSIE driver reads every field.
pub const SsieCfg = extern struct {
    role: u32,
    format: u32,
    data_word: u32,
    system_word: u32,
    bclk_div: u32,
    use_gpt_clk: bool,
    long_frame: bool,
    bckp_rising: bool,
    lrckp_low: bool,
    spdp_high: bool,
    byte_swap: bool,
    lr_continue: bool,
    bck_idle_stop: bool,
    enable_aucke: bool,
    tx_threshold: u8,
    rx_threshold: u8,
};

/// `ra8_i2c_cfg_t`.
pub const I2cCfg = extern struct {
    bus_hz: u32,
    pclkb_hz: u32,
};

pub extern fn ra8_ssie_init(channel: u8, cfg: *const SsieCfg) u32;
pub extern fn ra8_ssie_write_buffer(channel: u8, data: [*]const u32, len: u16, out_written: *u16) u32;
pub extern fn ra8_i2c_init(channel: u8, cfg: *const I2cCfg) u32;
pub extern fn ra8_i2c_write(channel: u8, addr_7b: u8, data: [*]const u8, len: usize, send_stop: bool) u32;
pub extern fn ra8_gpio_input_init(pin: u16, pull: u32) u32;
pub extern fn ra8_gpio_read(pin: u16, out_level: *u32) u32;
pub extern fn ra8_gpio_release(pin: u16) u32;
pub extern fn ra8_mpc_set_open_drain(port: u32, pin_index: u32, enable: bool) u32;
pub extern fn ra8_cgc_usbhs_pll_enable() u32;
pub extern fn ra8_usb_device_init(speed: u32) u32;
pub extern fn ra8_usb_host_init(speed: u32) u32;

/// `ra8_board_info_t` from the board's connectors header: three borrowed
/// string pointers, never owned by the caller.
pub const BoardInfo = extern struct {
    name: [*:0]const u8,
    doc_rev: [*:0]const u8,
    mcu: [*:0]const u8,
};

/// `ra8_icu_irq_cfg_t`. Fields map straight onto IRQCRi, HUM 14.2.12 p 535.
pub const IcuIrqCfg = extern struct {
    sense: u8,
    filter_div: u8,
    filter_en: bool,
};

pub const IsrHandler = *const fn (?*anyopaque) callconv(.c) void;

pub extern fn ra8_gpio_toggle(pin: u16) u32;
pub extern fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: *const IcuIrqCfg) u32;
pub extern fn ra8_isr_register(
    event: u16,
    handler: IsrHandler,
    ctx: ?*anyopaque,
    priority: u8,
    out_slot: ?*u16,
) u32;
