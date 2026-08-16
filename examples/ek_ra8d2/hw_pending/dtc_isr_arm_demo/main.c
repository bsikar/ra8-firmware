
/**
 * @file examples/ek_ra8d2/hw_pending/dtc_isr_arm_demo/main.c
 * @brief DTC arm/disarm demo built on the ra8_isr_set_dtc() HAL primitive
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The DTC counterpart to ``dtc_transfer_demo`` -- same 1 KB SRAM-to-SRAM
 * block copy, same ELC-software-event activation -- but it arms and disarms
 * DTC activation through the ``ra8_isr_set_dtc()`` HAL primitive (issue
 * #579) instead of open-coding the ``ICU.IELSRn.DTCE`` read-modify-write.
 * Because the primitive owns that write, this app never includes
 * ``ra8_icu_regs.h`` at all: the DTC-vs-CPU routing decision for the
 * allocated slot no longer leaks into application code.
 *
 * Once a second the loop runs two phases and reports whether BOTH matched
 * their expectation:
 *
 *   1. **Armed.** Fill a 1 KB source with a deterministic pattern
 *      (``i ^ (i >> 8)``), zero the destination, program the Transfer
 *      Information block, ``ra8_isr_set_dtc(slot, true)``, then fire ELC
 *      software event 0. The DTC activates and copies the block; the
 *      destination must equal the source.
 *   2. **Disarmed.** Refill the destination with a sentinel
 *      (``0xA5A5A5A5``), reprogram the TI, ``ra8_isr_set_dtc(slot, false)``,
 *      then fire the same event again. With ``DTCE`` clear the DTC does not
 *      activate, so the destination must still be entirely the sentinel --
 *      proof that clearing ``DTCE`` truly gates the transfer.
 *
 * ``good = armed_ok && disarmed_ok`` gates the banner
 * ``"dtc-arm: armed+disarmed OK\r\n"`` on the J-Link OB CDC channel; any
 * failure prints ``"dtc-arm: FAILED\r\n"``. LED1 toggles on success, LED2
 * on failure, and the ``g_dtc_*`` globals mirror the result for headless
 * probing (HIL / board emulator).
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` models the DTC
 * descriptor engine (``board_periph_dtc.c``) AND its ``DTCE`` gate: the ELC
 * software event activates the controller only when a ``DTCE``-enabled
 * IELSR slot links the event, so the armed phase copies and the disarmed
 * phase leaves the sentinel intact -- the same ``match`` the banner
 * reports. Not yet confirmed on silicon; the DTC activation path is
 * byte-identical to the silicon-validated ``dtc_transfer_demo`` (the
 * primitive performs the exact same ``IELSRn.DTCE`` write), so promotion to
 * ``hw_validated/hil/`` needs only a bench run. See ``README.md``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_dtc.h"
#include "ra8_dtc_regs.h"
#include "ra8_elc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "dtc_arm";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dtc_arm_baud         = 115200U,     /**< SCI8 baud rate.                       */
  k_dtc_arm_period_ms    = 1000U,       /**< Delay between passes.                 */
  k_dtc_arm_buf_bytes    = 1024U,       /**< Bytes copied per armed pass.          */
  k_dtc_arm_buf_words    = 256U,        /**< 32-bit words per buffer / block.      */
  k_dtc_arm_poll_limit   = 200000U,     /**< Bounded wait for the block to land.   */
  k_dtc_arm_settle       = 20000U,      /**< Bounded settle for the disarmed pass. */
  k_dtc_arm_dst_sentinel = 0xA5A5A5A5U, /**< Disarmed-phase destination fill.      */
} dtc_arm_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dtc_arm_byte_sh  = 8U,  /**< Source-pattern shift (``i ^ (i >> 8)``). */
  k_dtc_arm_isr_prio = 12U, /**< NVIC priority for the DTC-complete slot. */
} dtc_arm_byte_t;

/**
 * @brief ELC software event 0 -> ICU event number.
 *
 * @details
 * HUM Table 19.3 (p 824) row "0x0CC | ELC | ELC_SWEVT0 | Software event 0":
 * ``ra8_elc_software_trigger(0)`` writes ELSEGR0.SEG which raises this
 * event; routed to an IELSR slot with DTCE = 1 it activates the DTC. App
 * local (the shared ``ra8_elc_event_t`` table only carries events the HAL
 * itself wires), mirroring ``dtc_transfer_demo``.
 */
typedef enum : uint16_t {
  k_dtc_arm_event_swevt0 = 0x0CCU, /**< ELC software event 0 (HUM Table 19.3). */
} dtc_arm_event_t;

/** @brief ELSEGRn index fired by ::ra8_elc_software_trigger. */
typedef enum : uint8_t {
  k_dtc_arm_swevt_index = 0U, /**< ELSEGR0 -> ELC_SWEVT0 (0x0CC). */
} dtc_arm_swevt_t;

/**
 * @brief DTC Transfer-Information mode-bit field values.
 *
 * @details
 * HUM Ch 18.2.2 "MRA" (p 786) and 18.2.3 "MRB" (p 787): a block-mode,
 * 32-bit-word, increment-both copy. ``2`` selects "increment" for SM/DM,
 * "32-bit word" for SZ, and "block transfer" for MD.
 */
typedef enum : uint8_t {
  k_dtc_arm_md_block = 0x2U, /**< MRA.MD[7:6] = 10b: block transfer mode. */
  k_dtc_arm_sz_word  = 0x2U, /**< MRA.SZ[5:4] = 10b: 32-bit word units.   */
  k_dtc_arm_sm_inc   = 0x2U, /**< MRA.SM[3:2] = 10b: increment SAR.       */
  k_dtc_arm_dm_inc   = 0x2U, /**< MRB.DM[3:2] = 10b: increment DAR.       */
} dtc_arm_mr_field_t;

/**
 * @brief Bit positions inside the DTC TI ``MR`` word.
 *
 * @details
 * HUM Figure 18.4 (p 799) lays the first TI long-word out as
 * MR[31:24] = MRA, MR[23:16] = MRB, MR[15:8] = MRC, MR[7:0] = reserved.
 */
typedef enum : uint8_t {
  k_dtc_arm_mra_md_pos   = 6U,  /**< MRA.MD field position.     */
  k_dtc_arm_mra_sz_pos   = 4U,  /**< MRA.SZ field position.     */
  k_dtc_arm_mra_sm_pos   = 2U,  /**< MRA.SM field position.     */
  k_dtc_arm_mrb_dm_pos   = 2U,  /**< MRB.DM field position.     */
  k_dtc_arm_mra_byte_pos = 24U, /**< MRA byte offset within MR. */
  k_dtc_arm_mrb_byte_pos = 16U, /**< MRB byte offset within MR. */
} dtc_arm_mr_pos_t;

/**
 * @brief DTC count-register values for one 256-word block.
 *
 * @details
 * HUM Ch 18.2.7 "CRA" (p 790): in block mode CRAH/CRAL hold the block size
 * and "the transfer count is ... 256 when the set value is 0x00". HUM
 * Ch 18.2.8 "CRB" (p 791): CRB is the block count.
 */
typedef enum : uint16_t {
  k_dtc_arm_cra_block_256 = 0x0000U, /**< CRAH = CRAL = 0 => 256-unit block. */
  k_dtc_arm_crb_one_block = 0x0001U, /**< CRB = 1 => one block per pass.     */
} dtc_arm_count_t;

/**
 * @brief DTC vector-table geometry.
 *
 * @details
 * HUM Ch 18.3.1 (p 796) + Figure 18.3 (p 798): ``DTCVBR`` points at a table
 * of 4-byte entries, one per interrupt vector number; entry n (at
 * ``DTCVBR + n*4``) holds the 16-byte-aligned start address of that
 * source's TI. ``DTCVBR`` itself must be 1 KB-aligned (HUM Ch 18.2.11 p 792
 * "the lower 10 bits should be 0").
 */
typedef enum : uint32_t {
  k_dtc_arm_vt_entries = 96U,   /**< One pointer per IELSR slot 0..95.    */
  k_dtc_arm_vt_align   = 1024U, /**< DTCVBR 1 KB alignment (HUM 18.2.11). */
  k_dtc_arm_ti_align   = 16U,   /**< TI start address multiple of 16.     */
} dtc_arm_vt_geom_t;

/** @enum dtc_arm_cra_t @brief CRA=0x0000 encodes a full 256-unit block. */
typedef enum : uint32_t {
  k_dtc_arm_cra_block_units = 256U, /**< Units per block when CRAH/CRAL = 0. */
} dtc_arm_cra_t;

static_assert((uint32_t)k_dtc_arm_buf_words == (uint32_t)k_dtc_arm_cra_block_units,
              "CRA=0x0000 encodes a 256-unit block; buffer must be 256 words");

/** @brief Output line tags. */
static const uint8_t k_dtc_arm_ok_msg[]  = "dtc-arm: armed+disarmed OK\r\n";
static const uint8_t k_dtc_arm_bad_msg[] = "dtc-arm: FAILED\r\n";

/** @brief Source / destination buffers (32-bit aligned by element type). */
static uint32_t s_src[k_dtc_arm_buf_words];
static uint32_t s_dst[k_dtc_arm_buf_words];

/**
 * @var s_dtc_vt
 * @brief DTC vector table -- one 4-byte TI start address per IELSR slot.
 * @warning 1 KB-aligned: ``DTCVBR`` requires the lower 10 bits be 0.
 */
[[gnu::aligned(k_dtc_arm_vt_align)]] static uint32_t s_dtc_vt[k_dtc_arm_vt_entries];

/**
 * @var s_dtc_ti
 * @brief The 16-byte Transfer Information block the DTC reads each pass.
 * @warning 16-byte-aligned (HUM Ch 18.3.1 p 796).
 */
[[gnu::aligned(k_dtc_arm_ti_align)]] static r_dtc_xfer_info_t s_dtc_ti;

/** @brief IELSR slot allocated for the DTC activation = DTC vector number. */
static uint16_t s_dtc_slot;

/**
 * @var g_dtc_armed_ok
 * @brief 1 when the armed pass copied the block correctly.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_dtc_armed_ok = 0U;

/**
 * @var g_dtc_disarmed_ok
 * @brief 1 when the disarmed pass left the destination untouched.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_disarmed_ok = 0U;

/**
 * @var g_dtc_activations
 * @brief Count of ELC software-event triggers issued (armed + disarmed).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_activations = 0U;

/**
 * @var g_dtc_isr_count
 * @brief Count of DTC-complete callbacks fanned through the HAL dispatch.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_isr_count = 0U;

/**
 * @var g_dtc_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void dtc_arm_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief DTC completion callback (fanned out by ::ra8_dtc_dispatch).
 *
 * @param[in] ctx    Unused registration context.
 * @param[in] status DTCSTS snapshot at completion.
 * @pre Attached via ::ra8_dtc_attach_handler.
 * @post ::g_dtc_isr_count incremented once.
 * @note ISR context; not re-entrant.
 * @since 0.1.0
 */
static void dtc_arm_complete_cb(void* ctx, uint16_t status)
{
  (void)ctx;
  (void)status;
  ++g_dtc_isr_count;
}

/**
 * @brief IELSR-slot ISR for the DTC-complete interrupt.
 *
 * @details
 * When the single block finishes, the DTC clears ICU.IELSRn.DTCE and raises
 * the slot's CPU interrupt (HUM Figure 18.5 p 801). This routes the event
 * through the HAL dispatch into ::dtc_arm_complete_cb.
 *
 * @param[in] ctx Unused registration context.
 * @pre Registered via ::ra8_isr_register for ELC software event 0.
 * @post ::ra8_dtc_dispatch has run exactly once.
 * @note ISR context; not re-entrant.
 * @since 0.1.0
 */
static void dtc_arm_swevt_isr(void* ctx)
{
  (void)ctx;
  ra8_dtc_dispatch();
}

/** @brief Bring CGC + SysTick + ISR + ELC + SCI8 + LEDs + MSTP up. */
static void dtc_arm_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_isr_init() != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_elc_init() != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dtc_arm_baud) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
}

/**
 * @brief Fill ``s_src`` with a deterministic pattern and ``s_dst`` with a value.
 *
 * @par MC/DC:
 * Trivial loop with no compound decision -- only the implicit loop exit
 * condition. No N+1 vectors required.
 *
 * @param[in] dst_init Value written to every destination word before a pass.
 * @pre Buffers are statically allocated.
 * @post Every word in ``s_src`` is set; ``s_dst`` is all ``dst_init``.
 * @since 0.1.0
 */
static void dtc_arm_fill(uint32_t dst_init)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_arm_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dtc_arm_byte_sh);
    s_dst[i] = dst_init;
  }
}

/**
 * @brief Write the 16-byte TI block describing the block copy.
 *
 * @details
 * Rebuilt every pass because the DTC writes the post-transfer TI back to
 * SRAM (MRA.WBDIS = 0, HUM Ch 18.2.2 p 786), consuming SAR/DAR/CRA/CRB.
 * Field encoding per HUM Figure 18.4 (p 799): MR holds MRA/MRB/MRC, then
 * SAR, DAR, CRB, CRA.
 *
 * @par MC/DC:
 * Straight-line assignment -- no decision points.
 *
 * @pre ``s_src`` / ``s_dst`` are populated.
 * @post ``s_dtc_ti`` describes a 256-word, 32-bit, increment-both copy.
 * @since 0.1.0
 */
static void dtc_arm_program_ti(void)
{
  const uint8_t mra = (uint8_t)(((uint8_t)k_dtc_arm_md_block << k_dtc_arm_mra_md_pos) |
                                ((uint8_t)k_dtc_arm_sz_word << k_dtc_arm_mra_sz_pos) |
                                ((uint8_t)k_dtc_arm_sm_inc << k_dtc_arm_mra_sm_pos));
  const uint8_t mrb = (uint8_t)((uint8_t)k_dtc_arm_dm_inc << k_dtc_arm_mrb_dm_pos);
  /* MR[31:24]=MRA, MR[23:16]=MRB, MR[15:8]=MRC(0); HUM Ch 18.2.2 p 786 /
   * 18.2.3 p 787 / 18.2.4 p 789, Figure 18.4 p 799. SRAM (not MMIO). */
  s_dtc_ti.MR =
    ((uint32_t)mra << k_dtc_arm_mra_byte_pos) | ((uint32_t)mrb << k_dtc_arm_mrb_byte_pos);
  s_dtc_ti.SAR = (uint32_t)(uintptr_t)s_src;
  s_dtc_ti.DAR = (uint32_t)(uintptr_t)s_dst;
  s_dtc_ti.CRB = (uint16_t)k_dtc_arm_crb_one_block;
  s_dtc_ti.CRA = (uint16_t)k_dtc_arm_cra_block_256;
}

/**
 * @brief Initialise the DTC, allocate its activation slot, and enable it.
 *
 * @details
 * Programs ``DTCVBR`` to ``s_dtc_vt``, attaches the completion callback,
 * allocates an IELSR slot for ELC software event 0 (whose index is the DTC
 * vector number), points that slot's vector-table entry at the TI block, and
 * starts the engine (DTCST = 1). Halts on any failure.
 *
 * @pre ::ra8_isr_init / ::ra8_elc_init have run; IRQs not yet enabled.
 * @post The DTC is enabled and ``s_dtc_vt[s_dtc_slot]`` points at the TI.
 * @since 0.1.0
 */
static void dtc_arm_bringup_or_halt(void)
{
  if (ra8_dtc_init(s_dtc_vt) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_dtc_attach_handler(dtc_arm_complete_cb, nullptr) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (ra8_isr_register((ra8_elc_event_t)k_dtc_arm_event_swevt0,
                       dtc_arm_swevt_isr,
                       nullptr,
                       (uint8_t)k_dtc_arm_isr_prio,
                       &s_dtc_slot) != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
  if (s_dtc_slot >= (uint16_t)k_dtc_arm_vt_entries) {
    dtc_arm_panic_halt();
  }
  /* DTCVBR + slot*4 holds the 16-byte-aligned TI start address; bit0 is the
   * privilege attribution (0 = privileged). HUM Ch 18.3.1 p 796 + Figure 18.3
   * p 798. SRAM vector-table write (not MMIO). */
  s_dtc_vt[s_dtc_slot] = (uint32_t)(uintptr_t)&s_dtc_ti;
  if (ra8_dtc_enable() != k_ra8_ok) {
    dtc_arm_panic_halt();
  }
}

/**
 * @brief Verify ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @par MC/DC:
 * Compound decision in the loop: ``s_dst[i] != s_src[i]``. One atomic
 * condition x 2 vectors -- match (armed steady-state) and one mismatch
 * (covered by the headless emulator, which never runs the transfer).
 *
 * @return 1 if all elements equal, 0 otherwise.
 * @pre Buffers are filled.
 * @post Return value is 0 or 1.
 * @since 0.1.0
 */
static uint8_t dtc_arm_all_match_src(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_arm_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Verify every word of ``s_dst`` still holds ``expect``.
 *
 * @par MC/DC:
 * Compound decision in the loop: ``s_dst[i] != expect``. One atomic
 * condition x 2 vectors -- untouched (disarmed steady-state) and one
 * differing word (a DTC that wrongly ran with DTCE clear).
 *
 * @param[in] expect Value every destination word must still hold.
 * @return 1 if every word equals @p expect, 0 otherwise.
 * @pre ``s_dst`` was filled with @p expect before the pass.
 * @post Return value is 0 or 1.
 * @since 0.1.0
 */
static uint8_t dtc_arm_all_equal(uint32_t expect)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_arm_buf_words; ++i) {
    if (s_dst[i] != expect) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Run one ARMED pass: DTCE = 1, fire the event, expect a full copy.
 *
 * @details
 * Zeroes the destination, rebuilds the TI, arms the slot via
 * ``ra8_isr_set_dtc(s_dtc_slot, true)``, fires ELC software event 0, polls
 * (bounded) for the last destination word to land, and reports whether the
 * whole buffer copied.
 *
 * @param[out] out_ok 1 if the destination matched the source, else 0.
 * @return ``k_ra8_ok`` once the (bounded) attempt finishes, or the error
 *         from arming the slot / firing the software event.
 * @retval k_ra8_ok            The pass completed (see ``*out_ok``).
 * @retval k_ra8_err_null_ptr  ``out_ok`` was NULL.
 * @pre The DTC is enabled and ``s_dtc_slot`` is registered.
 * @post ``*out_ok`` reflects the post-copy comparison.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_arm_run_armed(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  dtc_arm_fill(0U);
  dtc_arm_program_ti();
  const ra8_err_t arm = ra8_isr_set_dtc(s_dtc_slot, true);
  if (arm != k_ra8_ok) {
    return arm;
  }
  const ra8_err_t trig = ra8_elc_software_trigger((uint8_t)k_dtc_arm_swevt_index);
  if (trig != k_ra8_ok) {
    return trig;
  }
  ++g_dtc_activations;

  for (uint32_t i = 0U; i < (uint32_t)k_dtc_arm_poll_limit; ++i) {
    if (s_dst[k_dtc_arm_buf_words - 1U] == s_src[k_dtc_arm_buf_words - 1U]) {
      break;
    }
  }

  *out_ok = dtc_arm_all_match_src();
  return k_ra8_ok;
}

/**
 * @brief Run one DISARMED pass: DTCE = 0, fire the event, expect NO copy.
 *
 * @details
 * Fills the destination with the sentinel, rebuilds the TI, disarms the slot
 * via ``ra8_isr_set_dtc(s_dtc_slot, false)``, fires the same ELC software
 * event, waits a bounded settle window, and reports whether the destination
 * is still entirely the sentinel -- proof the cleared ``DTCE`` gated the DTC.
 *
 * @param[out] out_ok 1 if the destination was untouched, else 0.
 * @return ``k_ra8_ok`` once the (bounded) attempt finishes, or the error
 *         from disarming the slot / firing the software event.
 * @retval k_ra8_ok            The pass completed (see ``*out_ok``).
 * @retval k_ra8_err_null_ptr  ``out_ok`` was NULL.
 * @pre The DTC is enabled and ``s_dtc_slot`` is registered.
 * @post ``*out_ok`` reflects the untouched-destination check.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_arm_run_disarmed(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  dtc_arm_fill((uint32_t)k_dtc_arm_dst_sentinel);
  dtc_arm_program_ti();
  const ra8_err_t disarm = ra8_isr_set_dtc(s_dtc_slot, false);
  if (disarm != k_ra8_ok) {
    return disarm;
  }
  const ra8_err_t trig = ra8_elc_software_trigger((uint8_t)k_dtc_arm_swevt_index);
  if (trig != k_ra8_ok) {
    return trig;
  }
  ++g_dtc_activations;

  /* No copy is expected, so there is nothing to poll for; give the (gated)
   * DTC a bounded settle window, then confirm the destination is untouched.
   * The volatile read keeps the loop from being optimised away. */
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_arm_settle; ++i) {
    if (s_dst[k_dtc_arm_buf_words - 1U] != (uint32_t)k_dtc_arm_dst_sentinel) {
      break;
    }
  }

  *out_ok = dtc_arm_all_equal((uint32_t)k_dtc_arm_dst_sentinel);
  return k_ra8_ok;
}

void main(void)
{
  dtc_arm_setup_or_halt();
  dtc_arm_bringup_or_halt();
  /* Completion is detected by polling (armed) or a settle window (disarmed),
   * so the global interrupt is intentionally left masked -- as in
   * dtc_transfer_demo, enabling the DTC-complete CPU interrupt lets its ISR
   * (which writes DTCSTS) race the in-flight transfer on silicon. The IELSR
   * slot armed by ra8_isr_set_dtc still activates the DTC; we simply do not
   * take the completion IRQ. */

  while (1) {
    uint8_t         armed_ok    = 0U;
    uint8_t         disarmed_ok = 0U;
    const ra8_err_t e_arm       = dtc_arm_run_armed(&armed_ok);
    const ra8_err_t e_dis       = dtc_arm_run_disarmed(&disarmed_ok);

    g_dtc_armed_ok    = (e_arm == k_ra8_ok && armed_ok != 0U) ? 1U : 0U;
    g_dtc_disarmed_ok = (e_dis == k_ra8_ok && disarmed_ok != 0U) ? 1U : 0U;

    /* The DTCE primitive is correct only if arming COPIED and disarming did
     * NOT -- both must hold. This compound decision is the demo's verdict. */
    const uint8_t good = (g_dtc_armed_ok != 0U && g_dtc_disarmed_ok != 0U) ? 1U : 0U;

    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_dtc_arm_ok_msg, (size_t)(sizeof(k_dtc_arm_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_dtc_arm_bad_msg,
                                         (size_t)(sizeof(k_dtc_arm_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_dtc_heartbeat;
    ra8_delay_ms(k_dtc_arm_period_ms);
  }
}
