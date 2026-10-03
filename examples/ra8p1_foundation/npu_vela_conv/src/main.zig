//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! npu_vela_conv (RA8FW-416). Runs the real Vela-compiled conv_int8 model on
//! the RA8P1 Ethos-U55 through the firmware's own loader and driver: loads the
//! distilled .npub, copies the golden input to where the stream reads it, submits,
//! runs, waits, and prints PASS only when all 256 output bytes equal the TFLM
//! golden. The board and HAL C layers are reached through extern.

const model = @import("model");
const golden = @import("golden");

pub const baud: u32 = 115_200;
/// Runtime arena: scratch (512) + input (256) + output (256), 16-aligned.
pub const arena_bytes: u32 = 1024;
/// Where the command stream reads the input and writes the output: Vela's
/// offline allocation puts both inside the scratch region (RA8FW-487).
pub const input_region: usize = model.input_region;
pub const input_offset: usize = model.input_offset;
pub const output_region: usize = model.output_region;
pub const output_offset: usize = model.output_offset;
pub const region_slots: usize = 8;
pub const pass_line = "npu_vela_conv: PASS\r\n";
pub const fail_line = "npu_vela_conv: FAIL\r\n";

/// Mirrors ra8_npu_job_t (libs/ra8_hal/inc/ra8_npu.h).
pub const Job = extern struct {
    cmd_stream: ?*const anyopaque = null,
    cmd_stream_bytes: u32 = 0,
    region_count: u8 = 0,
    region_base: [region_slots]u64 = [_]u64{0} ** region_slots,
};

/// Mirrors ra8_npu_arena_t (libs/ra8_hal/inc/ra8_npu_loader.h).
pub const Arena = extern struct {
    base: [*]u8,
    bytes: u32,
};

const ok: c_int = 0;
extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_npu_init() c_int;
extern fn ra8_npu_arena_bytes(blob: *const anyopaque, blob_bytes: u32, out: *u32) c_int;
extern fn ra8_npu_load(blob: *const anyopaque, blob_bytes: u32, arena: *const Arena, job: *Job) c_int;
extern fn ra8_npu_submit(job: *const Job) c_int;
extern fn ra8_npu_run() c_int;
extern fn ra8_npu_wait() c_int;

const blob: [model.bytes.len]u8 align(16) = model.bytes;
var arena_buf: [arena_bytes]u8 align(16) = undefined;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

fn region(job: *const Job, slot: usize) [*]u8 {
    return @ptrFromInt(@as(usize, @intCast(job.region_base[slot])));
}

/// Loads the blob into the arena and fills the input region with the golden input.
fn load(job: *Job) bool {
    var need: u32 = 0;
    if (ra8_npu_arena_bytes(&blob, blob.len, &need) != ok or need > arena_bytes) return false;
    const arena = Arena{ .base = &arena_buf, .bytes = arena_bytes };
    if (ra8_npu_load(&blob, blob.len, &arena, job) != ok) return false;
    if (job.region_count <= output_region) return false;
    const input = region(job, input_region) + input_offset;
    for (golden.input, 0..) |v, i| input[i] = @bitCast(v);
    return true;
}

fn execute(job: *const Job) bool {
    if (ra8_npu_submit(job) != ok) return false;
    if (ra8_npu_run() != ok) return false;
    return ra8_npu_wait() == ok;
}

/// True when every output byte equals the TFLM golden.
pub fn matches(output: []const u8) bool {
    if (output.len != golden.output.len) return false;
    for (golden.output, output) |want, got| {
        if (@as(u8, @bitCast(want)) != got) return false;
    }
    return true;
}

fn runModel() bool {
    if (ra8_npu_init() != ok) return false;
    var job = Job{};
    if (!load(&job)) return false;
    if (!execute(&job)) return false;
    const output = region(&job, output_region) + output_offset;
    return matches(output[0..golden.output.len]);
}

export fn main() callconv(.c) c_int {
    if (ra8_cgc_init() != ok) park();
    if (ra8_board_uart_console_init(baud) != ok) park();
    say(if (runModel()) pass_line else fail_line);
    park();
}
