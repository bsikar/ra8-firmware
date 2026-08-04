/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_rtt.c
 * @brief SEGGER RTT up-buffer drain model for ra8_emulator
 *
 * @details
 * Models the HOST side of the SEGGER Real-Time Transfer channel: the debug
 * probe that finds the in-RAM ``_SEGGER_RTT``-compatible control block and
 * drains its terminal up-buffer. On real hardware the J-Link OB scans target
 * RAM for the 10-byte ``"SEGGER RTT"`` ID string the control block begins
 * with, then reads bytes out of up-buffer 0's ring (advancing the read offset
 * so the firmware sees the buffer emptying). This block does exactly that
 * against the emulated SRAM, so an RTT-logging app (rtt_log_demo) surfaces
 * its banner in ra8_emulator with no UART pin -- the same text JLinkRTTViewer
 * would show on the bench.
 *
 * Control-block layout drained here (SEGGER_RTT.h, 32-bit target):
 * ``char id[16]`` ("SEGGER RTT" + NUL padding), ``u32 max_up``,
 * ``u32 max_down``, then ``max_up`` up-buffer descriptors of
 * ``{const char* name; u8* buf; u32 size; u32 wr_off; u32 rd_off; u32 flags}``.
 * Only up-buffer 0 (the standard "Terminal" channel) is drained; down-buffers
 * (host -> target input) are not modelled.
 *
 * Discovery is by RAM scan, not ELF symbol: the firmware deliberately writes
 * the ID string byte-by-byte at runtime so an image scan cannot match a stale
 * literal-pool copy, and a scan needs no per-app symbol name (the demo's block
 * is a file-static, not ``_SEGGER_RTT``). The scan walks the 4 MiB SRAM window
 * on a backoff cadence until the block appears, then re-verifies the ID once
 * per drain so a clobbered block is forgotten and re-discovered.
 *
 * Drained bytes are assembled into lines (CR dropped, latched on LF) and
 * surfaced exactly like the SCI console: each completed line prints to stdout
 * as ``[rtt] <line>`` and lands in the board_console RTT channel, whose newest
 * line the run loop's RA8_EMU_STOP_ON banner guard also checks -- so the EIL
 * gate scrapes an RTT banner the same way it scrapes a UART one.
 *
 * This block owns NO MMIO window (base = span = 0): RTT is a plain-RAM
 * protocol, so only the tick / reset / report hooks do work and the required
 * read / write handlers are never-dispatched stubs.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief SRAM window the control-block scan covers (mirrors main.c's map). */
typedef enum : uint64_t {
  k_rtt_scan_base = 0x22000000UL, /**< On-chip SRAM base (dual-core banks). */
  k_rtt_scan_span = 0x00400000UL, /**< The full 4 MiB window main.c maps.   */
  k_rtt_scan_step = 0x00010000UL, /**< Bytes staged per scan sub-read.      */
} rtt_scan_window_t;

/** @brief Control-block field offsets (SEGGER_RTT.h, 32-bit target layout). */
typedef enum : uint32_t {
  k_rtt_off_max_up  = 16U, /**< u32 count of up-buffers (after id[16]).   */
  k_rtt_off_up0     = 24U, /**< Up-buffer 0 descriptor (after max_down).  */
  k_rtt_up_off_buf  = 4U,  /**< u8* ring storage pointer within a desc.   */
  k_rtt_up_off_size = 8U,  /**< u32 ring capacity in bytes.               */
  k_rtt_up_off_wr   = 12U, /**< u32 write offset (target-owned).          */
  k_rtt_up_off_rd   = 16U, /**< u32 read offset (host-owned; we advance). */
} rtt_cb_layout_t;

/** @brief Model tuning: scan cadence, sanity caps, and line assembly. */
typedef enum : uint32_t {
  k_rtt_id_len          = 10U,      /**< Length of the "SEGGER RTT" ID match.  */
  k_rtt_scan_first_tick = 64U,      /**< Ticks before the first RAM scan.      */
  k_rtt_scan_max_gap    = 1024U,    /**< Backoff cap between fruitless scans.  */
  k_rtt_max_up_sane     = 16U,      /**< Reject max_up above this (garbage).   */
  k_rtt_size_sane       = 1048576U, /**< Reject ring sizes above 1 MiB.        */
  k_rtt_drain_max       = 4096U,    /**< Bytes drained per tick (bounded).     */
  k_rtt_line_max        = 240U,     /**< Chars buffered before a forced flush. */
} rtt_tune_t;

/**
 * @brief The 10-byte control-block ID a debug probe scans RAM for.
 *
 * @details Spelled as a char array (not a string literal) for the same reason
 * the firmware writes it byte-by-byte: this host tool's own rodata must never
 * be mistaken for a control block if it is ever staged through a scan buffer.
 *
 * @note Read-only; compared with memcmp against staged SRAM bytes.
 * @since 0.1.0
 */
static const char k_rtt_id[k_rtt_id_len] = {'S', 'E', 'G', 'G', 'E', 'R', ' ', 'R', 'T', 'T'};

/**
 * @brief RTT drain-model state (discovery, line assembly, observability).
 *
 * @details @c cb_addr is 0 until the scan finds a validated control block;
 * @c next_scan / @c scan_gap implement the backoff cadence; @c line /
 * @c line_len assemble the in-flight text line; the counters feed the
 * end-of-run report.
 *
 * @invariant @c line_len < ::k_rtt_line_max at all times.
 * @invariant @c cb_addr is either 0 or points at a validated ID match.
 */
typedef struct {
  uint64_t cb_addr;                   /**< Control-block address (0 = not found).  */
  uint32_t ticks;                     /**< Ticks elapsed (scan cadence clock).     */
  uint32_t next_scan;                 /**< Tick of the next discovery scan.        */
  uint32_t scan_gap;                  /**< Current backoff gap between scans.      */
  uint32_t drained;                   /**< Total bytes drained from up-buffer 0.   */
  uint32_t lines;                     /**< Completed lines surfaced.               */
  uint32_t line_len;                  /**< Chars buffered in @c line.              */
  char     line[k_rtt_line_max + 1U]; /**< In-flight (not yet newline-ended) line. */
} rtt_state_t;

static rtt_state_t s_rtt;

/** @brief Staging buffer for scan sub-reads (step + ID overlap carry). */
static uint8_t s_rtt_stage[(uint32_t)k_rtt_scan_step + (uint32_t)k_rtt_id_len];

/** @brief Staging buffer for the per-tick ring drain (bounded segments). */
static uint8_t s_rtt_seg[k_rtt_drain_max];

/**
 * @brief Read a 32-bit little-endian word from emulated memory.
 *
 * @param[in,out] uc   Unicorn engine to read through.
 * @param[in]     addr Guest address of the word.
 * @return The word value, or 0 if the read failed (unmapped address).
 * @pre @p uc is a live engine with the SRAM window mapped.
 * @pre @p addr points into mapped guest memory for a meaningful result.
 * @post No guest state is modified (pure read).
 * @post The returned value is 0 on any read failure.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static uint32_t rtt_rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  if (uc_mem_read(uc, addr, &v, sizeof(v)) != UC_ERR_OK) {
    return 0U;
  }
  return v;
}

/**
 * @brief Validate a candidate control block beyond its ID match.
 *
 * @details A 10-byte ID hit alone could be a coincidental byte pattern (or a
 * half-initialised block), so the descriptor fields are sanity-checked the
 * way a real probe does before trusting the ring: at least one up-buffer must
 * be declared (and not absurdly many), the ring storage pointer must be
 * non-NULL, the ring capacity must be non-zero and sane, and both offsets
 * must lie inside the ring. A block that is mid-``rtt_init`` simply fails
 * here and is re-tried on the next scan.
 *
 * @param[in,out] uc      Unicorn engine (descriptor fields are read from RAM).
 * @param[in]     cb_addr Candidate control-block base (the ID match address).
 * @return true when every descriptor sanity check passes.
 * @retval true  The block is a live, fully-initialised RTT control block.
 * @retval false A field is out of range; the candidate is rejected.
 * @pre @p uc is a live engine with the SRAM window mapped.
 * @pre @p cb_addr begins with the 10-byte RTT ID (the caller matched it).
 * @post No guest state is modified (pure validation).
 * @post A true result means up-buffer 0 is safe to drain this tick.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static bool rtt_cb_valid(uc_engine* uc, uint64_t cb_addr)
{
  const uint32_t max_up = rtt_rd32(uc, cb_addr + (uint64_t)k_rtt_off_max_up);
  if ((max_up == 0U) || (max_up > (uint32_t)k_rtt_max_up_sane)) {
    return false;
  }
  const uint64_t up0  = cb_addr + (uint64_t)k_rtt_off_up0;
  const uint32_t buf  = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_buf);
  const uint32_t size = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_size);
  const uint32_t wr   = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_wr);
  const uint32_t rd   = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_rd);
  if ((buf == 0U) || (size == 0U) || (size > (uint32_t)k_rtt_size_sane)) {
    return false;
  }
  return (wr < size) && (rd < size);
}

/**
 * @brief Match the staged bytes for a validated control-block ID.
 *
 * @details Walks ::s_rtt_stage memchr-first (candidates share the ID's first
 * byte) and memcmp-confirms each candidate, then ::rtt_cb_valid-checks the
 * matching guest address. Bounded: every iteration advances at least one
 * byte through the fixed-size stage.
 *
 * @param[in,out] uc         Unicorn engine (descriptor validation reads RAM).
 * @param[in]     stage_addr Guest address of ::s_rtt_stage byte 0.
 * @param[in]     len        Staged byte count (<= sizeof ::s_rtt_stage).
 * @return Guest address of the first validated control block, or 0.
 * @retval 0 No validated ID match in this stage.
 * @pre @p len is at least ::k_rtt_id_len (the caller guarantees it).
 * @pre ::s_rtt_stage holds @p len bytes read from @p stage_addr.
 * @post No guest state is modified (pure match).
 * @post A non-zero result begins with the 10-byte RTT ID.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static uint64_t rtt_match_stage(uc_engine* uc, uint64_t stage_addr, uint64_t len)
{
  const uint8_t* p    = s_rtt_stage;
  uint64_t       left = len;
  while (left >= (uint64_t)k_rtt_id_len) {
    const uint8_t* hit =
      memchr(p, (int)k_rtt_id[0], (size_t)(left - ((uint64_t)k_rtt_id_len - 1U)));
    if (hit == nullptr) {
      return 0U;
    }
    if (memcmp(hit, k_rtt_id, (size_t)k_rtt_id_len) == 0) {
      const uint64_t cb = stage_addr + (uint64_t)(hit - s_rtt_stage);
      if (rtt_cb_valid(uc, cb)) {
        return cb;
      }
    }
    left -= (uint64_t)(hit - p) + 1U;
    p = hit + 1U;
  }
  return 0U;
}

/**
 * @brief Scan the emulated SRAM window for the RTT control-block ID.
 *
 * @details Stages the window through ::s_rtt_stage in ::k_rtt_scan_step
 * sub-reads, carrying an ID-length overlap so a control block straddling a
 * stage boundary still matches, and hands each stage to ::rtt_match_stage --
 * exactly how the J-Link OB discovers the block on silicon. The first
 * validated match is latched into @c s_rtt.cb_addr. The walk is bounded by
 * the fixed window span (64 sub-reads).
 *
 * @param[in,out] uc Unicorn engine (the SRAM bytes are read through it).
 * @return Nothing.
 * @pre @p uc is a live engine with the SRAM window mapped.
 * @pre @c s_rtt.cb_addr is 0 (the caller only scans while undiscovered).
 * @post On a validated match @c s_rtt.cb_addr holds the block address.
 * @post No guest state is modified (pure scan).
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_scan(uc_engine* uc)
{
  for (uint64_t off = 0U; off < (uint64_t)k_rtt_scan_span; off += (uint64_t)k_rtt_scan_step) {
    /* Stage one step plus the ID-length overlap into the NEXT step (clamped at
     * the window end) so a block straddling the boundary is still found. */
    uint64_t want = (uint64_t)k_rtt_scan_step + (uint64_t)k_rtt_id_len;
    if ((off + want) > (uint64_t)k_rtt_scan_span) {
      want = (uint64_t)k_rtt_scan_span - off;
    }
    if (uc_mem_read(uc, (uint64_t)k_rtt_scan_base + off, s_rtt_stage, (size_t)want) != UC_ERR_OK) {
      return; /* SRAM window unreadable: give up this scan pass */
    }
    if (want < (uint64_t)k_rtt_id_len) {
      return; /* tail shorter than the ID: nothing left to match */
    }
    const uint64_t cb = rtt_match_stage(uc, (uint64_t)k_rtt_scan_base + off, want);
    if (cb != 0U) {
      s_rtt.cb_addr = cb;
      return;
    }
  }
}

/**
 * @brief Feed one drained byte into the line assembler.
 *
 * @details Mirrors the ITM / UART console formatting: CR is dropped, and a
 * LF (or a full buffer) latches the pending text -- printed to stdout as
 * ``[rtt] <line>`` and pushed into the board_console RTT channel, where the
 * run loop's RA8_EMU_STOP_ON banner guard reads the newest line back.
 *
 * @param[in] byte The drained up-buffer byte.
 * @return Nothing.
 * @pre @c s_rtt.line_len < ::k_rtt_line_max (the flush below maintains it).
 * @pre board_console is initialised (static storage guarantees it).
 * @post On LF the completed line is surfaced and the buffer reset.
 * @post Any other non-CR byte is appended to the pending line.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_line_feed(uint8_t byte)
{
  const char c = (char)byte;
  if (c == '\r') {
    return;
  }
  if ((c == '\n') || (s_rtt.line_len >= (uint32_t)k_rtt_line_max)) {
    s_rtt.line[s_rtt.line_len] = '\0';
    (void)fprintf(stdout, "[rtt] %s\n", s_rtt.line);
    (void)fflush(stdout);
    board_console_push(k_board_console_ch_rtt, s_rtt.line);
    s_rtt.lines++;
    s_rtt.line_len = 0U;
    if (c == '\n') {
      return;
    }
  }
  s_rtt.line[s_rtt.line_len] = c;
  s_rtt.line_len++;
}

/**
 * @brief Drain up-buffer 0 of the discovered control block, host-style.
 *
 * @details Re-verifies the block ID (a warm-clobbered block is forgotten and
 * re-discovered by the scan), reads the live descriptor, then consumes up to
 * ::k_rtt_drain_max pending bytes this tick -- at most two contiguous
 * segments around the ring wrap -- and stores the advanced read offset back,
 * exactly as JLinkRTTViewer does so the firmware's writer sees space free up.
 * A descriptor that fails validation this tick is skipped (drained next tick
 * once the firmware finishes touching it).
 *
 * @param[in,out] uc Unicorn engine (ring bytes + offsets live in guest RAM).
 * @return Nothing.
 * @pre @c s_rtt.cb_addr points at a previously validated control block.
 * @pre @p uc is a live engine with the SRAM window mapped.
 * @post The guest read offset equals the pre-drain write offset (or advanced
 *       by ::k_rtt_drain_max on an oversized backlog).
 * @post Every drained byte passed through ::rtt_line_feed in ring order.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_drain(uc_engine* uc)
{
  uint8_t id_now[k_rtt_id_len];
  if ((uc_mem_read(uc, s_rtt.cb_addr, id_now, sizeof(id_now)) != UC_ERR_OK) ||
      (memcmp(id_now, k_rtt_id, sizeof(id_now)) != 0)) {
    s_rtt.cb_addr = 0U; /* block gone (reboot / clobber): re-discover by scan */
    return;
  }
  const uint64_t up0  = s_rtt.cb_addr + (uint64_t)k_rtt_off_up0;
  const uint32_t buf  = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_buf);
  const uint32_t size = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_size);
  const uint32_t wr   = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_wr);
  uint32_t       rd   = rtt_rd32(uc, up0 + (uint64_t)k_rtt_up_off_rd);
  if ((buf == 0U) || (size == 0U) || (size > (uint32_t)k_rtt_size_sane) || (wr >= size) ||
      (rd >= size) || (wr == rd)) {
    return; /* invalid mid-update descriptor, or simply nothing pending */
  }
  const uint32_t avail = (wr > rd) ? (wr - rd) : ((size - rd) + wr);
  const uint32_t n     = (avail > (uint32_t)k_rtt_drain_max) ? (uint32_t)k_rtt_drain_max : avail;
  uint32_t       got   = 0U;
  /* At most two contiguous segments: rd..min(rd+n, size), then a wrap to 0. */
  const uint32_t first = ((size - rd) < n) ? (size - rd) : n;
  if (uc_mem_read(uc, (uint64_t)buf + (uint64_t)rd, s_rtt_seg, (size_t)first) == UC_ERR_OK) {
    got = first;
    if ((n > first) &&
        (uc_mem_read(uc, (uint64_t)buf, &s_rtt_seg[first], (size_t)(n - first)) == UC_ERR_OK)) {
      got = n;
    }
  }
  if (got == 0U) {
    return; /* ring storage unreadable: leave the offsets untouched */
  }
  for (uint32_t i = 0U; i < got; i++) {
    rtt_line_feed(s_rtt_seg[i]);
  }
  rd = (rd + got) % size;
  /* Store the advanced read offset back so the firmware sees the drain. */
  (void)uc_mem_write(uc, up0 + (uint64_t)k_rtt_up_off_rd, &rd, sizeof(rd));
  s_rtt.drained += got;
}

/**
 * @brief Per-chunk advance: discover the control block, then keep it drained.
 *
 * @details While undiscovered, runs ::rtt_scan on a backoff cadence (first at
 * tick ::k_rtt_scan_first_tick, then doubling the gap up to
 * ::k_rtt_scan_max_gap) so an app that never initialises RTT costs a bounded
 * handful of scans per run instead of one per tick. Once found, drains
 * up-buffer 0 every tick -- far faster than any firmware writer, so the ring
 * never back-pressures the target.
 *
 * @param[in,out] uc Unicorn engine (all block state lives in guest RAM).
 * @return Nothing.
 * @pre The block registry called ::rtt_reset at init (state is coherent).
 * @pre @p uc is a live engine with the SRAM window mapped.
 * @post @c s_rtt.ticks advanced by one.
 * @post Any pending up-buffer bytes of a discovered block were surfaced.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_tick(uc_engine* uc)
{
  s_rtt.ticks++;
  if (s_rtt.cb_addr == 0U) {
    if (s_rtt.ticks < s_rtt.next_scan) {
      return;
    }
    rtt_scan(uc);
    if (s_rtt.cb_addr == 0U) {
      /* Exponential backoff, capped: a non-RTT app pays a bounded scan cost. */
      if (s_rtt.scan_gap < (uint32_t)k_rtt_scan_max_gap) {
        s_rtt.scan_gap *= 2U;
      }
      s_rtt.next_scan = s_rtt.ticks + s_rtt.scan_gap;
      return;
    }
  }
  rtt_drain(uc);
}

/**
 * @brief Reset the RTT drain model to power-on state (warm reboot).
 *
 * @return Nothing.
 * @pre Called from the single-threaded reset path (board_periph_init).
 * @pre The block registry owns the call cadence (never re-entered).
 * @post The control block is forgotten and the scan cadence re-armed.
 * @post All counters and the in-flight line buffer read zero/empty.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_reset(void)
{
  s_rtt           = (rtt_state_t){};
  s_rtt.next_scan = (uint32_t)k_rtt_scan_first_tick;
  s_rtt.scan_gap  = (uint32_t)k_rtt_scan_first_tick;
}

/**
 * @brief End-of-run RTT section: block address + drained totals.
 *
 * @return Nothing.
 * @pre The run loop has finished (report cadence owned by the registry).
 * @pre stderr is writable (the summary sink).
 * @post One summary line is printed iff a control block was discovered.
 * @post Model state is unchanged (pure report).
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void rtt_report(void)
{
  if (s_rtt.cb_addr == 0U) {
    return; /* no RTT user in this firmware: stay quiet */
  }
  (void)fprintf(stderr,
                "  SEGGER RTT    : control block @0x%08llX, drained %u byte(s), %u line(s)\n",
                (unsigned long long)s_rtt.cb_addr,
                s_rtt.drained,
                s_rtt.lines);
}

/**
 * @brief MMIO read stub -- never dispatched (this block owns no window).
 *
 * @param[in,out] uc   Unicorn engine; unused.
 * @param[in]     addr Access address; unused.
 * @param[in]     size Access width; unused.
 * @return Always 0.
 * @retval 0 The only value (unreachable in practice: span is 0).
 * @pre None (the zero-length window means the core never routes here).
 * @pre The descriptor's span stays 0 (see ::k_rtt_block).
 * @post No state is touched.
 * @post The return value is 0.
 * @note Present only because the registry requires a read handler.
 * @since 0.1.0
 */
static uint64_t rtt_mmio_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return 0U;
}

/**
 * @brief MMIO write stub -- never dispatched (this block owns no window).
 *
 * @param[in,out] uc    Unicorn engine; unused.
 * @param[in]     addr  Access address; unused.
 * @param[in]     size  Access width; unused.
 * @param[in]     value Written value; unused.
 * @return Nothing.
 * @pre None (the zero-length window means the core never routes here).
 * @pre The descriptor's span stays 0 (see ::k_rtt_block).
 * @post No state is touched.
 * @post The write is discarded.
 * @note Present only because the registry requires a write handler.
 * @since 0.1.0
 */
static void rtt_mmio_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  (void)value;
}

/** @brief Per-tick order slot for the RTT drain (right after the SCI console). */
typedef enum : uint32_t {
  k_rtt_block_order = 32U, /**< After SCI (30), before the core IRQ section (35). */
} rtt_order_t;

/** @brief This block's descriptor: tick/reset/report only, no MMIO window. */
static const board_periph_block_t k_rtt_block = {
  .base   = 0U,
  .span   = 0U, /* RAM-resident protocol: dispatch never routes here. */
  .order  = (uint32_t)k_rtt_block_order,
  .read   = rtt_mmio_read,
  .write  = rtt_mmio_write,
  .tick   = rtt_tick,
  .reset  = rtt_reset,
  .report = rtt_report,
  .name   = "SEGGER RTT",
};

/** @brief Self-register the RTT drain model before main (host constructor). */
[[gnu::constructor]] static void rtt_block_register(void)
{
  board_periph_register_block(&k_rtt_block);
}
