//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Hardware and emulator binding for `RingTransport` over RA8D2 shared SRAM
//! (`0x2210_0000`) and the IPC FIFO peripheral (`0x4002_0000`).
//!
//! Design constraints enforced by this module:
//! 1. `memoryBarrier()` wraps `dmb sy` on Arm targets inside `asm volatile`
//!    with a memory clobber so devirtualized calls never reorder data copies
//!    across `head` / `tail` stores.
//! 2. `verifyNonCacheableMpu()` checks the Armv8-M MPU map before trusting a
//!    shared-SRAM ring when the M85 D-cache is enabled, ensuring the ring sits
//!    in an enabled writable Normal Non-cacheable region (`MAIR == 0x44`).
//! 3. `formatRings()` and `resetAfterReformat()` enforce coordinated recovery:
//!    both ring headers must be rewritten by `Ring.writeHeader()` while the
//!    peer is held before either end resets its `RingTransport`.
//!
//! IPC FIFO writes (`notify`) are strictly non-blocking hints: when all four
//! FIFO stages are full, new doorbells coalesce without error or data loss
//! because every payload byte remains in the shared-SRAM ring.

const std = @import("std");
const builtin = @import("builtin");

const ring_mod = @import("ring.zig");
const Ring = ring_mod.Ring;
const Layout = ring_mod.Layout;
const RingTransport = @import("ring_transport.zig").RingTransport;
const Signal = @import("signal.zig").Signal;
const Transport = @import("transport.zig").Transport;

/// Secure peripheral base of the RA8D2 IPC block (HUM Ch 3.2).
pub const base_addr: usize = 0x4002_0000;
/// Non-secure alias of the RA8D2 IPC block.
pub const ns_base_addr: usize = 0x5002_0000;

/// Base of the EK-RA8D2 CPU0 <-> CPU1 shared SRAM window (576 KiB before CPU1 SRAM).
pub const shared_sram_base: usize = 0x2210_0000;
/// Byte span of the shared SRAM window (`0x2210_0000..0x2219_0000`).
pub const shared_sram_size: usize = 0x0009_0000;

/// Normal inner/outer non-cacheable Armv8-M MAIR byte (`0x44`).
pub const normal_non_cacheable_attr: u8 = 0x44;

/// Armv8-M MPU MMIO register addresses (Cortex-M85 TRM / Armv8-M ARM).
pub const MpuAddr = struct {
    pub const type_reg: usize = 0xE000_ED90;
    pub const ctrl: usize = 0xE000_ED94;
    pub const rnr: usize = 0xE000_ED98;
    pub const rbar: usize = 0xE000_ED9C;
    pub const rlar: usize = 0xE000_EDA0;
    pub const mair0: usize = 0xE000_EDC0;
    pub const mair1: usize = 0xE000_EDC4;
};

/// IPC FIFO channel geometry (`ra8_ipc_regs.h`).
pub const ChannelGeom = struct {
    pub const first_offset: usize = 0x0C0;
    pub const stride: usize = 0x020;
    pub const count: u8 = 4;
    pub const fifo_depth: u32 = 4;
};

/// In-channel register offsets for each IPC FIFO channel window.
pub const ChannelReg = struct {
    pub const sta: usize = 0x00;
    pub const iset: usize = 0x04;
    pub const txd: usize = 0x08;
    pub const rxd: usize = 0x0C;
    pub const clr: usize = 0x10;
};

/// Bit masks for IPC channel status (`STA`) and clear (`CLR`) registers.
pub const Bits = struct {
    pub const irq_mask: u32 = 0x0000_00FF;
    pub const sta_rdy: u32 = 0x0001_0000;
    pub const sta_full: u32 = 0x0002_0000;
    pub const sta_rerr: u32 = 0x0100_0000;
    pub const sta_ferr: u32 = 0x0200_0000;
    pub const clr_rst: u32 = 0x0001_0000;
    pub const clr_rclr: u32 = 0x0100_0000;
    pub const clr_fclr: u32 = 0x0200_0000;
    pub const clr_all: u32 = irq_mask | clr_rst | clr_rclr | clr_fclr;
};

/// Local CPU core identifier matching `ra8_ipc_core_id_t`.
pub const Core = enum(u1) {
    cpu0 = 0,
    cpu1 = 1,
};

/// Logical bidirectional channel pair (`0` -> ch0/ch2, `1` -> ch1/ch3).
pub const Pair = enum(u1) {
    pair0 = 0,
    pair1 = 1,
};

pub const MpuError = error{
    MpuDisabled,
    NoRegions,
    UnalignedAddress,
    OutsideSharedSram,
    WindowTooSmall,
    CacheableOrUnmapped,
    ReadOnly,
};

pub const ResetError = error{
    PeerNotHeld,
    HeadersNotRewritten,
};

/// Abstract 32-bit register bus so hardware MMIO and host mocks share one path.
pub const Bus = struct {
    ctx: *anyopaque,
    vtable: *const VTable,

    pub const VTable = struct {
        read32: *const fn (ctx: *anyopaque, addr: usize) u32,
        write32: *const fn (ctx: *anyopaque, addr: usize, value: u32) void,
    };

    pub fn read32(self: Bus, addr: usize) u32 {
        return self.vtable.read32(self.ctx, addr);
    }

    pub fn write32(self: Bus, addr: usize, value: u32) void {
        self.vtable.write32(self.ctx, addr, value);
    }
};

var mmio_dummy: u8 = 0;

const mmio_vtable: Bus.VTable = .{
    .read32 = mmioRead32,
    .write32 = mmioWrite32,
};

/// Real volatile MMIO bus for Cortex-M85 and Cortex-M33 firmware targets.
pub const mmio: Bus = .{
    .ctx = @ptrCast(&mmio_dummy),
    .vtable = &mmio_vtable,
};

fn mmioRead32(_: *anyopaque, addr: usize) u32 {
    const ptr: *const volatile u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWrite32(_: *anyopaque, addr: usize, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(addr);
    ptr.* = value;
}

/// Compiler + hardware memory barrier (`dmb sy` on Arm) that cannot be reordered
/// across payload copies even if the `Signal` vtable call is devirtualized.
pub inline fn memoryBarrier() void {
    switch (builtin.cpu.arch) {
        .thumb, .thumbeb, .arm, .armeb => asm volatile ("dmb sy" ::: .{ .memory = true }),
        else => asm volatile ("" ::: .{ .memory = true }),
    }
}

/// Outbound IPC FIFO channel for `(core, pair)` matching `ra8_ipc_channel_for_send`.
pub fn sendChannel(core: Core, pair: Pair) u8 {
    return switch (core) {
        .cpu0 => 2 + @as(u8, @backingInt(pair)),
        .cpu1 => @as(u8, @backingInt(pair)),
    };
}

/// Inbound IPC FIFO channel for `(core, pair)` matching `ra8_ipc_channel_for_recv`.
pub fn recvChannel(core: Core, pair: Pair) u8 {
    return switch (core) {
        .cpu0 => @as(u8, @backingInt(pair)),
        .cpu1 => 2 + @as(u8, @backingInt(pair)),
    };
}

/// MMIO address of one channel's register inside the IPC peripheral window.
pub fn channelRegAddr(ipc_base: usize, channel: u8, reg_offset: usize) usize {
    return ipc_base + ChannelGeom.first_offset + (@as(usize, channel) * ChannelGeom.stride) + reg_offset;
}

/// Check that `[addr, addr + len)` is 4-byte aligned and fits inside the
/// EK-RA8D2 shared SRAM window below CPU1's private SRAM bank.
pub fn validateSharedWindow(addr: usize, len: usize) MpuError!void {
    if ((addr & 3) != 0) return error.UnalignedAddress;
    if (len < Layout.header_bytes + Layout.min_capacity) return error.WindowTooSmall;
    const end = std.math.add(usize, addr, len) catch return error.OutsideSharedSram;
    const win_end = shared_sram_base + shared_sram_size;
    if (addr < shared_sram_base or end > win_end) return error.OutsideSharedSram;
}

/// Cast a validated shared-SRAM address range into an aligned byte slice.
pub fn sharedSlice(addr: usize, len: usize) MpuError![]align(4) u8 {
    try validateSharedWindow(addr, len);
    const ptr: [*]align(4) u8 = @ptrFromInt(addr);
    return ptr[0..len];
}

/// Split one contiguous shared buffer into two 32-byte-line-aligned ring slices
/// (`tx` then `rx`) so producer and consumer cache lines stay disjoint.
pub fn carvePair(mem: []align(4) u8) Ring.InitError!struct { tx: []align(4) u8, rx: []align(4) u8 } {
    const min_ring = Layout.header_bytes + Layout.min_capacity;
    if (mem.len < 2 * min_ring) return error.RingTooSmall;
    const raw_mid = mem.len / 2;
    const mid = std.mem.alignBackward(usize, raw_mid, Layout.line);
    if (mid < min_ring or mem.len - mid < min_ring) return error.RingTooSmall;
    const second: [*]align(4) u8 = @alignCast(mem[mid..].ptr);
    return .{
        .tx = mem[0..mid],
        .rx = second[0 .. mem.len - mid],
    };
}

/// Verify via the Armv8-M MPU registers that `[addr, addr + len)` is covered
/// by an enabled, writable, Normal Non-cacheable (`MAIR == 0x44`) MPU region.
/// Device memory (`0x00..0x0F`) is rejected because `Ring` relies on unaligned
/// byte copies across shared Normal memory.
pub fn verifyNonCacheableMpu(bus: Bus, addr: usize, len: usize) MpuError!void {
    try validateSharedWindow(addr, len);
    const dregion = (bus.read32(MpuAddr.type_reg) >> 8) & 0xFF;
    if (dregion == 0) return error.NoRegions;
    if ((bus.read32(MpuAddr.ctrl) & 1) == 0) return error.MpuDisabled;

    const end_incl = addr + len - 1;
    const mair0 = bus.read32(MpuAddr.mair0);
    const mair1 = bus.read32(MpuAddr.mair1);

    var idx: u32 = 0;
    while (idx < dregion) : (idx += 1) {
        bus.write32(MpuAddr.rnr, idx);
        const rbar = bus.read32(MpuAddr.rbar);
        const rlar = bus.read32(MpuAddr.rlar);
        if ((rlar & 1) == 0) continue;

        const r_base: usize = rbar & ~@as(usize, 0x1F);
        const r_limit: usize = (rlar | 0x1F);
        if (addr < r_base or end_incl > r_limit) continue;

        // Armv8-M RBAR.AP[2:1] (bits 2:1): 0b00=RW priv, 0b01=RW all,
        // 0b10=RO priv, 0b11=RO all.
        const ap = (rbar >> 1) & 0x3;
        if (ap >= 2) return error.ReadOnly;

        const attr_idx: u5 = @intCast((rlar >> 1) & 0x7);
        const attr = mairByte(mair0, mair1, attr_idx);
        if (attr != normal_non_cacheable_attr) return error.CacheableOrUnmapped;
        return;
    }
    return error.CacheableOrUnmapped;
}

fn mairByte(mair0: u32, mair1: u32, attr_idx: u5) u8 {
    if (attr_idx < 4) {
        const shift: u5 = attr_idx * 8;
        return @truncate(mair0 >> shift);
    }
    const shift: u5 = (attr_idx - 4) * 8;
    return @truncate(mair1 >> shift);
}

/// `Signal` implementation backed by `memoryBarrier()` and one RA8D2 IPC FIFO pair.
pub const IpcSignal = struct {
    bus: Bus,
    ipc_base: usize = base_addr,
    tx_channel: u8,
    rx_channel: u8,
    doorbell_word: u32 = 1,
    doorbells_sent: u32 = 0,
    doorbells_coalesced: u32 = 0,
    barrier_hook: ?*const fn (ctx: ?*anyopaque) void = null,
    hook_ctx: ?*anyopaque = null,

    pub fn init(core: Core, pair: Pair, bus: Bus) IpcSignal {
        return .{
            .bus = bus,
            .tx_channel = sendChannel(core, pair),
            .rx_channel = recvChannel(core, pair),
        };
    }

    pub fn withBase(self: IpcSignal, base: usize) IpcSignal {
        var copy = self;
        copy.ipc_base = base;
        return copy;
    }

    /// Return the `rpc.Signal` interface pointing at `self` (must not move).
    pub fn signal(self: *IpcSignal) Signal {
        return .{ .ctx = self, .vtable = &vtable };
    }

    /// Non-zero when the receive IPC FIFO currently holds at least one doorbell.
    pub fn hasDoorbell(self: *const IpcSignal) bool {
        const sta = self.bus.read32(channelRegAddr(self.ipc_base, self.rx_channel, ChannelReg.sta));
        return (sta & Bits.sta_rdy) != 0;
    }

    /// Drain all queued doorbell words (bounded to FIFO depth) and clear flags.
    pub fn drainDoorbell(self: *const IpcSignal) u32 {
        const sta_addr = channelRegAddr(self.ipc_base, self.rx_channel, ChannelReg.sta);
        const rxd_addr = channelRegAddr(self.ipc_base, self.rx_channel, ChannelReg.rxd);
        const clr_addr = channelRegAddr(self.ipc_base, self.rx_channel, ChannelReg.clr);
        var drained: u32 = 0;
        while (drained < ChannelGeom.fifo_depth) {
            if ((self.bus.read32(sta_addr) & Bits.sta_rdy) == 0) break;
            _ = self.bus.read32(rxd_addr);
            drained += 1;
        }
        const sta = self.bus.read32(sta_addr);
        const err_clear = (sta & Bits.irq_mask) | (if ((sta & Bits.sta_rerr) != 0) Bits.clr_rclr else 0);
        if (err_clear != 0) self.bus.write32(clr_addr, err_clear);
        return drained;
    }

    /// Reset TX and RX FIFO stages and sticky status latches.
    pub fn resetChannels(self: *const IpcSignal) void {
        self.bus.write32(channelRegAddr(self.ipc_base, self.tx_channel, ChannelReg.clr), Bits.clr_all);
        self.bus.write32(channelRegAddr(self.ipc_base, self.rx_channel, ChannelReg.clr), Bits.clr_all);
    }

    const vtable: Signal.VTable = .{
        .barrier = barrierCb,
        .notify = notifyCb,
    };

    fn from(ctx: *anyopaque) *IpcSignal {
        return @ptrCast(@alignCast(ctx));
    }

    fn barrierCb(ctx: *anyopaque) void {
        memoryBarrier();
        const self = from(ctx);
        if (self.barrier_hook) |hook| hook(self.hook_ctx);
    }

    fn notifyCb(ctx: *anyopaque) void {
        const self = from(ctx);
        const sta_addr = channelRegAddr(self.ipc_base, self.tx_channel, ChannelReg.sta);
        const txd_addr = channelRegAddr(self.ipc_base, self.tx_channel, ChannelReg.txd);
        const clr_addr = channelRegAddr(self.ipc_base, self.tx_channel, ChannelReg.clr);

        const sta = self.bus.read32(sta_addr);
        if ((sta & Bits.sta_ferr) != 0) {
            self.bus.write32(clr_addr, Bits.clr_fclr);
        }
        if ((sta & Bits.sta_full) != 0) {
            self.doorbells_coalesced +%= 1;
            return;
        }
        self.bus.write32(txd_addr, self.doorbell_word);
        self.doorbells_sent +%= 1;
    }
};

/// One unidirectional/bidirectional `RingTransport` endpoint bound to two rings
/// in shared SRAM and one `IpcSignal` doorbell pair.
pub const Channel = struct {
    out_ring: Ring,
    in_ring: Ring,
    ipc: IpcSignal,
    link: RingTransport = undefined,

    pub fn init(out_mem: []align(4) u8, in_mem: []align(4) u8, ipc: IpcSignal) Ring.InitError!Channel {
        return .{
            .out_ring = try Ring.init(out_mem),
            .in_ring = try Ring.init(in_mem),
            .ipc = ipc,
        };
    }

    /// Bind `RingTransport` to `self.ipc`; `self` must remain pinned in memory.
    pub fn bind(self: *Channel) void {
        self.link = RingTransport.init(self.out_ring, self.in_ring, self.ipc.signal());
    }

    pub fn transport(self: *Channel) Transport {
        return self.link.transport();
    }

    /// Reformat both ring headers while the peer is held and reset IPC FIFOs.
    pub fn formatRings(self: *const Channel, peer_held: bool) ResetError!void {
        if (!peer_held) return error.PeerNotHeld;
        self.out_ring.writeHeader();
        self.in_ring.writeHeader();
        memoryBarrier();
        self.ipc.resetChannels();
    }

    /// Clear `RingTransport` failure state ONLY after both headers were
    /// freshly rewritten (`head == 0` and `tail == 0`).
    pub fn resetAfterReformat(self: *Channel) ResetError!void {
        const out_idx = self.out_ring.load() catch return error.HeadersNotRewritten;
        const in_idx = self.in_ring.load() catch return error.HeadersNotRewritten;
        if (out_idx.head != 0 or out_idx.tail != 0 or in_idx.head != 0 or in_idx.tail != 0) {
            return error.HeadersNotRewritten;
        }
        self.link.reset();
        _ = self.ipc.drainDoorbell();
    }
};
