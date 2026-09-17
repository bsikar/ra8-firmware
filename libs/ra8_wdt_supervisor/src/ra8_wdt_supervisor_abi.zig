//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_wdt_supervisor`: the nine symbols declared by the
//! unchanged `inc/ra8_wdt_supervisor.h`, the singleton module state, and the
//! ThreadX seam.
//!
//! The kernel seam is the only part that varies by target. The deleted C kept a
//! private header full of `static inline` ThreadX stand-ins for the host unit
//! build (`RA8_OFF_TARGET`) and pulled the real `tx_api.h` on silicon; this file
//! carries both halves behind the `off-target` build option, which defaults from
//! the target so CMake passes nothing.

const std = @import("std");
const core = @import("internal/root.zig");
const build_config = @import("build_config");

/// True on the host unit build, where ThreadX is not linked.
const off_target = build_config.off_target;

/// `ra8_err_t` values this module returns. The enum is 16-bit in `ra8_err.h`.
const Err = struct {
    const ok: u16 = 0x000;
    const no_mem: u16 = 0x102;
    const invalid_arg: u16 = 0x103;
    const not_found: u16 = 0x106;
    const busy: u16 = 0x109;
    const not_initialized: u16 = 0x10F;
    const rtos_error: u16 = 0x301;
    const null_ptr: u16 = 0x504;
};

/// Mirror of `ra8_wdt_sup_cfg_t`.
pub const Cfg = extern struct {
    stack: ?*anyopaque,
    stack_size_bytes: u32,
    priority: u32,
    refresh_period_ms: u32,
};

/// Mirror of `ra8_wdt_sup_now_fn_t`.
pub const NowFn = *const fn () callconv(.c) u32;
/// Mirror of `ra8_wdt_sup_refresh_fn_t`.
pub const RefreshFn = *const fn () callconv(.c) void;

comptime {
    const word = @sizeOf(usize);
    std.debug.assert(@offsetOf(Cfg, "stack") == 0);
    std.debug.assert(@offsetOf(Cfg, "stack_size_bytes") == word);
    std.debug.assert(@offsetOf(Cfg, "priority") == word + 4);
    std.debug.assert(@offsetOf(Cfg, "refresh_period_ms") == word + 8);
    std.debug.assert(@alignOf(Cfg) == word);
}

/// The WDT kick the default refresh hook wraps. Declared on every target, as
/// the C declared it: the host unit image already supplies this symbol.
extern fn ra8_wdt_refresh_deferred() void;

/// ThreadX seam.
///
/// On silicon the vendored `tx_api.h` maps every public `tx_*` name onto an
/// error-checking `_txe_*` entry point that takes the control-block size as a
/// trailing argument and rejects a mismatch, so the sizes below have to be
/// exact rather than a generous reserve. They were measured against the
/// vendored ThreadX cortex_m85 GNU port with arm-none-eabi-gcc 13.3:
/// `sizeof(TX_MUTEX) == 52`, `sizeof(TX_THREAD) == 176`. A ThreadX config
/// change that resizes either control block means re-measuring them here.
const tx = struct {
    const success: c_uint = 0;
    const no_inherit: c_uint = 0;
    const wait_forever: c_ulong = 0xFFFFFFFF;
    const auto_start: c_uint = 1;
    const no_time_slice: c_ulong = 0;

    const mutex_block_bytes: usize = 52;
    const thread_block_bytes: usize = 176;

    /// Canaries the deleted host shim stamped into its stand-in blocks, kept so
    /// a stray pointer pun between the two is still detectable.
    const mutex_canary: u32 = 0xA5A5A5A5;
    const thread_canary: u32 = 0x5A5A5A5A;

    const object_name: [*:0]const u8 = "ra8_wdt_sup";

    pub const MutexBlock = if (off_target)
        extern struct { magic: u32 = 0 }
    else
        extern struct { words: [mutex_block_bytes / 4]u32 = [_]u32{0} ** (mutex_block_bytes / 4) };

    pub const ThreadBlock = if (off_target)
        extern struct { magic: u32 = 0 }
    else
        extern struct { words: [thread_block_bytes / 4]u32 = [_]u32{0} ** (thread_block_bytes / 4) };

    extern fn _txe_mutex_create(mutex: *anyopaque, name: [*:0]u8, inherit: c_uint, block_size: c_uint) c_uint;
    extern fn _txe_mutex_get(mutex: *anyopaque, wait_option: c_ulong) c_uint;
    extern fn _txe_mutex_put(mutex: *anyopaque) c_uint;
    extern fn _txe_mutex_delete(mutex: *anyopaque) c_uint;
    extern fn _txe_thread_create(
        thread: *anyopaque,
        name: [*:0]u8,
        entry: *const fn (c_ulong) callconv(.c) void,
        entry_input: c_ulong,
        stack_start: ?*anyopaque,
        stack_size: c_ulong,
        priority: c_uint,
        preempt_threshold: c_uint,
        time_slice: c_ulong,
        auto_start_flag: c_uint,
        block_size: c_uint,
    ) c_uint;
    extern fn _txe_thread_terminate(thread: *anyopaque) c_uint;
    extern fn _txe_thread_delete(thread: *anyopaque) c_uint;
    extern fn _tx_thread_sleep(timer_ticks: c_ulong) c_uint;
    extern fn _tx_time_get() c_ulong;

    pub fn mutexCreate(mutex: *MutexBlock) c_uint {
        if (off_target) {
            mutex.magic = mutex_canary;
            return success;
        }
        return _txe_mutex_create(@ptrCast(mutex), @constCast(object_name), no_inherit, @intCast(mutex_block_bytes));
    }

    pub fn mutexGet(mutex: *MutexBlock) c_uint {
        if (off_target) return success;
        return _txe_mutex_get(@ptrCast(mutex), wait_forever);
    }

    pub fn mutexPut(mutex: *MutexBlock) c_uint {
        if (off_target) return success;
        return _txe_mutex_put(@ptrCast(mutex));
    }

    pub fn mutexDelete(mutex: *MutexBlock) c_uint {
        if (off_target) return success;
        return _txe_mutex_delete(@ptrCast(mutex));
    }

    pub fn threadCreate(
        thread: *ThreadBlock,
        entry: *const fn (c_ulong) callconv(.c) void,
        stack: ?*anyopaque,
        stack_size_bytes: u32,
        priority: u32,
    ) c_uint {
        if (off_target) {
            thread.magic = thread_canary;
            return success;
        }
        return _txe_thread_create(
            @ptrCast(thread),
            @constCast(object_name),
            entry,
            0,
            stack,
            @intCast(stack_size_bytes),
            @intCast(priority),
            @intCast(priority),
            no_time_slice,
            auto_start,
            @intCast(thread_block_bytes),
        );
    }

    pub fn threadTerminate(thread: *ThreadBlock) c_uint {
        if (off_target) return success;
        return _txe_thread_terminate(@ptrCast(thread));
    }

    pub fn threadDelete(thread: *ThreadBlock) c_uint {
        if (off_target) return success;
        return _txe_thread_delete(@ptrCast(thread));
    }

    pub fn threadSleep(ticks: u32) c_uint {
        if (off_target) return success;
        return _tx_thread_sleep(@intCast(ticks));
    }

    pub fn timeGet() u32 {
        if (off_target) return 0;
        return @truncate(@as(u64, _tx_time_get()));
    }
};

/// Singleton module state, the Zig counterpart of the C's `static s_state`.
const State = struct {
    initialized: bool = false,
    started: bool = false,
    cfg: Cfg = .{ .stack = null, .stack_size_bytes = 0, .priority = 0, .refresh_period_ms = 0 },
    registry: core.Registry = .{},
    mutex: tx.MutexBlock = .{},
    thread: tx.ThreadBlock = .{},
    now: ?NowFn = null,
    refresh: ?RefreshFn = null,
};

var state: State = .{};

/// Default monotonic-time hook: `tx_time_get` scaled by the kernel tick.
fn defaultNow() callconv(.c) u32 {
    return tx.timeGet() *% core.default_tick_ms;
}

/// Default WDT-refresh hook.
fn defaultRefresh() callconv(.c) void {
    ra8_wdt_refresh_deferred();
}

/// Read the monotonic clock through the installed hook.
///
/// The C dereferenced `s_state.now` unconditionally on every post-init path,
/// where init has always installed a hook. Falling back to the default instead
/// of trapping is a hardening on a path that is unreachable through the public
/// API.
fn nowMs() u32 {
    const hook = state.now orelse defaultNow;
    return hook();
}

/// The supervisor thread body: tick, then sleep one refresh period.
fn threadEntry(arg: c_ulong) callconv(.c) void {
    _ = arg;
    while (true) {
        var refreshed: bool = false;
        _ = ra8_wdt_supervisor_tick(&refreshed);
        _ = tx.threadSleep(state.cfg.refresh_period_ms);
    }
}

pub export fn ra8_wdt_supervisor_init(cfg: ?*const Cfg) callconv(.c) u16 {
    const view: ?core.CfgView = if (cfg) |block| .{
        .has_stack = block.stack != null,
        .stack_size_bytes = block.stack_size_bytes,
        .priority = block.priority,
        .refresh_period_ms = block.refresh_period_ms,
    } else null;
    if (core.validateCfg(view)) |fault| {
        return switch (fault) {
            .null_ptr => Err.null_ptr,
            .invalid_arg => Err.invalid_arg,
        };
    }
    if (state.initialized) return Err.busy;

    state.registry.clear();
    state.cfg = cfg.?.*;
    state.now = defaultNow;
    state.refresh = defaultRefresh;
    state.started = false;

    if (tx.mutexCreate(&state.mutex) != tx.success) return Err.rtos_error;

    state.initialized = true;
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_deinit() callconv(.c) u16 {
    if (state.initialized) {
        if (state.started) {
            _ = tx.threadTerminate(&state.thread);
            _ = tx.threadDelete(&state.thread);
        }
        _ = tx.mutexDelete(&state.mutex);
    }
    state = .{};
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_register_thread(
    name: ?[*:0]const u8,
    deadline_ms: u32,
    out_handle: ?*u8,
) callconv(.c) u16 {
    const handle_out = out_handle orelse return Err.null_ptr;
    handle_out.* = core.handle_invalid;

    const name_ptr = name orelse return Err.null_ptr;
    if (deadline_ms == 0) return Err.invalid_arg;
    if (!state.initialized) return Err.not_initialized;

    if (tx.mutexGet(&state.mutex) != tx.success) return Err.rtos_error;

    var result: u16 = Err.no_mem;
    if (state.registry.findFree()) |idx| {
        state.registry.fill(idx, name_ptr, deadline_ms, nowMs());
        handle_out.* = idx;
        result = Err.ok;
    }

    _ = tx.mutexPut(&state.mutex);
    return result;
}

pub export fn ra8_wdt_supervisor_checkin(handle: u8) callconv(.c) u16 {
    if (handle >= core.max_threads) return Err.invalid_arg;
    if (!state.initialized) return Err.not_initialized;

    if (tx.mutexGet(&state.mutex) != tx.success) return Err.rtos_error;

    var result: u16 = Err.not_found;
    if (state.registry.isRegistered(handle)) {
        state.registry.slots[handle].last_checkin_ms = nowMs();
        result = Err.ok;
    }

    _ = tx.mutexPut(&state.mutex);
    return result;
}

pub export fn ra8_wdt_supervisor_start() callconv(.c) u16 {
    if (!state.initialized) return Err.not_initialized;
    if (state.started) return Err.busy;

    const rc = tx.threadCreate(
        &state.thread,
        threadEntry,
        state.cfg.stack,
        state.cfg.stack_size_bytes,
        state.cfg.priority,
    );
    if (rc != tx.success) return Err.rtos_error;

    state.started = true;
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_tick(out_did_refresh: ?*bool) callconv(.c) u16 {
    if (!state.initialized) {
        if (out_did_refresh) |out| out.* = false;
        return Err.not_initialized;
    }

    if (tx.mutexGet(&state.mutex) != tx.success) {
        if (out_did_refresh) |out| out.* = false;
        return Err.rtos_error;
    }

    const verdict = state.registry.verdict(nowMs());

    _ = tx.mutexPut(&state.mutex);

    const will_refresh = verdict.willRefresh();
    if (will_refresh) {
        if (state.refresh) |hook| hook();
    }
    if (out_did_refresh) |out| out.* = will_refresh;
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_set_now_hook(now: ?NowFn) callconv(.c) u16 {
    state.now = now orelse defaultNow;
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_set_refresh_hook(refresh: ?RefreshFn) callconv(.c) u16 {
    state.refresh = refresh orelse defaultRefresh;
    return Err.ok;
}

pub export fn ra8_wdt_supervisor_thread_count() callconv(.c) u8 {
    return state.registry.used();
}
