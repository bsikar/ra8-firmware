/**
 * @file board_periph_block.h
 * @brief Decentralized peripheral-block registry for the board emulator core
 *
 * @details
 * The internal contract between the board_periph core (the block registry +
 * MMIO dispatch + the ICU/NVIC routing) and each per-block model file
 * (board_periph_gpio.c, board_periph_timer.c, board_periph_sci.c,
 * board_periph_i2c.c, and any future block). It is NOT part of the public
 * board_periph.h surface main.c uses; it exists so a new peripheral block can
 * join the model with no edit to a hand-maintained central list.
 *
 * A block describes itself with a ::board_periph_block_t -- its absolute
 * register address range plus read / write / tick / reset function pointers --
 * and registers that descriptor with the core. Registration is decentralized:
[[gnu::constructor]]  * each block file self-registers from a file-scope @c
 * that runs before @c main (ra8_emulator is a host program, so constructors are a
 * sound startup mechanism), so ADDING A BLOCK is exactly "(a) a new
 * board_periph_<blk>.c and (b) a CMakeLists source line" -- no other file
 * changes. Registration order does not matter: MMIO dispatch is by disjoint
 * address range, and the per-tick advance is ordered by the descriptor's
 * ::board_periph_block_t::order field, so two blocks added in parallel never
 * conflict.
 *
 * The core also publishes the shared framework services a block needs back from
 * it: ::board_periph_icu_raise_event (so a timer / UART block can pend an
 * interrupt through the one ICU IELSR -> NVIC path the core owns) and
 * ::board_periph_trace (the --trace flag).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read handler for a modelled peripheral block.
 *
 * @param[in,out] uc   Unicorn engine (a handler may read emulated memory).
 * @param[in]     addr Absolute peripheral address being read (inside the
 *                     block's registered range).
 * @param[in]     size Access width in bytes (1 / 2 / 4).
 * @return The register value the block reports for @p addr.
 * @since 0.1.0
 */
typedef uint64_t (*board_periph_read_fn)(uc_engine* uc, uint64_t addr, unsigned size);

/**
 * @brief Write handler for a modelled peripheral block.
 *
 * @param[in,out] uc    Unicorn engine (a handler may read emulated memory).
 * @param[in]     addr  Absolute peripheral address being written (inside the
 *                      block's registered range).
 * @param[in]     size  Access width in bytes (1 / 2 / 4).
 * @param[in]     value Value being written.
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*board_periph_write_fn)(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value);

/**
 * @brief Per-emulation-chunk advance for a block, or NULL if it has none.
 *
 * @param[in,out] uc Unicorn engine (the block may raise an ICU event, which
 *                   reads IELSR / NVIC from PPB).
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*board_periph_tick_fn)(uc_engine* uc);

/**
 * @brief Reset a block to its power-on state, or NULL if it keeps none.
 *
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*board_periph_reset_fn)(void);

/**
 * @brief Print a block's end-of-run summary section, or NULL if it has none.
 *
 * @details Called by the core's ::board_periph_report in ascending
 * ::board_periph_block_t::order, so the summary keeps its historical section
 * order (LEDs, timers, UART, ... touch) without a central list.
 *
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*board_periph_report_fn)(void);

/**
 * @brief Which modelled device(s) expose a given peripheral block.
 *
 * @details
 * Nearly every RA8 peripheral register base is byte-identical across the family,
 * so a block is device-agnostic (::k_board_block_dev_any) and answers on every
 * modelled device. A block that models hardware present on ONE device only --
 * the RA8P1's Arm Ethos-U55 NPU, which does not exist on the RA8D2 -- tags
 * itself so the core dispatches it ONLY when that device is the active emulation
 * target (see ::board_periph_set_device). On
 * any other device the tagged block is skipped and its address window falls
 * through to the sparse fallback, exactly as an unmodelled reserved region does,
 * which keeps the RA8D2 dispatch byte-for-behaviour unchanged.
 *
 * @invariant Left zero (``k_board_block_dev_any``) by every device-agnostic
 *            block's designated initializer, so existing blocks need no edit.
 * @see board_periph_block_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_board_block_dev_any   = 0U, /**< Present on every modelled device (default). */
  k_board_block_dev_ra8p1 = 1U, /**< RA8P1-only (Ethos-U55 NPU).                 */
} board_block_device_t;

/**
 * @brief A modelled peripheral block's self-description for the core registry.
 *
 * @details
 * @c base / @c span give the block's absolute register window; the core
 * dispatches an MMIO access to @c read / @c write when its address falls in
 * @c [base, base + span). @c tick (may be NULL) advances the block once per
 * emulation chunk and @c reset (may be NULL) returns it to power-on state. The
 * descriptor must have static lifetime -- the core stores the pointer, not a
 * copy. @c order makes the per-tick advance deterministic regardless of
 * registration order: blocks tick in ascending @c order (ties keep registration
 * order), preserving the historical AGT/GPT-before-SCI cadence.
 *
 * @c observe selects the block's ownership mode. A normal (``observe == false``)
 * block OWNS its window: the core answers an in-range MMIO read from @c read and
 * a write from @c write, reports the access handled, and the caller's sparse
 * fallback never sees it. An observe-only (``observe == true``) block instead
 * SNOOPS its window: the core still calls @c write so the block can shadow the
 * register stream, but reports the access NOT handled so the caller's sparse
 * model continues to serve reads and record writes. This is for a window that
 * something outside the registry already reads back (e.g. main.c's panel
 * compositor reads the GLCDC graphics-layer registers from the sparse shadow to
 * build the @c --ppm / live frame); an owning block would divert those writes
 * from the sparse shadow and blank the compositor, so the GLCDC model snoops
 * instead -- it decodes the active framebuffer descriptor without disturbing the
 * existing read path. An observe block's @c read is never called (the sparse
 * model answers); supply a stub for it.
 */
typedef struct {
  uint64_t               base;    /**< Absolute window base address.             */
  uint64_t               span;    /**< Window length in bytes.                   */
  uint32_t               order;   /**< Ascending tick order (lower ticks first). */
  board_periph_read_fn   read;    /**< MMIO read handler (required).             */
  board_periph_write_fn  write;   /**< MMIO write handler (required).            */
  board_periph_tick_fn   tick;    /**< Per-chunk advance, or NULL.               */
  board_periph_reset_fn  reset;   /**< Power-on reset, or NULL.                  */
  board_periph_report_fn report;  /**< End-of-run summary section, or NULL.      */
  const char*            name;    /**< Short label (diagnostics only).           */
  bool                   observe; /**< Observe-only: snoop, do not own the MMIO. */
  board_block_device_t   device;  /**< Device gate: which chip(s) expose it.     */
  bool loop_only; /**< Only own the window when --usbhs-loop is set (else sparse). */
} board_periph_block_t;

/**
 * @brief Recommended @c order values so parallel blocks tick in a stable cadence.
 *
 * @details
 * A block picks one of these for ::board_periph_block_t::order. Spacing leaves
 * room for new blocks between the existing ones without renumbering. Only the
 * relative order matters; ties are broken by registration order. The historical
 * cadence the core preserved was timers (AGT then GPT) before SCI, so the timer
 * block sits below the SCI block here.
 */
typedef enum : uint32_t {
  k_block_order_gpio  = 10U, /**< GPIO/PORT (no tick today).       */
  k_block_order_timer = 20U, /**< GPT + AGT timers.                */
  k_block_order_sci   = 30U, /**< SCI_B UART.                      */
  k_block_order_i2c   = 40U, /**< I3C/I2C + GT911 (no tick today). */
} board_periph_block_order_t;

/**
 * @brief Register a peripheral block's descriptor with the core registry.
 *
 * @details
 * Called by each block file from a file-scope @c __attribute__((constructor)),
 * so every block is registered before @c main runs. The core keeps the supplied
 * pointer (the descriptor must be static) and dispatches MMIO / tick / reset
 * through it. Registering more blocks than the fixed registry capacity drops the
 * extra (a build-time assert in the core guards the common case); registration
 * order is irrelevant to behaviour.
 *
 * @param[in] block Static block descriptor to add (ignored if NULL).
 * @return Nothing.
 * @post Subsequent ::board_periph_read / _write / _tick / reset see @p block.
 * @since 0.1.0
 */
void board_periph_register_block(const board_periph_block_t* block);

/**
 * @brief Raise a peripheral ELC event through the core's ICU -> NVIC path.
 *
 * @details
 * The single entry point a block uses to assert an interrupt: the core owns the
 * ICU IELSR event-link table, the NVIC enable shadow and the pending-IRQ ring,
 * so a timer / UART block calls this rather than touching that state. The core
 * latches IELSR.IR for the slot linked to @p event and, if that NVIC line is
 * enabled, pends it for the engine to take.
 *
 * @param[in,out] uc    Unicorn engine (the ICU reads IELSR / NVIC from PPB).
 * @param[in]     event ELC event number the block is asserting.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_icu_raise_event(uc_engine* uc, uint16_t event);

/**
 * @brief Find the IELSR slot that activates the DTC for an ELC event.
 *
 * @details
 * The DTC block model needs to know which IELSR slot a software / peripheral
 * event is routed to with DTC activation enabled, so it can index the DTC
 * vector table (@c DTCVBR + slot*4) at the matching Transfer Information block.
 * The core owns the IELSR event-link table (the same state
 * ::board_periph_icu_raise_event scans), so it answers the query: the first
 * slot whose IELS event-select field equals @p event and whose DTCE bit is set.
 *
 * @param[in] event ELC event number to resolve (e.g. ELC_SWEVT0 = 0x0CC).
 * @return The IELSR slot index [0, 95] when a DTCE-enabled slot links @p event,
 *         or a value >= 96 when none does.
 * @since 0.1.0
 */
uint32_t board_periph_icu_dtc_slot(uint16_t event);

/**
 * @brief Whether --trace is active (blocks log transitions when true).
 *
 * @return true when the run was started with --trace, else false.
 * @since 0.1.0
 */
bool board_periph_trace(void);

#ifdef __cplusplus
}
#endif
