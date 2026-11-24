//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_dfu_boot_state_*`: torn-write-safe dual-copy
//! persistence of the trial boot-state record in dedicated code-MRAM pages
//! (`0x020FE000`, `0x020FE020`) on target and an equivalent RAM shadow off target.

const builtin = @import("builtin");
const std = @import("std");
const boot_state = @import("boot_state");
const err = @import("err");
const mram = @import("mram_port");
const slot = @import("slot");

const off_target = builtin.target.os.tag != .freestanding;

/// C-compatible layout for `ra8_dfu_boot_plan_t` in `ra8_dfu.h`.
const CBootPlan = extern struct {
    action: slot.Action,
    is_trial: bool,
    next_tries: u8,
    dfu_target: slot.Slot,
};

comptime {
    std.debug.assert(@sizeOf(CBootPlan) == 4);
}

/// Host RAM shadow backing the two 32-byte ping-pong pages.
var shadow_copies: [boot_state.layout.copy_count][@sizeOf(boot_state.Record)]u8 align(@alignOf(boot_state.Record)) =
    @splat(@splat(boot_state.layout.erased_byte));

const ShadowFlash = struct {
    pub fn programPage(_: ShadowFlash, addr: u32, erased: []const u8, body: []const u8) error{Flash}!void {
        const idx: usize = switch (addr) {
            boot_state.layout.copy0_addr => 0,
            boot_state.layout.copy1_addr => 1,
            else => return error.Flash,
        };
        @memcpy(&shadow_copies[idx], erased);
        @memcpy(&shadow_copies[idx], body);
    }
};

const CopyPair = struct {
    copy0: boot_state.Record,
    copy1: boot_state.Record,
};

fn readCopies() CopyPair {
    if (off_target) {
        const c0: *const boot_state.Record = @ptrCast(&shadow_copies[0]);
        const c1: *const boot_state.Record = @ptrCast(&shadow_copies[1]);
        return .{ .copy0 = c0.*, .copy1 = c1.* };
    }
    const c0: *const volatile boot_state.Record = @ptrFromInt(boot_state.layout.copy0_addr);
    const c1: *const volatile boot_state.Record = @ptrFromInt(boot_state.layout.copy1_addr);
    return .{ .copy0 = c0.*, .copy1 = c1.* };
}

fn parseSlot(raw: u8) ?slot.Slot {
    if (raw > @backingInt(slot.Slot.none)) return null;
    return @fromBackingInt(@intCast(raw));
}

fn commitTargetMram(addr: u32, rec_bytes: []const u8) linksection(mram.sram_text) u16 {
    const prep_err = mram.prepareWindow(boot_state.layout.base, boot_state.layout.window_end);
    if (prep_err != err.Err.ok.raw()) return prep_err;
    return mram.writePages(addr, rec_bytes);
}

fn eraseTargetMram() linksection(mram.sram_text) u16 {
    const prep_err = mram.prepareWindow(boot_state.layout.base, boot_state.layout.window_end);
    if (prep_err != err.Err.ok.raw()) return prep_err;
    boot_state.eraseAll(mram.Mram{}) catch return mram.lastErr();
    return err.Err.ok.raw();
}

export fn ra8_dfu_boot_state_valid(state_ptr: ?*const boot_state.Record) bool {
    const state = state_ptr orelse return false;
    return boot_state.isValid(state);
}

export fn ra8_dfu_boot_state_prepare(
    trial_slot: u8,
    tries_left: u8,
    confirmed: bool,
    slot_seq: u32,
    write_seq: u32,
    out_ptr: ?*boot_state.Record,
) u16 {
    const out = out_ptr orelse return err.Err.null_ptr.raw();
    const s = parseSlot(trial_slot) orelse return err.Err.invalid_arg.raw();
    const rec = boot_state.build(s, tries_left, confirmed, slot_seq, write_seq) catch {
        return err.Err.invalid_arg.raw();
    };
    out.* = rec;
    return err.Err.ok.raw();
}

export fn ra8_dfu_boot_state_pick(
    copy0_ptr: ?*const boot_state.Record,
    copy1_ptr: ?*const boot_state.Record,
    out_ptr: ?*boot_state.Record,
    out_copy_idx: ?*u8,
) u16 {
    const copy0 = copy0_ptr orelse return err.Err.null_ptr.raw();
    const copy1 = copy1_ptr orelse return err.Err.null_ptr.raw();
    const out = out_ptr orelse return err.Err.null_ptr.raw();

    const picked = boot_state.pick(copy0, copy1) orelse return err.Err.not_found.raw();
    out.* = picked.record;
    if (out_copy_idx) |idx| {
        idx.* = picked.copy_index;
    }
    return err.Err.ok.raw();
}

export fn ra8_dfu_boot_state_read(out_ptr: ?*boot_state.Record) u16 {
    const out = out_ptr orelse return err.Err.null_ptr.raw();
    const copies = readCopies();
    const picked = boot_state.pick(&copies.copy0, &copies.copy1) orelse return err.Err.not_found.raw();
    out.* = picked.record;
    return err.Err.ok.raw();
}

fn commitState(
    copies: CopyPair,
    s: slot.Slot,
    tries_left: u8,
    confirmed: bool,
    slot_seq: u32,
) u16 {
    if (off_target) {
        _ = boot_state.writeRecord(
            ShadowFlash{},
            &copies.copy0,
            &copies.copy1,
            s,
            tries_left,
            confirmed,
            slot_seq,
        ) catch return err.Err.hw_error.raw();
        return err.Err.ok.raw();
    }
    const plan = boot_state.planWrite(&copies.copy0, &copies.copy1);
    const rec = boot_state.build(s, tries_left, confirmed, slot_seq, plan.write_seq) catch {
        return err.Err.invalid_arg.raw();
    };
    const addr = boot_state.copyAddr(plan.copy_index) catch return err.Err.invalid_arg.raw();
    return commitTargetMram(addr, std.mem.asBytes(&rec));
}

export fn ra8_dfu_boot_state_write(
    trial_slot: u8,
    tries_left: u8,
    confirmed: bool,
    slot_seq: u32,
) u16 {
    const s = parseSlot(trial_slot) orelse return err.Err.invalid_arg.raw();
    return commitState(readCopies(), s, tries_left, confirmed, slot_seq);
}

export fn ra8_dfu_confirm() u16 {
    const copies = readCopies();
    const confirm_plan = boot_state.planConfirm(&copies.copy0, &copies.copy1) catch |e| {
        return switch (e) {
            error.NotFound => err.Err.not_found.raw(),
            error.InvalidState => err.Err.invalid_state.raw(),
        };
    };
    if (confirm_plan.outcome == .already_confirmed) return err.Err.ok.raw();
    const s = parseSlot(confirm_plan.record.trial_slot) orelse return err.Err.invalid_state.raw();
    return commitState(
        copies,
        s,
        confirm_plan.record.tries_left,
        true,
        confirm_plan.record.slot_seq,
    );
}

export fn ra8_dfu_boot_state_erase_all() u16 {
    if (off_target) {
        boot_state.eraseAll(ShadowFlash{}) catch return err.Err.hw_error.raw();
        return err.Err.ok.raw();
    }
    return eraseTargetMram();
}

export fn ra8_dfu_boot_decide_trial(
    dfu_trigger: bool,
    a_valid: bool,
    a_seq: u32,
    b_valid: bool,
    b_seq: u32,
    state_ptr: ?*const boot_state.Record,
    out_plan_ptr: ?*CBootPlan,
) u16 {
    const out = out_plan_ptr orelse return err.Err.null_ptr.raw();
    const trial_state: ?slot.TrialState = if (state_ptr) |st| blk: {
        if (!boot_state.isValid(st)) break :blk null;
        const ts = parseSlot(st.trial_slot) orelse break :blk null;
        break :blk .{
            .trial_slot = ts,
            .tries_left = st.tries_left,
            .confirmed = st.confirmed != 0,
        };
    } else null;

    const plan = slot.decideWithState(
        dfu_trigger,
        .{ .valid = a_valid, .seq = a_seq },
        .{ .valid = b_valid, .seq = b_seq },
        trial_state,
    );
    out.* = .{
        .action = plan.action,
        .is_trial = plan.is_trial,
        .next_tries = plan.next_tries,
        .dfu_target = plan.dfu_target,
    };
    return err.Err.ok.raw();
}

comptime {
    if (off_target) {
        @export(&testCorruptCopy, .{ .name = "priv_dfu_boot_state_corrupt_copy" });
        @export(&testResetStore, .{ .name = "priv_dfu_boot_state_reset_store" });
    }
}

fn testCorruptCopy(copy_idx: u8, raw32: ?*const [@sizeOf(boot_state.Record)]u8) callconv(.c) u16 {
    if (copy_idx >= boot_state.layout.copy_count) return err.Err.invalid_arg.raw();
    const bytes = raw32 orelse return err.Err.null_ptr.raw();
    @memcpy(&shadow_copies[copy_idx], bytes);
    return err.Err.ok.raw();
}

fn testResetStore() callconv(.c) void {
    @memset(&shadow_copies[0], boot_state.layout.erased_byte);
    @memset(&shadow_copies[1], boot_state.layout.erased_byte);
}
