/**
 * @file ra_flash.h
 * @brief Code MRAM + Extra MRAM + Option-Setting driver -- DANGEROUS, brick-capable
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * On RA8D2 the "code flash" is **MRAM**, a non-volatile, byte-erasable
 * magnetoresistive RAM array exposed at ``0x02000000`` for 1 MiB. The
 * controller block (``R_MRMS``, secure base ``0x4013_C000``, HUM
 * Ch 59.5.1 p 3551) presents a small write-buffer + status register
 * window. Three independent write paths exist:
 *
 * - **Direct STR programming** of the code-MRAM page through the
 * ``MRCPC0`` / ``MRCPC1`` gate (HUM Ch 59.4.2 Figure 59.4 p 3548).
 * - **MACI command sequencer** for configuration-set, anti-rollback
 * counters, status-clear, and forced-stop (HUM Ch 59.4.4 p 3550 +
 * HUM Ch 7 p 278..299 for the OFS layout).
 * - **Block-protection writes** to ``MRCBPROT0`` / ``MRCBPROT1`` to
 * permanently or temporarily lock individual pages.
 *
 * This wave (Round 3) brings the driver to **full HUM Ch 7 + Ch 59
 * coverage**:
 *
 * - ``ra_flash_init`` / ``ra_flash_deinit`` -- bring-up + safe-state.
 * - ``ra_flash_get_status`` / ``ra_flash_clear_status`` -- MRCPS.
 * - ``ra_flash_get_extended_status`` -- MSTATR + MASTAT + MREZS.
 * - ``ra_flash_set_rww_disable`` -- MRCPFB prefetch park.
 * - ``ra_flash_write_block`` / ``ra_flash_erase_block`` -- 1..32 byte
 * direct programming.
 * - ``ra_flash_block_protect_set`` -- per-world MRCBPROT lock/unlock.
 * - ``ra_flash_set_startup_area`` -- temporary (MSUACR) and permanent
 * (configuration-set) boot-bank swap.
 * - ``ra_flash_get_startup_area`` -- read MSUASMON.
 * - ``ra_flash_arc_increment`` / ``ra_flash_arc_read`` --
 * anti-rollback counter MACI flow.
 * - ``ra_flash_config_set_write`` -- low-level MACI program primitive:
 * Configuration Set (0x40) for the OFS area, Program (0xE8) for the
 * extra-MRAM data area, chosen by target region.
 * - ``ra_flash_zeroize_huk`` -- MREZC trigger for the W-HUK zeroize.
 * - ``ra_flash_force_stop`` / ``ra_flash_reset`` -- abort + recover.
 * - ``ra_flash_set_security_attribution`` -- MSAR per-region SA.
 * - ``ra_flash_set_irq_enable`` -- per-source enable for the four
 * documented IRQ paths.
 * - ``ra_flash_callback_set`` -- single-callback IRQ dispatcher.
 * - ``ra_flash_dispatch_isr`` -- explicit dispatch entry the
 * BSP IRQ vector calls.
 * - ``ra_flash_set_ecc_encoder_enable`` /
 * ``ra_flash_set_ecc_decoder_enable`` -- MRCEECC / MRCDECC controls.
 * - ``ra_flash_get_ecc_error_addr`` -- TED / DEC fault addresses.
 * - ``ra_flash_get_program_error_addr`` -- MRCPEA.
 * - ``ra_flash_update_clock_freq`` -- re-issue MRCFREQ / MREFREQ.
 * - ``ra_flash_msuinitr_kick`` -- re-fetch OFS via MSUINITR.
 * - ``ra_flash_set_update_transfer`` / ``ra_flash_get_update_status``
 * -- MCTRCNTR / MCTRSTATR / MCTRLSR.
 * - ``ra_flash_extra_mram_write`` / ``ra_flash_extra_mram_erase`` --
 * extra-MRAM (data) program / erase via the MACI Program command
 * (0xE8, HUM Ch 59.7.4.5 p 3591).
 * - ``ra_flash_enter_pe_mode`` / ``ra_flash_exit_pe_mode`` -- exposed
 * for callers that need to batch multiple MACI commands without
 * paying the per-call P/E transition cost.
 *
 * @warning **THIS DRIVER CAN PERMANENTLY BRICK THE MCU.** A program
 * that overwrites its own reset vectors, FSBL hash, the
 * option-setting trust anchors at ``0x0100_A100``, the
 * permanent block-protect bits, or the start-up area control
 * register may be unrecoverable through SWD and require
 * vendor RMA. NEVER call any *write* / *erase* /
 * *block_protect* / *config_set* / *startup* / *zeroize*
 * API with a destination inside the running image's
 * ``.text`` section, the secure boot area, or the OFS region
 * unless you are absolutely certain. The driver does **not**
 * sanity-check the destination beyond enforcing the 1 MB
 * MRAM window -- caller is fully responsible.
 *
 * @warning ``ra_flash_zeroize_huk`` is **one-shot and permanent**. Once
 * fired, the silicon's wrapped HUK (W-HUK) is destroyed and
 * every secret that was wrapped under it becomes irretrievable.
 * The matching ``MREZS.WHUKZF`` flag latches forever. 
 * callers should keep this API behind a project-level
 * policy gate.
 *
 * @warning ``ra_flash_block_protect_set`` with the *permanent* flag
 * set writes a one-shot fuse. Once set, the block can never
 * be re-enabled without a new fuse cycle on the part. There
 * is no undo.
 *
 * @warning The driver does not protect against power loss mid-write.
 * A reset between the data store and the ``MRCFLR`` flush
 * leaves the affected 32-byte page in an indeterminate
 * state. For at-rest critical regions use a journaled
 * two-bank scheme on top of this driver.
 *
 * @warning Programming **must not** run from MRAM that is being
 * programmed. The HUM allows BGO (background read of the
 * opposite memory while the other is programmed) but the
 * code-MRAM path requires the write loop itself to live
 * in SRAM. ``ra_flash_write_block`` does **not** relocate
 * itself -- caller must ensure the call site is in SRAM
 * (e.g. ``__attribute__((section(".sram_text")))``) when
 * targeting MRAM addresses.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_flash_regs.h"
#include "ra_err.h"

/*
 * Thin umbrella: the declarations that used to live here were split into
 * cohesive self-contained sub-headers to keep every header under the
 * project file-size budget. Consumers continue to ``#include "ra_flash.h"``
 * unchanged; this umbrella pulls in every sub-header in dependency order
 * (shared types first, then the function-prototype surfaces).
 */
#include "ra_flash_core.h"  /* core register-level MRAM driver API */
#include "ra_flash_fsp.h"   /* FSP r_mram parity surface           */
#include "ra_flash_types.h" /* enums, structs, callback typedef    */

#ifdef __cplusplus
}
#endif
