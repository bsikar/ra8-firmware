/**
 * @file examples/ek_ra8d2/hw_validated/hil/dtc_coherency_hil/main.c
 * @brief DTC vector-table / TI coherency HIL proof with the M85 D-cache ENABLED
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85) hardware-in-the-loop test that proves the DTC
 * cache-coherency wiring keeps a software-triggered DTC mem-to-mem copy correct
 * while the M85 L1 D-cache is turned ON.
 *
 * The Data Transfer Controller fetches its **DTC vector table** -- and, one
 * indirection on, each per-source **Transfer Information (TI)** block -- straight
 * from SRAM at activation. With the D-cache enabled the CPU's writes to those
 * descriptors (the vector-table entry and the TI fields) sit dirty in cache where
 * the engine cannot see them, so a missing clean would let the DTC fetch stale
 * descriptors and transfer the wrong data, or nothing at all. The HAL
 * ``ra8_dtc_enable()`` now cleans the caller-populated vector table back to RAM
 * before ``DTCST = 1`` (commit b80c247b); the DTC is direction-blind (like the
 * DMAC), so this app -- as the owning driver -- cleans the TI block and the
 * source buffer and invalidates the destination buffer itself.
 *
 * This app builds with ``RA8_BOOT_ENABLE_CACHE_MPU`` (set in its CMakeLists.txt).
 * With that flag the shared boot (``libs/ra8_board_ek_ra8d2/boot/system_init.c``)
 * brings up the MPU (5 regions) + I-cache + D-cache + branch predictor before
 * ``main()`` runs. The vector table, the TI block, and both data buffers live in
 * MPU **region 1** (``0x22000000``, M85-private **cacheable** SRAM), so the cache
 * hazard is real on silicon.
 *
 * Sequence (one-shot, then WFI):
 *   1. ``ra8_cgc_init()`` + ``ra8_mstp_init()`` + ``ra8_isr_init()`` +
 *      ``ra8_elc_init()`` + the SCI8 J-Link OB console + the two LEDs.
 *   2. ``ra8_dtc_init(s_dtc_vt)`` programmes ``DTCVBR``; ``ra8_isr_register()``
 *      allocates an IELSR slot for ELC software event 0 (its slot index is the
 *      DTC activation-source vector number).
 *   3. Fill ``s_src`` with a deterministic pattern (M85 stores -> **dirty in the
 *      D-cache**) and ``s_dst`` with a sentinel so a no-op copy fails the verify.
 *   4. Write the 16-byte TI block (a 256-word, 32-bit, increment-both block copy)
 *      and point ``s_dtc_vt[slot]`` at it -- both more **dirty** CPU writes.
 *   5. ``ra8_cache_dcache_clean_by_addr(s_src)`` and ``... (s_dtc_ti)`` (the app
 *      owns the TI + source); ``ra8_dtc_enable()`` (the HAL cleans the vector
 *      table, then ``DTCST = 1``); arm ``IELSRn.DTCE``; fire ELC software event 0.
 *   6. Wait for the copy to land in SRAM by invalidating ``s_dst`` and re-reading
 *      it each poll iteration (the read therefore observes RAM, not the stale
 *      sentinel still cached) until ``s_dst == s_src`` (bounded). This poll IS the
 *      completion gate **and** the coherency proof: if any descriptor clean were
 *      missing on silicon the DTC would fetch a stale entry, the copy would be
 *      wrong/absent, and the poll would time out.
 *   7. PASS -> emit ``"dtc_coherency_hil: dtc coherent PASS\r\n"`` over VCOM and
 *      the same verdict over ITM (``ra8_log_info``), toggle LED1. Mismatch / timeout
 *      -> a distinct ``... FAIL`` line (never containing "PASS") + LED2. Either way
 *      the core parks in WFI so ``ra8_emulator``'s idle-stop terminates the run.
 *
 * The completion IRQ is intentionally left masked (the app never calls
 * ``ra8_isr_globals_enable``): the registered slot + ``DTCE`` still activate the
 * DTC, the app simply polls for the result instead of taking the interrupt, which
 * avoids the completion ISR (which writes ``DTCSTS``) racing the in-flight copy.
 *
 * @par ra8_emulator note (sim path)
 * ``tools/ra8_emulator`` DOES model the DTC transfer engine
 * (``board_periph_dtc.c``): the ELC software-event trigger reads the vector-table
 * entry at ``DTCVBR + slot*4``, fetches the TI, and actually MOVES the bytes in
 * emulated memory, so the **real** ``ra8_dtc`` + ELC path runs in sim and the copy
 * verifies. ra8_emulator's memory is byte-exact and it does **not** model the L1
 * D-cache, so the clean / invalidate calls are exercised (the line-size and
 * barrier logic run) but have no caching effect, and the poll falls through on its
 * first iteration. The cache hazard this app guards against is only observable on
 * real silicon. ``RA8_SIMULATOR_MODE`` is a host-unit-test define and is NOT set
 * for the ARM cross-build that ra8_emulator executes, so there is nothing to
 * ``#ifdef`` here.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cache.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_dtc.h"
#include "ra8_dtc_regs.h"
#include "ra8_elc.h"
#include "ra8_err.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/** @brief Diagnostic / ITM log tag. */
static const char* s_tag = "dtc_coherency_hil";

/**
 * @enum dtc_coh_config_t
 * @brief Compile-time scalar parameters for the DTC coherency self-test.
 * @details Groups the console baud, buffer geometry, the destination sentinel,
 *          and the bounded completion-poll limit.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dtc_coh_baud       = 115200U,     /**< VCOM console line rate (8N1).             */
  k_dtc_coh_buf_words  = 256U,        /**< 32-bit words per buffer (256 x 4).        */
  k_dtc_coh_buf_bytes  = 1024U,       /**< Bytes copied per pass (>= 256, 1 KiB).    */
  k_dtc_coh_poll_limit = 100000U,     /**< Bounded wait for the copy to land in RAM. */
  k_dtc_coh_sentinel   = 0xA5A5A5A5U, /**< Destination pre-fill; no-op copy fails.   */
} dtc_coh_config_t;

/**
 * @enum dtc_coh_byte_t
 * @brief Single-byte scalar constants for the self-test.
 * @details The pattern xor-shift, the D-cache line size, and the NVIC priority of
 *          the (masked) DTC-activation slot.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dtc_coh_byte_sh  = 8U,  /**< Source-pattern shift (``i ^ (i >> 8)``). */
  k_dtc_coh_align    = 32U, /**< D-cache line size (bytes).               */
  k_dtc_coh_isr_prio = 12U, /**< NVIC priority for the DTC-complete slot. */
} dtc_coh_byte_t;

/**
 * @enum dtc_coh_event_t
 * @brief ELC software event 0 -> ICU event number.
 * @details HUM Table 19.3 (p 824) row "0x0CC | ELC | ELC_SWEVT0 | Software event
 *          0": ``ra8_elc_software_trigger(0)`` raises this event; routed to an
 *          IELSR slot with ``DTCE = 1`` it activates the DTC. App-local (the
 *          shared ``ra8_elc_event_t`` table only carries events the HAL itself
 *          wires), mirroring ``dtc_transfer_demo``.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_dtc_coh_event_swevt0 = 0x0CCU, /**< ELC software event 0 (HUM Table 19.3). */
} dtc_coh_event_t;

/**
 * @enum dtc_coh_swevt_t
 * @brief ELSEGRn index fired by ::ra8_elc_software_trigger.
 * @details ELSEGR0 generates ELC software event 0 (``0x0CC``).
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dtc_coh_swevt_index = 0U, /**< ELSEGR0 -> ELC_SWEVT0 (0x0CC). */
} dtc_coh_swevt_t;

/**
 * @enum dtc_coh_mr_field_t
 * @brief DTC Transfer-Information mode-bit field values.
 * @details HUM Ch 18.2.2 "MRA" (p 786) and 18.2.3 "MRB" (p 787): a block-mode,
 *          32-bit-word, increment-both copy. ``2`` selects "increment" for SM/DM,
 *          "32-bit word" for SZ, and "block transfer" for MD.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dtc_coh_md_block = 0x2U, /**< MRA.MD[7:6] = 10b: block transfer mode. */
  k_dtc_coh_sz_word  = 0x2U, /**< MRA.SZ[5:4] = 10b: 32-bit word units.   */
  k_dtc_coh_sm_inc   = 0x2U, /**< MRA.SM[3:2] = 10b: increment SAR.       */
  k_dtc_coh_dm_inc   = 0x2U, /**< MRB.DM[3:2] = 10b: increment DAR.       */
} dtc_coh_mr_field_t;

/**
 * @enum dtc_coh_mr_pos_t
 * @brief Bit positions inside the DTC TI ``MR`` word.
 * @details HUM Figure 18.4 (p 799) lays the first TI long-word out as
 *          MR[31:24] = MRA, MR[23:16] = MRB, MR[15:8] = MRC, MR[7:0] = reserved.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dtc_coh_mra_md_pos   = 6U,  /**< MRA.MD field position.     */
  k_dtc_coh_mra_sz_pos   = 4U,  /**< MRA.SZ field position.     */
  k_dtc_coh_mra_sm_pos   = 2U,  /**< MRA.SM field position.     */
  k_dtc_coh_mrb_dm_pos   = 2U,  /**< MRB.DM field position.     */
  k_dtc_coh_mra_byte_pos = 24U, /**< MRA byte offset within MR. */
  k_dtc_coh_mrb_byte_pos = 16U, /**< MRB byte offset within MR. */
} dtc_coh_mr_pos_t;

/**
 * @enum dtc_coh_count_t
 * @brief DTC count-register values for one 256-word block.
 * @details HUM Ch 18.2.7 "CRA" (p 790): in block mode CRAH/CRAL hold the block
 *          size and "the transfer count is ... 256 when the set value is 0x00".
 *          HUM Ch 18.2.8 "CRB" (p 791): CRB is the block count.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_dtc_coh_cra_block_256 = 0x0000U, /**< CRAH = CRAL = 0 => 256-unit block. */
  k_dtc_coh_crb_one_block = 0x0001U, /**< CRB = 1 => one block per pass.     */
} dtc_coh_count_t;

/**
 * @enum dtc_coh_vt_geom_t
 * @brief DTC vector-table geometry.
 * @details HUM Ch 18.3.1 (p 796) + Figure 18.3 (p 798): ``DTCVBR`` points at a
 *          table of 4-byte entries, one per interrupt vector number; entry n (at
 *          ``DTCVBR + n*4``) holds the 16-byte-aligned start address of that
 *          source's TI. ``DTCVBR`` itself must be 1 KiB-aligned (HUM Ch 18.2.2).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dtc_coh_vt_entries = 96U,   /**< One pointer per IELSR slot 0..95.    */
  k_dtc_coh_vt_align   = 1024U, /**< DTCVBR 1 KiB alignment (HUM 18.2.2). */
  k_dtc_coh_cra_units  = 256U,  /**< Units per block when CRAH/CRAL = 0.  */
} dtc_coh_vt_geom_t;

static_assert((uint32_t)k_dtc_coh_buf_words == (uint32_t)k_dtc_coh_cra_units,
              "CRA=0x0000 encodes a 256-unit block; buffer must be 256 words");
static_assert((uint32_t)k_dtc_coh_buf_words * sizeof(uint32_t) == (uint32_t)k_dtc_coh_buf_bytes,
              "buffer byte count must equal words * 4");
static_assert((uint32_t)k_dtc_coh_buf_bytes >= (uint32_t)k_dtc_coh_align,
              "buffer must span at least one D-cache line");

/**
 * @var k_dtc_coh_pass_banner
 * @brief Deterministic one-shot HIL success banner (uart_scrape gate).
 * @details Emitted only on the all-pass path over the VCOM console. The trailing
 *          CRLF terminates the line on the wire; the HIL gate matches the text
 *          exactly. Contains "PASS" and is not a substring of the FAIL banner.
 * @warning Do not modify; ``hil.conf`` ``HIL_EXPECT`` matches it verbatim.
 * @since 0.1.0
 */
static const uint8_t k_dtc_coh_pass_banner[] = "dtc_coherency_hil: dtc coherent PASS\r\n";

/**
 * @var k_dtc_coh_fail_banner
 * @brief Failure banner: the copy was wrong, absent, or timed out.
 * @details Distinct from the PASS banner and free of "PASS"; caught by the
 *          ``hil.conf`` ``HIL_EXPECT_NEGATIVE`` alternation.
 * @since 0.1.0
 */
static const uint8_t k_dtc_coh_fail_banner[] = "dtc_coherency_hil: dtc coherent FAIL\r\n";

/**
 * @var s_src
 * @brief Source buffer -- M85 writes leave it dirty in the D-cache.
 * @details 1 KiB, 32-byte (D-cache line) aligned and an exact whole number of
 *          lines, so ``ra8_cache_dcache_clean_by_addr`` cleans exactly the buffer
 *          with no partial-line spill. Lives in MPU region 1 (cacheable SRAM).
 * @warning The DTC reads this from RAM; it must be cleaned before activation.
 * @since 0.1.0
 */
alignas(k_dtc_coh_align) static uint32_t s_src[k_dtc_coh_buf_words];

/**
 * @var s_dst
 * @brief Destination buffer the DTC writes; the M85 must invalidate to read it.
 * @details 1 KiB, 32-byte aligned and a whole number of cache lines so
 *          ``ra8_cache_dcache_invalidate_by_addr`` drops exactly the buffer's
 *          lines. Lives in MPU region 1 (cacheable SRAM).
 * @warning Stale cached lines here are the exact hazard this app proves.
 * @since 0.1.0
 */
alignas(k_dtc_coh_align) static uint32_t s_dst[k_dtc_coh_buf_words];

/**
 * @var s_dtc_vt
 * @brief DTC vector table -- one 4-byte TI start address per IELSR slot.
 * @details Lives in MPU region 1 (cacheable SRAM). The CPU write to
 *          ``s_dtc_vt[slot]`` is dirty in cache; ``ra8_dtc_enable()`` cleans this
 *          region back to RAM before ``DTCST = 1`` so the DTC fetches the live
 *          entry, not a stale line -- the wiring this app validates.
 * @warning 1 KiB-aligned: ``DTCVBR`` requires the lower 10 bits be 0.
 * @since 0.1.0
 */
alignas(k_dtc_coh_vt_align) static uint32_t s_dtc_vt[k_dtc_coh_vt_entries];

/**
 * @var s_dtc_ti
 * @brief The 16-byte Transfer Information block the DTC reads at activation.
 * @details Lives in MPU region 1 (cacheable SRAM), 32-byte aligned (one cache
 *          line; satisfies the 16-byte TI requirement, HUM Ch 18.3.1 p 796). The
 *          CPU writes to its fields are dirty in cache; the app cleans it before
 *          activation so the DTC reads the live descriptor from RAM.
 * @warning The DTC reads this from RAM; it must be cleaned before activation.
 * @since 0.1.0
 */
alignas(k_dtc_coh_align) static r_dtc_xfer_info_t s_dtc_ti;

/**
 * @var s_dtc_slot
 * @brief IELSR slot allocated for the DTC activation = DTC vector number.
 * @details Assigned by ::ra8_isr_register; indexes both the IELSR table and the
 *          ``s_dtc_vt`` vector table.
 * @note Written once during bring-up, then read-only.
 * @since 0.1.0
 */
static uint16_t s_dtc_slot;

/**
 * @brief Park the Cortex-M85 forever in a WFI idle loop.
 *
 * @details Reached after the one-shot banner has been emitted (pass or fail), or
 * on a fatal init failure. The WFI lets ``ra8_emulator``'s idle detector stop the
 * run cleanly and models the low-power posture on silicon.
 *
 * @return This function never returns.
 * @note The core stays asleep between (unconfigured) wake events.
 *
 * @pre The self-test verdict has been emitted, or a fatal error occurred.
 * @pre No further forward progress is required of the M85.
 * @post The core makes no further architectural progress.
 * @post No further bus activity is generated.
 *
 * @note Not thread-safe; terminal sink.
 * @since 0.1.0
 */
[[noreturn]] static void dtc_coh_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief DTC completion callback (fanned out by ::ra8_dtc_dispatch).
 *
 * @details Registered for symmetry with the activation slot; it does not run in
 * this app because the completion IRQ is left globally masked (the app polls).
 *
 * @param[in] ctx    Unused registration context.
 * @param[in] status Unused DTCSTS snapshot at completion.
 *
 * @return Nothing.
 *
 * @pre Attached via ::ra8_dtc_attach_handler.
 * @pre Reached only if global IRQs are enabled (they are not, here).
 * @post No application state is modified.
 * @post No hardware register is modified by this callback.
 *
 * @note ISR context; not re-entrant.
 * @since 0.1.0
 */
static void dtc_coh_complete_cb(void* ctx, uint16_t status)
{
  (void)ctx;
  (void)status;
}

/**
 * @brief IELSR-slot ISR for the DTC-complete interrupt (masked in this app).
 *
 * @details Routes the slot event through the HAL dispatch into
 * ::dtc_coh_complete_cb. Present so ::ra8_isr_register has a valid handler; it does
 * not run because the completion IRQ stays globally masked.
 *
 * @param[in] ctx Unused registration context.
 *
 * @return Nothing.
 *
 * @pre Registered via ::ra8_isr_register for ELC software event 0.
 * @pre Reached only if global IRQs are enabled (they are not, here).
 * @post ::ra8_dtc_dispatch has run at most once per entry.
 * @post No application state is modified.
 *
 * @note ISR context; not re-entrant.
 * @since 0.1.0
 */
static void dtc_coh_swevt_isr(void* ctx)
{
  (void)ctx;
  ra8_dtc_dispatch();
}

/**
 * @brief Bring CGC + MSTP + ISR + ELC + SCI8 console + LEDs up. Panic-halts.
 *
 * @details The DTC0 module clock shares MSTPA22 with DMAC0 and is ungated by the
 * ``ra8_dtc`` driver via ``ra8_mstp``. The SCI8 J-Link OB console (TXD8 = PD_02 /
 * RXD8 = PD_03 @ 115200 8N1) comes up through the BSP console API. ELC + ISR are
 * needed to allocate the DTC activation slot and fire the software event.
 *
 * @return Nothing.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has enabled the MPU + caches (RA8_BOOT_ENABLE_CACHE_MPU).
 * @post On success CGC, MSTP, ISR, ELC, the console and both LEDs are ready.
 * @post On any failure the function does not return (parks in WFI).
 *
 * @note Single-threaded init context; not thread-safe.
 * @since 0.1.0
 */
static void dtc_coh_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_isr_init() != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_elc_init() != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dtc_coh_baud) != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dtc_coh_park();
  }
}

/**
 * @brief Initialise the DTC and allocate its ELC-software-event activation slot.
 *
 * @details Programmes ``DTCVBR`` to ``s_dtc_vt``, attaches the (masked)
 * completion callback, and allocates an IELSR slot for ELC software event 0 whose
 * index is the DTC activation-source vector number. Halts on any failure or an
 * out-of-range slot.
 *
 * @return Nothing.
 *
 * @pre ::dtc_coh_setup_or_halt has run (ISR + ELC initialised).
 * @pre Global IRQs are still masked (boot leaves PRIMASK set).
 * @post ``DTCVBR == s_dtc_vt`` and ``s_dtc_slot < k_dtc_coh_vt_entries``.
 * @post On any failure the function does not return (parks in WFI).
 *
 * @note Single-threaded init context; not thread-safe.
 * @since 0.1.0
 */
static void dtc_coh_bringup_or_halt(void)
{
  if (ra8_dtc_init(s_dtc_vt) != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_dtc_attach_handler(dtc_coh_complete_cb, nullptr) != k_ra8_ok) {
    dtc_coh_park();
  }
  if (ra8_isr_register((ra8_elc_event_t)k_dtc_coh_event_swevt0,
                       dtc_coh_swevt_isr,
                       nullptr,
                       (uint8_t)k_dtc_coh_isr_prio,
                       &s_dtc_slot) != k_ra8_ok) {
    dtc_coh_park();
  }
  if (s_dtc_slot >= (uint16_t)k_dtc_coh_vt_entries) {
    dtc_coh_park();
  }
}

/**
 * @brief Fill ``s_src`` with a deterministic pattern; prime ``s_dst``.
 *
 * @details ``s_src[i] = i ^ (i >> 8)`` are M85 stores that sit dirty in the
 *          D-cache. ``s_dst`` is primed with a sentinel distinct from every source
 *          word so a copy that never lands fails the verify.
 *
 * @par MC/DC:
 * Single counting loop, no compound decision; only the implicit loop bound.
 * No N+1 vectors required.
 *
 * @return Nothing.
 *
 * @pre Buffers are statically allocated and 32-byte aligned.
 * @pre Called single-threaded before the DTC is armed.
 * @post Every word of ``s_src`` holds the pattern.
 * @post Every word of ``s_dst`` holds ``k_dtc_coh_sentinel``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void dtc_coh_fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_coh_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dtc_coh_byte_sh);
    s_dst[i] = (uint32_t)k_dtc_coh_sentinel;
  }
}

/**
 * @brief Write the 16-byte TI block and point the slot's vector entry at it.
 *
 * @details Field encoding per HUM Figure 18.4 (p 799): MR holds MRA/MRB/MRC, then
 * SAR, DAR, CRB, CRA -- a 256-word, 32-bit, increment-both block copy of
 * ``s_src`` into ``s_dst``. ``s_dtc_vt[s_dtc_slot]`` is set to the TI start
 * address (bit 0 = privilege attribution, left 0 = privileged). Both the TI
 * fields and the vector-table entry are CPU writes that land dirty in the D-cache.
 *
 * @par MC/DC:
 * Straight-line assignment -- no decision points.
 *
 * @return Nothing.
 *
 * @pre ``s_src`` / ``s_dst`` are populated and ``s_dtc_slot`` is valid.
 * @post ``s_dtc_ti`` describes the block copy and ``s_dtc_vt[s_dtc_slot]`` points
 *       at it (both possibly still dirty in cache until cleaned).
 *
 * @note Not thread-safe. SRAM writes only (no MMIO).
 * @since 0.1.0
 */
static void dtc_coh_program_ti(void)
{
  const uint8_t mra = (uint8_t)(((uint8_t)k_dtc_coh_md_block << k_dtc_coh_mra_md_pos) |
                                ((uint8_t)k_dtc_coh_sz_word << k_dtc_coh_mra_sz_pos) |
                                ((uint8_t)k_dtc_coh_sm_inc << k_dtc_coh_mra_sm_pos));
  const uint8_t mrb = (uint8_t)((uint8_t)k_dtc_coh_dm_inc << k_dtc_coh_mrb_dm_pos);
  /* MR[31:24]=MRA, MR[23:16]=MRB, MR[15:8]=MRC(0); HUM Ch 18.2.2 p 786 /
   * 18.2.3 p 787 / 18.2.4 p 789, Figure 18.4 p 799. SRAM (not MMIO). */
  s_dtc_ti.MR =
    ((uint32_t)mra << k_dtc_coh_mra_byte_pos) | ((uint32_t)mrb << k_dtc_coh_mrb_byte_pos);
  s_dtc_ti.SAR         = (uint32_t)(uintptr_t)s_src;
  s_dtc_ti.DAR         = (uint32_t)(uintptr_t)s_dst;
  s_dtc_ti.CRB         = (uint16_t)k_dtc_coh_crb_one_block;
  s_dtc_ti.CRA         = (uint16_t)k_dtc_coh_cra_block_256;
  s_dtc_vt[s_dtc_slot] = (uint32_t)(uintptr_t)&s_dtc_ti;
}

/**
 * @brief Re-arm DTC activation on the slot (set ``IELSRn.DTCE``).
 *
 * @details The DTC activates from an IELSR slot only when its DTCE bit is set; the
 * read-modify-write preserves the IELS event field that ::ra8_isr_register wrote.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_error`` if the slot accessor
 *         returned NULL.
 * @retval k_ra8_ok           DTCE is set for the slot.
 * @retval k_ra8_err_hw_error The IELSR accessor returned NULL.
 *
 * @pre ``s_dtc_slot`` was assigned by ::ra8_isr_register.
 * @pre Called single-threaded with the completion IRQ masked.
 * @post IELSRn.DTCE for the slot reads 1.
 * @post No other IELSR field is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_coh_arm_slot(void)
{
  volatile uint32_t* slot_reg = ra8_icu_ielsr(s_dtc_slot);
  if (slot_reg == nullptr) {
    return k_ra8_err_hw_error;
  }
  /* HUM Ch 14.2.10 "IELSRn : ICU Event Link Setting Register n" p 524:
   * DTCE[24] enables DTC activation; the IELS event field is preserved. */
  *slot_reg = *slot_reg | (uint32_t)k_ra8_ielsr_dtce_mask;
  return k_ra8_ok;
}

/**
 * @brief Verify ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @details Must run on an invalidated ``s_dst`` so the reads miss the cache and
 *          fetch the DTC-written bytes from SRAM. A non-empty mismatch (sentinel
 *          still present, or a stale-descriptor partial copy) returns 0.
 *
 * @par MC/DC:
 * One atomic condition ``s_dst[i] != s_src[i]`` x 2 vectors: all-match (steady
 * state, this run) and one mismatch (the cache-hazard regression that occurs on
 * silicon if a descriptor clean is omitted -- the DTC fetches a stale entry and
 * the copy is wrong/absent).
 *
 * @return 1 if all words equal, 0 otherwise.
 * @retval 1 Buffers identical -- copy + cache maintenance coherent.
 * @retval 0 At least one word differs.
 *
 * @pre Both buffers are filled and ``s_dst`` lines have been invalidated.
 * @post The return value is exactly 0 or 1.
 * @post Neither buffer is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static uint8_t dtc_coh_verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_coh_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Clean src + TI, enable + trigger the DTC, then wait for the copy.
 *
 * @details Implements the descriptor + data coherency pattern:
 *   1. ``ra8_cache_dcache_clean_by_addr(s_src)`` and ``... (s_dtc_ti)`` flush the
 *      dirty source bytes and TI fields to SRAM so the DTC reads live descriptors
 *      and data (the app owns the TI + source; the HAL owns the vector table).
 *   2. ``ra8_dtc_enable()`` cleans the caller-populated vector table back to RAM
 *      (commit b80c247b), then sets ``DTCST = 1``.
 *   3. ::dtc_coh_arm_slot sets ``IELSRn.DTCE`` so the event activates the DTC.
 *   4. ``ra8_elc_software_trigger`` fires ELC software event 0, activating the DTC.
 *   5. Poll for completion by invalidating ``s_dst`` and re-verifying each
 *      iteration -- the read therefore observes RAM, not the dirty sentinel still
 *      cached, so the loop ends exactly when the DTC's copy has fully landed
 *      (bounded by ::k_dtc_coh_poll_limit). This poll is both the completion gate
 *      and the coherency proof: a stale descriptor would leave ``s_dst`` wrong and
 *      time the loop out.
 *
 * @param[out] out_ok 1 if the destination matched the source, else 0.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            The copy landed and matched within the poll bound.
 * @retval k_ra8_err_null_ptr  ``out_ok`` was NULL.
 * @retval k_ra8_err_hw_error  A cache-maintenance op or the IELSR accessor failed.
 * @retval k_ra8_err_hw_timeout The copy never matched within the poll bound.
 * @retval (other)            Propagated ``ra8_dtc_enable`` / ``ra8_elc_software_trigger``
 *                            error.
 *
 * @pre Buffers are filled and the TI + vector entry are programmed.
 * @pre Global IRQs are masked (the completion IRQ is not taken).
 * @post On ``k_ra8_ok`` ``s_dst`` holds the source pattern in SRAM and its cache
 *       lines are invalid.
 * @post ``*out_ok`` reflects the post-copy comparison.
 *
 * @note Not thread-safe; single-shot init-context use.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_coh_run_once(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");
  *out_ok = 0U;

  /* clean-before (app owns the TI + source): dirty lines -> SRAM so the DTC
   * reads the real descriptor and the real pattern from memory. */
  if (ra8_cache_dcache_clean_by_addr(s_src, (uint32_t)sizeof(s_src)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  if (ra8_cache_dcache_clean_by_addr(&s_dtc_ti, (uint32_t)sizeof(s_dtc_ti)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }

  /* The HAL cleans the caller-populated vector table here, then DTCST = 1. */
  const ra8_err_t en = ra8_dtc_enable();
  if (en != k_ra8_ok) {
    return en;
  }
  const ra8_err_t arm = dtc_coh_arm_slot();
  if (arm != k_ra8_ok) {
    return arm;
  }
  const ra8_err_t trig = ra8_elc_software_trigger((uint8_t)k_dtc_coh_swevt_index);
  if (trig != k_ra8_ok) {
    return trig;
  }

  /* Completion + coherency gate: invalidate-after each poll so the verify reads
   * what the DTC wrote to SRAM (not the dirty sentinel still in cache), and stop
   * as soon as the full block has landed. Bounded so a stale-descriptor copy
   * (the regression this app guards against) times out instead of hanging. */
  bool done = false;
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_coh_poll_limit; ++i) {
    if (ra8_cache_dcache_invalidate_by_addr(s_dst, (uint32_t)sizeof(s_dst)) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    if (dtc_coh_verify() != 0U) {
      done = true;
      break;
    }
  }
  if (!done) {
    return k_ra8_err_hw_timeout;
  }
  *out_ok = 1U;
  return k_ra8_ok;
}

/**
 * @brief Emit the PASS/FAIL result over VCOM and ITM, toggle an LED.
 *
 * @param[in] ok Non-zero if the coherency check passed.
 *
 * @return Nothing.
 *
 * @pre The console has been initialised by ::dtc_coh_setup_or_halt.
 * @pre Both LEDs have been initialised.
 * @post Exactly one terminal banner has been written and flushed to VCOM.
 * @post The matching result line has been emitted over ITM.
 *
 * @note Not thread-safe; single-shot reporting.
 * @since 0.1.0
 */
static void dtc_coh_report(uint8_t ok)
{
  if (ok != 0U) {
    (void)ra8_board_uart_console_write(k_dtc_coh_pass_banner,
                                       (size_t)(sizeof(k_dtc_coh_pass_banner) - 1U));
    (void)ra8_board_uart_console_flush();
    ra8_log_info(s_tag, "dtc coherent PASS");
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  } else {
    (void)ra8_board_uart_console_write(k_dtc_coh_fail_banner,
                                       (size_t)(sizeof(k_dtc_coh_fail_banner) - 1U));
    (void)ra8_board_uart_console_flush();
    ra8_log_info(s_tag, "dtc coherent FAIL");
    (void)ra8_board_led_toggle(k_ra8_board_led2);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: one-shot DTC descriptor-coherency proof, then WFI.
 *
 * @details Brings up the clock tree, MSTP, ISR/ELC, and the VCOM console, programs
 * a software-triggered DTC block copy, runs it with the L1 caches + MPU enabled by
 * the shared boot (``RA8_BOOT_ENABLE_CACHE_MPU``), emits the matching PASS / FAIL
 * banner over the console and ``ra8_log``, then parks in WFI.
 *
 * @return Never returns (parks in WFI after reporting).
 * @retval (none) Control stays in the WFI idle loop.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit enabled the MPU + caches via RA8_BOOT_ENABLE_CACHE_MPU.
 * @post Exactly one PASS/FAIL banner is emitted, then the core parks.
 * @post The core ends in WFI so ra8_emulator's idle-stop terminates the run.
 *
 * @note Single-threaded; the completion IRQ is left masked and the result polled.
 * @since 0.1.0
 */
int32_t main(void)
{
  ra8_log_init();
  ra8_log_info(s_tag, "==== dtc_coherency_hil: M85 D-cache DTC descriptor coherency ====");

  dtc_coh_setup_or_halt();
  dtc_coh_bringup_or_halt();

  dtc_coh_fill_buffers();
  dtc_coh_program_ti();

  uint8_t         ok   = 0U;
  const ra8_err_t err  = dtc_coh_run_once(&ok);
  const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
  dtc_coh_report(good);

  dtc_coh_park();
  return 0;
}
#pragma GCC diagnostic pop
