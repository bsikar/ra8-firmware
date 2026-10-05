//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA per-queue descriptor and ring helpers (RA8FW-749), ported from
//! ra8_eth_gwca_queue.c. Register access and logging go through an `ops`
//! value so host tests run on in-memory registers. HUM chapter 34.
//! ra8_eth_gwca_reload_queue stays in C (its BALR poll needs the host
//! fake-MMIO wait seam).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const no_data: u16 = 0x10A;
pub const null_ptr: u16 = 0x504;

/// `ra8_gwdcc_dt_t` values (ra8_ether_regs.h).
pub const dt_linkfix: u8 = 0;
pub const dt_fempty: u8 = 4;
pub const dt_fsingle: u8 = 8;
pub const dt_link: u8 = 14;

/// GWDCC fields (ra8_ether_regs.h).
pub const gwdcc_ede: u32 = 1 << 8;
pub const gwdcc_sl: u32 = 1 << 10;
pub const gwdcc_dqt: u32 = 1 << 11;
pub const gwdcc_dcp_shift: u5 = 16;
pub const gwdcc_dcp_mask: u32 = 0x7 << 16;

pub const max_queues: u32 = 32;
pub const max_tx_queues: u32 = 64;
const dcp_max: u8 = 7;
const ring_min_depth: u32 = 2;
const ring_max_bytes: u32 = 2048;

/// Mirror of `ra8_gwca_basic_descriptor_t`. The C struct packs volatile u8
/// bitfields: b1 = ds_h[3:0] | info0[7:4], b2 = err[2:0] | die[3] | dt[7:4].
/// Byte read-modify-writes keep the access width GCC uses for the bitfields.
pub const Desc = extern struct {
    ds_l: u8 = 0,
    b1: u8 = 0,
    b2: u8 = 0,
    ptr_h: u8 = 0,
    ptr_l: u32 = 0,
};

comptime {
    if (@sizeOf(Desc) != 8) @compileError("GWCA basic descriptor must be 8 bytes");
    if (@offsetOf(Desc, "ptr_h") != 3 or @offsetOf(Desc, "ptr_l") != 4) @compileError("Desc layout");
}

/// Mirror of `ra8_eth_gwca_queue_cfg_t` (ra8_eth_gwca.h).
pub const QueueCfg = extern struct {
    priority: u8 = 0,
    is_tx: bool = false,
    stop_on_last: bool = false,
    extended: bool = false,
    chain_head: ?*anyopaque = null,
};

pub fn getDt(d: *const volatile Desc) u8 {
    return d.b2 >> 4;
}

pub fn setDt(d: *volatile Desc, dt: u8) void {
    d.b2 = (d.b2 & 0x0F) | (dt << 4);
}

fn setDsH(d: *volatile Desc, v: u32) void {
    d.b1 = (d.b1 & 0xF0) | @as(u8, @truncate(v & 0xF));
}

fn setDs(d: *volatile Desc, len: u32) void {
    d.ds_l = @truncate(len & 0xFF);
    setDsH(d, len >> 8);
}

pub fn composeGwdcc(cfg: *const QueueCfg) u32 {
    var value: u32 = 0;
    if (cfg.is_tx) value |= gwdcc_dqt;
    if (cfg.stop_on_last) value |= gwdcc_sl;
    if (cfg.extended) value |= gwdcc_ede;
    value |= (@as(u32, cfg.priority) << gwdcc_dcp_shift) & gwdcc_dcp_mask;
    return value;
}

fn writePtr(d: *volatile Desc, addr: usize) void {
    d.ptr_h = @truncate(@as(u64, addr) >> 32);
    d.ptr_l = @truncate(addr);
}

pub fn setLinkfixEntry(entry: *volatile Desc, head: usize) void {
    setDt(entry, dt_linkfix);
    writePtr(entry, head);
}

pub fn decodePtr(d: *const volatile Desc) ?[*]u8 {
    const addr: u64 = (@as(u64, d.ptr_h) << 32) | d.ptr_l;
    if (addr == 0) return null;
    return @ptrFromInt(@as(usize, @truncate(addr)));
}

pub fn configureQueue(ops: anytype, table: ?[*]volatile Desc, q: u32, cfg: ?*const QueueCfg) u16 {
    const t = table orelse return ops.nullPtr("configure_queue: table null");
    const c = cfg orelse return ops.nullPtr("configure_queue: cfg null");
    const head = c.chain_head orelse return ops.nullPtr("configure_queue: chain_head null");
    if (c.priority > dcp_max) return invalid_arg;
    const gwdcc = ops.gwdcc(q) orelse return invalid_arg;
    gwdcc.* = composeGwdcc(c);
    setLinkfixEntry(&t[q], @intFromPtr(head));
    return ok;
}

pub fn initRing(ops: anytype, chain: ?[*]volatile Desc, depth: u32, slot_bytes: u32) u16 {
    const ch = chain orelse return ops.nullPtr("init_ring: chain null");
    if (depth < ring_min_depth or slot_bytes > ring_max_bytes) return invalid_arg;
    var i: u32 = 0;
    while (i < depth - 1) : (i += 1) {
        ch[i] = .{};
        setDt(&ch[i], dt_fempty);
        setDs(&ch[i], slot_bytes);
    }
    setLinkfixEntry(&ch[depth - 1], @intFromPtr(&ch[0]));
    setDt(&ch[depth - 1], dt_link);
    return ok;
}

pub fn setDescriptorBuffer(ops: anytype, desc: ?*volatile Desc, buffer: ?*anyopaque) u16 {
    const d = desc orelse return ops.nullPtr("set_descriptor_buffer: desc null");
    writePtr(d, @intFromPtr(buffer));
    return ok;
}

pub fn attachBuffers(ops: anytype, chain: ?[*]volatile Desc, depth: u32, slot_bytes: u32, pool: ?[*]u8) u16 {
    const ch = chain orelse return ops.nullPtr("attach_buffers: chain null");
    const p = pool orelse return ops.nullPtr("attach_buffers: pool null");
    if (depth < 2 or slot_bytes == 0) return invalid_arg;
    var i: u32 = 0;
    while (i < depth - 1) : (i += 1) {
        const offset = @as(usize, i) * @as(usize, slot_bytes);
        writePtr(&ch[i], @intFromPtr(p + offset));
    }
    return ok;
}

pub fn kickTx(ops: anytype, q: u32) u16 {
    if (q >= max_tx_queues) return invalid_arg;
    const reg = ops.gwtrc(if (q < 32) 0 else 1);
    reg.* = reg.* | (@as(u32, 1) << @as(u5, @truncate(q % 32)));
    return ok;
}

pub fn findSlot(ops: anytype, chain: ?[*]const volatile Desc, depth: u32, match_dt: u8, start: u32, out: ?*u32) u16 {
    const ch = chain orelse return ops.nullPtr("find_slot: chain null");
    const o = out orelse return ops.nullPtr("find_slot: out_index null");
    if (depth < 2) return invalid_arg;
    const count = depth - 1;
    if (start >= count) return invalid_arg;
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const slot = (start + i) % count;
        if (getDt(&ch[slot]) == match_dt) {
            o.* = slot;
            return ok;
        }
    }
    return no_data;
}

pub fn txFrame(ops: anytype, chain: ?[*]volatile Desc, depth: u32, tail: ?*u32, frame: ?[*]const u8, len: u32, slot_bytes: u32) u16 {
    const ch = chain orelse return ops.nullPtr("tx_frame: chain null");
    const t = tail orelse return ops.nullPtr("tx_frame: tail_idx null");
    const f = frame orelse return ops.nullPtr("tx_frame: frame null");
    if (len == 0 or len > slot_bytes) return invalid_arg;
    // The C driver reached find_slot's depth check after `tail % (depth - 1)`;
    // depth < 2 gives the same invalid_arg without the division.
    if (depth < 2) return invalid_arg;
    var slot: u32 = 0;
    const err = findSlot(ops, ch, depth, dt_fempty, t.* % (depth - 1), &slot);
    if (err != ok) return err;
    const buf = decodePtr(&ch[slot]) orelse return invalid_arg;
    @memcpy(buf[0..len], f[0..len]);
    setDs(&ch[slot], len);
    setDt(&ch[slot], dt_fsingle);
    t.* = (slot + 1) % (depth - 1);
    return ok;
}
