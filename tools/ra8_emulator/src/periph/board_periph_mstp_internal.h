/**
 * @file board_periph_mstp_internal.h
 * @brief Module-private module-stop (MSTP) gate state shared with the core
 *
 * @details
 * On the RA8D2 every peripheral has a Module Stop bit in one of
 * @c MSTPCRA..MSTPCRE (R_MSTP at @c 0x4020_3000 -- HUM Ch 11.2.6..11.2.10
 * p 443-450). When that bit is @b set the peripheral is clock-gated OFF: it
 * does not respond to bus accesses, so its registers read 0 and writes are
 * dropped. At reset every peripheral bit is 1 (stopped); firmware clears the
 * bit -- via ``ra8_mstp_enable`` -- before it may touch the peripheral.
 *
 * ra8_emulator modelled no module-stop state at all, so a peripheral answered its
 * registers whether or not firmware had released it. A driver that forgot to
 * cancel module-stop therefore worked perfectly in the emulator and did
 * nothing on hardware (#405, the same masked-pass shape as #247's power domain
 * and #131's protected writes).
 *
 * This seam closes that gap. @c board_periph_mstp_model.c owns the five
 * @c MSTPCRx words and the address->bit gate table; the board_periph core
 * (@c board_periph.c) consults ::priv_board_mstp_addr_stopped before dispatching an
 * MMIO access to an owning block, and drops it -- read 0 / write ignored --
 * exactly as the silicon does when the block is stopped. The model half is
 * kept free of any Unicorn dependency so the gate table is unit-testable on the
 * host (tests/test_ra8_emulator_mstp_gate.c); the block glue that needs the engine
 * lives in @c board_periph_mstp.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief R_MSTP register-window geometry (HUM Ch 11.2.6..11.2.10 p 443-450).
 *
 * @details
 * The Secure alias of the block is at @c 0x4020_3000; ra8_emulator's MMIO
 * callbacks rebuild every access -- Secure and the IDAU bit[28] Non-secure
 * alias @c 0x5020_3000 alike -- onto this Secure base, so a single window
 * covers both views. @c MSTPCRA..MSTPCRE occupy the first five 32-bit words.
 *
 * @invariant @c k_board_mstp_win_span == 5 * 4 bytes.
 * @see priv_board_mstp_apply_write
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_board_mstp_win_base = 0x40203000UL, /**< R_MSTP base (Secure alias).     */
  k_board_mstp_win_span = 0x14UL,       /**< MSTPCRA..E = 5 x uint32 = 20 B. */
} board_mstp_geom_t;

/**
 * @brief Restore @c MSTPCRA..MSTPCRE to their all-stopped reset values.
 *
 * @details
 * Every peripheral bit returns to 1 (module stopped), matching the silicon
 * power-on state, so a gated block reads 0 until firmware ungates it. The
 * MSTPCRA SRAM bits (0..3) are left running (0), mirroring the safe pattern
 * ``ra8_mstp_init`` writes (HUM Ch 11.2.6 p 443).
 *
 * @return Nothing.
 * @post Every application-peripheral module-stop bit reads 1 (stopped).
 * @post The dropped-access counters and the family-lookup cache are cleared.
 * @note Not thread-safe; ra8_emulator drives all blocks from one thread.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for board module-stop reset. @pre The call executes on the emulator's single owning thread.
 */
RA8_PRIV void priv_board_mstp_reset(void);

/**
 * @brief Apply a firmware write to the @c MSTPCRA..MSTPCRE shadow.
 *
 * @details
 * Merges @p value into the tracked register bytes at @p off (a 0-based offset
 * into the 20-byte block), so any 8/16/32-bit access width lands correctly.
 * Clearing a bit ungates that peripheral; setting it re-gates it. HUM Ch 11.2.6
 * p 443: "bit clear == ungate the peripheral". The model accepts every bit
 * (PSAR Non-secure attribution is not re-modelled here) because firmware never
 * relies on a Secure write to a delegated bit sticking.
 *
 * @param[in] off   Byte offset within the R_MSTP window (0..19).
 * @param[in] size  Access width in bytes (1/2/4).
 * @param[in] value Little-endian value being written.
 * @return Nothing.
 * @post Bytes of @p value inside the window replace the tracked bytes.
 * @note Not thread-safe; single-threaded run-loop use.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for board module-stop apply write. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
RA8_PRIV void priv_board_mstp_apply_write(uint64_t off, unsigned size, uint32_t value);

/**
 * @brief Read the tracked @c MSTPCRA..MSTPCRE shadow (the read-back path).
 *
 * @details
 * Returns the bytes of the shadow at @p off, so the firmware's mandated
 * module-stop read-back (HUM Ch 11.2.6 Note 2 p 443) sees the value it just
 * wrote and settles immediately -- deterministic, unlike the sparse fallback's
 * spin-settle toggle.
 *
 * @param[in] off  Byte offset within the R_MSTP window (0..19).
 * @param[in] size Access width in bytes (1/2/4).
 * @return The tracked register bytes at @p off, or 0 for bytes outside the
 *         window.
 * @pre @p off addresses the R_MSTP window.
 * @post No state is mutated (pure read).
 * @note Not thread-safe; single-threaded run-loop use.
 * @since 0.1.0
  * @retval value The operation-specific board module-stop read reg value.
 * @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
RA8_PRIV uint32_t priv_board_mstp_read_reg(uint64_t off, unsigned size);

/**
 * @brief Report whether the peripheral instance owning @p addr is module-stopped.
 *
 * @details
 * Maps @p addr through the address->module-stop-bit gate table (each entry
 * cites the governing MSTPCRx bit from HUM Ch 11.2.6..11.2.10) and returns
 * whether that bit is currently set. An address covered by no gate entry -- a
 * peripheral with no module-stop control (GPIO, ICU, SYSC), or one ra8_emulator
 * does not gate -- is never stopped, so unmodelled and un-gated blocks answer
 * exactly as before.
 *
 * @param[in] addr Absolute peripheral register address being accessed.
 * @return @c true when @p addr belongs to a gated instance whose module-stop
 *         bit is set (unclocked); @c false otherwise.
 * @retval true  The owning instance is stopped: reads 0, writes dropped.
 * @retval false Not gated, or the owning instance is running.
 * @post No state is mutated (pure query).
 * @note Not thread-safe; single-threaded run-loop / test use.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for board module-stop addr stopped. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
RA8_PRIV bool priv_board_mstp_addr_stopped(uint64_t addr);

/**
 * @brief Record that an MMIO access to a module-stopped peripheral was dropped.
 *
 * @details
 * Called by the core dispatch when ::priv_board_mstp_addr_stopped forced a read to 0
 * or a write to be discarded. Bumps the dropped-access counter and remembers
 * the offending peripheral's label so the end-of-run report can make the
 * masked bug LOUD instead of silent.
 *
 * @param[in] addr     Absolute address that was gated off.
 * @param[in] is_write @c true for a dropped write, @c false for a zeroed read.
 * @return Nothing.
 * @post The matching counter grows by one and the last-gated label is updated.
 * @note Not thread-safe; single-threaded run-loop use.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for board module-stop note gated access. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
RA8_PRIV void priv_board_mstp_note_gated_access(uint64_t addr, bool is_write);

/**
 * @brief Number of reads zeroed because their peripheral was module-stopped.
 * @return The dropped-read counter since the last ::priv_board_mstp_reset.
 * @post No state is mutated.
 * @since 0.1.0
  * @details Number of reads zeroed because their peripheral was module-stopped; this step is contained within the board periph module-stop model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific board module-stop gated read count value.
 * @pre Arguments satisfy the ranges documented for board module-stop gated read count. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
RA8_PRIV uint32_t priv_board_mstp_gated_read_count(void);

/**
 * @brief Number of writes dropped because their peripheral was module-stopped.
 * @return The dropped-write counter since the last ::priv_board_mstp_reset.
 * @post No state is mutated.
 * @since 0.1.0
  * @details Number of writes dropped because their peripheral was module-stopped; this step is contained within the board periph module-stop model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific board module-stop gated write count value.
 * @pre Arguments satisfy the ranges documented for board module-stop gated write count. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
RA8_PRIV uint32_t priv_board_mstp_gated_write_count(void);

/**
 * @brief Label of the peripheral whose access was most recently gated off.
 * @return A static string (e.g. "SCI"), or "-" when nothing has been gated.
 * @post No state is mutated.
 * @since 0.1.0
 */
RA8_PRIV const char* priv_board_mstp_last_gated_name(void);

#ifdef __cplusplus
}
#endif
