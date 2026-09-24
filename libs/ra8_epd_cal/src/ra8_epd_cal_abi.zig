//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_epd_cal/inc/ra8_epd_cal.h`. The record codec
//! lives in `internal/root.zig`; this file owns the injected seams, the
//! resolution ladder (controller -> per-device record -> operator value ->
//! bench flag), the `ra8_err_t` mapping and the diagnostic log lines the C
//! implementation emitted.
//!
//! No HAL header is reachable from here: the controller and the non-volatile
//! store are touched only through the function pointers in the config struct,
//! which is what keeps every branch host-testable.

const std = @import("std");
const build_config = @import("build_config");
const implementation = @import("internal/root.zig");

/// Decoded per-device calibration record (`ra8_epd_cal_record_t`).
pub const Record = implementation.Record;
/// Panel's documented VCOM window (`ra8_epd_cal_limits_mv_t`).
pub const Limits = implementation.Limits;
/// Serialised record size in bytes (`k_ra8_epd_cal_blob_size`).
pub const blob_size = implementation.blob_size;
/// Current on-flash schema version (`k_ra8_epd_cal_schema_version`).
pub const schema_version = implementation.schema_version;

/// Subset of `ra8_err_t` this library returns.
pub const CalError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    invalid_size = 0x105,
    not_found = 0x106,
    not_supported = 0x107,
    hw_error = 0x204,
    crc_mismatch = 0x405,
    validation_failed = 0x501,
    range_check_failed = 0x503,
    null_ptr = 0x504,
};

/// Raw `ra8_err_t` as it crosses the ABI, so a seam may hand back any code in
/// the repo's error space and have it propagate unchanged.
pub const RawErr = u16;

/// Which authority supplied a resolved VCOM (`ra8_epd_cal_source_t`).
pub const Source = enum(u8) {
    none = 0,
    panel = 1,
    record = 2,
    provisioned = 3,
    bench = 4,
};

/// Read the serialised record out of non-volatile storage.
pub const NvReadFn = *const fn (ctx: ?*anyopaque, dst: [*]u8, len: usize) callconv(.c) RawErr;
/// Write the serialised record back to non-volatile storage.
pub const NvWriteFn = *const fn (ctx: ?*anyopaque, src: [*]const u8, len: usize) callconv(.c) RawErr;
/// Read the controller's current VCOM magnitude.
pub const PanelGetFn = *const fn (ctx: ?*anyopaque, out_mv: *u16) callconv(.c) RawErr;
/// Drive a VCOM magnitude into the controller.
pub const PanelSetFn = *const fn (ctx: ?*anyopaque, mv: u16) callconv(.c) RawErr;

/// Controller access seam (`ra8_epd_cal_panel_ops_t`).
pub const PanelOps = extern struct {
    get: ?PanelGetFn = null,
    set: ?PanelSetFn = null,
    ctx: ?*anyopaque = null,
};

/// Per-device record store seam (`ra8_epd_cal_store_t`).
pub const Store = extern struct {
    read: ?NvReadFn = null,
    write: ?NvWriteFn = null,
    ctx: ?*anyopaque = null,
};

/// Everything `ra8_epd_cal_resolve` needs (`ra8_epd_cal_cfg_t`).
pub const Config = extern struct {
    limits: Limits = .{},
    panel: PanelOps = .{},
    store: Store = .{},
    provisioned_mv: u16 = 0,
    has_provisioned: u8 = 0,
};

/// Outcome of `ra8_epd_cal_resolve` (`ra8_epd_cal_result_t`).
pub const Result = extern struct {
    vcom_mv: u16 = 0,
    source: Source = .none,
};

/// Logging tag used by every error path in this library.
const tag: [*:0]const u8 = "EPD_CAL";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Bench escape hatch, `-DRA8_BENCH_VCOM_MV=<millivolts>` in the C build and
/// `-Dbench-vcom-mv=<millivolts>` here.
///
/// It is off unless someone types the number on the command line, it is the
/// lowest-authority source so it can never override a real calibration, it is
/// range-checked like every other source, and it refuses to compile into a
/// production build. Every boot that uses it logs loudly, because a bench
/// build that quietly reached a customer's panel is the outcome being guarded
/// against.
const bench_vcom_mv: ?u16 = build_config.bench_vcom_mv;

comptime {
    if (bench_vcom_mv != null and build_config.production_build) {
        @compileError("RA8_BENCH_VCOM_MV must not ship -- provision the panel's real VCOM");
    }

    if (@sizeOf(CalError) != 2) @compileError("ra8_err_t width");
    if (@intFromEnum(CalError.ok) != 0) @compileError("k_ra8_ok value");
    if (@intFromEnum(CalError.invalid_arg) != 0x103) @compileError("k_ra8_err_invalid_arg value");
    if (@intFromEnum(CalError.invalid_state) != 0x104) @compileError("k_ra8_err_invalid_state value");
    if (@intFromEnum(CalError.invalid_size) != 0x105) @compileError("k_ra8_err_invalid_size value");
    if (@intFromEnum(CalError.not_found) != 0x106) @compileError("k_ra8_err_not_found value");
    if (@intFromEnum(CalError.not_supported) != 0x107) @compileError("k_ra8_err_not_supported value");
    if (@intFromEnum(CalError.hw_error) != 0x204) @compileError("k_ra8_err_hw_error value");
    if (@intFromEnum(CalError.crc_mismatch) != 0x405) @compileError("k_ra8_err_crc_mismatch value");
    if (@intFromEnum(CalError.validation_failed) != 0x501) @compileError("k_ra8_err_validation_failed value");
    if (@intFromEnum(CalError.range_check_failed) != 0x503) @compileError("k_ra8_err_range_check_failed value");
    if (@intFromEnum(CalError.null_ptr) != 0x504) @compileError("k_ra8_err_null_ptr value");

    if (@sizeOf(Source) != 1) @compileError("ra8_epd_cal_source_t width");
    if (@sizeOf(Result) != 4) @compileError("ra8_epd_cal_result_t size");
    if (@offsetOf(Result, "source") != 2) @compileError("ra8_epd_cal_result_t source offset");

    // Pointer-width aware, so the same asserts hold for the 64-bit host build
    // and the 32-bit Arm cross build.
    const word = @sizeOf(usize);
    if (@offsetOf(Config, "limits") != 0) @compileError("ra8_epd_cal_cfg_t limits offset");
    if (@offsetOf(Config, "panel") != word) @compileError("ra8_epd_cal_cfg_t panel offset");
    if (@offsetOf(Config, "store") != word * 4) @compileError("ra8_epd_cal_cfg_t store offset");
    if (@offsetOf(Config, "provisioned_mv") != word * 7) @compileError("ra8_epd_cal_cfg_t provisioned offset");
    if (@offsetOf(Config, "has_provisioned") != word * 7 + 2) @compileError("ra8_epd_cal_cfg_t has_provisioned offset");
    if (@sizeOf(PanelOps) != word * 3) @compileError("ra8_epd_cal_panel_ops_t size");
    if (@sizeOf(Store) != word * 3) @compileError("ra8_epd_cal_store_t size");
}

fn err(code: CalError) RawErr {
    return @intFromEnum(code);
}

fn configBoolsValid(config: *const Config) bool {
    return config.has_provisioned <= 1;
}

/// Read and validate the per-device record through the store seam.
///
/// Every failure mode, unbound seam through corrupt record, reports "no
/// record" to the resolver so it falls through to the next source. The
/// distinction is logged rather than returned, because the resolver's
/// behaviour is the same for all of them.
fn readRecord(store: *const Store, out_record: *Record) RawErr {
    const read = store.read orelse return err(.not_supported);

    var blob: [implementation.blob_size]u8 = @splat(0);
    const read_err = read(store.ctx, &blob, implementation.blob_size);
    if (read_err != err(.ok)) {
        ra8_log_emit_warn(tag, "calibration record read failed");
        return read_err;
    }

    const decoded = implementation.deserialize(&blob) catch |decode_err| switch (decode_err) {
        error.ShortBuffer => return err(.invalid_size),
        error.NotFound => return err(.not_found),
        error.UnsupportedSchema => {
            ra8_log_emit_error(tag, "deserialize: record schema newer than this build");
            return err(.not_supported);
        },
        error.ValidationFailed => return err(.validation_failed),
        error.CrcMismatch => {
            // Magic present but the body does not check out: the record was
            // written and then damaged. Worth shouting about, because it means
            // durable calibration was lost rather than never provisioned.
            ra8_log_emit_error(tag, "calibration record CRC mismatch -- record corrupt");
            return err(.crc_mismatch);
        },
    };

    out_record.* = decoded;
    return err(.ok);
}

/// Try the controller's own persisted VCOM (resolution source 1).
fn tryPanel(cfg: *const Config, out_result: *Result) bool {
    const get = cfg.panel.get orelse return false;

    var panel_mv: u16 = 0;
    if (get(cfg.panel.ctx, &panel_mv) != err(.ok)) return false;
    if (!implementation.vcomInRange(panel_mv, cfg.limits)) {
        ra8_log_emit_warn(tag, "controller VCOM out of range -- ignored");
        return false;
    }

    out_result.* = .{ .vcom_mv = panel_mv, .source = .panel };
    return true;
}

/// Try the per-device record in storage (resolution source 2).
fn tryRecord(cfg: *const Config, out_result: *Result) bool {
    var record: Record = .{};
    if (readRecord(&cfg.store, &record) != err(.ok)) return false;
    if (!implementation.vcomInRange(record.vcom_mv, cfg.limits)) {
        ra8_log_emit_warn(tag, "stored VCOM out of range -- ignored");
        return false;
    }

    out_result.* = .{ .vcom_mv = record.vcom_mv, .source = .record };
    return true;
}

/// `ra8_epd_cal_vcom_in_range`: the single decision every source's value
/// passes through.
pub export fn ra8_epd_cal_vcom_in_range(mv: u16, limits: ?*const Limits) callconv(.c) u8 {
    const window = limits orelse return 0;
    return @intFromBool(implementation.vcomInRange(mv, window.*));
}

/// `ra8_epd_cal_serialize`: pack a record, CRC trailer included.
pub export fn ra8_epd_cal_serialize(
    rec: ?*const Record,
    dst: ?[*]u8,
    dst_size: usize,
) callconv(.c) RawErr {
    const record = rec orelse {
        ra8_log_emit_error(tag, "serialize: rec null");
        return err(.null_ptr);
    };
    const destination = dst orelse {
        ra8_log_emit_error(tag, "serialize: dst null");
        return err(.null_ptr);
    };

    implementation.serialize(record.*, destination[0..dst_size]) catch |encode_err| switch (encode_err) {
        error.ShortBuffer => return err(.invalid_size),
        error.ZeroVcom => {
            ra8_log_emit_error(tag, "serialize: refusing a zero VCOM");
            return err(.invalid_arg);
        },
    };
    return err(.ok);
}

/// `ra8_epd_cal_deserialize`: decode and validate a serialised record.
pub export fn ra8_epd_cal_deserialize(
    src: ?[*]const u8,
    src_size: usize,
    out_rec: ?*Record,
) callconv(.c) RawErr {
    const source = src orelse {
        ra8_log_emit_error(tag, "deserialize: src null");
        return err(.null_ptr);
    };
    const out = out_rec orelse {
        ra8_log_emit_error(tag, "deserialize: out null");
        return err(.null_ptr);
    };

    const decoded = implementation.deserialize(source[0..src_size]) catch |decode_err| switch (decode_err) {
        error.ShortBuffer => return err(.invalid_size),
        // Blank or never-written storage lands here. Not an error worth
        // logging on every boot of an unprovisioned device.
        error.NotFound => return err(.not_found),
        error.UnsupportedSchema => {
            ra8_log_emit_error(tag, "deserialize: record schema newer than this build");
            return err(.not_supported);
        },
        error.ValidationFailed => return err(.validation_failed),
        error.CrcMismatch => return err(.crc_mismatch),
    };

    out.* = decoded;
    return err(.ok);
}

/// `ra8_epd_cal_resolve`: walk the sources in descending order of authority
/// and fail rather than guess.
pub export fn ra8_epd_cal_resolve(cfg: ?*const Config, out_result: ?*Result) callconv(.c) RawErr {
    const config = cfg orelse {
        ra8_log_emit_error(tag, "resolve: cfg null");
        return err(.null_ptr);
    };
    const result = out_result orelse {
        ra8_log_emit_error(tag, "resolve: out null");
        return err(.null_ptr);
    };

    if (!configBoolsValid(config)) return err(.invalid_arg);

    result.* = .{ .vcom_mv = 0, .source = .none };
    if (!implementation.limitsUsable(config.limits)) {
        ra8_log_emit_error(tag, "resolve: VCOM limits are zero or inverted");
        return err(.invalid_arg);
    }

    if (tryPanel(config, result)) return err(.ok);
    if (tryRecord(config, result)) return err(.ok);
    if (config.has_provisioned != 0 and implementation.vcomInRange(config.provisioned_mv, config.limits)) {
        result.* = .{ .vcom_mv = config.provisioned_mv, .source = .provisioned };
        return err(.ok);
    }

    if (bench_vcom_mv) |bench| {
        if (implementation.vcomInRange(bench, config.limits)) {
            ra8_log_emit_warn(tag, "USING BENCH VCOM FROM RA8_BENCH_VCOM_MV -- NOT FOR SHIPPING");
            result.* = .{ .vcom_mv = bench, .source = .bench };
            return err(.ok);
        }
        ra8_log_emit_error(tag, "RA8_BENCH_VCOM_MV is outside the panel's window -- ignored");
    }

    // Nothing trusted. Fail safe: the caller must leave the panel dark rather
    // than drive it at an invented bias.
    ra8_log_emit_error(tag, "no trusted VCOM -- refusing to drive the panel");
    return err(.not_found);
}

/// `ra8_epd_cal_apply`: drive the resolved VCOM and confirm it took.
pub export fn ra8_epd_cal_apply(cfg: ?*const Config, result: ?*const Result) callconv(.c) RawErr {
    const config = cfg orelse {
        ra8_log_emit_error(tag, "apply: cfg null");
        return err(.null_ptr);
    };
    const resolved = result orelse {
        ra8_log_emit_error(tag, "apply: result null");
        return err(.null_ptr);
    };

    if (!configBoolsValid(config)) return err(.invalid_arg);

    if (resolved.source == .none) return err(.invalid_state);
    const set = config.panel.set orelse return err(.not_supported);
    if (!implementation.vcomInRange(resolved.vcom_mv, config.limits)) {
        ra8_log_emit_error(tag, "apply: VCOM out of range at the point of use");
        return err(.range_check_failed);
    }

    const set_err = set(config.panel.ctx, resolved.vcom_mv);
    if (set_err != err(.ok)) return set_err;

    // Confirm through the read seam that the value is actually in effect. The
    // production `set` binding verifies its own write, but this module must
    // not assume that of an arbitrary injected seam, and an unconfirmable bias
    // is not one to leave on a panel.
    const get = config.panel.get orelse {
        ra8_log_emit_error(tag, "apply: no read seam -- VCOM cannot be confirmed");
        return err(.not_supported);
    };

    var readback: u16 = 0;
    if (get(config.panel.ctx, &readback) != err(.ok)) return err(.hw_error);
    // Equality alone, deliberately: `resolved.vcom_mv` was range-checked
    // above, so an equal readback is in range by construction. Range-checking
    // a value the panel reports matters where such a value is adopted rather
    // than confirmed, which is `tryPanel`, and it is checked there.
    if (readback != resolved.vcom_mv) {
        ra8_log_emit_error(tag, "apply: VCOM readback disagrees -- panel stays dark");
        return err(.validation_failed);
    }
    return err(.ok);
}

/// `ra8_epd_cal_provision`: make an operator-supplied VCOM durable.
pub export fn ra8_epd_cal_provision(cfg: ?*const Config, vcom_mv: u16) callconv(.c) RawErr {
    const config = cfg orelse {
        ra8_log_emit_error(tag, "provision: cfg null");
        return err(.null_ptr);
    };

    if (!configBoolsValid(config)) return err(.invalid_arg);

    const write = config.store.write orelse return err(.not_supported);
    if (!implementation.vcomInRange(vcom_mv, config.limits)) {
        ra8_log_emit_error(tag, "provision: VCOM outside the panel's window");
        return err(.range_check_failed);
    }

    const record: Record = .{ .vcom_mv = vcom_mv, .schema_version = implementation.schema_version };
    var blob: [implementation.blob_size]u8 = @splat(0);
    implementation.serialize(record, &blob) catch |encode_err| switch (encode_err) {
        error.ShortBuffer => return err(.invalid_size),
        error.ZeroVcom => {
            ra8_log_emit_error(tag, "serialize: refusing a zero VCOM");
            return err(.invalid_arg);
        },
    };

    return write(config.store.ctx, &blob, implementation.blob_size);
}
