//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_devcfg/inc/ra8_devcfg.h`. The record codec and
//! the validation rules live in `internal/root.zig`; this file owns the
//! injected store seam, the module cache, the argument guards and the
//! `ra8_err_t` mapping the C implementation exposed.
//!
//! No HAL header is reachable from here. The durable medium is touched only
//! through the two function pointers in `ra8_devcfg_store_t`, which is what
//! keeps the resolver and the commit path host-testable without MMIO. The
//! production extra-MRAM binding stays in its own translation unit.
//!
//! Guard order is part of the contract: the null-argument vectors in
//! `tests/misc/src/test_ra8_devcfg.c` tell `null_ptr` from `not_initialized`
//! by which check fires first, so the checks below run in exactly the order
//! the C wrote them.

const implementation = @import("internal/root.zig");

/// Decoded per-unit payload (`ra8_devcfg_body_t`).
pub const Body = implementation.Body;
/// Decoded record: body plus header discriminators (`ra8_devcfg_record_t`).
pub const Record = implementation.Record;

/// Per-record status flags (`ra8_devcfg_flags_t`).
pub const flag_provisioned = implementation.flag_provisioned;
pub const flag_vcom_valid = implementation.flag_vcom_valid;
pub const flag_touch_valid = implementation.flag_touch_valid;

/// Subset of `ra8_err_t` this library returns.
pub const CfgError = enum(u16) {
    ok = 0,
    not_initialized = 0x10F,
    validation_failed = 0x501,
    null_ptr = 0x504,
};

/// Raw `ra8_err_t` as it crosses the ABI, so a store seam may hand back any
/// code in the repo's error space and have it propagate unchanged.
pub const RawErr = u16;

/// Backing-store read seam (`ra8_devcfg_read_fn_t`).
pub const ReadFn = *const fn (offset: u32, dst: [*]u8, len: u32) callconv(.c) RawErr;
/// Backing-store write seam (`ra8_devcfg_write_fn_t`).
pub const WriteFn = *const fn (offset: u32, src: [*]const u8, len: u32) callconv(.c) RawErr;

/// Dependency-injection vtable for the record backing store
/// (`ra8_devcfg_store_t`).
pub const Store = extern struct {
    read: ?ReadFn = null,
    write: ?WriteFn = null,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

const tag: [*:0]const u8 = "DEVCFG";

comptime {
    if (@sizeOf(CfgError) != 2) @compileError("ra8_err_t width");
    if (@intFromEnum(CfgError.ok) != 0) @compileError("k_ra8_ok value");
    if (@intFromEnum(CfgError.not_initialized) != 0x10F) @compileError("k_ra8_err_not_initialized");
    if (@intFromEnum(CfgError.validation_failed) != 0x501) @compileError("k_ra8_err_validation");
    if (@intFromEnum(CfgError.null_ptr) != 0x504) @compileError("k_ra8_err_null_ptr");

    // Pointer-width aware, so the same asserts hold for the 64-bit host build
    // and the 32-bit Arm cross build.
    const word = @sizeOf(usize);
    if (@offsetOf(Store, "write") != word) @compileError("store write offset");
    if (@sizeOf(Store) != word * 2) @compileError("store size");
}

/// Module cache state established by `ra8_devcfg_load` (`ra8_devcfg_state_t`).
const State = enum(u8) {
    unloaded = 0,
    loaded = 1,
    unprovisioned = 2,
};

var s_state: State = .unloaded;
var s_record: Record = .{};

fn raw(err: CfgError) RawErr {
    return @intFromEnum(err);
}

/// Read one copy through the store and validate it. A read fault or blank
/// window is "copy absent", so the resolver falls through rather than failing
/// the whole load; it never fabricates a record.
fn probe(read: ReadFn, offset: u32, out: *Record) bool {
    var buf: [implementation.record_len]u8 = @splat(0);
    if (read(offset, &buf, implementation.record_len) != raw(.ok)) {
        return false;
    }
    if (!implementation.copyValid(&buf)) {
        return false;
    }
    implementation.deserialize(&buf, out);
    return true;
}

/// Load and resolve the device configuration record from both copies.
pub export fn ra8_devcfg_load(store: ?*const Store) callconv(.c) RawErr {
    const s = store orelse {
        ra8_log_emit_error(tag, "load: store null");
        return raw(.null_ptr);
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "load: store->read null");
        return raw(.null_ptr);
    };

    var rec0: Record = .{};
    var rec1: Record = .{};
    const valid0 = probe(read, implementation.copy0_off, &rec0);
    const valid1 = probe(read, implementation.copy1_off, &rec1);

    if (valid0 and valid1) {
        // Both survive -- the higher sequence is the newer record; a tie takes 0.
        s_record = if (rec0.seq >= rec1.seq) rec0 else rec1;
    } else if (valid0) {
        s_record = rec0;
    } else if (valid1) {
        s_record = rec1;
    } else {
        s_state = .unprovisioned;
        ra8_log_emit_warn(tag, "no valid device config -- UNPROVISIONED");
        return raw(.validation_failed);
    }
    s_state = .loaded;
    return raw(.ok);
}

/// Fetch the validated panel VCOM magnitude in millivolts. Any error means
/// INV-VCOM-1 forbids driving the panel.
pub export fn ra8_devcfg_get_vcom_mv(out_mv: ?*u16) callconv(.c) RawErr {
    const out = out_mv orelse {
        ra8_log_emit_error(tag, "get_vcom: out_mv null");
        return raw(.null_ptr);
    };
    if (s_state != .loaded) {
        return raw(.not_initialized);
    }
    if ((s_record.flags & implementation.flag_vcom_valid) == 0) {
        ra8_log_emit_error(tag, "VCOM not marked valid -- refuse the panel");
        return raw(.validation_failed);
    }
    const mv = s_record.body.panel_vcom_mv;
    if (!implementation.vcomInRange(mv)) {
        ra8_log_emit_error(tag, "VCOM out of plausible range -- refuse the panel");
        return raw(.validation_failed);
    }
    out.* = mv;
    return raw(.ok);
}

/// Expose the decoded body of the loaded record. The pointer aliases the
/// module cache and stays valid until the next load or reset.
pub export fn ra8_devcfg_get_body(out_body: ?*?*const Body) callconv(.c) RawErr {
    const out = out_body orelse {
        ra8_log_emit_error(tag, "get_body: out_body null");
        return raw(.null_ptr);
    };
    if (s_state != .loaded) {
        return raw(.not_initialized);
    }
    out.* = &s_record.body;
    return raw(.ok);
}

/// Report whether neither record copy is valid: the provisioning gate. True
/// before any load has run, too.
pub export fn ra8_devcfg_is_blank() callconv(.c) bool {
    return s_state != .loaded;
}

/// Commit a new record to the stale copy slot with header-last ordering, so a
/// power cut before the header lands leaves that slot header-invalid and the
/// other copy the sole survivor.
pub export fn ra8_devcfg_commit(store: ?*const Store, rec: ?*const Record) callconv(.c) RawErr {
    const s = store orelse {
        ra8_log_emit_error(tag, "commit: store null");
        return raw(.null_ptr);
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "commit: store->read null");
        return raw(.null_ptr);
    };
    const write = s.write orelse {
        ra8_log_emit_error(tag, "commit: store->write null");
        return raw(.null_ptr);
    };
    const record = rec orelse {
        ra8_log_emit_error(tag, "commit: rec null");
        return raw(.null_ptr);
    };

    var cur0: Record = .{};
    var cur1: Record = .{};
    const valid0 = probe(read, implementation.copy0_off, &cur0);
    const valid1 = probe(read, implementation.copy1_off, &cur1);
    const seq0: u32 = if (valid0) cur0.seq else 0;
    const seq1: u32 = if (valid1) cur1.seq else 0;
    const new_seq = implementation.nextSeq(valid0, seq0, valid1, seq1);
    const target = implementation.targetOffset(valid0, seq0, valid1, seq1);

    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(record, new_seq, &buf);

    const body_err = write(
        target + implementation.hdr_bytes,
        buf[implementation.hdr_bytes..].ptr,
        implementation.body_bytes,
    );
    if (body_err != raw(.ok)) {
        ra8_log_emit_error(tag, "commit: body write failed");
        ra8_log_emit_error_val(tag, "Error", body_err);
        return body_err;
    }
    const hdr_err = write(target, &buf, implementation.hdr_bytes);
    if (hdr_err != raw(.ok)) {
        ra8_log_emit_error(tag, "commit: header write failed");
        ra8_log_emit_error_val(tag, "Error", hdr_err);
        return hdr_err;
    }
    return raw(.ok);
}

/// Drop the cached record so a later load re-resolves from scratch. Touches no
/// backing store.
pub export fn ra8_devcfg_reset() callconv(.c) void {
    s_state = .unloaded;
    s_record = .{};
}

comptime {
    // The production extra-MRAM binding keeps its own file, as its own
    // translation unit did. Reference it here so the archive still exports
    // `ra8_devcfg_default_store` alongside the membrane above.
    _ = @import("store_extra_mram.zig");
}
