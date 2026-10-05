//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device mass-storage Bulk-Only Transport state machine (RA8FW-756):
//! CBW parse, SCSI dispatch, CSW build and the BOT phase steps. Pure: the
//! caller passes the state; the controller calls stay in usb_pmsc_abi.zig.

const std = @import("std");
const scsi = @import("usb_pmsc_scsi.zig");

pub const State = scsi.State;
pub const Storage = scsi.Storage;

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_invalid_state: u16 = 0x104;
pub const err_invalid_size: u16 = 0x105;

/// `ra8_usb_pmsc_state_t`.
pub const state_idle: u8 = 0;
pub const state_cbw_rx: u8 = 1;
pub const state_cdb_decode: u8 = 2;
pub const state_data_tx: u8 = 3;
pub const state_data_rx: u8 = 4;
pub const state_csw_tx: u8 = 5;

/// `ra8_usb_pmsc_csw_status_t`.
pub const csw_passed: u8 = 0x00;
pub const csw_failed: u8 = 0x01;

pub const speed_fs: u8 = 0;
pub const speed_hs: u8 = 1;

pub const cbw_len: u32 = 31;
pub const csw_len: u32 = 13;
pub const cdb_max_len: u32 = 16;
pub const cbw_signature: u32 = 0x43425355;
pub const csw_signature: u32 = 0x53425355;

const cbw_off_tag = 4;
const cbw_off_data_length = 8;
const cbw_off_flags = 12;
const cbw_off_lun = 13;
const cbw_off_cdb_length = 14;
const cbw_off_cdb = 15;
const csw_off_tag = 4;
const csw_off_residue = 8;
const csw_off_status = 12;
const flag_data_in: u8 = 0x80;
const lun_mask: u8 = 0x0F;
const cdb_len_mask: u8 = 0x1F;

pub fn bulkMaxPacket(speed: u8) u16 {
    return if (speed == speed_hs) 512 else 64;
}

fn unpackLe(src: [*]const u8) u32 {
    return std.mem.readInt(u32, src[0..4], .little);
}

fn packLe(v: u32, dst: [*]u8) void {
    std.mem.writeInt(u32, dst[0..4], v, .little);
}

/// Initialized and storage attached, else invalid_state.
pub fn ready(st: *const State) u16 {
    if (!st.initialized or !st.storage_attached) return err_invalid_state;
    return ok;
}

pub fn feedCbw(st: *State, cbw: [*]const u8) u16 {
    const r = ready(st);
    if (r != ok) return r;
    if (unpackLe(cbw) != cbw_signature) {
        st.bot_state = state_csw_tx;
        st.cbw_tag = unpackLe(cbw + cbw_off_tag);
        return err_invalid_arg;
    }
    st.cbw_tag = unpackLe(cbw + cbw_off_tag);
    st.cbw_data_length = unpackLe(cbw + cbw_off_data_length);
    st.cbw_dir_in = (cbw[cbw_off_flags] & flag_data_in) != 0;
    st.cbw_lun = cbw[cbw_off_lun] & lun_mask;
    st.cbw_cdb_len = cbw[cbw_off_cdb_length] & cdb_len_mask;
    @memcpy(&st.cbw_cdb, cbw[cbw_off_cdb..][0..cdb_max_len]);
    st.bot_state = state_cdb_decode;
    st.last_data_len = 0;
    return ok;
}

fn dispatchScsi(st: *State, buf: [*]u8, capacity: u32, data_len: *u32, csw: *u8) u16 {
    return switch (st.cbw_cdb[0]) {
        0x00 => ok, // TEST UNIT READY
        0x12 => scsi.inquiry(&st.storage, buf, capacity, data_len),
        0x25 => scsi.readCapacity(&st.storage, buf, capacity, data_len),
        0x03 => scsi.requestSense(buf, capacity, data_len),
        0x1A => scsi.modeSense(buf, capacity, data_len),
        0x28 => scsi.read10(&st.storage, &st.cbw_cdb, buf, capacity, data_len),
        0x2A => scsi.write10(&st.storage, &st.cbw_cdb, buf, data_len),
        else => blk: {
            csw.* = csw_failed;
            break :blk ok;
        },
    };
}

/// Runs the cached CDB; a handler error becomes a failed CSW, not a return.
pub fn dispatch(st: *State, buf: [*]u8, capacity: u32, data_len: *u32, csw: *u8) u16 {
    const r = ready(st);
    if (r != ok) return r;
    if (st.bot_state != state_cdb_decode) return err_invalid_state;
    if (capacity == 0) return err_invalid_size;
    data_len.* = 0;
    csw.* = csw_passed;
    if (dispatchScsi(st, buf, capacity, data_len, csw) != ok) {
        csw.* = csw_failed;
        data_len.* = 0;
    }
    st.last_data_len = data_len.*;
    st.bot_state = if (data_len.* == 0)
        state_csw_tx
    else if (st.cbw_dir_in) state_data_tx else state_data_rx;
    return ok;
}

pub fn buildCsw(st: *State, status: u8, residue: u32, out: [*]u8) u16 {
    if (!st.initialized) return err_invalid_state;
    @memset(out[0..csw_len], 0);
    packLe(csw_signature, out);
    packLe(st.cbw_tag, out + csw_off_tag);
    packLe(residue, out + csw_off_residue);
    out[csw_off_status] = status;
    st.bot_state = state_idle;
    return ok;
}

pub fn step(st: *State) u16 {
    const r = ready(st);
    if (r != ok) return r;
    st.bot_state = switch (st.bot_state) {
        state_idle => state_cbw_rx,
        state_cbw_rx, state_cdb_decode => st.bot_state,
        state_data_tx, state_data_rx => state_csw_tx,
        else => state_idle,
    };
    return ok;
}

pub fn attach(st: *State, storage: *const Storage) void {
    st.storage = storage.*;
    st.storage_attached = true;
    st.bot_state = state_idle;
}

/// The state after a successful ra8_usb_device_init on `speed`.
pub fn resetForInit(st: *State, speed: u8) void {
    st.* = std.mem.zeroes(State);
    st.speed = speed;
    st.bot_state = state_idle;
    st.initialized = true;
}

pub fn markClosed(st: *State) void {
    st.initialized = false;
    st.storage_attached = false;
    st.bot_state = state_idle;
}
