//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the GWCA queue and ring helpers in ra8_eth_gwca.h (RA8FW-749).
//! Logic lives in internal/eth_gwca_queue.zig. ra8_eth_gwca_reload_queue
//! joined it in RA8FW-764, replacing ra8_eth_gwca_queue.c.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const q = @import("internal/eth_gwca_queue.zig");

const tag = "ETHGWC";
/// `k_ra8_gwca0_base_addr` and the GWCA offsets (ra8_ether_regs.h).
const gwca0_base: usize = 0x403CE000;
const off_gwtrc0: usize = 0x0200;
const off_gwtrc1: usize = 0x0204;
const off_gwdcc_base: usize = 0x0400;

/// Host builds link the C fake-MMIO wait seam (ra8_hw_err.h) so the C
/// suites can inject a stuck BALR. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn gwdcc(_: Hw, queue: u32) ?*volatile u32 {
        if (queue >= q.max_queues) return null;
        return @ptrFromInt(gwca0_base + off_gwdcc_base + @as(usize, queue) * 4);
    }
    pub fn gwtrc(_: Hw, idx: u1) *volatile u32 {
        return @ptrFromInt(gwca0_base + if (idx == 0) off_gwtrc0 else off_gwtrc1);
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return q.null_ptr;
    }
    pub fn balrClear(_: Hw, reg: *volatile u32, iter: u32) bool {
        const cond = (reg.* & q.gwdcc_balr) == 0;
        return if (hosted) seam.ra8_fake_mmio_wait_eval(reg, iter, cond) else cond;
    }
    pub fn logError(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn priv_ra8_eth_gwca_set_linkfix_entry(entry: *volatile q.Desc, chain_head: ?*const anyopaque) void {
    q.setLinkfixEntry(entry, @intFromPtr(chain_head));
}

export fn priv_ra8_eth_gwca_decode_ptr(desc: ?*const volatile q.Desc) ?[*]u8 {
    return q.decodePtr(desc orelse return null);
}

export fn ra8_eth_gwca_configure_queue(table: ?[*]volatile q.Desc, queue: u32, cfg: ?*const q.QueueCfg) u16 {
    return q.configureQueue(Hw{}, table, queue, cfg);
}

export fn ra8_eth_gwca_init_ring(chain: ?[*]volatile q.Desc, ring_depth: u32, slot_bytes: u32) u16 {
    return q.initRing(Hw{}, chain, ring_depth, slot_bytes);
}

export fn ra8_eth_gwca_set_descriptor_buffer(desc: ?*volatile q.Desc, buffer: ?*anyopaque) u16 {
    return q.setDescriptorBuffer(Hw{}, desc, buffer);
}

export fn ra8_eth_gwca_attach_buffers(chain: ?[*]volatile q.Desc, ring_depth: u32, slot_bytes: u32, pool: ?[*]u8) u16 {
    return q.attachBuffers(Hw{}, chain, ring_depth, slot_bytes, pool);
}

export fn ra8_eth_gwca_kick_tx(queue: u32) u16 {
    return q.kickTx(Hw{}, queue);
}

export fn ra8_eth_gwca_find_slot(chain: ?[*]const volatile q.Desc, ring_depth: u32, match_dt: u8, start_idx: u32, out_index: ?*u32) u16 {
    return q.findSlot(Hw{}, chain, ring_depth, match_dt, start_idx, out_index);
}

export fn ra8_eth_gwca_tx_frame(chain: ?[*]volatile q.Desc, ring_depth: u32, tail_idx: ?*u32, frame: ?[*]const u8, frame_len: u32, slot_bytes: u32) u16 {
    return q.txFrame(Hw{}, chain, ring_depth, tail_idx, frame, frame_len, slot_bytes);
}

export fn ra8_eth_gwca_reload_queue(queue_index: u32) u16 {
    return q.reloadQueue(Hw{}, queue_index);
}
