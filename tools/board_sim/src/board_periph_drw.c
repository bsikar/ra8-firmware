/**
 * @file board_periph_drw.c
 * @brief Faithful-inert DRW ("D/AVE 2D") engine model for board_sim (issue #247)
 *
 * @details
 * Models the RA8D2 DRW 2D graphics engine (ra8_drw_regs.h, ra8_drw.c) at
 * @c 0x40444000 as it behaves while its power domain is gated: INERT.
 * board_sim's governing invariant is SIM == HIL: an app must produce the
 * IDENTICAL result in the emulator and on hardware, hardware defects included.
 *
 * @par Why the engine looked dead (issue #247, resolved)
 * The DRW never rasterized on the bench because it was never POWERED. It sits
 * in the graphics power domain, which @c PDCTRGD gates OFF at reset (HUM
 * Ch 11.2.14 p 452, reset value @c 0x81; domain contents -- MIPI DSI, MIPI CSI,
 * VIN, DRW, GLCDC -- in HUM Ch 11.5.1 Table 11.7 p 480). Cancelling the DRW
 * module-stop bit is not sufficient. With the domain powered on, an EK-RA8D2
 * reads @c HWREVISION = @c 0x0FBE0107 (it reads 0 while gated) and the engine
 * rasterizes: @c drw_fill_demo now paints a real rectangle into its
 * framebuffer. The earlier conclusion recorded here -- that "HWREVISION reads 0
 * on silicon" was a property of the part -- was a symptom of the dark domain,
 * not a fact about the DRW.
 *
 * This model therefore keeps the engine inert but now gates the observable
 * signal on ::board_pdctr_graphics_powered, so firmware that skips the
 * power-domain bring-up sees the same dead register the bench gives it.
 * Modelling the RASTERIZER itself is deliberately still not attempted: the
 * on-silicon geometry semantics are only partly characterised (a 16x16 fill
 * request currently paints 8x8, and the alpha byte does not reach the
 * framebuffer), and inventing pixels the bench does not produce would be a
 * fresh SIM != HIL divergence of exactly the kind this file exists to prevent.
 *
 * The register interface stays live enough that the driver's control flow
 * completes without hanging or faulting:
 *  - Every write (CONTROL / CONTROL2 / ORIGIN / limiters / COLOR1 / SIZE /
 *    texture / CLUT / IRQCTL / ...) is ACCEPTED and discarded. In particular
 *    writing @c ORIGIN -- the HUM Ch 62.2.31 render trigger -- does NOT
 *    rasterize, matching silicon, so no pixel is ever written to emulated
 *    memory.
 *  - @c STATUS (read alias of CONTROL @ 0x000) reads all-zero: the engine is
 *    never busy and latches no IRQ or bus-error flag, so @c ra8_drw_wait_idle
 *    returns on its first poll.
 *  - @c HWREVISION (read alias of CONTROL2 @ 0x004) reads 0 while the graphics
 *    power domain is gated and @c 0x0FBE0107 once it is powered -- both values
 *    captured from an EK-RA8D2 over J-Link.
 *
 * The consequence for the two DRW demos matches the bench exactly:
 *  - @c drw_blend_demo composites nothing, hashes its untouched zero framebuffer
 *    (FNV-1a-32 over 4096 zero bytes = 0x76EFDDC5), and prints
 *    @c "drw: blit+blend crc=76EFDDC5 PASS" -- the silicon golden pinned in its
 *    @c hil.conf. The SIL gate passes because the emulator reproduces the
 *    silicon truth, not because it fakes a render.
 *  - @c drw_fill_demo's centre pixel stays clear, so it honestly reports
 *    @c "drw: fill match=N" in the emulator, just as it does on silicon until
 *    the engine is brought to life under issue #247.
 *
 * @par Removed: the idealized-render path.
 * A prior revision of this file composited solid-box fills and a global-alpha
 * source-over blend into the emulated framebuffer to PREVIEW what a working
 * engine would render (flipping @c drw_fill_demo to @c match=Y and hashing
 * @c drw_blend_demo to a non-zero CRC). That preview was useful while developing
 * issue #247 but it violated SIM == HIL -- it produced pixels the silicon does
 * not -- so it is deleted rather than hidden behind an opt-in flag no gate would
 * exercise. Reviving a reference compositor for #247, if ever wanted, is a
 * git-history retrieval, not a maintenance burden carried in the default model.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "board_periph_block.h"
#include "board_periph_pdctr_internal.h"

/** @brief DRW block geometry (ra8_drw_regs.h, HUM Ch 62). */
typedef enum : uint64_t {
  k_drw_base = 0x40444000UL, /**< DRW Secure base (HUM Ch 62). */
  k_drw_span = 0x104UL,      /**< Register window (260 bytes). */
} drw_geom_t;

/** @brief The two read-aliased DRW status registers this inert model answers. */
typedef enum : uint64_t {
  k_drw_off_status     = 0x000UL, /**< R STATUS (alias of CONTROL).      */
  k_drw_off_hwrevision = 0x004UL, /**< R HWREVISION (alias of CONTROL2). */
} drw_off_t;

/**
 * @brief DRW hardware revision stamp read back from real EK-RA8D2 silicon.
 *
 * @details Captured over J-Link with the graphics power domain enabled
 * (HUM Ch 62.2.6 "HWREVISION" p 3696). Reads 0 while the domain is gated.
 */
typedef enum : uint32_t {
  k_drw_hwrevision_value = 0x0FBE0107U, /**< Bench-observed HWREVISION. */
} drw_hwrev_t;

/** @brief Per-tick order slot for the DRW block (relative order only). */
typedef enum : uint32_t {
  k_drw_block_order = 160U, /**< After the DOC block; report order. */
} drw_order_t;

/** @brief MMIO read inside the DRW window -- inert engine, so idle/zero. */
static uint64_t drw_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_drw_base;
  switch (off) {
    case (uint64_t)k_drw_off_status:
      /* HUM Ch 62.2.5 "STATUS: Status Control Register" p 3695. The engine is
       * modelled inert (issue #247): never busy, no latched IRQ or bus error, so
       * STATUS reads all-zero and ra8_drw_wait_idle succeeds on its first poll. */
    case (uint64_t)k_drw_off_hwrevision:
      /* HUM Ch 62.2.6 "HWREVISION: Hardware Revision Register" p 3696.
       *
       * HWREVISION reads 0 only while the DRW is UNPOWERED. The engine sits in
       * the graphics power domain, which PDCTRGD gates OFF at reset (HUM
       * Ch 11.2.14 p 452, reset value 0x81; domain contents in Ch 11.5.1
       * Table 11.7 p 480). Issue #247 originally recorded "HWREVISION reads 0
       * on silicon" as a property of the part; it is not -- it was the symptom
       * of a domain nothing had ever powered on. Bench-confirmed on an
       * EK-RA8D2: 0x00000000 with PDCTRGD = 0x81, and 0x0FBE0107 once PDDE is
       * cleared. Report the same, so firmware that skips the power-domain
       * bring-up sees the dead register here exactly as it does on the bench. */
      if (!board_pdctr_graphics_powered()) {
        return 0U;
      }
      return (uint64_t)k_drw_hwrevision_value;
    default:
      /* Every other DRW register is write-only on silicon (HUM Ch 62.2) and the
       * driver never reads it back (CONTROL2 and COLOR1 are shadowed in
       * software), so a deterministic zero is both harmless and faithful. */
      return 0U;
  }
}

/** @brief MMIO write inside the DRW window -- accepted and discarded (inert). */
static void drw_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  (void)value;
  /* Inert engine (issue #247): CONTROL / CONTROL2 / ORIGIN / limiter / colour /
   * texture / CLUT / IRQCTL writes are all ACCEPTED so the driver's register
   * sequence runs to completion, but none synthesises a pixel. Writing ORIGIN
   * (the HUM Ch 62.2.31 render trigger) does NOT rasterize on silicon, so the
   * emulated framebuffer is left exactly as the CPU last wrote it. No state is
   * shadowed because nothing reads it back (see drw_read). */
}

/** @brief DRW block descriptor (self-registered with the core). */
static const board_periph_block_t k_drw_block = {
  .base   = (uint64_t)k_drw_base,
  .span   = (uint64_t)k_drw_span,
  .order  = (uint32_t)k_drw_block_order,
  .read   = drw_read,
  .write  = drw_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "DRW",
};

/** @brief Register the DRW block before main (host constructor). */
[[gnu::constructor]] static void drw_block_register(void)
{
  board_periph_register_block(&k_drw_block);
}
