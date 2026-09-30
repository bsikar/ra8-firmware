//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Short ASCII names for `ra8_err_t` codes.
//!
//! This lived in ra8_log.c because the log backend is the only caller. The
//! table is the authority for the spellings that reach a log line, so the
//! codes are written out rather than imported: a renumbered `ra8_err.h` must
//! show up as a test failure here, not as a silently relabelled line.

/// One code and the name a log line prints for it.
pub const Entry = struct {
    code: c_int,
    name: [:0]const u8,
};

/// What a code with no entry prints as.
pub const unknown: [:0]const u8 = "unknown";

/// Every named code, in `ra8_err.h` order.
pub const table = [_]Entry{
    .{ .code = 0x000, .name = "ok" },
    .{ .code = 0x101, .name = "fail" },
    .{ .code = 0x102, .name = "no_mem" },
    .{ .code = 0x103, .name = "invalid_arg" },
    .{ .code = 0x104, .name = "invalid_state" },
    .{ .code = 0x105, .name = "invalid_size" },
    .{ .code = 0x106, .name = "not_found" },
    .{ .code = 0x107, .name = "not_supported" },
    .{ .code = 0x108, .name = "timeout" },
    .{ .code = 0x109, .name = "busy" },
    .{ .code = 0x10a, .name = "no_data" },
    .{ .code = 0x10b, .name = "would_block" },
    .{ .code = 0x10c, .name = "exists" },
    .{ .code = 0x10d, .name = "empty" },
    .{ .code = 0x10e, .name = "cancelled" },
    .{ .code = 0x10f, .name = "not_initialized" },
    .{ .code = 0x110, .name = "estop" },
    .{ .code = 0x111, .name = "not_empty" },
    .{ .code = 0x112, .name = "access_denied" },
    .{ .code = 0x201, .name = "hw_init_failed" },
    .{ .code = 0x202, .name = "hw_not_ready" },
    .{ .code = 0x203, .name = "hw_timeout" },
    .{ .code = 0x204, .name = "hw_error" },
    .{ .code = 0x205, .name = "gpio_conflict" },
    .{ .code = 0x206, .name = "gpio_invalid_port" },
    .{ .code = 0x207, .name = "gpio_invalid_pin" },
    .{ .code = 0x208, .name = "out_of_range" },
    .{ .code = 0x209, .name = "hw_unmapped" },
    .{ .code = 0x301, .name = "rtos_error" },
    .{ .code = 0x302, .name = "rtos_thread_create" },
    .{ .code = 0x303, .name = "rtos_semaphore" },
    .{ .code = 0x304, .name = "rtos_mutex" },
    .{ .code = 0x305, .name = "rtos_queue" },
    .{ .code = 0x306, .name = "rtos_timer" },
    .{ .code = 0x401, .name = "comm_error" },
    .{ .code = 0x402, .name = "spi_error" },
    .{ .code = 0x403, .name = "uart_error" },
    .{ .code = 0x404, .name = "i2c_error" },
    .{ .code = 0x405, .name = "crc_mismatch" },
    .{ .code = 0x406, .name = "protocol_error" },
    .{ .code = 0x407, .name = "nack" },
    .{ .code = 0x408, .name = "conflict" },
    .{ .code = 0x409, .name = "retry_limit" },
    .{ .code = 0x501, .name = "validation_failed" },
    .{ .code = 0x502, .name = "checksum_mismatch" },
    .{ .code = 0x503, .name = "range_check_failed" },
    .{ .code = 0x504, .name = "null_ptr" },
    .{ .code = 0x505, .name = "decomp_output_cap" },
    .{ .code = 0x506, .name = "decomp_ratio" },
    .{ .code = 0x507, .name = "decomp_entries" },
    .{ .code = 0x508, .name = "decomp_depth" },
    .{ .code = 0x509, .name = "decomp_iterations" },
};

/// The name for `code`, or `unknown`.
pub fn lookup(code: c_int) [:0]const u8 {
    for (table) |entry| {
        if (entry.code == code) return entry.name;
    }
    return unknown;
}
