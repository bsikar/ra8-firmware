/**
 * @file ra8_board_memmap.h
 * @brief The physical memory map of the project's RA8P1 (R7KA8P1KFLCAC) target board, published once
 * for every consumer
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Where everything is, stated once. Until now the answer lived only in the
 * `MEMORY{}` block of a linker script, which is readable by the linker and by
 * nothing else, so every host-side consumer that needed the same numbers
 * retyped them: `tools/ra8_emulator/src/engine/emu_memmap.c` restates the
 * region table as C literals, `tools/ra8_emulator/inc/emu_memmap.h` restates
 * three of the bases a second time as an enum, and `tests/mocks/ra8_fake_mmap.c`
 * declares a third copy under a third set of names. Four spellings of one fact,
 * and correcting one of them corrected nothing else (#758).
 *
 * This header is the board layer's answer: the same regions the board's
 * `ld/linker_script.ld` declares, as ordinary C constants a host tool or a
 * firmware translation unit can include. It is DATA, not policy. It says where
 * the windows are; it does not say what may be placed in them, which stays the
 * linker script's business, and an application script is still free to carve a
 * smaller `MEMORY{}` out of these windows (`apps/board/stand_alone/ereader`
 * takes 512K of the 1M MRAM so its Non-Secure image has dedicated bytes).
 *
 * PINNED, NOT DUPLICATED. `scripts/checks/check_board_memory_map.py` parses the
 * `MEMORY{}` block next door and this enum, and fails the build when the two
 * disagree on a region's origin or length, or when either declares a region the
 * other does not. So this is a second SPELLING of the board's map, deliberately,
 * but it cannot become a second VERSION of it.
 *
 * NOT YET CONSUMED. Landing the descriptor and its pin is the first slice of
 * #758. Pointing the emulator's region table, the emulator's core-register
 * window and `ra8_fake_mmap.c` at these constants, and generating the linker
 * script's `MEMORY{}` from them, are the slices that follow; until they land,
 * those three copies still exist and the gate does not yet know about them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_board_memmap_base_t
 * @brief Start address of every region the board's linker script declares.
 *
 * @details
 * Addresses are the SECURE aliases, which is how the linker script spells them
 * and how the flashing tool has to target them. The two `ns_` entries are the
 * Non-Secure partition placeholders: they are views of the same physical bytes
 * as the secure windows above them in the single-image build, and the SAU
 * programmes the split at boot.
 *
 * @note Every value here is pinned to `ld/linker_script.ld` by
 *       `scripts/checks/check_board_memory_map.py`.
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_board_mram_base    = 0x02000000UL, /**< Code MRAM, secure alias.     */
  k_ra8_board_ofs_cfg_base = 0x02C9F000UL, /**< Option-setting config words. */
  k_ra8_board_ofs_otp_base = 0x02E07000UL, /**< OTP / anti-rollback area.    */
  k_ra8_board_itcm_base    = 0x00000000UL, /**< Instruction TCM.             */
  k_ra8_board_dtcm_base    = 0x20000000UL, /**< Data TCM.                    */
  k_ra8_board_sram_base    = 0x22000000UL, /**< On-chip SRAM, less NOINIT.   */
  k_ra8_board_noinit_base  = 0x220FFF00UL, /**< Warm-reset crash-log record. */
  k_ra8_board_sdram_base   = 0x68000000UL, /**< External SDRAM.              */
  k_ra8_board_ns_mram_base = 0x02080000UL, /**< Non-Secure MRAM placeholder. */
  k_ra8_board_ns_sram_base = 0x22100000UL, /**< Non-Secure SRAM placeholder. */
} ra8_board_memmap_base_t;

/**
 * @enum ra8_board_memmap_size_t
 * @brief Length in bytes of every region the board's linker script declares.
 *
 * @details
 * `k_ra8_board_sram_size` is 256 bytes short of a full mebibyte because the top
 * of SRAM is carved off into the NOINIT region for the crash-log record that
 * has to survive a warm reset; the two lengths sum to 1 MiB.
 *
 * @note Every value here is pinned to `ld/linker_script.ld` by
 *       `scripts/checks/check_board_memory_map.py`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_board_mram_size    = 0x00100000UL, /**< 1 MiB.                       */
  k_ra8_board_ofs_cfg_size = 0x00000800UL, /**< 2 KiB.                       */
  k_ra8_board_ofs_otp_size = 0x00011000UL, /**< 68 KiB.                      */
  k_ra8_board_itcm_size    = 0x00010000UL, /**< 64 KiB.                      */
  k_ra8_board_dtcm_size    = 0x00010000UL, /**< 64 KiB.                      */
  k_ra8_board_sram_size    = 0x000FFF00UL, /**< 1 MiB less the 256 B NOINIT. */
  k_ra8_board_noinit_size  = 0x00000100UL, /**< 256 B.                       */
  k_ra8_board_sdram_size   = 0x04000000UL, /**< 64 MiB.                      */
  k_ra8_board_ns_mram_size = 0x00080000UL, /**< 512 KiB.                     */
  k_ra8_board_ns_sram_size = 0x000A0000UL, /**< 640 KiB.                     */
} ra8_board_memmap_size_t;

#ifdef __cplusplus
}
#endif
