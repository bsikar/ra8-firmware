/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/src/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the e-reader-on-M33 demo
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It demonstrates the #150 power-saving model: an e-reader spends
 * almost all its time idle on a rendered page, so handing that held page to the
 * M33 @ 250 MHz -- while the M85 @ 1 GHz sleeps -- is the high-leverage battery
 * win. "Power saving = drop to the slow core."
 *
 * What the M85 does here:
 *   1. Publishes the progress mailbox (see `ereader_m33.h`) and stamps its magic
 *      so the M33 trusts it, and arms the IPC0 receive IRQ so a later page-turn
 *      poke from the M33 can wake it out of WFI.
 *   2. Releases the Cortex-M33 with `ra8_cpu1_release` (HUM Ch 2.9.1) into the
 *      reader, confirms it booted (signature), then waits for the M33 to render
 *      and publish one held page.
 *   3. Reads back the M33's published framebuffer descriptor -- SDRAM base,
 *      RGB565 geometry, glyph count and the CRC-32 the M33 folded over the
 *      rendered pixels -- validates it, and logs a single deterministic banner
 *      "ereader_m33: rgb565 256x64 sdram crc=<8 hex> PASS". ra8_emulator echoes only
 *      the primary core's ITM, so the M85 speaks for the M33.
 *   4. Runs the #150 MODE-SWITCH cycle ::k_erm33_max_turns times: it PARKS --
 *      writes the CGC clock-gate (an LPM clock-stop) and drops into Sleep-mode
 *      WFI -- handing the live core to the slow M33, which holds the page and
 *      polls a (fake) touch. On a page turn the M33 pokes IPC0 and the M85
 *      wakes, does the "heavy" next-page work the 1 GHz core owns, acknowledges,
 *      and re-parks. It logs the handoff verdict, then parks for good.
 *
 * Why the M85 reports the M33's CRC rather than re-CRCing the framebuffer: on the
 * ra8_emulator the two cores share only the on-chip SRAM mailbox; each
 * core's external-SDRAM window is a separate mapping, so the parked M85 cannot
 * read the bytes the M33 wrote at 0x68000000. The M33 reads its own SDRAM
 * framebuffer back to fold the CRC (the proof the pixels landed) and publishes it
 * through the shared mailbox; the M85 narrates that value. On silicon the single
 * physical SDRAM is shared, so an M85 re-read would match.
 *
 * @note `ra8_log_info` is compiled to a no-op unless the build defines INFO-level
 *       logging (a Debug build). `just apps::emulator::run ereader_m33` builds
 *       Debug so `[itm]`
 *       lines appear; a release build runs the same logic but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ereader_m33.h"
#include "ra8_attributes.h"
#include "ra8_boot_entry.h"
#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_ipc.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_lpm.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra8_ls_cpu1_stack_top;

/**
 * @enum m85_poll_t
 * @brief Bounded iteration limits for the M85 polling loops.
 * @details Large enough that a normally-running M33 always completes within
 *          budget, yet finite so the M85 never hangs if the M33 does not boot.
 *          ra8_emulator runs cpu0 in 500k-instruction chunks and cpu1 in 100k
 *          chunks between them, so the M33's render lands in a few dozen
 *          interleaves -- far inside these budgets.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_sig_poll_budget  = 10000000UL,  /**< Max iters waiting for M33 signature. */
  k_m85_done_poll_budget = 200000000UL, /**< Max iters waiting for M33 done flag. */
} m85_poll_t;

/**
 * @enum m85_banner_t
 * @brief Capacity of the verdict banner the M85 assembles in a stack buffer.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_banner_cap = 96U, /**< Bytes reserved for the assembled verdict line. */
} m85_banner_t;

/**
 * @enum m85_hex_t
 * @brief Constants for formatting a 32-bit value as 8 uppercase hex digits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_hex_nibbles = 8U,    /**< Hex digits in a 32-bit value. */
  k_nibble_bits = 4U,    /**< Bits per hex nibble.          */
  k_nibble_mask = 0x0FU, /**< Low-nibble mask.              */
} m85_hex_t;

/**
 * @enum m85_dec_t
 * @brief Constants for formatting a 32-bit value as decimal digits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dec_digits_max = 10U, /**< Decimal digits in a 32-bit value (4294967295). */
  k_dec_radix      = 10U, /**< Base-10 radix for the digit extraction.        */
} m85_dec_t;

/**
 * @enum m85_work_t
 * @brief Bound for the M85's deterministic "heavy next-page work" stand-in.
 * @details The on-wake compute the 1 GHz core owns on a page turn (pagination,
 *          decompression) is modelled here as a fixed-count integer fold so the
 *          ra8_emulator cycle stays deterministic; the bound keeps it short.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_heavy_work_iters = 256U, /**< Iterations of the placeholder next-page fold. */
} m85_work_t;

/**
 * @brief Append a NUL-terminated string into a bounded banner buffer.
 *
 * @param[in,out] dst Destination buffer (never NULL).
 * @param[in,out] off In/out write offset; advanced by the bytes copied.
 * @param[in]     cap Capacity of @p dst (one byte reserved for the final NUL).
 * @param[in]     src Source string (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p dst, @p off and @p src are non-NULL; `*off < cap`.
 * @pre @p cap is the true size of @p dst.
 * @post `*off <= cap - 1` and `dst[*off]` is the terminating NUL.
 * @post Iteration bounded by @p cap (NASA Rule 2).
 *
 * @note Single-threaded; boot context only.
 * @since 0.1.0
 */
static void banner_append(char* dst, uint32_t* off, uint32_t cap, const char* src)
{
  if (dst == nullptr) {
    return;
  }
  if (off == nullptr) {
    return;
  }
  if (src == nullptr) {
    return;
  }
  RA8_LOOP_BOUND(k_banner_cap);
  for (uint32_t i = 0U; i < cap; i++) {
    const char c = src[i];
    if (c == '\0') {
      break;
    }
    if (*off >= (cap - 1U)) {
      break;
    }
    dst[*off] = c;
    *off += 1U;
  }
  dst[*off] = '\0';
}

/**
 * @brief Append @p value as 8 uppercase hex digits into the banner buffer.
 *
 * @param[in,out] dst   Destination buffer (never NULL).
 * @param[in,out] off   In/out write offset; advanced by up to 8.
 * @param[in]     cap   Capacity of @p dst.
 * @param[in]     value 32-bit value to format big-endian (MSB nibble first).
 *
 * @return Nothing.
 *
 * @pre @p dst and @p off are non-NULL; `*off < cap`.
 * @pre @p cap is the true size of @p dst.
 * @post `*off <= cap - 1`.
 * @post Iteration bounded by ::k_hex_nibbles (NASA Rule 2).
 *
 * @note Single-threaded; boot context only.
 * @since 0.1.0
 */
static void banner_append_hex(char* dst, uint32_t* off, uint32_t cap, uint32_t value)
{
  if (dst == nullptr) {
    return;
  }
  if (off == nullptr) {
    return;
  }
  static const char digits[] = "0123456789ABCDEF";
  RA8_LOOP_BOUND(k_hex_nibbles);
  for (uint32_t i = 0U; i < (uint32_t)k_hex_nibbles; i++) {
    if (*off >= (cap - 1U)) {
      break;
    }
    const uint32_t shift  = ((uint32_t)k_hex_nibbles - 1U - i) * (uint32_t)k_nibble_bits;
    const uint32_t nibble = (value >> shift) & (uint32_t)k_nibble_mask;
    dst[*off]             = digits[nibble];
    *off += 1U;
  }
}

/**
 * @brief Append @p value as decimal digits (no leading zeros) into the banner.
 *
 * @details Extracts the decimal digits least-significant first into a small
 * scratch array, then copies them back most-significant first. A lone 0 prints
 * as "0". Both passes are bounded by ::k_dec_digits_max (NASA Rule 2).
 *
 * @param[in,out] dst   Destination buffer (never NULL).
 * @param[in,out] off   In/out write offset; advanced by the digits copied.
 * @param[in]     cap   Capacity of @p dst.
 * @param[in]     value 32-bit value to format in base 10.
 *
 * @return Nothing.
 *
 * @pre @p dst and @p off are non-NULL; `*off < cap`.
 * @pre @p cap is the true size of @p dst.
 * @post `*off <= cap - 1`.
 * @post Both loops are bounded by ::k_dec_digits_max (NASA Rule 2).
 *
 * @note Single-threaded; boot context only.
 * @since 0.1.0
 */
static void banner_append_u32(char* dst, uint32_t* off, uint32_t cap, uint32_t value)
{
  if (dst == nullptr) {
    return;
  }
  if (off == nullptr) {
    return;
  }
  char     tmp[k_dec_digits_max];
  uint32_t count = 0U;
  uint32_t v     = value;
  RA8_LOOP_BOUND(k_dec_digits_max);
  for (uint32_t i = 0U; i < (uint32_t)k_dec_digits_max; i++) {
    tmp[count] = (char)('0' + (char)(v % (uint32_t)k_dec_radix));
    count += 1U;
    v /= (uint32_t)k_dec_radix;
    if (v == 0U) {
      break;
    }
  }
  RA8_LOOP_BOUND(k_dec_digits_max);
  for (uint32_t i = 0U; i < count; i++) {
    if (*off >= (cap - 1U)) {
      break;
    }
    dst[*off] = tmp[count - 1U - i];
    *off += 1U;
  }
}

/**
 * @brief Publish the shared mailbox and stamp its magic before release.
 *
 * @param[out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre Called before `ra8_cpu1_release` so the M33 sees a live mailbox.
 * @post All progress fields read back as 0 and `status` is `running`.
 * @post `magic` holds ::k_erm33_magic after a `dsb` published the zeros first.
 *
 * @note Single owner (M85) at this point; no concurrency.
 * @since 0.1.0
 */
static void prep_mailbox(volatile erm33_mailbox_t* mb)
{
  mb->magic       = 0U;
  mb->m33_sig     = 0U;
  mb->status      = (uint32_t)k_erm33_status_running;
  mb->fb_base     = 0U;
  mb->fb_width    = 0U;
  mb->fb_height   = 0U;
  mb->fb_stride   = 0U;
  mb->fb_format   = 0U;
  mb->fb_crc      = 0U;
  mb->glyph_count = 0U;
  mb->done        = 0U;
  mb->turn_req    = 0U;
  mb->turn_ack    = 0U;
  mb->turn_done   = 0U;
  __asm volatile("dsb" ::: "memory");
  mb->magic = (uint32_t)k_erm33_magic;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Poll the mailbox until the M33 stamps its boot signature.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether the M33 boot signature appeared within budget.
 * @retval true  `m33_sig == k_erm33_m33_sig` within ::k_m85_sig_poll_budget.
 * @retval false Poll budget exhausted before the signature appeared.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M85 has already called `ra8_cpu1_release`.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_sig_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_m33_sig(const volatile erm33_mailbox_t* mb)
{
  RA8_LOOP_BOUND(k_m85_sig_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_sig_poll_budget; i++) {
    if (mb->m33_sig == (uint32_t)k_erm33_m33_sig) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Poll the mailbox until the M33 publishes its held page (`done`).
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether `done` reached 1 within budget.
 * @retval true  `done == 1` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before done was set.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M33 has been confirmed alive via ::wait_for_m33_sig.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_done(const volatile erm33_mailbox_t* mb)
{
  RA8_LOOP_BOUND(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    if (mb->done == 1U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Validate the M33's published held-page descriptor.
 *
 * @details Confirms the render succeeded and that every published field is
 * self-consistent: the framebuffer base lies inside the SDRAM window, the
 * geometry equals ::erm33_fb_geom_t, the format is RGB565, and the glyph count
 * and CRC are non-zero (a blank or absent render publishes neither).
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether the M33 published a valid held page.
 * @retval true  status ok and every descriptor field is consistent.
 * @retval false Any check failed.
 *
 * @pre @p mb is the fixed-address mailbox pointer and `done == 1`.
 * @pre A `dsb` has ordered the M33's publish before the M85's read.
 * @post No mailbox field is modified.
 * @post The return value gates the PASS / FAIL banner.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool verify_page(const volatile erm33_mailbox_t* mb)
{
  if (mb->status != (uint32_t)k_erm33_status_ok) {
    return false;
  }
  if ((mb->fb_base < (uint32_t)k_erm33_sdram_base) ||
      (mb->fb_base >= (uint32_t)k_erm33_sdram_end)) {
    return false;
  }
  if ((mb->fb_width != (uint32_t)k_erm33_fb_width) ||
      (mb->fb_height != (uint32_t)k_erm33_fb_height)) {
    return false;
  }
  if (mb->fb_stride != (uint32_t)k_erm33_fb_stride) {
    return false;
  }
  if (mb->fb_format != (uint32_t)k_erm33_fb_format_rgb565) {
    return false;
  }
  if ((mb->glyph_count == 0U) || (mb->fb_crc == 0U)) {
    return false;
  }
  return true;
}

/**
 * @brief Assemble and log the single deterministic verdict banner.
 *
 * @param[in] crc  The CRC-32 the M33 published over its rendered pixels.
 * @param[in] pass Whether ::verify_page accepted the held page.
 *
 * @return Nothing.
 *
 * @pre `ra8_log_init` has run (the banner reaches ITM in a Debug build).
 * @pre @p crc is the value read from the mailbox.
 * @post Exactly one banner line is emitted.
 * @post No shared state is modified.
 *
 * @note The ra8_emulator gate greps this line for `crc=<hex>` and the verdict word.
 * @since 0.1.0
 */
static void emit_verdict(uint32_t crc, bool pass)
{
  char     buf[k_banner_cap];
  uint32_t off = 0U;
  banner_append(buf, &off, (uint32_t)k_banner_cap, "ereader_m33: rgb565 256x64 sdram crc=");
  banner_append_hex(buf, &off, (uint32_t)k_banner_cap, crc);
  banner_append(buf, &off, (uint32_t)k_banner_cap, pass ? " PASS" : " FAIL");
  ra8_log_info("M85", buf);
}

/**
 * @enum m85_ipc_t
 * @brief IPC channel the M85 watches for the M33's page-turn wake.
 * @details The M33 (CPU1) pokes IPC0 channel 0 (the CPU1 -> CPU0 receive
 *          direction); the M85 arms this channel's IRQ-line-0 receive event so a
 *          page-turn poke wakes it out of Sleep-mode WFI -- the same #149 wake
 *          path the sibling `compile_on_m33` driver uses for compile-done.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ipc_wake_channel = 0U, /**< IPC0 channel 0 (CPU1 -> CPU0 receive side). */
} m85_ipc_t;

/**
 * @var s_m33_woke
 * @brief Set by the IPC0 receive callback when the M33 signals a page turn.
 * @details Diagnostic wake hint only; the authoritative exit condition for the
 *          WFI wait remains the mailbox `turn_req` field, which the M33 orders
 *          (with a `dsb`) ahead of its IPC poke, so a wake always observes a
 *          settled request.
 * @note Written from IPC IRQ context, read in thread context -- hence volatile.
 * @warning Not the completion signal; it only proves the wake fired.
 * @since 0.1.0
 */
static volatile bool s_m33_woke;

/**
 * @brief IPC0 receive event callback: record that a page-turn poke arrived.
 *
 * @param[in] ctx      Unused registration context.
 * @param[in] channel  Channel that fired (always ::k_ipc_wake_channel here).
 * @param[in] event_id IRQ line that fired (always line 0 here).
 *
 * @return Nothing.
 *
 * @pre Attached to IPC channel 0 IRQ line 0 via `ra8_ipc_attach_event_handler`.
 * @pre Runs in IPC IRQ handler context (invoked from `ra8_ipc_dispatch`).
 * @post ::s_m33_woke reads true.
 * @post No other state is modified.
 *
 * @note ISR context; touches only the one volatile flag.
 * @since 0.1.0
 */
static void ipc_wake_handler(void* ctx, uint8_t channel, ra8_ipc_irq_event_id_t event_id)
{
  (void)ctx;
  (void)channel;
  (void)event_id;
  s_m33_woke = true;
}

/**
 * @brief IPC0 receive ISR trampoline: decode the channel's pending events.
 *
 * @param[in] ctx Unused registration context.
 *
 * @return Nothing.
 *
 * @pre Registered for ::k_ra8_ipc_elc_event_irq0 via `ra8_isr_register`.
 * @pre The NVIC line for the IPC0 receive event is enabled.
 * @post Any pending IPC0 channel-0 IRQ line has been dispatched and cleared.
 * @post ::ipc_wake_handler has run for each pending line.
 *
 * @note Runs in NVIC handler-mode; forwards to the HAL dispatcher.
 * @since 0.1.0
 */
static void ipc0_receive_isr(void* ctx)
{
  (void)ctx;
  ra8_ipc_dispatch((uint8_t)k_ipc_wake_channel);
}

/**
 * @brief Arm the IPC0 receive interrupt so the M85 can WFI-wake on a page turn.
 *
 * @details Initialises the ISR substrate, configures IPC0 channel 0 for the
 * IRQ-line-0 event, attaches ::ipc_wake_handler, routes the
 * ::k_ra8_ipc_elc_event_irq0 ELC event to ::ipc0_receive_isr through the NVIC,
 * then unmasks interrupts globally. Each step is its own guarded return so the
 * failing stage is unambiguous (no compound decisions).
 *
 * @return Whether the wake path was fully armed.
 * @retval true  IPC0 RX IRQ is routed, attached, and interrupts are enabled.
 * @retval false A setup stage failed; the caller falls back to a bounded poll.
 *
 * @pre Called once during M85 bring-up, before `ra8_cpu1_release`.
 * @pre Interrupts are not yet globally enabled.
 * @post On true, an IPC0 channel-0 poke vectors into ::ipc0_receive_isr.
 * @post On true, PRIMASK is clear (interrupts globally enabled).
 *
 * @note Single-threaded boot context; not reentrant.
 * @since 0.1.0
 */
static bool arm_ipc_wake(void)
{
  if (ra8_isr_init() != k_ra8_ok) {
    return false;
  }
  const ra8_ipc_config_t cfg = {
    .channel      = (uint8_t)k_ipc_wake_channel,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_irq0,
  };
  if (ra8_ipc_init(&cfg) != k_ra8_ok) {
    return false;
  }
  if (ra8_ipc_attach_event_handler((uint8_t)k_ipc_wake_channel,
                                   k_ra8_ipc_irq_event_0,
                                   ipc_wake_handler,
                                   nullptr) != k_ra8_ok) {
    return false;
  }
  if (ra8_isr_register((ra8_elc_event_t)k_ra8_ipc_elc_event_irq0,
                       ipc0_receive_isr,
                       nullptr,
                       (uint8_t)k_ra8_isr_prio_default,
                       nullptr) != k_ra8_ok) {
    return false;
  }
  ra8_isr_globals_enable();
  return true;
}

/**
 * @brief Configure the LPM block once so a plain WFI is a CPU Sleep.
 *
 * @details Unlocks PRCR.PRC1, writes SBYCR / DPSBYCR / SSCR1 and clears LPSCR to
 * System-Active via `ra8_lpm_init`, then re-locks PRCR. With LPMD = 0 the M85's
 * WFI is an ordinary CPU Sleep -- peripherals (and the M33) keep their clocks, so
 * the armed IPC0 receive IRQ still wakes the core. Deeper modes (Software
 * Standby) would stop the M33 too, which is wrong for this hand-off cycle.
 *
 * @return Nothing.
 *
 * @pre Called once during M85 bring-up, single-threaded.
 * @pre The ra8_lpm HAL owns the HUM citations for these SYSC writes.
 * @post LPSCR.LPMD == 0 (the next WFI is a plain CPU Sleep).
 * @post PRCR.PRC1 is re-locked.
 *
 * @note ra8_emulator routes these SYSC writes to its catch-all (no fault); on
 *       silicon they are the real LPM register writes.
 * @since 0.1.0
 */
static void m85_lpm_configure(void)
{
  if (ra8_lpm_prcr_unlock() != k_ra8_ok) {
    return;
  }
  const ra8_lpm_config_t cfg = {
    .opa_bus_keep = true,
  };
  (void)ra8_lpm_init(&cfg);
  (void)ra8_lpm_prcr_relock();
}

/**
 * @brief Gate or restore the HOCO via the LPM clock-stop matrix (CGC OCR write).
 *
 * @details The M85 e-reader runs on its boot clock and never needs the
 * high-speed on-chip oscillator while it is parked, so the park writes
 * HOCOCR.HCSTP through `ra8_lpm_set_clock_stop` to model the power drop, and the
 * wake clears it again. This is the "CGC clock-gate / down-clock" register write
 * the #150 model calls for: real on silicon, routed to ra8_emulator's catch-all in
 * the emulator so it neither faults nor changes the run.
 *
 * @param[in] stop true -- gate the HOCO (park); false -- restore it (wake).
 *
 * @return Nothing.
 *
 * @pre Called from single-threaded main context with the M33 already released.
 * @pre The ra8_lpm HAL owns the HUM citation for the OCR write.
 * @post On a successful PRCR unlock, HOCOCR.HCSTP reflects @p stop.
 * @post PRCR.PRC1 is re-locked.
 *
 * @note Not thread-safe; the unlock / write / relock is one critical section.
 * @since 0.1.0
 */
static void m85_gate_hoco(bool stop)
{
  if (ra8_lpm_prcr_unlock() != k_ra8_ok) {
    return;
  }
  (void)ra8_lpm_set_clock_stop(k_ra8_lpm_clock_hoco, stop);
  (void)ra8_lpm_prcr_relock();
}

/**
 * @brief Park in Sleep-mode WFI until the M33 requests page turn @p turn.
 *
 * @details Each iteration checks the mailbox `turn_req` and, if the requested
 * turn has not yet arrived, issues a `wfi`. The M33's page-turn poke of IPC0ISET0
 * raises the armed IPC0 receive interrupt (see ::arm_ipc_wake), which wakes the
 * core; the loop then observes the `turn_req` the M33 ordered ahead of the poke.
 * The bounded count is the NASA Rule 2 backstop for a missed wake.
 *
 * @param[in] mb   Pointer to the shared mailbox (never NULL).
 * @param[in] turn The page-turn number this park is waiting for (1-based).
 *
 * @return Whether `turn_req` reached @p turn within budget.
 * @retval true  `turn_req >= turn` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before the request arrived.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre ::arm_ipc_wake armed the IPC0 receive IRQ and enabled interrupts.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note The M85 sleeps in WFI between wakes -- this is the low-power yield point.
 * @since 0.1.0
 */
static bool m85_wait_turn(const volatile erm33_mailbox_t* mb, uint32_t turn)
{
  RA8_LOOP_BOUND(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    if (mb->turn_req >= turn) {
      return true;
    }
    __asm volatile("wfi" ::: "memory");
  }
  return false;
}

/**
 * @brief Deterministic stand-in for the M85's heavy next-page work.
 *
 * @details Folds a fixed ::k_heavy_work_iters-count integer sum seeded by the
 * turn number. On a real e-reader this is where the 1 GHz core does the genuinely
 * heavy page work -- paginating / decompressing the next chapter -- that justifies
 * waking it from the slow-core hold. Here it is a bounded, side-effect-free
 * computation so the ra8_emulator cycle stays byte-deterministic; the rendered page
 * content is unchanged, so the framebuffer CRC the M33 re-folds stays stable.
 *
 * @param[in] turn The page-turn number being serviced (the work seed).
 *
 * @return A deterministic fold of @p turn (logged as proof the work ran).
 * @retval (value) `turn + sum(0 .. k_heavy_work_iters-1)`.
 *
 * @pre @p turn is a 1-based page-turn index.
 * @pre Runs in thread mode on the woken M85, interrupts enabled.
 * @post No shared state is modified.
 * @post Iteration count bounded by ::k_heavy_work_iters (NASA Rule 2).
 *
 * @note Pure; placeholder for the real pagination work (a HIL follow-up).
 * @since 0.1.0
 */
static uint32_t m85_heavy_work(uint32_t turn)
{
  uint32_t acc = turn;
  RA8_LOOP_BOUND(k_heavy_work_iters);
  for (uint32_t i = 0U; i < (uint32_t)k_heavy_work_iters; i++) {
    acc += i;
  }
  return acc;
}

/**
 * @brief Bounded-poll the mailbox until the M33 republishes through @p turn.
 *
 * @details After the M85 acknowledges the final turn the M33 re-renders and sets
 * `turn_done`; the M85 reads that back to narrate the cycle verdict. The M33 does
 * not poke IPC for `turn_done` (only for `turn_req`), so this is a plain bounded
 * poll, not a WFI wait -- the M33 is actively re-rendering, so it settles quickly.
 *
 * @param[in] mb   Pointer to the shared mailbox (never NULL).
 * @param[in] turn The re-render generation to wait for (1-based).
 *
 * @return Whether `turn_done` reached @p turn within budget.
 * @retval true  `turn_done >= turn` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before the re-render published.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M85 has acknowledged @p turn via `turn_ack`.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded main context.
 * @since 0.1.0
 */
static bool m85_wait_turn_done(const volatile erm33_mailbox_t* mb, uint32_t turn)
{
  RA8_LOOP_BOUND(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    if (mb->turn_done >= turn) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Run the #150 mode-switch cycle: park, wake on a page turn, repeat.
 *
 * @details For each of ::k_erm33_max_turns turns the M85 gates the HOCO and parks
 * in Sleep-mode WFI (::m85_wait_turn), wakes on the M33's IPC poke, restores the
 * HOCO, does the heavy next-page work (::m85_heavy_work), and acknowledges by
 * writing `turn_ack` behind a `dsb` so the M33 sees a settled ack before it
 * re-renders. A wake-timeout aborts the cycle. The outer loop is bounded by
 * ::k_erm33_max_turns (NASA Rule 2).
 *
 * @param[in,out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether every page turn was serviced and acknowledged.
 * @retval true  All ::k_erm33_max_turns turns were woken and acked.
 * @retval false @p mb was NULL, or a park timed out before a request arrived.
 *
 * @pre @p mb is the fixed-address mailbox pointer with the first page held.
 * @pre ::arm_ipc_wake and ::m85_lpm_configure have run.
 * @post On true, `turn_ack == k_erm33_max_turns`.
 * @post The HOCO is restored (un-gated) on return.
 *
 * @note Single-threaded; the WFI inside ::m85_wait_turn is the yield point.
 * @since 0.1.0
 */
static bool run_handoff_cycle(volatile erm33_mailbox_t* mb)
{
  if (mb == nullptr) {
    return false;
  }
  RA8_LOOP_BOUND(k_erm33_max_turns);
  for (uint32_t turn = 1U; turn <= (uint32_t)k_erm33_max_turns; turn++) {
    m85_gate_hoco(true);
    const bool woke = m85_wait_turn(mb, turn);
    m85_gate_hoco(false);
    if (!woke) {
      ra8_log_info_val("M85", "page-turn wake timed out at turn", turn);
      return false;
    }
    [[maybe_unused]] const uint32_t work = m85_heavy_work(turn);
    mb->turn_ack                         = turn;
    __asm volatile("dsb" ::: "memory");
    ra8_log_info_val("M85", "woke from low-power for page turn", turn);
    ra8_log_info_val("M85", "M85 heavy next-page work result", work);
  }
  return true;
}

/**
 * @brief Assemble and log the single deterministic handoff-cycle verdict banner.
 *
 * @details Emits "ereader_m33: handoff turns=<n> crc=<8 hex> PARKED" on success,
 * narrating the M33's re-render count (`turn_done`) and the stable framebuffer
 * CRC the M33 re-folded on every page turn. The ra8_emulator gate greps this line.
 *
 * @param[in] mb   Pointer to the shared mailbox (never NULL).
 * @param[in] pass Whether the full cycle completed and the re-render published.
 *
 * @return Nothing.
 *
 * @pre `ra8_log_init` has run (the banner reaches ITM in a Debug build).
 * @pre @p mb is the fixed-address mailbox pointer.
 * @post Exactly one banner line is emitted.
 * @post No shared state is modified.
 *
 * @note The gate asserts both this banner and the page-0 verdict from
 *       ::emit_verdict to prove the whole park / wake / re-render cycle ran.
 * @since 0.1.0
 */
static void emit_cycle_verdict(const volatile erm33_mailbox_t* mb, bool pass)
{
  if (mb == nullptr) {
    return;
  }
  char     buf[k_banner_cap];
  uint32_t off = 0U;
  banner_append(buf, &off, (uint32_t)k_banner_cap, "ereader_m33: handoff turns=");
  banner_append_u32(buf, &off, (uint32_t)k_banner_cap, mb->turn_done);
  banner_append(buf, &off, (uint32_t)k_banner_cap, " crc=");
  banner_append_hex(buf, &off, (uint32_t)k_banner_cap, mb->fb_crc);
  banner_append(buf, &off, (uint32_t)k_banner_cap, pass ? " PARKED" : " FAIL");
  ra8_log_info("M85", buf);
}

/**
 * @brief Run the #150 mode-switch and log its verdict.
 *
 * @details Drives the park / wake / re-render cycle (::run_handoff_cycle), waits
 * for the M33's final re-render (::m85_wait_turn_done), and emits the handoff
 * verdict. Each guard is its own `if` (no compound boolean decision, so no MC/DC
 * obligation); the verdict is true only when every turn completed and the M33
 * republished a non-zero CRC.
 *
 * @param[in,out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer with the first page held.
 * @pre ::arm_ipc_wake and ::m85_lpm_configure have run.
 * @post Exactly one handoff verdict banner is emitted.
 * @post On a full cycle, `turn_done == k_erm33_max_turns`.
 *
 * @note Single-threaded; the WFI inside the cycle is the low-power yield point.
 * @since 0.1.0
 */
static void run_mode_switch(volatile erm33_mailbox_t* mb)
{
  if (mb == nullptr) {
    return;
  }
  ra8_log_info("M85", "entering #150 mode-switch cycle; M33 holds the page + polls touch");
  const bool cycled  = run_handoff_cycle(mb);
  bool       done_ok = false;
  if (cycled) {
    done_ok = m85_wait_turn_done(mb, (uint32_t)k_erm33_max_turns);
  }
  bool verdict = false;
  if (done_ok) {
    if (mb->fb_crc != 0U) {
      verdict = true;
    }
  }
  emit_cycle_verdict(mb, verdict);
}

/**
 * @brief Park the M85 in low-power WFI after the M33 owns the page.
 *
 * @return This function never returns.
 * @note The M85 sleeps until power-off (or a future M33 wake IRQ).
 *
 * @pre The handoff cycle has completed and the verdict was logged.
 * @pre The verdict banner has already been emitted.
 * @post The HOCO is gated and the M85 spends its time in WFI (the power win).
 * @post No mailbox field is modified.
 *
 * @note The final park gates the clock once and WFIs for good; the held page now
 *       lives on the M33 with no further page turns scripted.
 * @since 0.1.0
 */
[[noreturn]] static void park_low_power(void)
{
  ra8_log_info("M85", "M85 parked in low-power WFI; M33 holds the page");
  m85_gate_hoco(true);
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A fatal error (e.g. CPU1 release failure) has occurred and been logged.
 * @post The M85 makes no further forward progress.
 * @post No mailbox state changes.
 *
 * @note Distinct from ::park_low_power: this is an error halt, not low power.
 * @since 0.1.0
 */
[[noreturn]] static void park_forever(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief CPU0 (Cortex-M85) application entry.
 *
 * @details Publishes the mailbox, arms the IPC0 wake and configures the LPM
 * block, releases the Cortex-M33 into the reader, waits for the first held page,
 * logs the page-0 verdict, then runs the #150 mode-switch cycle -- parking in
 * low-power WFI and waking on the M33's page-turn pokes -- before logging the
 * handoff verdict and parking for good. See the file header for the narrative.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held inactive by hardware until released here.
 * @post The M33 has rendered + re-rendered the held page and the M85 is parked.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 ereader_m33 demo (#150 M85-park / M33-hold) ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");
  ra8_log_info("M85", "M33 renders a held page into external SDRAM (0x68000000)");

  volatile erm33_mailbox_t* mb = erm33_mailbox();
  prep_mailbox(mb);

  /* Arm the IPC0 receive IRQ + configure the LPM block BEFORE releasing the M33,
   * so the page-turn wake path and the Sleep-mode posture are ready the moment
   * the M33 starts running. A wake-arm failure is non-fatal: m85_wait_turn still
   * bounds-polls the mailbox (its WFI then just sleeps to the next interrupt). */
  if (arm_ipc_wake()) {
    ra8_log_info("M85", "IPC0 receive IRQ armed -- M85 can WFI-wake on a page turn");
  } else {
    ra8_log_info("M85", "IPC wake arm failed -- page-turn WFI falls back to bounded poll");
  }
  m85_lpm_configure();

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_sig(mb)) {
    ra8_log_info("M85", "M33 reader is alive");
  } else {
    ra8_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra8_log_info("M85", "M85 idle; waiting for the M33 to render the held page ...");
  if (!wait_for_done(mb)) {
    ra8_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }
  __asm volatile("dsb" ::: "memory");

  ra8_log_info_val("M85", "M33 published fb_base", mb->fb_base);
  ra8_log_info_val("M85", "M33 held-page glyphs", mb->glyph_count);
  ra8_log_info_val("M85", "M33 held-page pixels CRC32 (decimal)", mb->fb_crc);
  emit_verdict(mb->fb_crc, verify_page(mb));

  /* #150 mode-switch: hand the live core to the slow M33, park, and wake on each
   * scripted page turn to do the heavy next-page work, then park for good. */
  run_mode_switch(mb);
  park_low_power();
}
