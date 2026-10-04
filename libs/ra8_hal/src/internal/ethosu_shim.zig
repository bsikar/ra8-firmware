//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Arm ethos-u-core-driver adapter logic over ra8_npu (RA8FW-573): the
//! ethosu_invoke_v3 argument checks and the ra8_npu_job_t it submits.
//! The C exports live in ethosu_shim_abi.zig.

/// `k_ra8_npu_region_count`: BASEPn region pairs in one job.
pub const max_regions: usize = 8;

/// Mirror of `ra8_npu_job_t` (ra8_npu.h).
pub const Job = extern struct {
    cmd_stream: ?*const anyopaque = null,
    cmd_stream_bytes: u32 = 0,
    region_count: u8 = 0,
    region_base: [max_regions]u64 = [_]u64{0} ** max_regions,
};

/// `struct ethosu_driver`, opaque to C callers.
pub const Driver = extern struct {
    reserved: bool = false,
};

/// The ethosu_invoke_v3 arguments the adapter looks at.
pub const Invoke = struct {
    drv: ?*const Driver,
    cmd: ?*const anyopaque,
    size: c_int,
    bases: ?[*]const u64,
    count: c_int,
};

pub const ArgError = error{
    NullDriver,
    NullStream,
    EmptyStream,
    NegativeCount,
    TooManyRegions,
    NullBases,
};

/// Rejects what the C adapter rejected, in the same order. The region
/// bound comes before any copy so a large count never overruns the job.
pub fn check(a: Invoke) ArgError!void {
    if (a.drv == null) return error.NullDriver;
    if (a.cmd == null) return error.NullStream;
    if (a.size <= 0) return error.EmptyStream;
    if (a.count < 0) return error.NegativeCount;
    if (@as(u32, @intCast(a.count)) > max_regions) return error.TooManyRegions;
    if (a.count > 0 and a.bases == null) return error.NullBases;
}

/// The log line the C adapter wrote for each rejection.
pub fn message(err: ArgError) [*:0]const u8 {
    return switch (err) {
        error.NullDriver => "invoke: null driver",
        error.NullStream => "invoke: null command stream",
        error.EmptyStream => "invoke: empty command stream",
        error.NegativeCount => "invoke: negative region count",
        error.TooManyRegions => "invoke: too many regions",
        error.NullBases => "invoke: null base_addr array",
    };
}

/// Builds the job for arguments that passed `check`.
pub fn buildJob(a: Invoke) Job {
    var job = Job{
        .cmd_stream = a.cmd,
        .cmd_stream_bytes = @intCast(a.size),
        .region_count = @intCast(a.count),
    };
    if (a.bases) |bases| {
        for (job.region_base[0..job.region_count], 0..) |*slot, i| slot.* = bases[i];
    }
    return job;
}
