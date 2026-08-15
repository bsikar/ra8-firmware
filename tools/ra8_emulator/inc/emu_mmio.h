/**
 * @file emu_mmio.h
 * @brief Sparse MMIO model of the Renesas peripheral space
 *
 * @details
 * The fallback register model behind board_periph's block registry: each
 * touched address gets a slot; control writes are reflected back on read so
 * "configure then verify" works, and a spin-poll on one address eventually
 * alternates 0 / all-ones so a single-bit ready/idle poll completes either
 * way. Also owns the MRMS frequency-latch readback quirk, the BG_BGC
 * colour-cycle witness, the access counters the idle-stop signature and the
 * board view read, and the run-end MMIO report. The read/write callbacks are
 * installed by emu_memmap on the Secure window and its bit[28] Non-secure
 * alias (and again on the cpu1 engine), all dispatching to one model.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum periph_map_t
 * @brief Renesas peripheral window (modelled as logged MMIO).
 *
 * @details The Secure physical window; the IDAU bit[28] alias adds
 * 0x10000000. Reads of unmodelled registers return all-ones/0 alternation so
 * "wait for ready bit" polls fall through instead of spinning forever.
 *
 * @invariant The window covers every Renesas peripheral block.
 * @see emu_memmap_init()  Installs the callbacks over this window.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_periph_base = 0x40000000UL, /**< Peripheral window base.                         */
  k_periph_size = 0x10000000UL, /**< 0x40000000-0x4FFFFFFF: all Renesas peripherals. */
} periph_map_t;

/* GLCDC observation point. The lcd_color_cycle demo proves a live panel by
 * re-writing BG_BGC (background colour) every frame and pulsing BG_EN.VEN to
 * commit it. The generic model only keeps the last value per address, so the
 * distinct colours cycled are tracked separately as the tool's success witness.
 * GLCDC base 0x40342000 + 0x1014 = BG_BGC (HUM Ch 63). */
typedef enum : uint64_t {
  k_glcdc_bg_bgc  = 0x40343014UL, /**< GLCDC BG.BGC background colour.    */
  k_bgc_track_max = 32UL,         /**< Distinct BG_BGC values remembered. */
} glcdc_obs_t;

/**
 * @brief UC_MMIO read callback for the peripheral window.
 *
 * @details A modelled board_periph block answers first; the sparse fallback
 * reflects written control values until a spin-poll settles, then alternates
 * 0 / all-ones. The MRMS frequency latches strip their key byte on readback.
 *
 * @param[in,out] uc     Unicorn engine performing the read.
 * @param[in]     offset Byte offset within the mapped window.
 * @param[in]     size   Access width in bytes.
 * @param[in]     user   Hook cookie (unused).
 * @return The value the firmware reads.
 * @retval 0 One phase of the settle toggle (with all-ones as the other).
 * @pre The window was installed by emu_memmap_init().
 * @pre @p offset lies inside the mapped window.
 * @post The access counters and per-slot read runs are updated.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see mmio_write()  The write-side counterpart.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user);

/**
 * @brief UC_MMIO write callback for the peripheral window.
 *
 * @details Notifies the dual-core release watcher, tracks the BG_BGC
 * colour-cycle witness, offers the write to the modelled board_periph blocks
 * first, and otherwise records it in the sparse shadow for readback and the
 * run-end table.
 *
 * @param[in,out] uc     Unicorn engine performing the write.
 * @param[in]     offset Byte offset within the mapped window.
 * @param[in]     size   Access width in bytes.
 * @param[in]     value  Value being written.
 * @param[in]     user   Hook cookie (unused).
 * @return Nothing.
 * @pre The window was installed by emu_memmap_init().
 * @pre @p offset lies inside the mapped window.
 * @post The write landed in a block model or the sparse shadow.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see mmio_read()  The read-side counterpart.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user);

/**
 * @brief Side-effect-free read of the last value written to a peripheral reg.
 *
 * @details
 * Returns the value last written to @p addr, or 0 if it was never written --
 * searching the MMIO shadow WITHOUT allocating a slot and WITHOUT advancing
 * the spin-settle toggle that mmio_read() uses. ra8_emulator's own introspection
 * (e.g. the panel composer reading GLCDC registers) must see stable state: a
 * firmware that never programs the GLCDC would otherwise read the status-poll
 * fallthrough (an alternating 0/0xFFFFFFFF), which made the panel strobe
 * black<->white every frame. A real read of an unwritten register
 * reset-defaults to 0 here, so the panel is a steady background.
 *
 * @param[in] addr Absolute peripheral register address.
 * @return The last written value, or 0 when never written.
 * @retval 0 The register was never written (reset default).
 * @pre The sparse model is live (any time after setup).
 * @pre @p addr is a peripheral-window address.
 * @post No counters or settle state changed (pure lookup).
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
uint32_t mmio_peek(uint64_t addr);

/**
 * @brief Total peripheral MMIO reads this run (monotonic).
 *
 * @return The read counter.
 * @retval 0 No peripheral read has happened yet.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Feeds the idle-stop signature and the board-view telemetry.
 * @since 0.1.0
  * @details Total peripheral mmio reads this run (monotonic); this step is contained within the emu MMIO model and uses bounded caller or module-owned storage.
 * @post Ownership of caller-supplied storage is unchanged.
 */
uint32_t emu_mmio_reads(void);

/**
 * @brief Total peripheral MMIO writes this run (monotonic).
 *
 * @return The write counter.
 * @retval 0 No peripheral write has happened yet.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Feeds the idle-stop signature and the board-view telemetry.
 * @since 0.1.0
  * @details Total peripheral mmio writes this run (monotonic); this step is contained within the emu MMIO model and uses bounded caller or module-owned storage.
 * @post Ownership of caller-supplied storage is unchanged.
 */
uint32_t emu_mmio_writes(void);

/**
 * @brief Print the run-end MMIO counters line to injected error sink.
 *
 * @details Emits the `MMIO reads : ... writes: ... distinct addrs: ...`
 * report line, exactly as the run-end report always printed it.
 *
 * @return Nothing.
 * @pre The run has ended (counters are final).
 * @pre injected error sink is writable.
 * @post One report line was written.
 * @note Not thread-safe; call once at run end.
 * @see emu_mmio_print_bgc_and_table()  The rest of the MMIO report.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
void emu_mmio_print_counts(void);

/**
 * @brief Print the BG_BGC witness and the per-address MMIO table to injected error sink.
 *
 * @details Emits the GLCDC colour-cycle witness line (write count + distinct
 * colours) followed by the per-address reads/writes/last-write table, capped
 * at the print maximum with a `... (N more)` trailer -- exactly as the
 * run-end report always printed them.
 *
 * @return Nothing.
 * @pre The run has ended (the shadow is final).
 * @pre injected error sink is writable.
 * @post The witness + table lines were written.
 * @note Not thread-safe; call once at run end.
 * @see emu_mmio_print_counts()  The counters line printed just before.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
void emu_mmio_print_bgc_and_table(void);

#ifdef __cplusplus
}
#endif
