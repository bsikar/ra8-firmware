//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI of the ported half of the EK-RA8D2 board layer, and nothing
//! else. Every name here is the one the unchanged `inc/` headers already
//! declare, so no consumer changes.
//!
//! No `-Dabi-prefix` option, unlike `ra8_board_ra8p1`: that board needs a
//! renamed archive because its coverage suite links it alongside the default
//! EK-RA8D2 objects. This layer *is* the default, so nothing links two copies
//! of it and a prefix would buy nothing.

const audio = @import("internal/audio.zig");
const backdrop = @import("internal/backdrop.zig");
const bringup = @import("internal/bringup.zig");
const camera = @import("internal/camera.zig");
const camera_mode = @import("internal/camera_mode.zig");
const camera_xclk = @import("internal/camera_xclk.zig");
const clocks = @import("internal/clocks.zig");
const clock_profile = @import("internal/clock_profile.zig");
const clock_types = @import("internal/clock_types.zig");
const console_stream = @import("internal/console_stream.zig");
const dualcore = @import("internal/dualcore.zig");
const ethernet = @import("internal/ethernet.zig");
const hal = @import("internal/hal.zig");
const io_expander = @import("internal/io_expander.zig");
const pdm_mic = @import("internal/pdm_mic.zig");
const pdm_pins = @import("internal/pdm_pins.zig");
const stream = @import("internal/stream.zig");
const touch = @import("internal/touch.zig");
const uart_console = @import("internal/uart_console.zig");
const usb_port = @import("internal/usb_port.zig");
const usbhs = @import("internal/usbhs.zig");
const arduino = @import("internal/arduino.zig");
const board_info = @import("internal/board_info.zig");
const glcdc = @import("internal/glcdc.zig");
const glcdc_pins = @import("internal/glcdc_pins.zig");
const leds = @import("internal/leds.zig");
const panel = @import("internal/panel.zig");
const sdhi_pins = @import("internal/sdhi_pins.zig");
const pmod_pins = @import("internal/pmod_pins.zig");
const switches = @import("internal/switches.zig");
const xspi_pins = @import("internal/xspi_pins.zig");
const vocab = @import("internal/vocab.zig");

export fn ra8_board_shared_ram(out: ?*dualcore.SharedRam) u32 {
    return dualcore.describe(out);
}

export fn ra8_board_console_stream(out: ?*stream.IoStream) u32 {
    return console_stream.bind(out);
}

export fn ra8_board_usb_port_init(port: u32, role: u32) u32 {
    return usb_port.init(port, role);
}

export fn ra8_board_bringup(cfg: ?*const bringup.Cfg, out: ?*bringup.Rates) u32 {
    return bringup.run(cfg, out);
}

export fn ra8_board_clock_profile_to_chip(
    module: clock_types.Module,
    out_chip: ?*clock_types.Module,
) u32 {
    return clock_profile.toChip(module, out_chip);
}

export fn ra8_board_clock_profile_count(kind: u8) u8 {
    return clock_profile.count(kind);
}

export fn ra8_board_clock_profile_bind(clk: ?*clock_types.FwClock) u32 {
    return clock_profile.bind(clk);
}

export fn ra8_board_clock() *const clock_types.FwClock {
    return clock_profile.handle();
}

export fn ra8_board_clocks_init(out_rates: ?*hal.ClockRates) u32 {
    return clocks.init(out_rates);
}

export fn ra8_board_uart_console_init(baud: u32) u32 {
    return uart_console.init(baud);
}

// The pointer-and-length unwrapping stays here, in the C shape, so the console
// itself speaks slices. Order matters and matches the header: an empty write
// succeeds whatever `data` is, and only then does a null pointer fail.
export fn ra8_board_uart_console_write(data: ?[*]const u8, len: usize) u32 {
    if (len == 0) return vocab.Err.ok;
    const bytes = data orelse return vocab.Err.invalid_arg;
    return uart_console.write(bytes[0..len]);
}

export fn ra8_board_uart_console_read(out: ?[*]u8, cap: usize, out_len: ?*usize) u32 {
    const filled = out_len orelse return vocab.Err.invalid_arg;
    filled.* = 0;
    if (cap == 0) return vocab.Err.ok;
    const buf = out orelse return vocab.Err.invalid_arg;
    const result = uart_console.read(buf[0..cap]);
    filled.* = result.filled;
    return result.err;
}

export fn ra8_board_uart_console_flush() u32 {
    return uart_console.flush();
}

export fn priv_ra8_board_uart_console_is_up() bool {
    return uart_console.isUp();
}

export fn ra8_board_ethernet_init() u32 {
    return ethernet.init();
}

export fn priv_ra8_board_eth_eswm_bring_up(out_eswclk_hz: ?*u32) u32 {
    const out = out_eswclk_hz orelse return vocab.Err.null_ptr;
    return ethernet.eswmBringUp(out);
}

export fn priv_ra8_board_eth_etha_to_config() u32 {
    return ethernet.ethaToConfig();
}

export fn ra8_board_camera_get_ceu_config(
    mode: u8,
    frame_bytes_max: u32,
    out_config: ?*camera_mode.BoardConfig,
) u32 {
    const out = out_config orelse return vocab.Err.null_ptr;
    return camera_mode.get(mode, frame_bytes_max, out);
}

export fn ra8_board_camera_xclk_start(frequency_hz: u32) u32 {
    return camera_xclk.start(frequency_hz);
}

export fn ra8_board_camera_select_parallel() u32 {
    return camera.selectParallel();
}

export fn ra8_board_camera_route_parallel_pins() u32 {
    return camera.routeParallelPins();
}

export fn ra8_board_camera_reset() u32 {
    return camera.reset();
}

export fn ra8_board_camera_delay_ms(ctx: ?*anyopaque, milliseconds: u32) void {
    camera.delayMs(ctx, milliseconds);
}

export fn ra8_board_audio_init(sample_rate_hz: u32, bit_depth: u8, channels: u8) u32 {
    return audio.init(sample_rate_hz, bit_depth, channels);
}

export fn ra8_board_audio_play_sample_block(buf: ?[*]const i16, len: u32) u32 {
    return audio.playSampleBlock(buf, len);
}

export fn ra8_board_io_expander_set_usbhs_device_mode() u32 {
    return io_expander.setUsbhsDeviceMode();
}

export fn ra8_board_io_expander_set_usbhs_host_mode() u32 {
    return io_expander.setUsbhsHostMode();
}

export fn ra8_board_io_expander_apply_project_sw4_defaults() u32 {
    return io_expander.applyProjectSw4Defaults();
}

export fn ra8_board_io_expander_apply_sw4(output_byte: u8) u32 {
    return io_expander.applySw4(output_byte);
}

export fn ra8_board_io_expander_apply_sw4_mask(output_byte: u8, output_mask: u8) u32 {
    return io_expander.applyMask(output_byte, output_mask);
}

export fn ra8_board_io_expander_set_octospi_active() u32 {
    return io_expander.setOctospiActive();
}

export fn ra8_board_usbhs_pwr_set(on: bool) u32 {
    return usbhs.pwrSet(on);
}

export fn ra8_board_usbhs_device_init() u32 {
    return usbhs.deviceInit();
}

export fn ra8_board_usbhs_host_init() u32 {
    return usbhs.hostInit();
}

comptime {
    // Bench sessions resolve these by symbol while bisecting a USB-HS
    // bring-up fault, so they keep their C names.
    @export(&usbhs.probe, .{ .name = "g_usbhs_probe" });
    @export(&usbhs.role_probe, .{ .name = "g_usbhs_role_pin_probe" });
    @export(&usbhs.role_err, .{ .name = "g_usbhs_role_pin_err" });
}

export fn ra8_board_pdm_mic_route() u32 {
    return pdm_pins.routeAll();
}

export fn ra8_board_pdm_mic_get_config(microphone: u8, out_config: ?*pdm_mic.Config) u32 {
    const out = out_config orelse return vocab.Err.null_ptr;
    return pdm_mic.get(microphone, out);
}

export fn ra8_board_touch_open(cfg: ?*const touch.Cfg) u32 {
    const src = cfg orelse return vocab.Err.null_ptr;
    return touch.open(src);
}

export fn ra8_board_camera_i2c_ops(out: ?*camera.I2cBusOps) u32 {
    const dst = out orelse return vocab.Err.null_ptr;
    return camera.i2cOps(dst);
}

export fn ra8_board_get_info(out: ?*hal.BoardInfo) u32 {
    const dst = out orelse return vocab.Err.invalid_arg;
    return board_info.fill(dst);
}

export fn ra8_board_led_pin(led: u8, out_pin: ?*u16) u32 {
    const dst = out_pin orelse return vocab.Err.invalid_arg;
    return leds.readPin(led, dst);
}

/// The header declares this beside the other four and
/// ra8_board_ek_ra8d2_bringup.h:128 names it as prologue step 6, but it was
/// the one LED entry point the ABI never exported. `leds.init` has been here
/// the whole time; around thirty example main.c files call it and linked
/// short, as did the board's own bringup, which reaches it as an extern.
export fn ra8_board_led_init(led: u8) u32 {
    return leds.init(led);
}

export fn ra8_board_led_on(led: u8) u32 {
    return leds.on(led);
}

export fn ra8_board_led_off(led: u8) u32 {
    return leds.off(led);
}

export fn ra8_board_led_toggle(led: u8) u32 {
    return leds.toggle(led);
}

export fn ra8_board_sw_pin(sw: u8, out_pin: ?*u16) u32 {
    const dst = out_pin orelse return vocab.Err.invalid_arg;
    return switches.readPin(sw, dst);
}

export fn ra8_board_sw_init(sw: u8) u32 {
    return switches.init(sw);
}

export fn ra8_board_sw_read(sw: u8, out_pressed: ?*u8) u32 {
    const dst = out_pressed orelse return vocab.Err.invalid_arg;
    return switches.read(sw, dst);
}

export fn ra8_board_sw_attach_irq(
    sw: u8,
    cb: ?*const fn (?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
) u32 {
    return switches.attachIrq(sw, cb, ctx);
}

export fn ra8_board_glcdc_init(fmt: u8) u32 {
    return glcdc.init(fmt);
}

export fn ra8_board_panel_backdrop_begin(rgb888: u32) u32 {
    return backdrop.begin(rgb888);
}

export fn ra8_board_panel_backdrop_set(rgb888: u32) u32 {
    return backdrop.set(rgb888);
}

export fn ra8_board_lcd_panel_power_on() u32 {
    return panel.powerOn();
}

export fn ra8_board_backlight_set(on: bool) u32 {
    return panel.backlight(on);
}

export fn ra8_board_xspi_pins_init() u32 {
    return xspi_pins.init();
}

export fn ra8_board_pmod2_spi_bus_init() u32 {
    return pmod_pins.init();
}

export fn ra8_board_pmod2_spi_cs_set(asserted: bool) u32 {
    return pmod_pins.csSet(asserted);
}

export fn ra8_board_sdhi_pins_init() u32 {
    return sdhi_pins.init();
}

export fn ra8_board_arduino_pin_init(pin: u16, mode: u8) u32 {
    return arduino.pinInit(pin, mode);
}

export fn ra8_board_arduino_gpio_write(pin: u16, level: u32) u32 {
    return arduino.write(pin, level);
}

export fn ra8_board_arduino_gpio_read(pin: u16, out_level: ?*u32) u32 {
    const dst = out_level orelse return vocab.Err.invalid_arg;
    return arduino.read(pin, dst);
}

comptime {
    // The identity strings and the three GLCDC pin tables are read by name
    // from C: tests/misc/src/test_ra8_board_ek_ra8d2.c walks the tables and
    // the connectors header declares all six as extern.
    @export(&c_board_name, .{ .name = "k_ra8_board_name" });
    @export(&c_board_doc_rev, .{ .name = "k_ra8_board_doc_rev" });
    @export(&c_board_mcu, .{ .name = "k_ra8_board_mcu" });

    @export(&c_glcdc_rgb888_pins, .{ .name = "g_ra8_board_glcdc_rgb888_pins" });
    @export(&c_glcdc_rgb666_pins, .{ .name = "g_ra8_board_glcdc_rgb666_pins" });
    @export(&c_glcdc_rgb565_pins, .{ .name = "g_ra8_board_glcdc_rgb565_pins" });
    @export(&c_glcdc_rgb888_pin_count, .{ .name = "g_ra8_board_glcdc_rgb888_pin_count" });
    @export(&c_glcdc_rgb666_pin_count, .{ .name = "g_ra8_board_glcdc_rgb666_pin_count" });
    @export(&c_glcdc_rgb565_pin_count, .{ .name = "g_ra8_board_glcdc_rgb565_pin_count" });
}

const c_board_name: [*:0]const u8 = board_info.name.ptr;
const c_board_doc_rev: [*:0]const u8 = board_info.doc_rev.ptr;
const c_board_mcu: [*:0]const u8 = board_info.mcu.ptr;

/// `ra8_board_glcdc_pin_t`: a borrowed signal name and a packed pin.
const CGlcdcPin = extern struct {
    signal: [*:0]const u8,
    pin: u16,
};

fn cTable(comptime table: []const glcdc_pins.Entry) [table.len]CGlcdcPin {
    var out: [table.len]CGlcdcPin = undefined;
    for (table, 0..) |entry, i| out[i] = .{ .signal = entry.signal.ptr, .pin = entry.pin };
    return out;
}

const c_glcdc_rgb888_pins = cTable(&glcdc_pins.rgb888);
const c_glcdc_rgb666_pins = cTable(&glcdc_pins.rgb666);
const c_glcdc_rgb565_pins = cTable(&glcdc_pins.rgb565);
const c_glcdc_rgb888_pin_count: u32 = glcdc_pins.rgb888.len;
const c_glcdc_rgb666_pin_count: u32 = glcdc_pins.rgb666.len;
const c_glcdc_rgb565_pin_count: u32 = glcdc_pins.rgb565.len;
