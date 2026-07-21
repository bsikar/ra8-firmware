/**
 * @file examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong_ipc/ns_main.c
 * @brief Self-contained Non-Secure image at 0x02080000 for the IPC ping-pong demo.
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * After the S-side ``trustzone_init`` programmes the SAU and writes
 * IPCSAR=0x00050000 it BLXNS-es to ``ns_reset_handler`` here. Once CPU0
 * is in NS state the IPCSAR-attributed channels 0 (CPU1->CPU0) and 2
 * (CPU0->CPU1) become reachable. The S side has already released CPU1
 * via ``ra8_cpu1_release`` before the BLXNS, so CPU1 is fetching its
 * own image and is the IPC peer for the ping-pong loop below.
 *
 * The NS image is intentionally self-contained: it cannot call any
 * function whose code resides in S MRAM without an NSC veneer, so the
 * IPC accessors are open-coded against the register window. Counters
 * live in NS SRAM and are read externally via SWD (the J-Link debug
 * controller sees both worlds).
 *
 * ## Memory layout
 *
 * | Symbol                          | Section         | Address          |
 * |---------------------------------|-----------------|------------------|
 * | ``g_ra8_ns_vector_table``        | ``.ns_vectors`` | 0x02080000       |
 * | ``ns_reset_handler``            | ``.ns_text``    | 0x02080000+      |
 * | ``g_ns_pingpong_*`` counters    | ``.ns_bss``     | 0x22100000+      |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * NS-side constants -- open-coded from libs/ra8_hal/inc/ra8_ipc_regs.h.
 * Re-declared here to avoid the NS image pulling in any S-resident symbol.
 * =============================================================================
 */

/**
 * @enum ns_ipc_const_t
 * @brief Register layout for the IPC channels CPU0 owns in NS state.
 *
 * @details
 *  - ``k_ns_ipc_base`` is the IPC peripheral window in the secure alias.
 *    Bit 28 IDAU rule says this is S by default; the SAU NS_PERIPH
 *    region pulled in by ``ra8_tz_secure_boot_sau_init`` (now extended to
 *    0x40000000-0x5FFFFFE0) lets NS code reach it.
 *  - Channel 0 is at +0x0C0 (CPU1->CPU0 receive path).
 *  - Channel 2 is at +0x100 (CPU0->CPU1 send path).
 *  - Each channel is 5 x 32-bit registers laid out STA / ISET / TXD /
 *    RXD / CLR, then padding to the next channel at +0x20.
 *  - HUM Ch 3.2.10..3.2.14 p 214-216.
 */
typedef enum : uintptr_t {
  k_ns_ipc_base     = 0x50020000UL, /**< IPC peripheral base (NS alias). */
  k_ns_ipc_ch0_addr = 0x500200C0UL, /**< CPU0 RX from CPU1 (NS alias).   */
  k_ns_ipc_ch2_addr = 0x50020100UL, /**< CPU0 TX to   CPU1 (NS alias).   */
} ns_ipc_const_t;

/**
 * @enum ns_ipc_off_t
 * @brief In-channel offsets for STA/ISET/TXD/RXD/CLR.
 */
typedef enum : uint8_t {
  k_ns_ipc_off_sta = 0x00U, /**< Status register.     */
  k_ns_ipc_off_set = 0x04U, /**< IRQ set (W).         */
  k_ns_ipc_off_txd = 0x08U, /**< FIFO transmit (W).   */
  k_ns_ipc_off_rxd = 0x0CU, /**< FIFO receive  (R).   */
  k_ns_ipc_off_clr = 0x10U, /**< Status / FIFO clear. */
} ns_ipc_off_t;

/**
 * @enum ns_ipc_sta_mask_t
 * @brief STA register bits we care about.
 *
 * @details HUM Ch 3.2.10 "IPC0STA0" p 214 -- bit 16 RDY = FIFO non-empty.
 */
typedef enum : uint32_t {
  k_ns_ipc_sta_rdy = 0x00010000UL, /**< RDY: receive FIFO non-empty. */
} ns_ipc_sta_mask_t;

/**
 * @enum ns_pingpong_step_t
 * @brief Progress markers written to ``g_ns_pingpong_step``.
 * @details The bench reads this word over J-Link to see how far the NS image
 *          got. Values ascend with execution order, so a dump alone says
 *          where it stopped.
 * @invariant Values ascend in execution order.
 * @see ns_pingpong_marker_t
 */
typedef enum : uint32_t {
  k_ns_step_bss_zeroed  = 1U, /**< .bss zeroed, entry marker stamped.  */
  k_ns_step_channels_up = 2U, /**< Both IPC channels cold-initialised. */
  k_ns_step_tx_start    = 3U, /**< About to write the ping.            */
  k_ns_step_tx_done     = 5U, /**< Survived the TX write.              */
  k_ns_step_rx_timeout  = 6U, /**< TX succeeded, RX timed out.         */
  k_ns_step_rx_word     = 7U, /**< RX returned a word.                 */
} ns_pingpong_step_t;

/**
 * @enum ns_pingpong_marker_t
 * @brief Sentinel written to ``g_ns_pingpong_entry_marker``.
 * @details Stamped AFTER the .bss zero so the bench can tell "BLXNS reached
 *          NS" apart from "the marker happened to land back at 0".
 * @invariant Not a plausible uninitialised-SRAM pattern.
 * @see ns_pingpong_step_t
 */
typedef enum : uint32_t {
  k_ns_entry_marker = 0xCAFEBABEUL, /**< NS entry was reached. */
} ns_pingpong_marker_t;

/**
 * @enum ns_ipc_clr_mask_t
 * @brief CLR register bits we use for init / status clear.
 *
 * @details
 *  - Bit 16 RST resets the FIFO + clears RDY/FULL.
 *  - Bits 7..0 (CLR7..CLR0) drop the eight IRQ status bits.
 *  - Bits 24/25 (RCLR/FCLR) drop the RERR/FERR FIFO error sticky bits.
 *  - HUM Ch 3.2.14 "IPC0CLR0" p 216-217.
 */
typedef enum : uint32_t {
  k_ns_ipc_clr_all = 0x030100FFUL, /**< RST | RCLR | FCLR | CLR7..CLR0. */
} ns_ipc_clr_mask_t;

/**
 * @enum ns_pingpong_magic_t
 * @brief Ping-pong handshake words exchanged between CPU0 and CPU1.
 */
typedef enum : uint32_t {
  k_ns_magic_ping = 0x1234U, /**< CPU0 -> CPU1 ping word. */
  k_ns_magic_pong = 0x4321U, /**< CPU1 -> CPU0 pong word. */
} ns_pingpong_magic_t;

/**
 * @enum ns_pingpong_poll_t
 * @brief Receive poll budget per ping-pong iteration.
 */
typedef enum : uint32_t {
  k_ns_recv_poll_max = 1000000UL, /**< Max iterations of the RDY spin. */
} ns_pingpong_poll_t;

/* =============================================================================
 * NS-side counters (placed in NS SRAM by the linker)
 * =============================================================================
 */

/**
 * @var g_ns_pingpong_match
 * @brief NS-side HIL liveness counter.
 *
 * @details Increment is one full CPU0->CPU1->CPU0 round-trip where CPU0
 * sent ``k_ns_magic_ping`` on channel 2 and CPU1 returned
 * ``k_ns_magic_pong`` on channel 0. Read externally over SWD; the
 * memprobe gate asserts strictly monotonic growth.
 *
 * @note Read by J-Link only. The S-side debugger sees NS SRAM through
 * the DAP regardless of attribution.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_ns_pingpong_match = 0U;

/**
 * @var g_ns_pingpong_mismatch
 * @brief NS-side HIL failure counter.
 *
 * @details Bumped whenever a CPU0 iteration could not complete -- RX
 * timed out after ``k_ns_recv_poll_max`` polls, or the returned word
 * was not the expected pong magic. Stays at 0 on a healthy bench.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_ns_pingpong_mismatch = 0U;

/**
 * @var g_ns_pingpong_step
 * @brief NS reset-handler progress tracker.
 *
 * @details
 *  - 0 = pre-NS / never reached
 *  - 1 = ns_reset_handler entry
 *  - 2 = channels reset
 *  - 3 = first send attempted
 *  - 4 = first round-trip completed (also bumps g_ns_pingpong_match)
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_ns_pingpong_step = 0U;

/**
 * @var g_ns_pingpong_last_rxd
 * @brief Most recent word read off the receive channel.
 *
 * @details Latched on every successful drain regardless of magic value,
 * so the bench can confirm "CPU1 is talking back" even if the magic
 * comparison disagrees with the expected pong word.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_ns_pingpong_last_rxd = 0U;

/**
 * @var g_ns_pingpong_entry_marker
 * @brief First word stamped by ``ns_reset_handler`` -- proves NS state
 *        actually entered the image (vs the S fallback path).
 *
 * @details If BLXNS in ``ra8_tz_secure_boot_jump_ns`` succeeded in
 * transferring control to ``ns_reset_handler``, this counter is
 * stamped to 0xCAFEBABE before any other NS work happens. If it
 * stays at 0 across a bench probe window, BLXNS failed to enter NS
 * state and the boot continued in S.
 *
 * @note Read externally by J-Link only.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_ns_pingpong_entry_marker = 0U;

/* =============================================================================
 * Inline register helpers
 * =============================================================================
 */

/**
 * @brief 32-bit MMIO write helper.
 * @param[in] addr Physical address.
 * @param[in] value Value to write.
 * @pre ``addr`` lies in a region the current SAU attribution permits.
 * @pre ``addr`` is 4-byte aligned.
 * @post One bus write of ``value`` issued, ordered after the previous
 *       store via the compiler ``memory`` clobber.
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static inline void ns_write32(uintptr_t addr, uint32_t value)
{
  *(volatile uint32_t*)addr = value;
}

/**
 * @brief 32-bit MMIO read helper.
 * @param[in] addr Physical address.
 * @return Value at ``addr``.
 * @pre ``addr`` is 4-byte aligned and reachable from NS.
 * @post Caller can act on the returned value (caller-side handles
 *       the volatile semantics).
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static inline uint32_t ns_read32(uintptr_t addr)
{
  return *(volatile uint32_t*)addr;
}

/**
 * @brief Reset one IPC channel's FIFO and clear all sticky status bits.
 *
 * @param[in] ch_base Base address of the channel (e.g. ``k_ns_ipc_ch0_addr``).
 *
 * @pre ``ch_base`` is one of the four channel windows documented in
 *      HUM Ch 3.2.10..3.2.14 p 214-216.
 * @pre IPCSAR has been programmed so the channel is NS-accessible.
 * @post Channel STA reports RDY=0, FULL=0, RERR=0, FERR=0, IRQ7..0=0.
 * @post FIFO contents discarded (any in-flight peer message lost; on
 *       a cold ping-pong bring-up the peer has not sent anything yet).
 *
 * @note Not thread-safe -- caller owns this channel.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static void ns_ipc_channel_reset(uintptr_t ch_base)
{
  /* HUM Ch 3.2.14 "IPC0CLR0" p 216 -- RST resets the FIFO and clears
   * RDY/FULL atomically; RCLR/FCLR clear the sticky FIFO error bits;
   * CLR7..CLR0 drop any latent IRQ event bits. */
  ns_write32(ch_base + (uintptr_t)k_ns_ipc_off_clr, (uint32_t)k_ns_ipc_clr_all);
}

/**
 * @brief Push one 32-bit word onto a channel's transmit FIFO.
 *
 * @param[in] ch_base Channel base address.
 * @param[in] msg     Word to push.
 *
 * @pre ``ch_base`` is owned by the caller (no concurrent sender).
 * @pre The peer has either drained the FIFO or is keeping up; we do
 *      not check FULL because the ping-pong handshake is depth-1.
 * @post One word added to the channel's transmit FIFO; the peer's
 *       STA.RDY rises one cycle later.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static void ns_ipc_send(uintptr_t ch_base, uint32_t msg)
{
  /* HUM Ch 3.2.12 "IPC0TXD0" p 215-216 -- writes push the word onto
   * the FIFO; FERR is raised separately and is cleared by
   * ns_ipc_channel_reset on init. */
  ns_write32(ch_base + (uintptr_t)k_ns_ipc_off_txd, msg);
}

/**
 * @brief Bounded RDY poll + single drain of a channel's receive FIFO.
 *
 * @param[in]  ch_base  Channel base address.
 * @param[out] out_word Receives the dequeued word on success.
 *
 * @return ``0`` on success, ``1`` on timeout.
 *
 * @pre ``out_word`` is non-NULL.
 * @pre Loop is bounded by ``k_ns_recv_poll_max`` (NASA P10 Rule 2).
 * @post On success ``*out_word`` holds the peer's word and STA.RDY
 *       has been cleared by the RXD read.
 * @post On timeout ``*out_word`` is unchanged.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static uint32_t ns_ipc_recv(uintptr_t ch_base, uint32_t* out_word)
{
  if (out_word == ((void*)0)) {
    return 1U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ns_recv_poll_max; ++i) {
    const uint32_t sta = ns_read32(ch_base + (uintptr_t)k_ns_ipc_off_sta);
    if ((sta & (uint32_t)k_ns_ipc_sta_rdy) != 0U) {
      /* HUM Ch 3.2.13 "IPC0RXD0" p 216 -- reading RXD pops one word
       * from the FIFO and re-evaluates RDY for the next iteration. */
      const uint32_t w = ns_read32(ch_base + (uintptr_t)k_ns_ipc_off_rxd);
      *out_word        = w;
      return 0U;
    }
  }
  return 1U;
}

/* =============================================================================
 * NS reset handler + main loop
 * =============================================================================
 */

/**
 * @brief NS world reset handler -- entry point of the BLXNS branch.
 *
 * @details
 * Initialises the two channels CPU0 owns (channel 0 for receive,
 * channel 2 for send), then loops ping->pong->bump-counter. The S-side
 * already released CPU1 via ``ra8_cpu1_release`` in the trustzone_init
 * path, so CPU1 is online by the time this handler runs.
 *
 * MSP_NS has been set by the BLXNS prologue in
 * ``ra8_tz_secure_boot_jump_ns`` from ``g_ra8_ns_vector_table[0]``, so
 * we do not touch the stack pointer here.
 *
 * BSS is implicitly zero (NS_BSS lives in MRAM at first power-on and
 * each warm reset re-flashes the image), so we do not zero it.
 *
 * @return Never returns.
 *
 * @pre BLXNS has placed the CPU in NS state.
 * @pre MSP_NS points at the top of the NS stack.
 * @post Round-trip loop running; ``g_ns_pingpong_match`` advances.
 *
 * @since 0.1.0
 */
extern uint32_t g_ra8_ls_ns_bss_start;
extern uint32_t g_ra8_ls_ns_bss_end;

[[gnu::section(".ns_text"), noreturn]] void ns_reset_handler(void);

[[gnu::section(".ns_text"), noreturn]] void ns_reset_handler(void)
{
  /* Zero the NS BSS region. NOLOAD means the linker does not populate
   * initial values from MRAM, and cold-boot SRAM contents are
   * undefined. The address comparison is via uintptr_t because the
   * two linker-provided extern symbols are different objects from a
   * static-analysis perspective even though the linker guarantees
   * they bracket a contiguous BSS run. */
  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t a = bss_start; a < bss_end; a += sizeof(uint32_t)) {
    *(volatile uint32_t*)a = 0U;
  }

  /* Stamp the entry marker AFTER the bss zero so the bench can tell
   * "BLXNS reached NS" apart from "marker happened to land back at 0". */
  g_ns_pingpong_entry_marker = (uint32_t)k_ns_entry_marker;

  g_ns_pingpong_step = (uint32_t)k_ns_step_bss_zeroed;

  /* Cold-init the CPU0-owned channels. */
  ns_ipc_channel_reset((uintptr_t)k_ns_ipc_ch0_addr);
  ns_ipc_channel_reset((uintptr_t)k_ns_ipc_ch2_addr);
  g_ns_pingpong_step = (uint32_t)k_ns_step_channels_up;

  for (;;) {
    g_ns_pingpong_step = (uint32_t)k_ns_step_tx_start;
    ns_ipc_send((uintptr_t)k_ns_ipc_ch2_addr, (uint32_t)k_ns_magic_ping);
    g_ns_pingpong_step = (uint32_t)k_ns_step_tx_done;

    uint32_t got = 0U;
    if (ns_ipc_recv((uintptr_t)k_ns_ipc_ch0_addr, &got) != 0U) {
      g_ns_pingpong_step = (uint32_t)k_ns_step_rx_timeout;
      g_ns_pingpong_mismatch += 1U;
      continue;
    }
    g_ns_pingpong_step     = (uint32_t)k_ns_step_rx_word;
    g_ns_pingpong_last_rxd = got;
    if (got != (uint32_t)k_ns_magic_pong) {
      g_ns_pingpong_mismatch += 1U;
      continue;
    }
    g_ns_pingpong_step = 4U;
    g_ns_pingpong_match += 1U;
  }
}

/* =============================================================================
 * NS vector table -- 8 slots at 0x02080000.
 *
 * Only the first two are meaningful for this image: initial MSP_NS and
 * the reset handler. The remaining slots are zero -- if any NS
 * exception fires the chip simply HardFaults, which the bench surfaces
 * via S-side fault diagnostics. The NS image does not enable any
 * interrupt source so this is acceptable.
 * =============================================================================
 */

extern uint32_t g_ra8_ls_ns_stack_top;

/**
 * @var g_ra8_ns_vector_table
 * @brief NS-world ARMv8-M vector table.
 *
 * @details Slot 0 = initial MSP_NS, slot 1 = reset entry (Thumb bit
 * set by the linker on function symbols), slots 2..7 reserved.
 *
 * @note Linker places this at 0x02080000 via the ``.ns_vectors`` section.
 *
 * @since 0.1.0
 */
[[gnu::section(".ns_vectors"), gnu::used]] const uint32_t g_ra8_ns_vector_table[8] = {
  (uint32_t)(uintptr_t)&g_ra8_ls_ns_stack_top, /**< [0] Initial MSP_NS. */
  (uint32_t)(uintptr_t)&ns_reset_handler,      /**< [1] Reset handler.  */
  0U,                                          /**< [2] NMI.            */
  0U,                                          /**< [3] HardFault.      */
  0U,                                          /**< [4] MemManage.      */
  0U,                                          /**< [5] BusFault.       */
  0U,                                          /**< [6] UsageFault.     */
  0U,                                          /**< [7] SecureFault.    */
};
