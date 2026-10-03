//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_npu_{de,}quantize_{i8,u8} (internal/npu_quant.zig,
//! RA8FW-543). Built as its own object in libra8_hal.a (RA8FW-542) so an
//! image links only the units it calls.

const common = @import("abi_common.zig");
const npu_quant = @import("internal/npu_quant.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_error = common.ra8_log_emit_error;

const tag = "NPUQ";

fn quantizeAbi(comptime T: type, comptime name: []const u8, in: ?[*]const f32, out: ?[*]T, count: usize, scale: f32, zero_point: i32) u16 {
    const src = in orelse {
        ra8_log_emit_error(tag, name ++ ": in must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    const dst = out orelse {
        ra8_log_emit_error(tag, name ++ ": out must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    npu_quant.quantize(T, src[0..count], dst[0..count], scale, zero_point) catch {
        ra8_log_emit_error(tag, name ++ ": scale must be > 0");
        return k_ra8_err_invalid_arg;
    };
    return k_ra8_ok;
}

fn dequantizeAbi(comptime T: type, comptime name: []const u8, in: ?[*]const T, out: ?[*]f32, count: usize, scale: f32, zero_point: i32) u16 {
    const src = in orelse {
        ra8_log_emit_error(tag, name ++ ": in must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    const dst = out orelse {
        ra8_log_emit_error(tag, name ++ ": out must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    npu_quant.dequantize(T, src[0..count], dst[0..count], scale, zero_point);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_npu_quantize_i8(const float*, int8_t*, size_t, float, int32_t)`.
export fn ra8_npu_quantize_i8(in: ?[*]const f32, out: ?[*]i8, count: usize, scale: f32, zero_point: i32) u16 {
    return quantizeAbi(i8, "quantize_i8", in, out, count, scale, zero_point);
}

/// `ra8_err_t ra8_npu_dequantize_i8(const int8_t*, float*, size_t, float, int32_t)`.
export fn ra8_npu_dequantize_i8(in: ?[*]const i8, out: ?[*]f32, count: usize, scale: f32, zero_point: i32) u16 {
    return dequantizeAbi(i8, "dequantize_i8", in, out, count, scale, zero_point);
}

/// `ra8_err_t ra8_npu_quantize_u8(const float*, uint8_t*, size_t, float, int32_t)`.
export fn ra8_npu_quantize_u8(in: ?[*]const f32, out: ?[*]u8, count: usize, scale: f32, zero_point: i32) u16 {
    return quantizeAbi(u8, "quantize_u8", in, out, count, scale, zero_point);
}

/// `ra8_err_t ra8_npu_dequantize_u8(const uint8_t*, float*, size_t, float, int32_t)`.
export fn ra8_npu_dequantize_u8(in: ?[*]const u8, out: ?[*]f32, count: usize, scale: f32, zero_point: i32) u16 {
    return dequantizeAbi(u8, "dequantize_u8", in, out, count, scale, zero_point);
}
