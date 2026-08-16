/**
 * @file examples/ek_ra8d2/hw_validated/hil/dtc_transfer_demo/main.c
 * @brief 1 KB DTC SRAM-to-SRAM block-copy + verify demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The Data Transfer Controller (DTC) is the CPU-transparent,
 * descriptor-table transfer engine that sits next to the DMAC. Unlike
 * the DMAC it has no software-start register (HUM Ch 18.3 p 796: "The
 * DTC is activated by an interrupt request"); every transfer is kicked
 * by a peripheral interrupt whose ICU.IELSRn slot has the DTCE bit set.
 * This demo drives that activation deterministically with an ELC
 * software event so no external stimulus is needed.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + the ISR/ELC/DTC blocks. Once
 * a second the loop:
 *
 *   1. Fills a 1 KB source buffer with a deterministic pattern
 *      (``i ^ (i >> 8)``) and zeroes the destination buffer.
 *   2. Writes a 16-byte Transfer Information (TI) block describing a
 *      block-mode, 32-bit-wide, increment-both copy of one 256-word
 *      block (HUM Figure 18.4 p 799), and points the slot's DTC
 *      vector-table entry at it.
 *   3. Re-arms ICU.IELSRn.DTCE on the slot, then fires ELC software
 *      event 0 (HUM Ch 19.2.2 p 819: "A software event can trigger a
 *      linked DTC event"), which activates the DTC.
 *   4. Waits (bounded) for the block to land, walks both buffers, and
 *      reports ``"dtc: copied 1024B match=Y\r\n"`` on the J-Link OB CDC
 *      channel.
 *
 * LED1 toggles on every successful copy; LED2 latches if the
 * destination ever differs from the source. The ``g_dtc_*`` globals
 * mirror the result for headless probing (HIL / board emulator).
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` models the DTC
 * descriptor-table transfer engine (``board_periph_dtc.c``): the ELC
 * software-event trigger activates the controller, which reads the TI via
 * ``DTCVBR`` and copies the block in emulated memory, so the banner reports
 * ``match=Y`` and the ``ra8_emulator_smoke.sh`` gate keys on it. Confirmed on a
 * real EK-RA8D2 (2026-06-28): on a TrustZone part the secure DTC reads its
 * vector table from ``DTCVBR_SEC`` (the HAL now programs it), and the demo
 * leaves the DTC-complete IRQ masked so its ISR cannot race the transfer.
 * See ``README.md`` for details.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_dtc.h"
#include "ra8_dtc_regs.h"
#include "ra8_elc.h"
#include "ra8_err.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "dtc_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dtc_demo_baud       = 115200U, /**< SCI8 baud rate.                     */
  k_dtc_demo_period_ms  = 1000U,   /**< Delay between copy passes.          */
  k_dtc_demo_buf_bytes  = 1024U,   /**< Bytes copied per pass (256 x 4).    */
  k_dtc_demo_buf_words  = 256U,    /**< 32-bit words per buffer / block.    */
  k_dtc_demo_poll_limit = 200000U, /**< Bounded wait for the block to land. */
} dtc_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dtc_demo_byte_sh  = 8U,  /**< Source-pattern shift (``i ^ (i >> 8)``). */
  k_dtc_demo_isr_prio = 12U, /**< NVIC priority for the DTC-complete slot. */
} dtc_demo_byte_t;

/**
 * @brief ELC software event 0 -> ICU event number.
 *
 * @details
 * HUM Table 19.3 (p 824) row "0x0CC | ELC | ELC_SWEVT0 | Software event
 * 0": ``ra8_elc_software_trigger(0)`` writes ELSEGR0.SEG which raises this
 * event; routed to an IELSR slot with DTCE = 1 it activates the DTC.
 * App-local (the shared ``ra8_elc_event_t`` table only carries events the
 * HAL itself wires), mirroring ``uart_irq_echo``.
 */
typedef enum : uint16_t {
  k_dtc_demo_event_swevt0 = 0x0CCU, /**< ELC software event 0 (HUM Table 19.3). */
} dtc_demo_event_t;

/** @brief ELSEGRn index fired by ::ra8_elc_software_trigger. */
typedef enum : uint8_t {
  k_dtc_demo_swevt_index = 0U, /**< ELSEGR0 -> ELC_SWEVT0 (0x0CC). */
} dtc_demo_swevt_t;

/**
 * @brief DTC Transfer-Information mode-bit field values.
 *
 * @details
 * HUM Ch 18.2.2 "MRA" (p 786) and 18.2.3 "MRB" (p 787): a block-mode,
 * 32-bit-word, increment-both copy. ``2`` selects "increment" for
 * SM/DM, "32-bit word" for SZ, and "block transfer" for MD.
 */
typedef enum : uint8_t {
  k_dtc_md_block = 0x2U, /**< MRA.MD[7:6] = 10b: block transfer mode. */
  k_dtc_sz_word  = 0x2U, /**< MRA.SZ[5:4] = 10b: 32-bit word units.   */
  k_dtc_sm_inc   = 0x2U, /**< MRA.SM[3:2] = 10b: increment SAR.       */
  k_dtc_dm_inc   = 0x2U, /**< MRB.DM[3:2] = 10b: increment DAR.       */
} dtc_mr_field_t;

/**
 * @brief Bit positions inside the DTC TI ``MR`` word.
 *
 * @details
 * HUM Figure 18.4 (p 799) lays the first TI long-word out as
 * MR[31:24] = MRA, MR[23:16] = MRB, MR[15:8] = MRC, MR[7:0] = reserved.
 */
typedef enum : uint8_t {
  k_dtc_mra_md_pos   = 6U,  /**< MRA.MD field position.     */
  k_dtc_mra_sz_pos   = 4U,  /**< MRA.SZ field position.     */
  k_dtc_mra_sm_pos   = 2U,  /**< MRA.SM field position.     */
  k_dtc_mrb_dm_pos   = 2U,  /**< MRB.DM field position.     */
  k_dtc_mra_byte_pos = 24U, /**< MRA byte offset within MR. */
  k_dtc_mrb_byte_pos = 16U, /**< MRB byte offset within MR. */
} dtc_mr_pos_t;

/**
 * @brief DTC count-register values for one 256-word block.
 *
 * @details
 * HUM Ch 18.2.7 "CRA" (p 790): in block mode CRAH/CRAL hold the block
 * size and "the transfer count is ... 256 when the set value is 0x00".
 * HUM Ch 18.2.8 "CRB" (p 791): CRB is the block count.
 */
typedef enum : uint16_t {
  k_dtc_cra_block_256 = 0x0000U, /**< CRAH = CRAL = 0 => 256-unit block. */
  k_dtc_crb_one_block = 0x0001U, /**< CRB = 1 => one block per pass.     */
} dtc_count_t;

/**
 * @brief DTC vector-table geometry.
 *
 * @details
 * HUM Ch 18.3.1 (p 796) + Figure 18.3 (p 798): ``DTCVBR`` points at a
 * table of 4-byte entries, one per interrupt vector number; entry n
 * (at ``DTCVBR + n*4``) holds the 16-byte-aligned start address of that
 * source's TI. ``DTCVBR`` itself must be 1 KB-aligned (HUM Ch 18.2.11
 * p 792 "the lower 10 bits should be 0").
 */
typedef enum : uint32_t {
  k_dtc_vt_entries = 96U,   /**< One pointer per IELSR slot 0..95.    */
  k_dtc_vt_align   = 1024U, /**< DTCVBR 1 KB alignment (HUM 18.2.11). */
  k_dtc_ti_align   = 16U,   /**< TI start address multiple of 16.     */
} dtc_vt_geom_t;

/** @enum dtc_cra_t @brief CRA=0x0000 encodes a full 256-unit block. */
typedef enum : uint32_t {
  k_dtc_cra_block_units = 256U, /**< Units per block when CRAH/CRAL = 0. */
} dtc_cra_t;

static_assert((uint32_t)k_dtc_demo_buf_words == (uint32_t)k_dtc_cra_block_units,
              "CRA=0x0000 encodes a 256-unit block; buffer must be 256 words");

/** @brief Output line tags. */
static const uint8_t k_dtc_demo_ok_msg[]  = "dtc: copied 1024B match=Y\r\n";
static const uint8_t k_dtc_demo_bad_msg[] = "dtc: copied 1024B match=N\r\n";

/** @brief Source / destination buffers (32-bit aligned by element type). */
static uint32_t s_src[k_dtc_demo_buf_words];
static uint32_t s_dst[k_dtc_demo_buf_words];

/**
 * @var s_dtc_vt
 * @brief DTC vector table -- one 4-byte TI start address per IELSR slot.
 * @warning 1 KB-aligned: ``DTCVBR`` requires the lower 10 bits be 0.
 */
[[gnu::aligned(k_dtc_vt_align)]] static uint32_t s_dtc_vt[k_dtc_vt_entries];

/**
 * @var s_dtc_ti
 * @brief The 16-byte Transfer Information block the DTC reads each pass.
 * @warning 16-byte-aligned (HUM Ch 18.3.1 p 796).
 */
[[gnu::aligned(k_dtc_ti_align)]] static r_dtc_xfer_info_t s_dtc_ti;

/** @brief IELSR slot allocated for the DTC activation = DTC vector number. */
static uint16_t s_dtc_slot;

/**
 * @var g_dtc_match
 * @brief 1 when the destination matched the source after the last pass.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_dtc_match = 0U;

/**
 * @var g_dtc_bytes
 * @brief Bytes the DTC moved on the last successful pass (0 on failure).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_bytes = 0U;

/**
 * @var g_dtc_activations
 * @brief Count of ELC software-event triggers issued.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dtc_activations = 0U;

/**
 * @var g_dtc_isr_count
 * @brief Count of DTC-complete interrupts fanned through the HAL dispatch.
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
static void dtc_demo_panic_halt(void)
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
static void dtc_demo_complete_cb(void* ctx, uint16_t status)
{
  (void)ctx;
  (void)status;
  ++g_dtc_isr_count;
}

/**
 * @brief IELSR-slot ISR for the DTC-complete interrupt.
 *
 * @details
 * When the single block finishes, the DTC clears ICU.IELSRn.DTCE and
 * raises the slot's CPU interrupt (HUM Figure 18.5 p 801). This routes
 * the event through the HAL dispatch into ::dtc_demo_complete_cb.
 *
 * @param[in] ctx Unused registration context.
 * @pre Registered via ::ra8_isr_register for ELC software event 0.
 * @post ::ra8_dtc_dispatch has run exactly once.
 * @note ISR context; not re-entrant.
 * @since 0.1.0
 */
static void dtc_demo_swevt_isr(void* ctx)
{
  (void)ctx;
  ra8_dtc_dispatch();
}

/** @brief Bring CGC + SysTick + ISR + ELC + SCI8 + LEDs + MSTP up. */
static void dtc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_isr_init() != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_elc_init() != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dtc_demo_baud) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
}

/**
 * @brief Fill ``s_src`` with a deterministic pattern and clear ``s_dst``.
 *
 * @par MC/DC:
 * Trivial loop with no compound decision -- only the implicit loop
 * exit condition. No N+1 vectors required.
 *
 * @pre Buffers are statically allocated.
 * @post Every word in ``s_src`` is set; ``s_dst`` is all-zero.
 * @since 0.1.0
 */
static void dtc_demo_fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_demo_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dtc_demo_byte_sh);
    s_dst[i] = 0U;
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
static void dtc_demo_program_ti(void)
{
  const uint8_t mra = (uint8_t)(((uint8_t)k_dtc_md_block << k_dtc_mra_md_pos) |
                                ((uint8_t)k_dtc_sz_word << k_dtc_mra_sz_pos) |
                                ((uint8_t)k_dtc_sm_inc << k_dtc_mra_sm_pos));
  const uint8_t mrb = (uint8_t)((uint8_t)k_dtc_dm_inc << k_dtc_mrb_dm_pos);
  /* MR[31:24]=MRA, MR[23:16]=MRB, MR[15:8]=MRC(0); HUM Ch 18.2.2 p 786 /
   * 18.2.3 p 787 / 18.2.4 p 789, Figure 18.4 p 799. SRAM (not MMIO). */
  s_dtc_ti.MR  = ((uint32_t)mra << k_dtc_mra_byte_pos) | ((uint32_t)mrb << k_dtc_mrb_byte_pos);
  s_dtc_ti.SAR = (uint32_t)(uintptr_t)s_src;
  s_dtc_ti.DAR = (uint32_t)(uintptr_t)s_dst;
  s_dtc_ti.CRB = (uint16_t)k_dtc_crb_one_block;
  s_dtc_ti.CRA = (uint16_t)k_dtc_cra_block_256;
}

/**
 * @brief Re-arm DTC activation on the slot and clear any latched IR.
 *
 * @details
 * The DTC clears ICU.IELSRn.DTCE to 0 and sets the RW1C IR flag when the
 * block completes (HUM Figure 18.5 p 801), so both are refreshed before
 * each activation. The read-modify-write preserves the IELS field and
 * write-1-clears a pending IR.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_error`` if the slot index
 *         is out of range.
 * @pre ``s_dtc_slot`` was assigned by ::ra8_isr_register.
 * @post IELSRn.DTCE for the slot reads 1; any pending IR is cleared.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_demo_arm_slot(void)
{
  volatile uint32_t* slot_reg = ra8_icu_ielsr(s_dtc_slot);
  if (slot_reg == nullptr) {
    return k_ra8_err_hw_error;
  }
  /* HUM Ch 14.2.17 "IELSRn : ICU Event Link Setting Register n" p 547:
   * DTCE[24] enables DTC activation; IR[16] is the RW1C status flag. */
  const uint32_t cur = *slot_reg;
  *slot_reg          = cur | (uint32_t)k_ra8_ielsr_dtce_mask;
  return k_ra8_ok;
}

/**
 * @brief Initialise the DTC, allocate its activation slot, and enable it.
 *
 * @details
 * Programs ``DTCVBR`` to ``s_dtc_vt``, attaches the completion callback,
 * allocates an IELSR slot for ELC software event 0 (whose index is the
 * DTC vector number), points that slot's vector-table entry at the TI
 * block, and starts the engine (DTCST = 1). Halts on any failure.
 *
 * @pre ::ra8_isr_init / ::ra8_elc_init have run; IRQs not yet enabled.
 * @post The DTC is enabled and ``s_dtc_vt[s_dtc_slot]`` points at the TI.
 * @since 0.1.0
 */
static void dtc_demo_bringup_or_halt(void)
{
  if (ra8_dtc_init(s_dtc_vt) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_dtc_attach_handler(dtc_demo_complete_cb, nullptr) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (ra8_isr_register((ra8_elc_event_t)k_dtc_demo_event_swevt0,
                       dtc_demo_swevt_isr,
                       nullptr,
                       (uint8_t)k_dtc_demo_isr_prio,
                       &s_dtc_slot) != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
  if (s_dtc_slot >= (uint16_t)k_dtc_vt_entries) {
    dtc_demo_panic_halt();
  }
  /* DTCVBR + slot*4 holds the 16-byte-aligned TI start address; bit0 is
   * the privilege attribution (0 = privileged). HUM Ch 18.3.1 p 796 +
   * Figure 18.3 p 798. SRAM vector-table write (not MMIO). */
  s_dtc_vt[s_dtc_slot] = (uint32_t)(uintptr_t)&s_dtc_ti;
  if (ra8_dtc_enable() != k_ra8_ok) {
    dtc_demo_panic_halt();
  }
}

/**
 * @brief Verify that ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @par MC/DC:
 * Compound decision in the loop: ``s_dst[i] != s_src[i]``. One atomic
 * condition x 2 vectors -- match (steady-state) and one mismatch
 * (covered by the headless emulator, which never runs the transfer).
 *
 * @return 1 if all elements equal, 0 otherwise.
 * @pre Buffers are filled.
 * @post Return value is 0 or 1.
 * @since 0.1.0
 */
static uint8_t dtc_demo_verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dtc_demo_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Program + activate one DTC block copy and wait for completion.
 *
 * @details
 * Rebuilds the TI, re-arms the slot, fires ELC software event 0, then
 * polls the last destination word until it lands (bounded). The DTC
 * writes ``s_dst`` in ascending order, so the final word settling is the
 * completion gate. If the engine never runs (e.g. the headless emulator
 * shadows the DTC window without transferring), the poll times out and
 * the verify reports a mismatch.
 *
 * @param[out] out_ok 1 if the destination matched the source, else 0.
 *
 * @return ``k_ra8_ok`` once the (bounded) attempt finishes, or the error
 *         from arming the slot / firing the software event.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ``s_src`` / ``s_dst`` populated; DTC enabled.
 * @post ``*out_ok`` reflects the post-copy comparison.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dtc_demo_run_once(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  dtc_demo_program_ti();
  const ra8_err_t arm = dtc_demo_arm_slot();
  if (arm != k_ra8_ok) {
    return arm;
  }
  const ra8_err_t trig = ra8_elc_software_trigger((uint8_t)k_dtc_demo_swevt_index);
  if (trig != k_ra8_ok) {
    return trig;
  }
  ++g_dtc_activations;

  for (uint32_t i = 0U; i < (uint32_t)k_dtc_demo_poll_limit; ++i) {
    if (s_dst[k_dtc_demo_buf_words - 1U] == s_src[k_dtc_demo_buf_words - 1U]) {
      break;
    }
  }

  *out_ok = dtc_demo_verify();
  return k_ra8_ok;
}

void main(void)
{
  dtc_demo_setup_or_halt();
  dtc_demo_bringup_or_halt();
  /* Completion is detected by polling ``s_dst`` (step 4 below), so the global
   * interrupt is intentionally left masked. On RA8 silicon, enabling the
   * DTC-complete CPU interrupt lets its ISR (which writes ``DTCSTS``) race the
   * in-flight DTC transfer and corrupt the copy; the headless emulator, which
   * runs the transfer synchronously on the ELSEGR write, does not expose this.
   * The IELSR slot + ``DTCE`` armed in bring-up still activate the DTC -- we
   * simply do not take the completion IRQ. */

  while (1) {
    dtc_demo_fill_buffers();
    uint8_t         ok   = 0U;
    const ra8_err_t err  = dtc_demo_run_once(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_dtc_match          = (uint32_t)good;
    g_dtc_bytes          = (good != 0U) ? (uint32_t)k_dtc_demo_buf_bytes : 0U;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_dtc_demo_ok_msg,
                                         (size_t)(sizeof(k_dtc_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_dtc_demo_bad_msg,
                                         (size_t)(sizeof(k_dtc_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_dtc_heartbeat;
    ra8_delay_ms(k_dtc_demo_period_ms);
  }
  dtc_demo_panic_halt();
}
